#include <doctest/doctest.h>
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <spdlog/sinks/ostream_sink.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

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
// Device-free: none of these touch a VkDevice, and none of them swap out
// spdlog::default_logger() the way the ostream_sink test above does --
// LogForwardSink is attached to whichever logger object is CURRENTLY the
// default the first time forwardSink() runs anywhere in this process (see
// its own header comment), so as long as every test that swaps loggers
// restores the original afterward (the one above already does), these
// cases can rely on RX_LOG_* reaching the very same sink instance
// regardless of test execution order.
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
// to log anything.
struct ForwardCallbackGuard {
    std::shared_ptr<rx::core::log::LogForwardSink> sink;
    ~ForwardCallbackGuard() { sink->set(nullptr, nullptr); }
};

}  // namespace

TEST_CASE("LogForwardSink delivers severity/category/message to an installed callback for a real logged record") {
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();

    CapturedRecord captured;
    sink->set(&captureCallback, &captured);
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
    sink->set(&captureCallback, &captured);
    sink->set(nullptr, nullptr);  // uninstall immediately -- no guard needed, already null.

    RX_LOG_ERROR("this record must not be delivered to captured");

    std::lock_guard<std::mutex> lock(captured.mutex);
    CHECK(captured.callCount == 0);
}

TEST_CASE("LogForwardSink delivers to the installed callback from a worker thread, not just the installer's own "
          "thread") {
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();

    CapturedRecord captured;
    sink->set(&captureCallback, &captured);
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
    sink->set(throwingCallback, nullptr);
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
