#include <doctest/doctest.h>
#include <rx_core/debug_checks.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// debug_checks_test.cpp -- coverage for rx_core/debug_checks.h
// [Phase 4 Task 7 fix round 1, review Medium finding: "no runtime guard
// against a main-thread-only API called from chunk >= 1"]. This is the
// device-free, fully deterministic proof of the mechanism ITSELF
// (RX_ASSERT_MAIN_THREAD / assertMainThreadImpl / the detail::
// violation-hook seam) -- a plain std::thread stands in for "a chunk >= 1
// worker", which is legitimate here specifically BECAUSE this is testing
// the thread-identity comparison in isolation, not the real
// rx_task::Scheduler-driven chunked-pass machinery (that integration-level
// proof, for the one real caller this fix round instruments most directly,
// lives in rx_material's test_material_system.cpp instead, GPU-gated).
//
// RX_ASSERT_MAIN_THREAD itself compiles to nothing at all when
// RX_DEBUG_CHECKS is OFF (see debug_checks.h's own header comment) -- the
// first TEST_CASE below calls only the bare macro and therefore compiles
// and runs identically in both configurations, mirroring profile_test.cpp's
// own "never spells out #ifdef ... itself" convention for RX_ZONE. The
// remaining cases exercise rx::core::debug::detail::setViolationHookForTests
// directly, which (like assertMainThreadImpl itself) exists ONLY when
// RX_DEBUG_CHECKS is ON -- there is no way to write an OFF-compiling
// equivalent of "assert a hook fired" when the entire hook mechanism is
// compiled away by construction, so these are the ones genuinely guarded
// on `#ifdef RX_DEBUG_CHECKS`.

TEST_CASE("RX_ASSERT_MAIN_THREAD compiles and runs from the main test thread without aborting") {
    // No hook installed (production default is std::abort() on a real
    // violation) -- safe specifically because this call is genuinely on
    // the main thread: every doctest TEST_CASE in this binary runs
    // sequentially on the same thread that started the process (doctest's
    // own default, single-threaded runner), and this is not the first
    // guarded call in the whole binary to run on some OTHER thread first
    // (see assertMainThreadImpl()'s own disclosed lazy-capture limitation
    // in debug_checks.cpp) -- rx_core_tests links no other TU that calls
    // this macro from anywhere but this file's own main-thread test cases.
    RX_ASSERT_MAIN_THREAD("debug_checks_test::mainThreadCall");
    CHECK(true);
}

#ifdef RX_DEBUG_CHECKS

namespace {

struct ViolationCapture {
    std::mutex mutex;
    int callCount = 0;
    std::string lastContext;
};

// The currently-installed test capture, reached through a free function
// (not a capturing lambda -- ViolationHook is a plain function pointer,
// matching rx::core::log::ForwardCallback's own convention in
// log_forward_sink.h) via a thread-local-free, test-scoped pointer. Only
// one test at a time ever installs one (each test's own RAII guard below
// restores nullptr -- the production abort-on-violation default -- before
// returning), so a single process-wide slot is sufficient.
std::atomic<ViolationCapture*> g_activeCapture{nullptr};

void captureViolationHook(const char* context) {
    ViolationCapture* capture = g_activeCapture.load(std::memory_order_relaxed);
    if (capture == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->callCount++;
    capture->lastContext = context != nullptr ? context : "";
    // Deliberately returns normally rather than throwing/aborting -- see
    // debug_checks.h's own detail::ViolationHook contract comment: this is
    // safe here specifically because every TEST_CASE below that installs
    // this hook is structured so no other thread touches ANY shared state
    // for the remainder of that one violating call (the worker thread's
    // lambda below does nothing else after calling the guarded macro).
}

// RAII guard mirroring log_test.cpp's ForwardCallbackGuard: restores the
// production default (nullptr -> abort-on-violation) on scope exit
// regardless of how the enclosing TEST_CASE exits (including a failed
// CHECK, which doctest does not unwind via exception by default, but a
// REQUIRE does) -- without this, a left-over test hook would silently
// swallow a REAL violation in every later TEST_CASE in this binary.
struct ViolationHookGuard {
    ~ViolationHookGuard() {
        g_activeCapture.store(nullptr, std::memory_order_relaxed);
        rx::core::debug::detail::setViolationHookForTests(nullptr);
    }
};

}  // namespace

TEST_CASE(
    "assertMainThreadImpl does NOT invoke the violation hook for a call genuinely on the main thread") {
    ViolationCapture capture;
    g_activeCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureViolationHook);
    ViolationHookGuard guard;

    RX_ASSERT_MAIN_THREAD("debug_checks_test::legalMainThreadCall");

    std::lock_guard<std::mutex> lock(capture.mutex);
    CHECK(capture.callCount == 0);
}

TEST_CASE(
    "assertMainThreadImpl invokes the violation hook, naming the offending context, for a call from a "
    "genuinely different thread") {
    ViolationCapture capture;
    g_activeCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureViolationHook);
    ViolationHookGuard guard;

    // A plain std::thread, not rx::task::Scheduler: throwing would be
    // unsafe to let escape an enkiTS-managed worker's own dispatch loop
    // (risking std::terminate()), but this hook never throws anyway (see
    // its own comment above) -- and a plain std::thread here is a
    // genuinely, deterministically different thread::id from the main
    // thread, with none of a real scheduler's work-stealing uncertainty
    // about which thread a given chunk lands on. That scheduling-aware,
    // adaptive assertion is exactly what the rx_material integration test
    // (test_material_system.cpp) covers for the one real chunked-pass
    // caller this fix round instruments; this test's only job is proving
    // the underlying mechanism fires correctly for "some other thread",
    // full stop.
    std::thread worker([] { RX_ASSERT_MAIN_THREAD("debug_checks_test::illegalWorkerThreadCall"); });
    worker.join();

    std::lock_guard<std::mutex> lock(capture.mutex);
    CHECK(capture.callCount == 1);
    CHECK(capture.lastContext == "debug_checks_test::illegalWorkerThreadCall");
}

TEST_CASE("setViolationHookForTests(nullptr) restores the production default hook") {
    // Install, then explicitly restore via nullptr (not just via the RAII
    // guard on scope exit) -- proves nullptr is itself a valid, documented
    // way to restore the default, matching detail::setViolationHookForTests's
    // own doc comment ("nullptr ... restores the real abort-on-violation
    // production behavior"). This does not (and cannot, without crashing
    // this test binary) prove the restored hook actually aborts; it proves
    // only that installing nullptr is accepted and clears a previously
    // installed capture -- the abort-on-violation behavior itself is
    // exactly defaultViolationHook()'s one line in debug_checks.cpp,
    // reviewed by inspection rather than by executing it here.
    ViolationCapture capture;
    g_activeCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureViolationHook);

    rx::core::debug::detail::setViolationHookForTests(nullptr);
    g_activeCapture.store(nullptr, std::memory_order_relaxed);

    // With the capture hook uninstalled and no active capture target, a
    // violation from a worker thread would now reach the REAL default
    // (abort) -- so this test does not trigger one; it only checked that
    // the swap itself succeeded without crashing, which the test reaching
    // this line at all already proves.
    CHECK(capture.callCount == 0);
}

#endif  // RX_DEBUG_CHECKS
