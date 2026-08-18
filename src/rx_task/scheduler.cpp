#include "rx_task/scheduler.h"

// enkiTS's own installed/BUILD_INTERFACE include layout puts TaskScheduler.h
// directly on the include path with no subdirectory prefix (verified
// directly against the installed tree: enkiTS::enkiTS's
// INTERFACE_INCLUDE_DIRECTORIES is "<prefix>/include/enkiTS", not
// "<prefix>/include") -- see third_party/CMakeLists.txt's own comment.
#include <TaskScheduler.h>

#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace rx::task {

namespace {

// Process-wide, most-recently-destroyed-Scheduler drop count backing
// detail::debugLastDroppedIoTaskCount() (scheduler.h). A plain static
// rather than a per-instance field on Scheduler itself because the whole
// point is to observe it AFTER the Scheduler that produced it is gone --
// see scheduler.h's own comment on this seam. Tests in this codebase run
// sequentially (doctest's default), so "most recently destroyed" is
// unambiguous in practice.
std::atomic<size_t> g_lastDroppedIoTaskCount{0};

// ---------------------------------------------------------------------
// [Fix round 1, CRITICAL -- supersedes BOTH the original two-phase-delete
// design AND this fix round's own first-draft "sequence-numbered reclaim"
// attempt] Persistent task + closure queue. See docs/threading.md's own
// "Pinned-task dispatch: persistent tasks, not per-submission ones"
// section for the full contract and the complete TSAN evidence trail this
// replaces; summarized here at the point of the fix itself.
//
// THE BUG THIS REPLACES: the original design (`IoTask`, one-shot
// `enki::IPinnedTask` per runOnIoThread()/runOnWorkerThread() call) tried
// to safely `delete` each task once it looked "done" -- gated first on
// `isPublished()` alone, then (after an ASan+ENKI_ASSERT-caught UAF)
// additionally on `GetIsComplete()`. BOTH gates are provably unsafe:
// enkiTS's own `RunPinnedTasks(threadNum_, priority_)` does
// `Execute(); m_RunningCount.fetch_sub(...); TaskComplete(pTask_, ...)`,
// and `TaskComplete()` KEEPS reading/writing `pTask_` (m_WaitingForTaskCount,
// m_pDependents, a redundant m_RunningCount store) for several more
// instructions AFTER the fetch_sub that makes `GetIsComplete()` observe
// true. A reaper thread gated on that signal can delete the object while
// the EXECUTING thread is still inside TaskComplete() -- TSAN-confirmed,
// 100% reproducible, by this fix round's own adversarial churn harness
// (PinnedTaskChurnTest, scheduler_test.cpp) AND independently by the
// reviewing coordinator's own harness. The SAME race pre-dates Task 15
// entirely (it was already latent in the original runOnIoThread()-only
// design from Stage 0's own F1 closure) -- this fix closes it for real,
// for both call sites, by construction, not by finding a fourth gate.
//
// THE FIX: eliminate per-submission enkiTS task objects ENTIRELY. Exactly
// ONE `enki::IPinnedTask` is ever registered per served thread (the
// dedicated IO thread; and ONE dedicated "worker task lane" thread for
// runOnWorkerThread(), chosen over round-robining across the ordinary
// worker pool -- see this file's own Scheduler::Scheduler() comment for
// why), each allocated ONCE at Scheduler construction and never touched
// by enkiTS's per-task completion machinery again until shutdown.
// runOnIoThread()/runOnWorkerThread() never call AddPinnedTask() at all --
// they push a plain `std::function<void()>` onto a ClosureQueue (a
// standard mutex+condition_variable MPSC queue, the exact same shape as
// this file's own pre-existing postToMain()/pumpMain() queue); the ONE
// persistent task's own Execute() loop drains the queue and invokes each
// closure ON ITSELF, same-thread, then lets it destruct when the local
// batch vector goes out of scope. There is no "is this task done yet"
// question anywhere in this design for enkiTS to answer, correctly or
// otherwise -- nothing is ever reaped, because nothing per-submission is
// ever allocated as an enkiTS task in the first place. Freeing only
// happens for the two PERSISTENT loop-task objects themselves, in
// ~Scheduler(), strictly after WaitforAllAndShutdown() returns (the one
// point enkiTS itself guarantees no thread will ever touch them again).
class ClosureQueue {
 public:
  // Safe from any thread (mirrors postToMain()'s own identical contract).
  void push(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(fn));
    }
    cv_.notify_one();
  }

  // Blocks (a genuine kernel-level wait via std::condition_variable, never
  // a spin loop -- matching this Scheduler's own documented "bursty, not
  // always-on" idle posture, docs/threading.md's "Host-engine coexistence"
  // section) until EITHER at least one closure is queued OR shutdown has
  // been requested, then drains and returns everything currently queued
  // (possibly empty, iff shutdown was requested with nothing left).
  std::vector<std::function<void()>> waitAndDrain() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
    std::vector<std::function<void()>> drained(std::make_move_iterator(queue_.begin()),
                                                std::make_move_iterator(queue_.end()));
    queue_.clear();
    return drained;
  }

  // Wakes the draining thread so it can observe shutdown and exit its loop
  // -- MUST be called before WaitforAllAndShutdown() (see ~Scheduler()'s
  // own comment for why: WaitforAllAndShutdown() blocks until the
  // persistent loop task's Execute() call returns, which only happens
  // once this queue tells it to).
  void requestShutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_ = true;
    }
    cv_.notify_all();
  }

  // Test/diagnostic + teardown-accounting only: how many closures are
  // still sitting here, unrun -- always 0 in every realistic run (see
  // ClosureQueueLoopTask::Execute()'s own comment on draining a final
  // burst before exiting); nonzero only in the same vanishingly narrow
  // caller-races-destructor window this project's own pre-existing F1 fix
  // already disclosed (a push() whose runOnIoThread()/runOnWorkerThread()
  // caller passed the acceptingIoTasks check but had not yet reached this
  // call when requestShutdown() ran).
  size_t sizeForTesting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool shutdown_ = false;
};

