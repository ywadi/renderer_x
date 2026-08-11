#include <doctest/doctest.h>
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/ostream_sink.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("log::init is idempotent") {
    rx::core::log::init();
    rx::core::log::init();
    CHECK(true);
}

TEST_CASE("RX_LOG_INFO writes the formatted message through spdlog's default logger") {
    rx::core::log::init();
    auto previousDefault = spdlog::default_logger();

    std::ostringstream capture;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(capture);
    auto testLogger = std::make_shared<spdlog::logger>("test", sink);
    testLogger->set_pattern("%v");
    spdlog::set_default_logger(testLogger);

    RX_LOG_INFO("hello {}", 42);

    spdlog::set_default_logger(previousDefault);

    // Windows spdlog terminates each formatted line with "\r\n"; Linux
    // spdlog uses a bare "\n". Strip both trailing characters before
    // comparing so this assertion is portable across native Linux and
    // Wine-hosted Windows builds alike.
    std::string captured = capture.str();
    while (!captured.empty() && (captured.back() == '\n' || captured.back() == '\r')) {
        captured.pop_back();
    }
    CHECK(captured == "hello 42");
}

// ===== LogForwardSink [spec Phase 4 design D23, seed 13] ===================
// Device-free: none of these touch a VkDevice. Most don't swap out
// spdlog::default_logger() the way the ostream_sink test above does --
// LogForwardSink is attached to whichever logger object is CURRENTLY the
// default the first time forwardSink() runs anywhere in this process (see
// its own header comment), so as long as every test that swaps loggers
// restores the original afterward (the one above already does, and the
// re-entrancy test below does too), these cases can rely on RX_LOG_*
// reaching the very same sink instance regardless of test execution order.
namespace {

struct CapturedRecord {
    std::mutex mutex;
    int32_t severity = -1;
    std::string category;
    std::string message;
    std::thread::id threadId{};
    int callCount = 0;
};

void captureCallback(int32_t severity, const char* category, const char* message, void* userData) {
    auto* captured = static_cast<CapturedRecord*>(userData);
    std::lock_guard<std::mutex> lock(captured->mutex);
    captured->severity = severity;
    captured->category = category != nullptr ? category : "";
    captured->message = message != nullptr ? message : "";
    captured->threadId = std::this_thread::get_id();
    captured->callCount++;
}

// RAII guard: uninstalls the forwarding sink's callback on scope exit no
// matter how the enclosing TEST_CASE exits, including a failed REQUIRE
// (doctest unwinds via a C++ exception in that case). Without this, a
// TEST_CASE that fails a REQUIRE between installing a callback and
// uninstalling it would leave a dangling `userData` pointer -- this stack
// frame's own CapturedRecord -- permanently installed in the process-wide
// singleton, corrupting every later TEST_CASE in this binary that happens
// to log anything. set()'s [[nodiscard]] bool is deliberately ignored
// here: called from the main test thread outside any callback invocation,
// this uninstall can only fail (return false) if this were itself
// re-entrant, which it never is.
struct ForwardCallbackGuard {
    std::shared_ptr<rx::core::log::LogForwardSink> sink;
    ~ForwardCallbackGuard() { (void)sink->set(nullptr, nullptr); }
};

}  // namespace

TEST_CASE("LogForwardSink delivers severity/category/message to an installed callback for a real logged record") {
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();

    CapturedRecord captured;
    REQUIRE(sink->set(&captureCallback, &captured));
    ForwardCallbackGuard guard{sink};

    RX_LOG_ERROR("forward sink test message {}", 42);

    std::lock_guard<std::mutex> lock(captured.mutex);
    CHECK(captured.callCount >= 1);
    CHECK(captured.severity == 4);  // Error, per LogForwardSink's own Trace..Error mapping.
    CHECK(captured.message == "forward sink test message 42");
    // Category = the spdlog logger name the record was issued through --
    // RX_LOG_* always goes through spdlog's unnamed default logger.
    CHECK(captured.category.empty());
}

TEST_CASE("LogForwardSink stops delivering once uninstalled via a nullptr callback") {
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();

    CapturedRecord captured;
    REQUIRE(sink->set(&captureCallback, &captured));
    REQUIRE(sink->set(nullptr, nullptr));  // uninstall immediately -- no guard needed, already null.

    RX_LOG_ERROR("this record must not be delivered to captured");

    std::lock_guard<std::mutex> lock(captured.mutex);
    CHECK(captured.callCount == 0);
}

TEST_CASE("LogForwardSink delivers to the installed callback from a worker thread, not just the installer's own "
          "thread") {
    // [Fix round 1, task-5-review.md Low #5] A raw std::thread, not
    // rx::task::Scheduler, per this task's binding resolution's generic
    // "worker-thread delivery" wording -- a real, deliberate deviation
    // from the original brief's more specific "via rx_task" phrasing,
    // acknowledged in task-5-report.md's fix-round-1 section. This still
    // asserts genuine cross-thread delivery (captured.threadId is
    // compared against the actual main thread id below, not merely
    // assumed), which is what the resolution as given requires.
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();

    CapturedRecord captured;
    REQUIRE(sink->set(&captureCallback, &captured));
    ForwardCallbackGuard guard{sink};

    std::thread::id mainThreadId = std::this_thread::get_id();
    std::thread worker([] { RX_LOG_ERROR("logged from a worker thread"); });
    worker.join();

    std::lock_guard<std::mutex> lock(captured.mutex);
    CHECK(captured.callCount >= 1);
    CHECK(captured.threadId != mainThreadId);
}

