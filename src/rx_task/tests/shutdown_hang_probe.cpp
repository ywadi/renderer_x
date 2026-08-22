// rx_task/tests/shutdown_hang_probe.cpp -- issue #76 teardown-hang fix,
// subprocess-based reproduction + proof harness. NOT a doctest binary and
// deliberately NOT registered as its own ctest test (see this directory's
// own CMakeLists.txt comment) -- this process is EXPECTED to std::abort()
// (SIGABRT) every time it runs correctly. scheduler_test.cpp's own
// "[Issue #76 teardown-hang fix, subprocess-based]" TEST_CASE spawns this
// as a child process and asserts on ITS exit behavior instead, which is
// the only sound way to prove an abort() path: exercising it directly
// inside a doctest TEST_CASE would take the whole binary (every OTHER
// test case in it) down with the same abort().
//
// Reproduces the reviewer's own hang-injection technique exactly (issue
// #76 review, task-i76-review.md, Finding 1 / adjudication (b)): a
// runOnWorkerThread() closure that never returns -- the same shape
// rx_asset's own async import compute phase (computeGltfImport(), called
// via Registry::importGltfAsync() -> runAsyncImportComputePhase()) would
// take if genuinely wedged. Reproduced directly here, minimally, with no
// dependency on rx_asset at all: the hang itself is purely a property of
// a runOnWorkerThread() closure that never returns, not of anything glTF-
// specific.
//
// Usage: shutdown_hang_probe <deadline_ms>
//   <deadline_ms> is passed straight through to Scheduler::create()'s own
//   shutdownJoinDeadline parameter. Production code leaves this at its
//   default (Scheduler::kDefaultShutdownJoinDeadline, 30s, deliberately
//   generous); this probe accepts a much shorter one from its caller so
//   the subprocess-based test that spawns it stays fast -- the exact same
//   code path is exercised either way, only the configured deadline VALUE
//   differs.
#include <rx_task/scheduler.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: shutdown_hang_probe <deadline_ms>\n");
        return 2;
    }
    const long deadlineMs = std::strtol(argv[1], nullptr, 10);
    if (deadlineMs <= 0) {
        std::fprintf(stderr, "shutdown_hang_probe: deadline_ms must be a positive integer\n");
        return 2;
    }

    auto scheduler = rx::task::Scheduler::create(/*workerCount=*/1, std::chrono::milliseconds(deadlineMs));
    if (!scheduler) {
        std::fprintf(stderr, "shutdown_hang_probe: Scheduler::create() failed\n");
        return 3;
    }

    // The reviewer's own hang-injection technique, reproduced exactly: a
    // runOnWorkerThread() closure that never returns.
    scheduler->runOnWorkerThread([] {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    // A brief, generous head start so the closure has genuinely entered
    // its own infinite loop (observed mid-closure by the destructor
    // below, not merely still queued) before teardown begins -- 200ms is
    // ample on any real hardware for a single lambda dispatch.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::fprintf(stderr,
                 "shutdown_hang_probe: destroying Scheduler now -- expect this process to abort() within ~%ldms\n",
                 deadlineMs);
    std::fflush(stderr);
    scheduler.reset();  // ~Scheduler() -- expected to std::abort() before returning.

    // Only reached if the bounded-shutdown fix somehow did NOT fire --
    // itself a genuine test failure, reported via a distinct, deliberately
    // unusual exit code (neither 0 nor a signal) so the parent test can
    // tell "returned normally instead of aborting" apart from every other
    // outcome.
    std::fprintf(stderr,
                 "shutdown_hang_probe: ~Scheduler() returned WITHOUT aborting -- the bounded-shutdown fix did not "
                 "fire\n");
    return 42;
}