// The persistent pinned task itself -- one instance drains the IO queue,
// pinned to the dedicated IO thread; a second, independent instance
// drains the worker-task queue, pinned to the dedicated worker-task-lane
// thread (see Scheduler::Scheduler()'s own comment for the thread-number
// layout). Both are constructed once and registered with enkiTS exactly
// once, at Scheduler construction -- this is the ONLY AddPinnedTask() call
// left anywhere in this file.
class ClosureQueueLoopTask final : public enki::IPinnedTask {
 public:
  ClosureQueueLoopTask(uint32_t threadNum, ClosureQueue& queue) : enki::IPinnedTask(threadNum), queue_(queue) {}

  void Execute() override {
    while (true) {
      // `batch` is a plain local std::vector<std::function<void()>> --
      // every closure in it is invoked and destroyed on THIS thread, by
      // ordinary RAII, the instant this loop iteration ends. No other
      // thread ever touches these objects: race-free by construction, not
      // by a completion signal anyone has to get right.
      std::vector<std::function<void()>> batch = queue_.waitAndDrain();
      if (batch.empty()) {
        // Only reachable when requestShutdown() was the reason
        // waitAndDrain() woke, AND nothing was left to run -- see
        // ClosureQueue::waitAndDrain()'s own comment. A shutdown that
        // arrives with a final burst still queued drains and runs that
        // burst first (the loop's own NEXT iteration then sees shutdown +
        // empty and returns) -- matching this Scheduler's pre-existing
        // documented guarantee that every closure already handed to it
        // runs to completion before teardown proceeds.
        return;
      }
      for (auto& fn : batch) {
        fn();
      }
    }
  }

 private:
  ClosureQueue& queue_;
};

