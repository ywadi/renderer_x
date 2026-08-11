#include <rx_core/log_forward_sink.h>

#include <cassert>
#include <cstdio>
#include <string>

#include <spdlog/spdlog.h>
// spdlog is built here as a precompiled library (SPDLOG_COMPILED_LIB) --
// base_sink.h's own template method bodies (base_sink-inl.h) are then
// normally pulled in only by spdlog's OWN build, which explicitly
// instantiates base_sink<std::mutex>/base_sink<null_mutex> (the only two
// Mutex types spdlog's own built-in sinks ever use) into libspdlog.a.
// LogForwardSink instantiates base_sink<std::recursive_mutex> instead
// (see log_forward_sink.h's own comment on why) -- a third instantiation
// spdlog's precompiled library was never built with, so this include
// (legal and idempotent regardless of SPDLOG_HEADER_ONLY -- base_sink-
// inl.h re-includes base_sink.h itself, guarded by that header's own
// `#pragma once`) makes the template bodies visible in THIS translation
// unit, letting the compiler instantiate that one specific
// specialization locally instead of expecting it to already exist in
// the prebuilt .a.
#include <spdlog/sinks/base_sink-inl.h>

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

// [Fix round 1, task-5-review.md Critical + item (c)] Set to true for the
// ENTIRE duration of this thread's own callback invocation inside
// sink_it_() below (RAII-reset via ForwardGuard, so it clears even if the
// callback throws) -- read by:
//   - sink_it_() itself, at entry: a true value here means this exact
//     call is a RE-ENTRANT one (the callback currently running on this
//     thread just logged something, which is what drove spdlog back into
//     this same sink's log()/sink_it_() on the same thread -- see
//     base_sink<std::recursive_mutex> in the header for why that
//     recursive call can even reach here instead of deadlocking one
//     level up). Detected, it returns immediately WITHOUT touching
//     callbackMutex_ or invoking anything again -- the re-entrant record
//     is silently dropped for forwarding purposes only; the same
//     logger's OTHER sinks (the real console sink included) are
//     completely unaffected by this early return, since spdlog's
//     logger::log_it_() calls every sink independently and this only
//     short-circuits OUR sink's own turn.
//   - set(), at entry: a true value here means set()/rxSetLogCallback()
//     is being called FROM INSIDE the currently-running callback itself
//     (directly, or indirectly through something that callback called)
//     -- the one misuse rxSetLogCallback()'s own doc comment forbids,
//     because that thread already holds callbackMutex_ for this
//     invocation's whole duration (see sink_it_()) and callbackMutex_ is
//     deliberately NOT recursive (unlike spdlog's own mutex_ above) --
//     see set()'s own doc comment for why a plain mutex is exactly what
//     makes its cross-thread blocking guarantee correct, and why this
//     thread_local check is what keeps the SAME-thread reentrant case
//     from self-deadlocking on it instead.
thread_local bool t_insideForward = false;

struct ForwardGuard {
    ForwardGuard() { t_insideForward = true; }
    ~ForwardGuard() { t_insideForward = false; }
};

}  // namespace

bool LogForwardSink::set(ForwardCallback callback, void* userData) {
    // [Fix round 1, task-5-review.md item (c)] Debug-build assertion --
    // compiled out under this project's own -DNDEBUG (both configured
    // presets build RelWithDebInfo, which CMake's own defaults define
    // NDEBUG for; verified directly against this tree's own
    // build.ninja). Present as documentation-and-defense-in-depth for a
    // genuine Debug build elsewhere, NOT as this project's real safety
    // net -- the `return false` below is what actually, always (NDEBUG
    // or not) prevents the self-deadlock this project's own builds would
    // otherwise be exposed to.
    assert(!t_insideForward &&
           "rxSetLogCallback()/LogForwardSink::set() must never be called from inside the currently-installed "
           "callback's own invocation -- that thread already holds callbackMutex_ for the duration of the "
           "callback, and this mutex is deliberately non-recursive, so calling back in would self-deadlock. "
           "Returning false instead (RX_E_FAIL at the rxSetLogCallback() ABI layer).");
    if (t_insideForward) {
        return false;  // rejected -- see the assert's own message above; no state touched, no lock attempted.
    }

    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback_ = callback;
    userData_ = callback != nullptr ? userData : nullptr;
    disabledByException_ = false;
    installed_.store(callback != nullptr);
    return true;
}

