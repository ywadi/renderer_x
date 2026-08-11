#include <rx_task/scheduler.h>

#include <doctest/doctest.h>
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Bounded busy-wait used only for cross-thread completion signals this
// test file itself creates (never used to paper over a real Scheduler
// API, which is synchronous wherever the brief requires it -- parallelFor()
// blocks until done by contract). Fails the calling CHECK/REQUIRE instead
// of hanging forever if `predicate` never becomes true, so a genuine
// regression (e.g. a deadlocked IO thread) shows up as a normal test
// failure within a few seconds, not a stuck CI job.
template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

}  // namespace

TEST_CASE("Scheduler::workerCount resolves 0 to hardware_concurrency()-1 clamped to a minimum of 1, "
          "and returns an explicit request unchanged") {
  auto zero = rx::task::Scheduler::create(0);
  REQUIRE(zero != nullptr);
  const uint32_t hw = std::thread::hardware_concurrency();
  const uint32_t expected = hw > 1 ? hw - 1 : 1;
  CHECK(zero->workerCount() == expected);
  CHECK(zero->workerCount() >= 1);

  auto explicitCount = rx::task::Scheduler::create(3);
  REQUIRE(explicitCount != nullptr);
  CHECK(explicitCount->workerCount() == 3);
}

TEST_CASE("Scheduler::parallelFor touches every one of 10k items exactly once") {
  auto scheduler = rx::task::Scheduler::create(0);
  REQUIRE(scheduler != nullptr);

  constexpr uint32_t kItemCount = 10000;
  constexpr uint32_t kGrainSize = 64;

  std::vector<std::atomic<uint32_t>> touchCounts(kItemCount);
  for (auto& count : touchCounts) {
    count.store(0);
  }

  scheduler->parallelFor(kItemCount, kGrainSize, [&](uint32_t begin, uint32_t end, uint32_t workerIndex) {
    REQUIRE(begin < end);
    REQUIRE(end <= kItemCount);
    (void)workerIndex;
    for (uint32_t i = begin; i < end; ++i) {
      touchCounts[i].fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (uint32_t i = 0; i < kItemCount; ++i) {
    CHECK(touchCounts[i].load() == 1);
  }
}

TEST_CASE("Scheduler::parallelFor's auto-grain overload (grainSize omitted) touches every item exactly "
          "once, for itemCount 0/1/63/64/65/10000 -- no caller-chosen chunk count needed "
          "[spec D4 amendment: \"Parallelism is the engine default, not a mode\"]") {
  auto scheduler = rx::task::Scheduler::create(2);
  REQUIRE(scheduler != nullptr);

  for (uint32_t itemCount : {0u, 1u, 63u, 64u, 65u, 10000u}) {
    std::vector<std::atomic<uint32_t>> touchCounts(itemCount);
    for (auto& count : touchCounts) {
      count.store(0);
    }

    scheduler->parallelFor(itemCount, [&](uint32_t begin, uint32_t end, uint32_t workerIndex) {
      REQUIRE(begin < end);
      REQUIRE(end <= itemCount);
      (void)workerIndex;
      for (uint32_t i = begin; i < end; ++i) {
        touchCounts[i].fetch_add(1, std::memory_order_relaxed);
      }
    });

    for (uint32_t i = 0; i < itemCount; ++i) {
      CHECK(touchCounts[i].load() == 1);
    }
  }
}

TEST_CASE("Scheduler::parallelFor's three-argument form with an explicit grainSize of 0 behaves "
          "identically to the auto-grain overload -- 0 literally means AUTO, not \"grain of 1\"") {
  auto scheduler = rx::task::Scheduler::create(2);
  REQUIRE(scheduler != nullptr);

  constexpr uint32_t kItemCount = 10000;
  std::vector<std::atomic<uint32_t>> touchCounts(kItemCount);
  for (auto& count : touchCounts) {
    count.store(0);
  }

  scheduler->parallelFor(kItemCount, 0, [&](uint32_t begin, uint32_t end, uint32_t) {
    for (uint32_t i = begin; i < end; ++i) {
      touchCounts[i].fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (uint32_t i = 0; i < kItemCount; ++i) {
    CHECK(touchCounts[i].load() == 1);
  }
}

TEST_CASE("Scheduler::autoGrainSize computes max(kMinGrain, itemCount / (workerCount * 4)) directly, "
          "without needing a live Scheduler instance [spec D4 amendment]") {
  CHECK(rx::task::Scheduler::kMinGrain == 64);

  // Below (or exactly at) the floor -- kMinGrain wins.
  CHECK(rx::task::Scheduler::autoGrainSize(0, 4) == 64);
  CHECK(rx::task::Scheduler::autoGrainSize(100, 4) == 64);      // 100 / 16 = 6
  CHECK(rx::task::Scheduler::autoGrainSize(10000, 39) == 64);   // 10000 / 156 truncated = 64

  // Above the floor -- the formula's own value wins.
  CHECK(rx::task::Scheduler::autoGrainSize(10000, 4) == 625);    // 10000 / 16 = 625
  CHECK(rx::task::Scheduler::autoGrainSize(1000, 1) == 250);     // 1000 / 4 = 250
  CHECK(rx::task::Scheduler::autoGrainSize(100000, 8) == 3125);  // 100000 / 32 = 3125

  // Defensive guard: workerCount == 0 (never produced by a real
  // Scheduler -- workerCount() is always >= 1 by construction -- but this
  // is a pure static function callable with arbitrary input) is treated
  // the same as workerCount == 1, not a division by zero.
  CHECK(rx::task::Scheduler::autoGrainSize(1000, 0) == rx::task::Scheduler::autoGrainSize(1000, 1));
}

TEST_CASE("Scheduler::parallelFor genuinely runs multiple workers CONCURRENTLY -- a deterministic "
          "barrier proof, not a scheduling-heuristic guess (task-2-review.md Important finding: the old "
          "distinctWorkersUsed > 1 heuristic reproduced 2 failures in 27 runs under `taskset -c 0,1`)") {
  // Explicit, independent of this machine's actual core count -- gated
  // below on >= 2 per the fix instructions, though this literal value
  // already satisfies that unconditionally.
  constexpr uint32_t kExplicitWorkerCount = 2;
  auto scheduler = rx::task::Scheduler::create(kExplicitWorkerCount);
  REQUIRE(scheduler != nullptr);
  REQUIRE(scheduler->workerCount() == kExplicitWorkerCount);

  // Gate (defensive, not expected to trigger given the literal value
  // above): this proof only makes sense with >= 2 concurrent participants
  // to rendezvous.
  if (scheduler->workerCount() < 2) {
    return;
  }
  const uint32_t n = scheduler->workerCount();

  // Barrier-style proof: each of the n dispatched units increments
  // `arrived` once, then busy-waits (yielding, never a hard spin) until it
  // observes `arrived == n`, bounded by a generous per-unit timeout. This
  // can ONLY complete if all n units are alive and able to make progress
  // AT THE SAME TIME: if parallelFor() ever delivered the whole [0, n)
  // range to a single thread as one sequential chunk (no real
  // concurrency), the FIRST item's wait would spin until its own timeout
  // -- no other item could ever run to bump `arrived` further, since that
  // very thread is stuck inside this call and cannot service another
  // chunk concurrently with itself. A timed-out unit fails cleanly
  // (recorded, checked after parallelFor() returns) instead of hanging the
  // test forever.
  std::atomic<uint32_t> arrived{0};
  std::atomic<uint32_t> timeouts{0};
  std::mutex idsMutex;
  std::vector<std::thread::id> ids;

  scheduler->parallelFor(n, 1, [&](uint32_t begin, uint32_t end, uint32_t) {
    REQUIRE(end - begin == 1);
    {
      std::lock_guard<std::mutex> lock(idsMutex);
      ids.push_back(std::this_thread::get_id());
    }
    arrived.fetch_add(1, std::memory_order_acq_rel);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (arrived.load(std::memory_order_acquire) < n) {
      if (std::chrono::steady_clock::now() >= deadline) {
        timeouts.fetch_add(1, std::memory_order_acq_rel);
        return;
      }
      std::this_thread::yield();
    }
  });

  REQUIRE(timeouts.load() == 0);
  CHECK(arrived.load() == n);

  // Info-only, not asserted -- exactly the heuristic this test replaces,
  // kept purely so a human reading test output can still see it.
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  RX_LOG_INFO("deterministic barrier proof: {} distinct thread id(s) observed for {} concurrent unit(s)", ids.size(),
              n);
}

TEST_CASE("Scheduler::parallelFor nests safely: an outer chunk's callback can call parallelFor() "
          "again without deadlock") {
  auto scheduler = rx::task::Scheduler::create(0);
  REQUIRE(scheduler != nullptr);

  constexpr uint32_t kOuter = 64;
  constexpr uint32_t kInner = 256;
  std::atomic<uint32_t> completedOuterItems{0};

  scheduler->parallelFor(kOuter, 4, [&](uint32_t begin, uint32_t end, uint32_t) {
    for (uint32_t i = begin; i < end; ++i) {
      std::atomic<uint32_t> innerSum{0};
      scheduler->parallelFor(kInner, 16, [&](uint32_t ibegin, uint32_t iend, uint32_t) {
        innerSum.fetch_add(iend - ibegin, std::memory_order_relaxed);
      });
      CHECK(innerSum.load() == kInner);
      completedOuterItems.fetch_add(1, std::memory_order_relaxed);
    }
  });

  CHECK(completedOuterItems.load() == kOuter);
}

TEST_CASE("Scheduler::postToMain runs queued functions on the pumpMain()-calling thread, "
          "in the order a single thread queued them") {
  auto scheduler = rx::task::Scheduler::create(2);
  REQUIRE(scheduler != nullptr);

  const std::thread::id mainId = std::this_thread::get_id();
  constexpr int kCount = 200;
  std::vector<int> order;
  std::vector<std::thread::id> seenIds;

  for (int i = 0; i < kCount; ++i) {
    scheduler->postToMain([&order, &seenIds, i] {
      order.push_back(i);
      seenIds.push_back(std::this_thread::get_id());
    });
  }

  // Nothing runs before pumpMain() is called.
  CHECK(order.empty());

  scheduler->pumpMain();

  REQUIRE(order.size() == static_cast<size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    CHECK(order[static_cast<size_t>(i)] == i);
  }
  for (const auto& id : seenIds) {
    CHECK(id == mainId);
  }
}

TEST_CASE("Scheduler::postToMain is safe to call from worker threads, and the queued work still "
          "runs only on whichever thread calls pumpMain()") {
  auto scheduler = rx::task::Scheduler::create(0);
  REQUIRE(scheduler != nullptr);

  const std::thread::id mainId = std::this_thread::get_id();
  std::mutex resultsMutex;
  std::vector<std::thread::id> executedOnIds;
  std::vector<std::thread::id> queuedFromIds;

  constexpr uint32_t kItemCount = 500;
  scheduler->parallelFor(kItemCount, 8, [&](uint32_t begin, uint32_t end, uint32_t) {
    std::thread::id queuedFrom = std::this_thread::get_id();
    scheduler->postToMain([&resultsMutex, &executedOnIds, &queuedFromIds, queuedFrom] {
      std::lock_guard<std::mutex> lock(resultsMutex);
      executedOnIds.push_back(std::this_thread::get_id());
      queuedFromIds.push_back(queuedFrom);
    });
    (void)begin;
    (void)end;
  });

  scheduler->pumpMain();

  // One postToMain() call per parallelFor() chunk -- the exact chunk
  // count is enkiTS's own splitting heuristic to decide, not something
  // this test pins down; only the bound (at least one chunk, never more
  // chunks than items) and the actual thread-affinity guarantee matter
  // here.
  REQUIRE(!executedOnIds.empty());
  REQUIRE(executedOnIds.size() <= kItemCount);
  REQUIRE(queuedFromIds.size() == executedOnIds.size());
  for (const auto& id : executedOnIds) {
    CHECK(id == mainId);
  }
}

TEST_CASE("Scheduler::runOnIoThread executes off the main thread, in FIFO order, "
          "and survives 1000 rapid submissions") {
  auto scheduler = rx::task::Scheduler::create(2);
  REQUIRE(scheduler != nullptr);

  const std::thread::id mainId = std::this_thread::get_id();
  constexpr int kCount = 1000;

  std::mutex resultsMutex;
  std::vector<int> order;
  std::vector<std::thread::id> seenIds;
  std::atomic<int> remaining{kCount};

  for (int i = 0; i < kCount; ++i) {
    scheduler->runOnIoThread([&resultsMutex, &order, &seenIds, &remaining, i] {
      {
        std::lock_guard<std::mutex> lock(resultsMutex);
        order.push_back(i);
        seenIds.push_back(std::this_thread::get_id());
      }
      remaining.fetch_sub(1, std::memory_order_release);
    });
  }

  const bool completed = waitUntil([&] { return remaining.load(std::memory_order_acquire) == 0; });
  REQUIRE(completed);

  REQUIRE(order.size() == static_cast<size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    CHECK(order[static_cast<size_t>(i)] == i);
  }

  REQUIRE(!seenIds.empty());
  for (const auto& id : seenIds) {
    CHECK(id != mainId);
  }
  // All 1000 ran on the SAME thread -- one dedicated IO thread, never a
  // pool of them.
  for (const auto& id : seenIds) {
    CHECK(id == seenIds.front());
  }
}

TEST_CASE("Scheduler teardown lets several already-queued runOnIoThread() tasks all run to completion "
          "-- none dropped -- even when destruction begins while the first is still in flight "
          "(task-2-review.md Minor finding's suggested repro; empirically this is the NORMAL case, not "
          "the drop path -- see scheduler.cpp's ~Scheduler() comment for the standalone-probe evidence)") {
  std::atomic<bool> firstTaskCompleted{false};
  std::atomic<int> laterTasksRan{0};
  constexpr int kLaterTaskCount = 4;

  {
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);

    scheduler->runOnIoThread([&firstTaskCompleted] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      firstTaskCompleted.store(true, std::memory_order_release);
    });
    // Queued immediately behind the first, still-sleeping task.
    for (int i = 0; i < kLaterTaskCount; ++i) {
      scheduler->runOnIoThread([&laterTasksRan] { laterTasksRan.fetch_add(1, std::memory_order_relaxed); });
    }

    // scheduler destructs here, at end of scope, WHILE the first task is
    // still sleeping -- ~Scheduler() blocks until it (and everything
    // queued behind it) actually finishes; see that method's own comment.
  }

  CHECK(firstTaskCompleted.load());
  CHECK(laterTasksRan.load() == kLaterTaskCount);
  CHECK(rx::task::detail::debugLastDroppedIoTaskCount() == 0);
}

TEST_CASE("Scheduler::runOnIoThread refuses (and counts) a call made from within an already-running IO "
          "task's own body once ~Scheduler() has begun tearing down, with no crash "
          "(task-2-review.md Minor finding fix)") {
  // NOTE on why this test is built this way rather than racing a second
  // thread's runOnIoThread() calls directly against scheduler.reset():
  // doing that safely is impossible in general -- once .reset() returns,
  // the Scheduler is genuinely freed, and a second thread with no other
  // synchronization cannot be guaranteed to stop calling into it before
  // that instant without either joining it first (which would leave
  // acceptingIoTasks untested, since it would still be true the whole
  // time) or racing a use-after-free on the Scheduler object itself (a
  // caller lifetime bug no internal flag can paper over). Instead, this
  // nests the second call INSIDE the first task's own Execute() body --
  // which is safe, because that body executes as part of
  // ~Scheduler()'s own call to WaitforAllAndShutdown() (Step 2), which by
  // definition has not yet returned while it is running, so impl_ is
  // still guaranteed alive. Step 1 (acceptingIoTasks := false) always
  // happens-before Step 2, so by the time this nested call runs, teardown
  // has provably already begun.
  std::atomic<bool> nestedCallAccepted{false};

  {
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    rx::task::Scheduler* raw = scheduler.get();

    scheduler->runOnIoThread([raw, &nestedCallAccepted] {
      // A brief sleep is not required for safety (this closure's dynamic
      // extent is entirely inside ~Scheduler()'s in-progress
      // WaitforAllAndShutdown() call regardless of timing), only to make
      // the ordering below deterministic rather than a race with how
      // quickly the main thread reaches ~Scheduler()'s first line.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      // By this point, Step 1 (acceptingIoTasks := false) of ~Scheduler()'s
      // teardown has completed, which is why this nested runOnIoThread()
      // call below gets dropped -- self-documents this nested call's
      // dependence on ~Scheduler step ordering per task-2-review.md.

      raw->runOnIoThread([&nestedCallAccepted] { nestedCallAccepted.store(true, std::memory_order_release); });
    });

    // scheduler destructs here, at end of scope: acceptingIoTasks flips
    // false (Step 1) essentially immediately, then WaitforAllAndShutdown()
    // (Step 2) blocks for the ~50ms above while the task runs and makes
    // its nested call.
  }

  CHECK_FALSE(nestedCallAccepted.load());
  CHECK(rx::task::detail::debugLastDroppedIoTaskCount() >= 1);
}

TEST_CASE("Scheduler::create/destroy repeats cleanly 3 times in sequence, exercising every API "
          "each cycle (no leak or deadlock on teardown)") {
  for (int cycle = 0; cycle < 3; ++cycle) {
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    CHECK(scheduler->workerCount() == 2);

    std::atomic<uint32_t> parallelSum{0};
    scheduler->parallelFor(100, 10, [&](uint32_t begin, uint32_t end, uint32_t) {
      parallelSum.fetch_add(end - begin, std::memory_order_relaxed);
    });
    CHECK(parallelSum.load() == 100);

    std::atomic<bool> ioRan{false};
    scheduler->runOnIoThread([&ioRan] { ioRan.store(true, std::memory_order_release); });
    REQUIRE(waitUntil([&] { return ioRan.load(std::memory_order_acquire); }));

    bool mainRan = false;
    scheduler->postToMain([&mainRan] { mainRan = true; });
    scheduler->pumpMain();
    CHECK(mainRan);

    // scheduler destructs here at end of scope, joining every internal
    // thread before the next cycle's create() runs.
  }
}

// --- Audit-closure round: F1 (HIGH, TSAN-reproduced UAF) -------------------
TEST_CASE("Scheduler::runOnIoThread survives many CONCURRENT submitters -- parallelFor chunks each firing "
          "runOnIoThread() once per item -- without crashing or losing a single submission (audit finding "
          "F1's repro shape: AddPinnedTaskInt's post-publish threadNum reads racing the IO thread's "
          "delete-after-run reap, TSAN-reproduced 3-4 reports/run pre-fix, 0/10 post-fix -- see scheduler.cpp's "
          "IoTask class comment for the two-phase-delete fix and task-2-report.md for the full tallies). "
          "Non-TSAN CI cannot observe the race itself, but this exercises the exact concurrent-publish "
          "pattern the fix closes -- the audit noted its prior absence from this suite.") {
  auto scheduler = rx::task::Scheduler::create(4);
  REQUIRE(scheduler != nullptr);

  // grain=1 as a FLOOR only -- enkiTS's actual chunk count is driven by its
  // own m_NumPartitions (thread-count-derived), not by grainSize alone, so
  // a large itemCount with grainSize=1 does NOT itself produce itemCount
  // separate chunks (found directly while building this test's TSAN
  // counterpart -- see task-2-report.md). Calling runOnIoThread() once per
  // ITEM inside each chunk's own [begin,end) loop, rather than once per
  // chunk, is what actually produces many concurrent submitters here,
  // regardless of how few chunks enkiTS decides to create.
  constexpr uint32_t kItems = 4000;
  std::atomic<uint32_t> ran{0};

  scheduler->parallelFor(kItems, 1, [&](uint32_t begin, uint32_t end, uint32_t) {
    for (uint32_t i = begin; i < end; ++i) {
      scheduler->runOnIoThread([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
  });

  REQUIRE(waitUntil([&] { return ran.load(std::memory_order_acquire) == kItems; }, std::chrono::seconds(30)));
  CHECK(ran.load() == kItems);
}

// --- Audit-closure round: F5-partial (RX_ASSERT_MAIN_THREAD on pumpMain) ---
#ifdef RX_DEBUG_CHECKS

namespace {

struct PumpMainGuardCapture {
  std::mutex mutex;
  std::vector<std::string> contexts;
};

std::atomic<PumpMainGuardCapture*> g_pumpMainCapture{nullptr};

void capturePumpMainViolation(const char* context) {
  PumpMainGuardCapture* capture = g_pumpMainCapture.load(std::memory_order_relaxed);
  if (capture == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(capture->mutex);
  capture->contexts.emplace_back(context != nullptr ? context : "");
}

// RAII guard mirroring debug_checks_test.cpp's own ViolationHookGuard:
// restores the production default (abort-on-violation) on scope exit
// regardless of how the enclosing TEST_CASE exits.
struct PumpMainGuardHookScope {
  ~PumpMainGuardHookScope() {
    g_pumpMainCapture.store(nullptr, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(nullptr);
  }
};

}  // namespace

// Deliberately placed LAST in this file: rx_core::debug::assertMainThreadImpl
// lazily captures "the main thread" on the FIRST call to ANY
// RX_ASSERT_MAIN_THREAD site in this whole process (debug_checks.h's own
// disclosed limitation) -- several earlier TEST_CASEs above already call
// scheduler->pumpMain() legitimately from doctest's own main/running thread
// (e.g. the postToMain tests), so by the time THIS test's worker thread
// calls it, "main" is already correctly learned. Reordering this file so a
// WORKER thread reached pumpMain() (and therefore this guard) before any
// legitimate main-thread call would invalidate that assumption -- exactly
// the hazard debug_checks_test.cpp's own header comment documents.
TEST_CASE("Scheduler::pumpMain's RX_ASSERT_MAIN_THREAD guard fires, naming pumpMain, for a call from a "
          "genuinely different thread (audit finding F5-partial)") {
  auto scheduler = rx::task::Scheduler::create(2);
  REQUIRE(scheduler != nullptr);

  PumpMainGuardCapture capture;
  g_pumpMainCapture.store(&capture, std::memory_order_relaxed);
  rx::core::debug::detail::setViolationHookForTests(&capturePumpMainViolation);
  PumpMainGuardHookScope hookScope;

  // A plain std::thread, not a parallelFor() worker -- matches
  // debug_checks_test.cpp's own reasoning: this hook never throws (safe
  // regardless), and a plain std::thread is a deterministically different
  // thread::id, with none of a real scheduler's work-stealing uncertainty
  // about which thread happens to run a given unit of work.
  std::thread worker([&scheduler] { scheduler->pumpMain(); });
  worker.join();

  std::lock_guard<std::mutex> lock(capture.mutex);
  REQUIRE(capture.contexts.size() == 1);
  CHECK(capture.contexts[0] == "Scheduler::pumpMain");
}

#endif  // RX_DEBUG_CHECKS
