#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace rx::task {

// rx::task::Scheduler -- the engine's single task scheduler [spec D2]:
// thinly wraps enki::TaskScheduler (zlib, pinned at v1.12 -- see
// third_party/CMakeLists.txt's own comment for the vendoring rationale)
// entirely inside scheduler.cpp's private Impl below, so this header never
// names an enki type at all. That is deliberate, not incidental: D2 chose
// enkiTS over Taskflow as the SOLE engine scheduler but explicitly wants
// the dependency to stay swappable, and a caller of this header never
// needs enkiTS's own include directory or any of its types to use a
// Scheduler.
//
// THREAD-AFFINITY (D5; see docs/threading.md for the full contract this
// header is one instance of): create() must be called on the thread that
// will act as this Scheduler's "main" thread for its entire lifetime --
// that same thread (and ONLY that thread) may call parallelFor() and
// pumpMain(). postToMain() and runOnIoThread() are safe to call from any
// thread, including from inside a parallelFor() chunk callback or from the
// dedicated IO thread itself.
//
// One Scheduler per application process is the intended usage (D2: "one
// per app"); nothing here enforces that as a singleton, but constructing
// more than one competes needlessly for the same hardware threads.
class Scheduler {
 public:
  // Creates and Initialize()s the underlying task scheduler. Must be
  // called from the thread that will subsequently be this Scheduler's
  // "main" thread (see the class comment) -- enkiTS registers whichever
  // thread calls its Initialize() as internal thread 0, and this factory
  // calls that from the calling thread directly, on purpose, so "the
  // thread that called create()" and "enkiTS's main-participating thread"
  // are always the same thread.
  //
  // `workerCount` is the number of background worker threads that
  // participate in parallelFor() work-stealing, IN ADDITION to the
  // calling/main thread (which also participates -- see parallelFor()) and
  // the single dedicated IO thread runOnIoThread() uses (which never runs
  // parallelFor() work -- see that method). Passing 0 resolves to
  // `std::thread::hardware_concurrency() - 1`, clamped to a minimum of 1
  // (so a Scheduler always has at least one background worker even on a
  // reported-2-hardware-thread machine). workerCount() below returns the
  // resolved value.
  //
  // WORKER COUNT IS THE CONSUMER'S BUDGET, NOT A MACHINE-WIDE DEFAULT
  // (see docs/threading.md's "Host-engine coexistence" section): the
  // `hardware_concurrency() - 1` default above is for a STANDALONE
  // consumer that owns the whole machine (this phase's own samples and
  // tests). An embedding host engine is expected to pass the worker
  // budget IT has decided to grant the renderer instead -- every
  // parallelFor() call self-scales to whatever workerCount() this
  // Scheduler actually ends up with via autoGrainSize(), so a smaller
  // granted budget costs the caller nothing beyond this one number.
  //
  // Returns nullptr if the underlying scheduler failed to start (out-of-
  // memory or a platform thread-creation failure only -- there is no
  // recoverable configuration-error path here).
  static std::unique_ptr<Scheduler> create(uint32_t workerCount = 0);

  // Shuts the underlying task scheduler down: stops accepting new
  // runOnIoThread() submissions, requests shutdown, waits for whatever the
  // IO thread is already running to finish (enkiTS's own
  // WaitforAllAndShutdown() drains everything already queued before it
  // returns -- empirically verified, see scheduler.cpp), then joins every
  // worker thread including the IO thread. Any runOnIoThread() submission
  // that arrives concurrently with this destructor (a caller responsibility
  // violation -- destroying a Scheduler while another thread might still
  // call it is a lifetime bug on the caller's part regardless of this
  // note) is refused rather than silently leaked: refused-at-the-door
  // submissions, and the vanishingly rare case of one that slips past that
  // check but never gets a chance to run before every thread is joined,
  // are both deleted (never executed) and counted -- see
  // detail::debugLastDroppedIoTaskCount() and scheduler.cpp's own comment
  // on why the latter path is defense-in-depth rather than something this
  // task could reproduce on demand. postToMain() work still queued when
  // pumpMain() is never called again is simply dropped (its captured
  // closure destroyed as an ordinary side effect) -- this is not a
  // drain-to-completion operation for that queue.
  ~Scheduler();

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  Scheduler(Scheduler&&) = delete;
  Scheduler& operator=(Scheduler&&) = delete;