uint32_t resolveWorkerCount(uint32_t requested) {
  if (requested != 0) {
    return requested;
  }
  const uint32_t hw = std::thread::hardware_concurrency();
  return hw > 1 ? hw - 1 : 1;
}

}  // namespace

struct Scheduler::Impl {
  enki::TaskScheduler taskScheduler;
  uint32_t resolvedWorkerCount = 0;

  // [Fix round 1] Thread-number layout: [1, resolvedWorkerCount] are the
  // ordinary parallelFor() worker pool (unchanged); ioThreadNum is the
  // dedicated IO thread (unchanged in spirit, renumbered by one slot);
  // workerLaneThreadNum is a NEW, second dedicated thread -- see
  // Scheduler::Scheduler()'s own comment on why runOnWorkerThread() now
  // targets one fixed dedicated thread rather than round-robining across
  // the ordinary worker pool. Neither is counted in workerCount() (which
  // has always excluded the IO thread; the new lane thread is excluded
  // the identical way).
  uint32_t ioThreadNum = 0;
  uint32_t workerLaneThreadNum = 0;

  ClosureQueue ioQueue;
  ClosureQueue workerQueue;
  std::unique_ptr<ClosureQueueLoopTask> ioLoopTask;
  std::unique_ptr<ClosureQueueLoopTask> workerLoopTask;

  // postToMain()'s queue: a plain mutex-guarded vector, deliberately NOT
  // built on any enkiTS mechanism [brief resolution 2] -- keeps this seam
  // thin, testable on its own, and independent of whether "main" is even
  // an enkiTS-registered thread at the moment pumpMain() is called.
  std::mutex mainQueueMutex;
  std::vector<std::function<void()>> mainQueue;

  // Flipped false as the very first step of ~Scheduler(), before either
  // ClosureQueue's requestShutdown() runs -- runOnIoThread()/
  // runOnWorkerThread() check this and refuse (counts, never submits) a
  // new closure once it is false. Fix-round 1 (Task 2), task-2-review.md
  // Minor finding; unchanged in spirit by this fix round's own redesign.
  std::atomic<bool> acceptingIoTasks{true};
  std::atomic<size_t> droppedAtIntakeCount{0};
};