TEST_CASE("LogForwardSink permanently disables a throwing callback after one console warning, without crashing") {
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();

    auto throwingCallback = +[](int32_t, const char*, const char*, void*) { throw std::runtime_error("boom"); };
    REQUIRE(sink->set(throwingCallback, nullptr));
    ForwardCallbackGuard guard{sink};

    CHECK_FALSE(sink->disabledByException());
    RX_LOG_ERROR("this record's callback throws");
    CHECK(sink->disabledByException());

    // Genuinely disabled, not merely a flag nobody acts on: a second log
    // record after the throw does not crash the process (this whole test
    // case completing IS part of that proof) and disabledByException()
    // stays true rather than somehow clearing itself.
    RX_LOG_ERROR("a second record after the callback was disabled");
    CHECK(sink->disabledByException());
}

// ===== Fix round 1 [task-5-review.md] =======================================

// --- Critical: uninstall must drain any in-flight invocation ---------------
// Reproduces the review's own probe: a callback that touches `userData`
// while genuinely, concurrently in flight, uninstalled immediately after,
// with `userData` freed the instant set() returns. Before this fix,
// set(nullptr, ...) returned while a prior sink_it_() call on another
// thread was still between copying the pair out and invoking the
// callback -- freeing userData right after set() returned was therefore a
// real, deterministically-reproducible use-after-free (see the review's
// own 5/5 reproduction). Run 20x under real concurrent load (four storm
// threads hammering the sink directly, not through RX_LOG_* -- see the
// comment below on why -- while the main thread installs, waits briefly
// for the storm to be genuinely in flight, uninstalls, and immediately
// frees/reallocates) rather than once, since a lock-ordering race is not
// guaranteed to manifest on a single iteration even when present.
//
// ASan/UBSan were considered per the review's own note but are not usable
// in this project's configured toolchain (both presets build through the
// zig cc/zig c++ wrappers; the review independently confirmed neither
// sanitizer runtime links for this exact target) -- this uses the same
// poison/observe pattern the review's own probe used instead: free the
// old userData, immediately allocate a same-size replacement, and confirm
// the replacement's content is never touched by the (now-uninstalled,
// and per the fix, provably no-longer-running) old callback.
namespace {

void poisonProbeCallback(int32_t, const char*, const char*, void* userData) {
    auto* counter = static_cast<std::atomic<int>*>(userData);
    counter->fetch_add(1, std::memory_order_relaxed);
    // Widen the race window deliberately -- without this, a storm thread
    // might finish its invocation before set(nullptr, ...) ever gets a
    // chance to race it, which would make this probe pass by accident
    // (no invocation in flight to drain) rather than by the fix actually
    // draining one.
    std::this_thread::sleep_for(std::chrono::microseconds(200));
}

}  // namespace