  // Blocking fan-out over the half-open range [0, itemCount): splits it
  // into contiguous chunks of at least the EFFECTIVE grain size (enkiTS's
  // own "min range" concept -- the last chunk may be smaller than that if
  // itemCount is not a multiple of it) and calls `fn` once per chunk with
  // that chunk's [begin, end) and the index of whichever worker executed
  // it. Returns only once every chunk has run.
  //
  // GRAIN SIZE [spec D4 amendment, "Parallelism is the engine default,
  // not a mode": "there is no on/off switch and no caller-chosen chunk
  // count... self-scaling, never toggled" -- see docs/threading.md's own
  // section on this]: `grainSize == 0` means AUTO -- the effective grain
  // is `autoGrainSize(itemCount, workerCount())` (below), which is also
  // exactly what the `parallelFor(itemCount, fn)` overload below uses; a
  // caller never HAS to choose a grain to get parallel execution. A
  // nonzero `grainSize` is used verbatim as the effective grain -- kept as
  // a measurement affordance for the rare caller doing controlled tuning
  // (the stress benchmark's `--threads`-adjacent instrumentation is the
  // motivating case), not a general-purpose "mode" callers are expected
  // to reach for.
  //
  // THE CALLING THREAD PARTICIPATES: this is enkiTS's documented default
  // behavior (TaskScheduler::WaitforTask() runs pending chunks itself
  // while it blocks), not an accident of this wrapper -- a single-item
  // parallelFor() on a Scheduler with zero background workers still runs
  // `fn` exactly once, on the calling thread, with no background thread
  // involved at all. `workerIndex` values therefore include the calling
  // thread's own index (0) alongside every background worker's index;
  // this wrapper draws no distinction between them in the callback.
  //
  // Safe to call reentrantly from within a chunk callback (a "nested"
  // parallelFor()) -- the inner call's WaitforTask() participates in
  // running the OUTER task's remaining chunks too while it waits, which is
  // exactly what makes this safe rather than a deadlock: enkiTS's task
  // pipe has no notion of "this thread already owns an outer wait", so a
  // nested call is just another task the same machinery drains.
  //
  // Must be called from this Scheduler's main thread (see the class
  // comment) or from within a task already running on one of this
  // Scheduler's own threads (worker or nested-parallelFor call) -- never
  // from an unrelated thread that never registered with this Scheduler.
  void parallelFor(uint32_t itemCount, uint32_t grainSize,
                    std::function<void(uint32_t begin, uint32_t end, uint32_t workerIndex)> fn);

  // The engine-default path [spec D4 amendment]: always auto-grain.
  // Equivalent to `parallelFor(itemCount, 0, fn)` -- exists so ordinary
  // callers never need to know grainSize/autoGrainSize() exist at all to
  // get parallel execution; see the three-argument overload's own doc
  // comment for the full grain-size contract this delegates into.
  void parallelFor(uint32_t itemCount, std::function<void(uint32_t begin, uint32_t end, uint32_t workerIndex)> fn);

  // Auto-grain floor: parallelFor()'s AUTO heuristic (grainSize == 0,
  // including the two-argument overload above) never picks an effective
  // grain smaller than this, regardless of itemCount/workerCount() --
  // floors the per-item scheduling overhead for trivial callback bodies.
  // See autoGrainSize()'s own doc comment for the full formula this
  // participates in.
  static constexpr uint32_t kMinGrain = 64;

