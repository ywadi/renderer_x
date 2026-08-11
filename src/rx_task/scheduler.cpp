#include "rx_task/scheduler.h"

// enkiTS's own installed/BUILD_INTERFACE include layout puts TaskScheduler.h
// directly on the include path with no subdirectory prefix (verified
// directly against the installed tree: enkiTS::enkiTS's
// INTERFACE_INCLUDE_DIRECTORIES is "<prefix>/include/enkiTS", not
// "<prefix>/include") -- see third_party/CMakeLists.txt's own comment.
#include <TaskScheduler.h>

#include <rx_core/log.h>

#include <mutex>
#include <thread>
#include <vector>

namespace rx::task {

namespace {

// One-shot pinned task backing runOnIoThread(). Deliberately NOT self-
// deleted from inside Execute() -- verified directly against enkiTS
// v1.12's TaskScheduler::RunPinnedTasks(threadNum_, priority_) source
// (TaskScheduler.cpp):
//
//   pPinnedTaskSet->Execute();
//   pPinnedTaskSet->m_RunningCount.fetch_sub(1, ...);   // touches *this
//   TaskComplete(pPinnedTaskSet, true, threadNum_);      // touches *this
//
// enkiTS itself dereferences the task object in the two lines immediately
// AFTER Execute() returns (decrementing ICompletable::m_RunningCount, then
// walking m_pDependents inside TaskComplete()). A `delete this;` at the
// end of Execute() would make both of those a use-after-free. Instead,
// Execute() appends `this` to a trash list owned by the IO thread's own
// IoLoopTask (below), which is only ever touched by the IO thread itself
// and only reaped once its own call to RunPinnedTasks() has FULLY
// returned -- i.e. strictly after enkiTS's own post-Execute() bookkeeping
// for every task run in that call has already happened.
class IoTask final : public enki::IPinnedTask {
 public:
  IoTask(uint32_t threadNum, std::function<void()> fn, std::vector<IoTask*>& trash)
      : enki::IPinnedTask(threadNum), fn_(std::move(fn)), trash_(trash) {}

  void Execute() override {
    fn_();
    trash_.push_back(this);
  }

 private:
  std::function<void()> fn_;
  std::vector<IoTask*>& trash_;
};

// The dedicated IO thread's entire body, itself run as a single long-lived
// pinned task on the one EXTRA internal enkiTS thread Scheduler::Impl
// configures beyond `resolvedWorkerCount` (see Scheduler::Scheduler()
// below). This is enkiTS's own documented idiom for a thread that runs
// ONLY pinned tasks and never participates in parallelFor() work-stealing
// -- verbatim the pattern enkiTS's own example/WaitForNewPinnedTasks.cpp
// ships as `RunPinnedTaskLoopTask`: once the extra internal thread's
// TaskingThreadFunction dispatch loop picks this task up and calls
// Execute(), it never returns (so that thread can never go back to
// stealing ordinary parallelFor() chunks) until shutdown is requested --
// which is exactly what makes this thread "dedicated" rather than merely
// "usually free".
class IoLoopTask final : public enki::IPinnedTask {
 public:
  IoLoopTask(enki::TaskScheduler* scheduler, uint32_t threadNum)
      : enki::IPinnedTask(threadNum), scheduler_(scheduler) {}

  void Execute() override {
    while (!scheduler_->GetIsShutdownRequested()) {
      // Sleeps until a pinned task targets this thread number, or shutdown
      // is requested (TaskScheduler::StopThreads() unconditionally signals
      // every thread's own wait-for-new-pinned-task semaphore while it
      // joins threads, so this is guaranteed to wake, never hang).
      scheduler_->WaitForNewPinnedTasks();
      scheduler_->RunPinnedTasks();
      // Safe to reap now -- see IoTask's own comment above. trash_ is
      // touched only from this thread (IoTask::Execute() runs only on the
      // IO thread by construction; this loop runs only on the IO thread
      // too), so no lock is needed.
      for (IoTask* task : trash_) {
        delete task;
      }
      trash_.clear();
    }
  }

  std::vector<IoTask*>& trash() { return trash_; }

 private:
  enki::TaskScheduler* scheduler_;
  std::vector<IoTask*> trash_;
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
  uint32_t ioThreadNum = 0;
  std::unique_ptr<IoLoopTask> ioLoopTask;