TEST_CASE("LogForwardSink::set uninstall blocks until in-flight callback invocations complete -- userData is safe "
          "to free immediately after (reviewer's UAF-reproduction probe, run 20x)") {
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();
    ForwardCallbackGuard guard{sink};

    for (int iteration = 0; iteration < 20; ++iteration) {
        auto* userData = new std::atomic<int>(iteration);
        REQUIRE(sink->set(&poisonProbeCallback, userData));

        std::atomic<bool> stop{false};
        std::vector<std::thread> stormThreads;
        stormThreads.reserve(4);
        for (int t = 0; t < 4; ++t) {
            // Drives the sink DIRECTLY (sink->log(), the same public
            // entry point base_sink<Mutex> gives every caller -- not
            // through RX_LOG_*/the shared default logger) so this storm
            // never touches the console sink or any other test's
            // logger-swap state: precise and quiet, exercising exactly
            // LogForwardSink::log()->sink_it_() under real concurrency.
            stormThreads.emplace_back([&sink, &stop] {
                spdlog::details::log_msg msg("", spdlog::level::err, "uninstall race probe storm message");
                while (!stop.load(std::memory_order_relaxed)) {
                    sink->log(msg);
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // let the storm get genuinely in flight.

        // Must not return until every invocation of poisonProbeCallback
        // already in flight (touching the OLD `userData`) has completed.
        REQUIRE(sink->set(nullptr, nullptr));

        // Immediately free the OLD userData and allocate a fresh,
        // same-size object in its place -- if set() above had returned
        // too early (the bug this fix closes), a still-running storm
        // thread's callback invocation would now be touching freed/
        // reused heap memory right here.
        delete userData;
        auto* freshData = new std::atomic<int>(-1);

        // Keep the storm running a little longer with NOTHING installed
        // -- proves the uninstalled fast path is itself safe under
        // continued concurrent load, not just quiescent.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        stop.store(true, std::memory_order_relaxed);
        for (auto& thread : stormThreads) {
            thread.join();
        }

        // freshData was never touched by the (uninstalled) callback --
        // poisonProbeCallback only ever increments, never writes -1, so
        // any UAF/heap-reuse corruption would show up here as a value
        // other than -1.
        CHECK(freshData->load() == -1);
        delete freshData;
    }
}

// --- Re-entrancy: a self-logging callback must not deadlock ----------------
// [Fix round 1, task-5-review.md items (a)/(b)] Holding callbackMutex_
// across the callback invocation (the Critical fix above) reintroduces a
// hazard a prior revision's "release before invoking" design was written
// to avoid: a callback that logs anything itself recurses into this same
// sink, same thread. Closed via base_sink<std::recursive_mutex> (lets the
// recursive call re-enter sink_it_() instead of deadlocking one level up,
// inside spdlog's own base_sink<Mutex>::log(), which is `final` and
// therefore not interceptible any other way) plus a thread_local guard
// that drops the re-entrant forward -- this proves BOTH halves: no
// deadlock (the test completing at all is part of that proof) AND the
// other sink on the same logger (standing in for the console) still
// receives the inner record.
namespace {

struct ReentrancyCapture {
    std::mutex mutex;
    int callCount = 0;
};

void reentrantCallback(int32_t, const char*, const char*, void* userData) {
    auto* captured = static_cast<ReentrancyCapture*>(userData);
    {
        std::lock_guard<std::mutex> lock(captured->mutex);
        captured->callCount++;
    }
    // Self-logging from inside the callback -- must not deadlock, and
    // must not reach this same callback again nested (dropped instead;
    // asserted via callCount staying at 1 below).
    RX_LOG_ERROR("inner message logged from inside the callback itself");
}

}  // namespace

TEST_CASE("LogForwardSink drops a re-entrant forward from a self-logging callback without deadlocking; the other "
          "sink on the same logger still receives the inner record") {
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();
    auto previousDefault = spdlog::default_logger();

    // A temporary logger mirroring the real default logger's shape
    // (console sink + forward sink together, not the forward sink
    // tested in isolation) -- an ostream sink stands in for "the
    // console" so this test can assert on exactly what it received.
    std::ostringstream capture;
    auto captureSink = std::make_shared<spdlog::sinks::ostream_sink_mt>(capture);
    auto testLogger = std::make_shared<spdlog::logger>("log_forward_sink_reentrancy_test",
                                                        spdlog::sinks_init_list{captureSink, sink});
    testLogger->set_pattern("%v");
    spdlog::set_default_logger(testLogger);

    ReentrancyCapture captured;
    REQUIRE(sink->set(&reentrantCallback, &captured));

    RX_LOG_ERROR("outer message");

    spdlog::set_default_logger(previousDefault);
    CHECK(sink->set(nullptr, nullptr));

    // Exactly one forwarded invocation -- the callback's own re-entrant
    // "inner message" call did not reach it a second time nested.
    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        CHECK(captured.callCount == 1);
    }

    // The capture sink is a SEPARATE sink object on the same logger --
    // it received BOTH records regardless of what our own forward sink's
    // callback did or didn't forward.
    std::string capturedText = capture.str();
    CHECK(capturedText.find("outer message") != std::string::npos);
    CHECK(capturedText.find("inner message") != std::string::npos);
}

// --- Misuse: calling set() from inside the callback must not deadlock ------
// [Fix round 1, task-5-review.md item (c)] The one call rxSetLogCallback()'s
// own doc comment forbids: the callback calls back into set() (hence what
// rxSetLogCallback() would do) on itself, from inside its own invocation.
// Must be rejected (false), never hang -- this test's own completion is
// part of the proof, on top of the explicit assertion below.
namespace {

struct SelfCallCapture {
    std::mutex mutex;
    int callCount = 0;
    bool rejected = false;
};

void selfCallingCallback(int32_t, const char*, const char*, void* userData) {
    auto* captured = static_cast<SelfCallCapture*>(userData);
    bool accepted = rx::core::log::forwardSink()->set(nullptr, nullptr);
    std::lock_guard<std::mutex> lock(captured->mutex);
    captured->callCount++;
    captured->rejected = !accepted;
}

}  // namespace

TEST_CASE("LogForwardSink::set rejects (returns false, does not deadlock) a call made from inside the "
          "currently-installed callback's own invocation") {
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();

    SelfCallCapture captured;
    REQUIRE(sink->set(&selfCallingCallback, &captured));
    ForwardCallbackGuard guard{sink};

    RX_LOG_ERROR("triggers the self-calling callback");  // completing this line at all proves no deadlock.

    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        CHECK(captured.callCount == 1);
        CHECK(captured.rejected);
    }

    // The rejected call touched no state: still installed, still
    // delivering, never disabled.
    CHECK_FALSE(sink->disabledByException());
    RX_LOG_ERROR("still delivering after the rejected self-call");
    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        CHECK(captured.callCount == 2);
    }
}