Scheduler::Scheduler(uint32_t workerCount) : impl_(std::make_unique<Impl>()) {
  impl_->resolvedWorkerCount = resolveWorkerCount(workerCount);

  enki::TaskSchedulerConfig config;
  // +2: two extra internal threads beyond the resolved worker count --
  // [Fix round 1] one more than before this fix round's own redesign. The
  // dedicated IO thread is unchanged in spirit (D2: "IPinnedTask maps to
  // the dedicated IO thread"). The SECOND extra thread is new: a
  // dedicated "worker task lane" for runOnWorkerThread(), reusing the
  // IDENTICAL persistent-task-plus-closure-queue mechanism the IO thread
  // already uses, rather than round-robining runOnWorkerThread()
  // submissions across the ordinary [1, resolvedWorkerCount] worker pool
  // (this file's own pre-fix-round design). That round-robin design is
  // what originally motivated the one-shot-task-per-submission shape this
  // fix round replaces entirely -- a dedicated lane sidesteps needing any
  // per-submission enkiTS task lifecycle at all, at the cost of one more
  // permanently-dedicated thread (the SAME cost this Scheduler already
  // accepts for the IO thread) instead of transiently borrowing from the
  // shared worker pool. A closure's OWN body remains free to fan out
  // across the full ordinary worker pool via a nested parallelFor() call
  // (legal: the lane thread is now a real, Scheduler-registered thread,
  // matching parallelFor()'s own "calling thread must be main or a task
  // already running on one of this Scheduler's own threads" contract) --
  // only the ONE outer dispatch point is serialized onto the lane thread,
  // not the CPU-heavy work a closure goes on to do inside it.
  // numExternalTaskThreads stays 0 -- this Scheduler never asks a
  // caller-owned std::thread to register itself; every thread enkiTS
  // knows about here is one enkiTS itself created and owns the
  // join-on-shutdown lifetime of.
  config.numTaskThreadsToCreate = impl_->resolvedWorkerCount + 2;
  impl_->taskScheduler.Initialize(config);

  // GetNumTaskThreads() == numTaskThreadsToCreate + numExternalTaskThreads
  // + 1 (main) == (resolvedWorkerCount + 2) + 0 + 1. The two dedicated
  // threads are whichever internal thread numbers enkiTS created last.
  impl_->ioThreadNum = impl_->taskScheduler.GetNumTaskThreads() - 1;
  impl_->workerLaneThreadNum = impl_->taskScheduler.GetNumTaskThreads() - 2;

  impl_->ioLoopTask = std::make_unique<ClosureQueueLoopTask>(impl_->ioThreadNum, impl_->ioQueue);
  impl_->workerLoopTask = std::make_unique<ClosureQueueLoopTask>(impl_->workerLaneThreadNum, impl_->workerQueue);
  // The ONLY two AddPinnedTask() calls anywhere in this file now -- both
  // at construction, both for a task that lives (and is registered) for
  // this Scheduler's ENTIRE lifetime. No per-submission registration
  // exists anymore.
  impl_->taskScheduler.AddPinnedTask(impl_->ioLoopTask.get());
  impl_->taskScheduler.AddPinnedTask(impl_->workerLoopTask.get());

  RX_LOG_INFO(
      "rx::task::Scheduler: {} worker thread(s) + 1 IO thread (thread {}) + 1 worker-task lane (thread {}) + main",
      impl_->resolvedWorkerCount, impl_->ioThreadNum, impl_->workerLaneThreadNum);
}

std::unique_ptr<Scheduler> Scheduler::create(uint32_t workerCount) {
  return std::unique_ptr<Scheduler>(new Scheduler(workerCount));
}

Scheduler::~Scheduler() {
  // Step 1: stop intake -- unchanged in spirit from the pre-fix-round
  // design (task-2-review.md Minor finding). runOnIoThread()/
  // runOnWorkerThread() check this before doing anything else; once
  // false, a new submission is refused, counted, and logged rather than
  // pushed onto a queue that is about to stop being drained.
  impl_->acceptingIoTasks.store(false, std::memory_order_release);

  // Step 2 [Fix round 1]: wake both persistent loop tasks so they observe
  // shutdown and return from Execute() -- MUST happen before Step 3
  // (WaitforAllAndShutdown()) below, which blocks until every pinned task
  // this Scheduler ever registered (both loop tasks, exactly two,
  // registered once each at construction) has returned from Execute().
  // Without this, WaitforAllAndShutdown() would block forever: nothing
  // else ever tells either loop task to stop looping. Each queue's own
  // requestShutdown() drains-and-runs any final already-queued burst
  // before its loop task actually returns (ClosureQueueLoopTask::
  // Execute()'s own comment) -- so every closure already handed to this
  // Scheduler by the time this step runs still executes, matching the
  // pre-existing documented guarantee.
  impl_->ioQueue.requestShutdown();
  impl_->workerQueue.requestShutdown();

  // Step 3: WaitforAllAndShutdown() requests enkiTS-level shutdown, then
  // -- verified directly against the real, installed enkiTS v1.12 library
  // in Stage 0's own audit (task-2-report.md) -- blocks until every
  // pinned task already handed to enkiTS (here: exactly the two
  // persistent loop tasks) has actually returned from Execute(), before
  // it joins any thread.
  impl_->taskScheduler.WaitforAllAndShutdown();

  // Step 4 [Fix round 1]: both loop tasks have now, by construction (Step
  // 3's own guarantee), fully returned from Execute() -- meaning every
  // closure either queue held has already run (ClosureQueueLoopTask's own
  // drain-to-empty-before-returning contract). The only closures that can
  // possibly remain in either queue now are ones whose push() call raced
  // PAST Step 1's acceptingIoTasks check but had not yet reached push()
  // itself when Step 2 ran -- the same vanishingly narrow theoretical
  // caller-races-destructor window this project's own pre-existing F1 fix
  // already disclosed (a thread can always be preempted for an arbitrary
  // stretch between two lines of code); this task's own adversarial churn
  // harness (PinnedTaskChurnTest) exercises exactly this window under load
  // and observes it, in practice, empty every time. Counted, never
  // silently lost -- both queues' destructors (via ~Impl(), immediately
  // after this constructor body returns) destroy whatever std::function
  // objects remain WITHOUT invoking them, which is correct: nothing will
  // ever run them, and each one's own captured resources (RAII) unwind
  // normally either way.
  const size_t leakedIo = impl_->ioQueue.sizeForTesting();
  const size_t leakedWorker = impl_->workerQueue.sizeForTesting();
  const size_t totalDropped = impl_->droppedAtIntakeCount.load(std::memory_order_acquire) + leakedIo + leakedWorker;
  g_lastDroppedIoTaskCount.store(totalDropped, std::memory_order_release);
  if (totalDropped > 0) {
    RX_LOG_WARN(
        "rx::task::Scheduler: {} task(s) dropped at teardown ({} refused at intake, {} still queued on the IO "
        "queue, {} still queued on the worker-task queue) -- never run, discarded without executing (the "
        "caller-races-destructor window ~Scheduler()'s own comment discloses)",
        totalDropped, impl_->droppedAtIntakeCount.load(std::memory_order_acquire), leakedIo, leakedWorker);
  }
}