  // The exact formula parallelFor() uses internally for its AUTO grain
  // [spec D4 amendment]: `max(kMinGrain, itemCount / (workerCount * 4))`.
  // Four chunks per worker is the balance point this formula encodes
  // between stealing granularity (more, smaller chunks let enkiTS's
  // work-stealing rebalance more finely across workers finishing at
  // different times) and per-task scheduling overhead (fewer, larger
  // chunks mean less enkiTS bookkeeping per item processed); kMinGrain
  // then floors the result so a trivial callback body on a small
  // itemCount never gets sliced finer than the point where per-chunk
  // overhead would dominate the actual work.
  //
  // A pure, static function -- deliberately callable without a live
  // Scheduler instance (directly unit-testable against known
  // itemCount/workerCount pairs), and exactly what a real parallelFor()
  // call computes internally when grainSize == 0, passing this
  // Scheduler's own workerCount(). `workerCount == 0` is treated the same
  // as `workerCount == 1` (defensive: no real Scheduler's workerCount()
  // is ever 0, but this function accepts arbitrary input).
  [[nodiscard]] static uint32_t autoGrainSize(uint32_t itemCount, uint32_t workerCount);

  // Enqueues `fn` to run, later, on this Scheduler's single dedicated IO
  // thread -- never on a parallelFor() worker, and never on the calling
  // thread itself (unless the calling thread happens to BE the IO thread,
  // e.g. an IO task itself queuing a follow-up IO task). Calls are
  // executed strictly FIFO relative to each other, in the order
  // runOnIoThread() was called (enkiTS's own pinned-task list guarantees
  // this -- see scheduler.cpp for exactly how). Safe to call from any
  // thread, including concurrently from several threads at once and from
  // within a parallelFor() chunk or a postToMain() callback.
  //
  // Intended for the D5 handoff pattern's blocking-I/O half (file reads,
  // decode-adjacent syscalls) -- see docs/threading.md. `fn` must not
  // block indefinitely: this Scheduler has exactly one IO thread, so a
  // stuck `fn` stalls every runOnIoThread() call queued after it.
  void runOnIoThread(std::function<void()> fn);

  // [Phase 4 Stage 1 Task 15, additive] Enqueues `fn` to run, later, on ONE
  // of this Scheduler's regular background parallelFor() worker threads
  // (round-robin over [1, workerCount()]) -- NEVER on the calling thread,
  // NEVER on the main thread, and NEVER on the dedicated IO thread. Unlike
  // runOnIoThread()'s target (a thread permanently dedicated to pinned
  // tasks, via IoLoopTask's own infinite Execute() loop below), the target
  // worker here is an ORDINARY parallelFor() participant: enkiTS's own
  // per-thread dispatch loop (TaskingThreadFunction) already checks for
  // and runs any pinned task addressed to it (TryRunTask() calls
  // RunPinnedTasks() first, every iteration -- verified directly against
  // the pinned enkiTS v1.12 source) as part of its NORMAL cycle, so this
  // costs nothing beyond one enkiTS pinned-task dispatch and returns the
  // worker to ordinary parallelFor() duty immediately after `fn` returns --
  // no new thread is created, and workerCount() is unchanged.
  //
  // WHY THIS EXISTS [documented per the repository's "don't touch rx_task
  // unless a genuine blocking defect forces it" policy -- flagged here
  // prominently, see docs/threading.md's own cross-reference and the
  // task-15-report.md rationale for the full analysis]: the async import
  // pipeline (Task 15) needs to run CPU-heavy, potentially slow decode
  // work (parse/transcode/tangent-generation/meshopt) via parallelFor()
  // WITHOUT blocking the main thread and WITHOUT ever touching the
  // dedicated IO thread (which must stay free for FIFO byte-source reads).
  // Every OTHER Scheduler primitive fails at least one of those two
  // requirements: parallelFor() itself may only legally be called from the
  // main thread (blocks it for the call's whole duration -- unacceptable
  // for a "deliberately slow decode" workload) or from a task already
  // running on one of this Scheduler's own threads; runOnIoThread()'s own
  // callback runs ON the dedicated IO thread, and calling parallelFor()
  // FROM one nests into that same IO thread's own WaitforTask()
  // participation (verified directly against the pinned enkiTS source:
  // WaitforTask()'s spin loop calls TryRunTask() on the CALLING thread,
  // which would let the IO thread itself execute decode chunks -- exactly
  // the invariant Task 15 must not violate); and this Scheduler
  // deliberately refuses arbitrary foreign threads (an unregistered
  // caller's `gtl_threadNum` collides with thread 0's own enkiTS
  // registration -- confirmed against the vendored source, not merely
  // asserted by the class comment above). runOnWorkerThread()'s own `fn`
  // runs on a genuine, ALREADY-registered enkiTS worker thread, so a
  // NESTED parallelFor() call from inside it is both legal (per the
  // paragraph above) and structurally incapable of reaching the IO thread
  // (IoLoopTask's own Execute() loop never returns to enkiTS's ordinary
  // TaskSet-stealing path at all, by construction -- see IoLoopTask below).
  //
  // Safe to call from any thread (mirrors runOnIoThread()'s own contract);
  // `fn` itself may safely call parallelFor() (nested/reentrant, exactly
  // as any worker-thread chunk callback may -- see parallelFor()'s own doc
  // comment) or postToMain(). Multiple concurrent submissions are safe and
  // do not need to target the same worker thread to be correct -- FIFO
  // ordering across DIFFERENT target threads is not guaranteed (unlike
  // runOnIoThread()'s single-thread FIFO contract), only per-target-thread
  // ordering is.
  void runOnWorkerThread(std::function<void()> fn);

