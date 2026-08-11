#pragma once
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
  // Returns nullptr if the underlying scheduler failed to start (out-of-
  // memory or a platform thread-creation failure only -- there is no
  // recoverable configuration-error path here).
  static std::unique_ptr<Scheduler> create(uint32_t workerCount = 0);

  // Shuts the underlying task scheduler down: requests shutdown, wakes and
  // joins every worker thread (including the dedicated IO thread's pinned-
  // task loop) and only then returns. Safe even with pending
  // postToMain()/runOnIoThread() work still queued (it is simply dropped,
  // matching a normal application-teardown expectation -- this is not a
  // drain-to-completion operation).
  ~Scheduler();

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  Scheduler(Scheduler&&) = delete;
  Scheduler& operator=(Scheduler&&) = delete;

  // Blocking fan-out over the half-open range [0, itemCount): splits it
  // into contiguous chunks of at least `grainSize` items (enkiTS's own
  // "min range" / grain-size concept -- the last chunk may be smaller than
  // `grainSize` if itemCount is not a multiple of it) and calls `fn` once
  // per chunk with that chunk's [begin, end) and the index of whichever
  // worker executed it. Returns only once every chunk has run.
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

}  // namespace rx::task