uint32_t Scheduler::autoGrainSize(uint32_t itemCount, uint32_t workerCount) {
  // Fix-round 2 [spec D4 amendment: "no caller-chosen chunk count...
  // self-scaling, never toggled"]. workerCount == 0 guarded to behave
  // like 1 -- see this function's own header doc comment; no real
  // Scheduler::workerCount() is ever 0 (create()'s resolveWorkerCount()
  // clamps to a minimum of 1), but this is a pure static function anyone
  // can call directly with arbitrary input.
  const uint32_t denom = (workerCount > 0 ? workerCount : 1) * 4;
  const uint32_t computed = itemCount / denom;
  return computed > kMinGrain ? computed : kMinGrain;
}

void Scheduler::parallelFor(uint32_t itemCount, uint32_t grainSize,
                             std::function<void(uint32_t begin, uint32_t end, uint32_t workerIndex)> fn) {
  if (itemCount == 0) {
    return;
  }
  RX_ZONE;
  RX_PLOT("parallelFor items", static_cast<int64_t>(itemCount));

  // grainSize == 0 means AUTO [spec D4 amendment] -- see this method's
  // header doc comment and autoGrainSize()'s own comment for the formula.
  // A nonzero grainSize is used verbatim (the measurement-affordance
  // path), exactly as before this fix round.
  const uint32_t effectiveGrain = grainSize > 0 ? grainSize : autoGrainSize(itemCount, impl_->resolvedWorkerCount);

  enki::TaskSet taskSet(itemCount, [&fn](enki::TaskSetPartition range, uint32_t threadNum) {
    fn(range.start, range.end, threadNum);
  });
  // Must be set before AddTaskSetToPipe(): enkiTS recomputes the task's
  // actual runtime split size (m_RangeToRun) from m_MinRange at that call
  // (TaskScheduler::AddTaskSetToPipeInt), not at TaskSet construction, but
  // only ever reads m_MinRange as of that moment.
  taskSet.m_MinRange = effectiveGrain;

  impl_->taskScheduler.AddTaskSetToPipe(&taskSet);
  // The calling thread participates here: WaitforTask() is enkiTS's own
  // documented "run pending chunks on this thread while waiting" call --
  // not a passive block. This is also what makes nested parallelFor()
  // (calling this method again from within `fn`) safe rather than a
  // deadlock: the inner call's own WaitforTask() drains whatever chunks
  // (inner or outer) are available, on whichever thread reaches it.
  impl_->taskScheduler.WaitforTask(&taskSet);
}