  // Enqueues `fn` to run later, on whichever thread next calls pumpMain()
  // (intended to always be this Scheduler's main thread -- see the class
  // comment). FIFO relative to other postToMain() calls made from the
  // SAME calling thread (a plain mutex-guarded queue -- see scheduler.cpp
  // -- gives a single total order across every caller, but only a single
  // thread's own sequence of calls has a meaningful "before/after" to
  // preserve; interleavings across concurrently-calling threads settle in
  // whatever order they acquire the queue's mutex). Safe to call from any
  // thread. This is the D5 handoff mechanism workers use to hand GPU-object
  // mutation back to the main thread (docs/threading.md) -- deliberately
  // NOT built on any enkiTS pinned-task mechanism (kept thin and testable
  // on its own).
  void postToMain(std::function<void()> fn);

  // Drains every function queued by postToMain() so far and runs each, in
  // FIFO order, on the CALLING thread -- intended to be called once per
  // frame from this Scheduler's main thread (the frame loop's own pump
  // point). A function queued by a postToMain() call made concurrently
  // with (or after) a given pumpMain() call may run on this call or a
  // later one; pumpMain() never blocks waiting for more work to arrive.
  //
  // Thread-affinity (D5, Phase 4; audit finding F5-partial): main-thread-
  // only -- carries a dev-time RX_ASSERT_MAIN_THREAD guard -- see
  // docs/threading.md.
  void pumpMain();

  // Number of background parallelFor() worker threads (the resolved value
  // if 0 was passed to create()) -- EXCLUDES the calling/main thread and
  // the dedicated IO thread, both of which exist and participate in this
  // Scheduler's work regardless of this count (see create()'s and
  // parallelFor()'s own comments for exactly how each participates).
  uint32_t workerCount() const;

 private:
  explicit Scheduler(uint32_t workerCount);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

namespace detail {

// Test-only seam -- NOT part of the stable public contract, mirroring
// rx::material::detail::debugCompileCount()'s own carve-out convention
// (there is no other way to observe this from outside: the count only
// exists during, and briefly after, a specific Scheduler's destruction).
// Returns how many runOnIoThread() submissions the MOST RECENTLY
// DESTROYED Scheduler in this process dropped (deleted without ever
// calling their fn) at teardown -- either refused outright because that
// Scheduler's destructor had already begun, or (the defense-in-depth
// path -- see scheduler.cpp) still unexecuted after its dedicated IO
// thread was fully joined. 0 if no Scheduler has been destroyed yet in
// this process, or the last one destroyed dropped nothing.
[[nodiscard]] size_t debugLastDroppedIoTaskCount();

}  // namespace detail

}  // namespace rx::task
