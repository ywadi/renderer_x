#include <rx_task/scheduler.h>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
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

TEST_CASE("Scheduler::parallelFor touches every one of 10k items exactly once, "
          "and uses more than one worker thread when workerCount() > 0") {
  auto scheduler = rx::task::Scheduler::create(0);
  REQUIRE(scheduler != nullptr);

  constexpr uint32_t kItemCount = 10000;
  constexpr uint32_t kGrainSize = 64;

  std::vector<std::atomic<uint32_t>> touchCounts(kItemCount);
  for (auto& count : touchCounts) {
    count.store(0);
  }

  // Generously sized -- large enough to hold any workerIndex enkiTS could
  // report (main + every background worker); this test only cares which
  // slots ever got touched, not the exact thread accounting.
  constexpr uint32_t kMaxTrackedWorkers = 256;
  std::vector<std::atomic<uint32_t>> perWorkerTouches(kMaxTrackedWorkers);
  for (auto& count : perWorkerTouches) {
    count.store(0);
  }

  scheduler->parallelFor(kItemCount, kGrainSize, [&](uint32_t begin, uint32_t end, uint32_t workerIndex) {
    REQUIRE(begin < end);
    REQUIRE(end <= kItemCount);
    for (uint32_t i = begin; i < end; ++i) {
      touchCounts[i].fetch_add(1, std::memory_order_relaxed);
    }
    if (workerIndex < kMaxTrackedWorkers) {
      perWorkerTouches[workerIndex].fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (uint32_t i = 0; i < kItemCount; ++i) {
    CHECK(touchCounts[i].load() == 1);
  }

  uint32_t distinctWorkersUsed = 0;
  for (auto& count : perWorkerTouches) {
    if (count.load() > 0) {
      ++distinctWorkersUsed;
    }
  }
  CHECK(distinctWorkersUsed >= 1);
  if (scheduler->workerCount() > 0) {
    // Not a hard guarantee of enkiTS's work-stealing heuristics for any
    // given run, but with 10k items split into 64-item grains (>150
    // chunks) and at least one background worker available, the
    // scheduler not using it at all would indicate something structurally
    // wrong with how this wrapper configured/queued the work.
    CHECK(distinctWorkersUsed > 1);
  }
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
