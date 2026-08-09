#include <doctest/doctest.h>
#include <rx_core/log.h>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>

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