bool LogForwardSink::disabledByException() const {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return disabledByException_;
}

void LogForwardSink::sink_it_(const spdlog::details::log_msg& msg) {
    // [Fix round 1, task-5-review.md item (a)/(b)] Re-entrancy guard,
    // checked FIRST, before anything else (including the fast-path
    // installed_ load below): if this thread is already inside a
    // forwarded callback invocation (see t_insideForward's own comment),
    // this call is the callback logging something itself -- drop it for
    // forwarding purposes only and return immediately. This is what lets
    // base_sink<std::recursive_mutex>'s own mutex_ be re-acquired by the
    // SAME thread one level up (in log(), which wraps this whole call)
    // without ever reaching a second, nested invocation of the callback
    // here.
    if (t_insideForward) {
        return;
    }

    // [Fix round 1, task-5-review.md Low #3] Fast path: near-zero cost
    // when nothing is installed -- one atomic load, no mutex acquired on
    // THIS sink's own side at all (see installed_'s own comment on why
    // this is safe as a hint rather than the authoritative check).
    if (!installed_.load()) {
        return;
    }

    // [Fix round 1, task-5-review.md Critical] callbackMutex_ is held for
    // the callback invocation's ENTIRE duration below, not just while
    // copying `callback_`/`userData_` out (an earlier revision released
    // the lock first specifically to avoid a self-deadlock on a
    // self-logging callback -- but that revision's own reasoning was
    // incomplete: base_sink<Mutex>::log() (spdlog, final, cannot be
    // overridden) ALREADY wraps this entire sink_it_() call in its own
    // mutex_ lock regardless of anything callbackMutex_ does, so a
    // self-logging callback was always going to hit THAT lock again on
    // the same thread first -- switching mutex_ to a recursive_mutex
    // (the header's base_sink<std::recursive_mutex>) plus the
    // t_insideForward guard above is what actually closes that hazard,
    // which means callbackMutex_ no longer needs to release early for
    // that reason at all. Holding it here instead is what makes set()'s
    // own blocking-until-drained guarantee true: set() cannot acquire
    // this same mutex, and therefore cannot return to ITS caller
    // (rxSetLogCallback(), hence a consuming engine that may free
    // userData immediately afterward), until this invocation below has
    // completely finished.
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (callback_ == nullptr) {
        return;  // race window between the fast-path load above and this lock; safe, authoritative re-check.
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

    ForwardGuard guard;  // t_insideForward = true for the invocation below; resets on return OR exception.
    try {
        callback_(severity, category.c_str(), message.c_str(), userData_);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                      "rx_core: log-forward callback threw std::exception(\"%s\") -- disabling it permanently "
                      "(this log record's own delivery is otherwise silently dropped, never re-thrown)\n",
                      e.what());
        // Still holding callbackMutex_ continuously since the check above
        // -- no other thread's set() could have raced in and replaced
        // callback_/userData_ in the meantime, so this is unconditionally
        // correct (no compare-before-clear needed, unlike an earlier
        // revision that released the lock before invoking).
        callback_ = nullptr;
        userData_ = nullptr;
        disabledByException_ = true;
        installed_.store(false);
    } catch (...) {
        std::fprintf(stderr,
                      "rx_core: log-forward callback threw a non-std::exception value -- disabling it "
                      "permanently\n");
        callback_ = nullptr;
        userData_ = nullptr;
        disabledByException_ = true;
        installed_.store(false);
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