void Scheduler::parallelFor(uint32_t itemCount,
                             std::function<void(uint32_t begin, uint32_t end, uint32_t workerIndex)> fn) {
  parallelFor(itemCount, 0, std::move(fn));
}

void Scheduler::runOnIoThread(std::function<void()> fn) {
  // Fix-round 1 (task-2-review.md Minor finding): refuse rather than
  // submit once teardown has begun -- see ~Scheduler()'s own comment for
  // the full sequencing this exists to support.
  if (!impl_->acceptingIoTasks.load(std::memory_order_acquire)) {
    impl_->droppedAtIntakeCount.fetch_add(1, std::memory_order_acq_rel);
    RX_LOG_WARN("rx::task::Scheduler::runOnIoThread() called after this Scheduler began shutting down -- dropping");
    return;
  }
  // [Fix round 1] No AddPinnedTask() call here anymore -- see this file's
  // own top-of-file ClosureQueue/ClosureQueueLoopTask comment for the full
  // redesign. FIFO ordering is `std::deque::push_back()` + drain-in-order,
  // the same guarantee a genuine enkiTS pinned-task FIFO gave, just
  // implemented directly rather than borrowed.
  impl_->ioQueue.push(std::move(fn));
}

void Scheduler::runOnWorkerThread(std::function<void()> fn) {
  // Same shutdown-safety posture as runOnIoThread() above.
  if (!impl_->acceptingIoTasks.load(std::memory_order_acquire)) {
    impl_->droppedAtIntakeCount.fetch_add(1, std::memory_order_acq_rel);
    RX_LOG_WARN(
        "rx::task::Scheduler::runOnWorkerThread() called after this Scheduler began shutting down -- dropping");
    return;
  }
  // [Fix round 1] Targets the ONE dedicated worker-task-lane thread (see
  // Scheduler::Scheduler()'s own comment) -- no round-robin cursor exists
  // anymore (the pre-fix-round design's `nextWorkerTaskThread` field is
  // gone entirely, not merely made atomic: there is only one lane to pick
  // now). A closure's own body remains free to fan out across the full
  // ordinary [1, resolvedWorkerCount] worker pool via a nested
  // parallelFor() call -- see scheduler.h's own updated doc comment.
  impl_->workerQueue.push(std::move(fn));
}

void Scheduler::postToMain(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(impl_->mainQueueMutex);
  impl_->mainQueue.push_back(std::move(fn));
}

void Scheduler::pumpMain() {
  // Audit finding F5-partial: pumpMain() runs whatever GPU-object-mutating
  // closures postToMain() queued (D5's own handoff pattern), so it is
  // main-thread-only exactly like every other guarded mutator in
  // docs/threading.md's list -- it simply had no runtime guard yet.
  RX_ASSERT_MAIN_THREAD("Scheduler::pumpMain");

  // Swap out under the lock, then run outside it: postToMain() calls
  // arriving while these run (e.g. from a worker still executing while
  // pumpMain() drains) are simply picked up by the NEXT pumpMain() call,
  // never blocked on or lost.
  std::vector<std::function<void()>> toRun;
  {
    std::lock_guard<std::mutex> lock(impl_->mainQueueMutex);
    toRun.swap(impl_->mainQueue);
  }
  for (auto& fn : toRun) {
    fn();
  }
}

uint32_t Scheduler::workerCount() const { return impl_->resolvedWorkerCount; }

namespace detail {

size_t debugLastDroppedIoTaskCount() { return g_lastDroppedIoTaskCount.load(std::memory_order_acquire); }

}  // namespace detail

}  // namespace rx::task