  // postToMain()'s queue: a plain mutex-guarded vector, deliberately NOT
  // built on any enkiTS mechanism [brief resolution 2] -- keeps this seam
  // thin, testable on its own, and independent of whether "main" is even
  // an enkiTS-registered thread at the moment pumpMain() is called.
  std::mutex mainQueueMutex;
  std::vector<std::function<void()>> mainQueue;
};

Scheduler::Scheduler(uint32_t workerCount) : impl_(std::make_unique<Impl>()) {
  impl_->resolvedWorkerCount = resolveWorkerCount(workerCount);

  enki::TaskSchedulerConfig config;
  // +1: one extra internal thread beyond the resolved worker count, handed
  // the IO-only pinned-task loop immediately below [D2: "IPinnedTask maps
  // to the dedicated IO thread"]. numExternalTaskThreads stays 0 -- this
  // Scheduler never asks a caller-owned std::thread to register itself;
  // every thread enkiTS knows about here is one enkiTS itself created and
  // owns the join-on-shutdown lifetime of.
  config.numTaskThreadsToCreate = impl_->resolvedWorkerCount + 1;
  impl_->taskScheduler.Initialize(config);

  // GetNumTaskThreads() == numTaskThreadsToCreate + numExternalTaskThreads
  // + 1 (main) == (resolvedWorkerCount + 1) + 0 + 1. The dedicated IO
  // thread is whichever internal thread number enkiTS created last.
  impl_->ioThreadNum = impl_->taskScheduler.GetNumTaskThreads() - 1;
  impl_->ioLoopTask = std::make_unique<IoLoopTask>(&impl_->taskScheduler, impl_->ioThreadNum);
  impl_->taskScheduler.AddPinnedTask(impl_->ioLoopTask.get());

  RX_LOG_INFO("rx::task::Scheduler: {} worker thread(s) + 1 IO thread (thread {}) + main", impl_->resolvedWorkerCount,
              impl_->ioThreadNum);
}

std::unique_ptr<Scheduler> Scheduler::create(uint32_t workerCount) {
  return std::unique_ptr<Scheduler>(new Scheduler(workerCount));
}

Scheduler::~Scheduler() {
  // WaitforAllAndShutdown() requests shutdown, lets already-running work
  // settle, then joins every internal thread -- including the IO thread:
  // IoLoopTask's while loop above observes GetIsShutdownRequested() and
  // returns, which is what lets enkiTS join that thread like any other.
  //
  // Any postToMain()/runOnIoThread() work still queued (never drained by
  // a pumpMain() call, or added in the brief window right as shutdown
  // begins) is dropped here, not run and not specially reclaimed -- the
  // same "non-empty teardown is a caller-timing choice, not this type's
  // job to paper over" posture rx_rhi_vk's DeletionQueue destructor
  // documents for its own equivalent case. Unlike DeletionQueue, this is
  // not logged: an application shutting down its Scheduler with
  // in-flight fire-and-forget work queued is an ordinary, expected
  // teardown race, not a leak of a caller-owned resource.
  impl_->taskScheduler.WaitforAllAndShutdown();
}

void Scheduler::parallelFor(uint32_t itemCount, uint32_t grainSize,
                             std::function<void(uint32_t begin, uint32_t end, uint32_t workerIndex)> fn) {
  if (itemCount == 0) {
    return;
  }
  enki::TaskSet taskSet(itemCount, [&fn](enki::TaskSetPartition range, uint32_t threadNum) {
    fn(range.start, range.end, threadNum);
  });
  // Must be set before AddTaskSetToPipe(): enkiTS recomputes the task's
  // actual runtime split size (m_RangeToRun) from m_MinRange at that call
  // (TaskScheduler::AddTaskSetToPipeInt), not at TaskSet construction, but
  // only ever reads m_MinRange as of that moment.
  taskSet.m_MinRange = grainSize > 0 ? grainSize : 1;

  impl_->taskScheduler.AddTaskSetToPipe(&taskSet);
  // The calling thread participates here: WaitforTask() is enkiTS's own
  // documented "run pending chunks on this thread while waiting" call --
  // not a passive block. This is also what makes nested parallelFor()
  // (calling this method again from within `fn`) safe rather than a
  // deadlock: the inner call's own WaitforTask() drains whatever chunks
  // (inner or outer) are available, on whichever thread reaches it.
  impl_->taskScheduler.WaitforTask(&taskSet);
}

void Scheduler::runOnIoThread(std::function<void()> fn) {
  // Freed by IoLoopTask, once its Execute() loop has safely reaped it --
  // see IoTask's own comment above for exactly why not sooner.
  auto* task = new IoTask(impl_->ioThreadNum, std::move(fn), impl_->ioLoopTask->trash());
  // Safe from any thread per enkiTS's own contract ("Pinned tasks can be
  // added from any thread" -- TaskScheduler.h). AddPinnedTaskInt() pushes
  // onto enkiTS's own lock-free intrusive list (WriterWriteFront(),
  // dequeued tail-first via ReaderReadBack() -- verified directly against
  // LockLessMultiReadPipe.h: this is a genuine FIFO, not a stack), and
  // wakes the IO thread immediately if it was parked in
  // WaitForNewPinnedTasks() -- this is what gives runOnIoThread() its FIFO
  // guarantee without this wrapper needing any queue of its own.
  impl_->taskScheduler.AddPinnedTask(task);
}

void Scheduler::postToMain(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(impl_->mainQueueMutex);
  impl_->mainQueue.push_back(std::move(fn));
}

void Scheduler::pumpMain() {
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

}  // namespace rx::task
