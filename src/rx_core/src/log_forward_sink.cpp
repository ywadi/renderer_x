#include <rx_core/log_forward_sink.h>

#include <cstdio>
#include <string>

#include <spdlog/spdlog.h>

namespace rx::core::log {

namespace {

// spdlog::level::level_enum's own numeric values (spdlog/common.h):
// trace=0, debug=1, info=2, warn=3, err=4, critical=5, off=6. Folded 1:1
// onto this engine's own severity scale, which stops at Error (4) -- see
// ForwardCallback's own doc comment in the header for why critical/off
// collapse the way they do below.
int32_t mapLevel(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace:
            return 0;
        case spdlog::level::debug:
            return 1;
        case spdlog::level::info:
            return 2;
        case spdlog::level::warn:
            return 3;
        case spdlog::level::err:
            return 4;
        case spdlog::level::critical:
            return 4;  // folds into Error -- this engine has no RX_LOG_CRITICAL.
        case spdlog::level::off:
        case spdlog::level::n_levels:
        default:
            // off/n_levels should never reach sink_it_ (they are filter
            // values, never assigned to a real log_msg) -- treated as
            // Error defensively rather than asserting, since a
            // logging-adjacent path is the wrong place to crash a
            // process over an unexpected-but-harmless enum value.
            return 4;
    }
}

}  // namespace

void LogForwardSink::set(ForwardCallback callback, void* userData) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback_ = callback;
    userData_ = callback != nullptr ? userData : nullptr;
    disabledByException_ = false;
}

bool LogForwardSink::disabledByException() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return disabledByException_;
}

void LogForwardSink::disableIfStillInstalled(ForwardCallback callback, void* userData) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (callback_ == callback && userData_ == userData) {
        callback_ = nullptr;
        userData_ = nullptr;
        disabledByException_ = true;
    }
}

void LogForwardSink::sink_it_(const spdlog::details::log_msg& msg) {
    // Snapshot under the lock, then release it BEFORE invoking the
    // callback: invoking an arbitrary caller-supplied function pointer
    // while holding callbackMutex_ would deadlock if that callback logs
    // anything itself (a re-entrant call back into this exact sink, on
    // the same thread, trying to lock the same non-recursive mutex again)
    // or calls back into set()/rxSetLogCallback() to change/uninstall the
    // callback. Copying the pair out and unlocking first avoids both.
    ForwardCallback callback;
    void* userData;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (callback_ == nullptr) {
            return;  // near-zero cost path: nothing installed, nothing more to do.
        }
        callback = callback_;
        userData = userData_;
    }

    // spdlog::string_view_t views (msg.payload/msg.logger_name) are not
    // guaranteed null-terminated -- copy into std::string first so the
    // `const char*` handed to the callback is safely null-terminated for
    // the callback's own duration (rx_api.h's RxLogCallback documents this
    // exact string-lifetime contract for any caller reaching this through
    // rxSetLogCallback()).
    std::string category(msg.logger_name.data(), msg.logger_name.size());
    std::string message(msg.payload.data(), msg.payload.size());
    int32_t severity = mapLevel(msg.level);

    try {
        callback(severity, category.c_str(), message.c_str(), userData);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                      "rx_core: log-forward callback threw std::exception(\"%s\") -- disabling it permanently "
                      "(this log record's own delivery is otherwise silently dropped, never re-thrown)\n",
                      e.what());
        disableIfStillInstalled(callback, userData);
    } catch (...) {
        std::fprintf(stderr,
                      "rx_core: log-forward callback threw a non-std::exception value -- disabling it "
                      "permanently\n");
        disableIfStillInstalled(callback, userData);
    }
}

void LogForwardSink::flush_() {
    // Nothing buffered on this sink's own side -- the callback (if any)
    // ran synchronously, inline, inside sink_it_() above.
}

std::shared_ptr<LogForwardSink> forwardSink() {
    static std::shared_ptr<LogForwardSink> sink;
    static std::once_flag flag;
    std::call_once(flag, [] {
        sink = std::make_shared<LogForwardSink>();
        spdlog::default_logger()->sinks().push_back(sink);
    });
    return sink;
}

}  // namespace rx::core::log
