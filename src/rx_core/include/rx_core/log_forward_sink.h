#pragma once

// log_forward_sink.h -- the spdlog sink backing rx_material's public
// rxSetLogCallback() ABI entry point [spec Phase 4 design D23, seed 13:
// "public log sink -- consuming engines need renderer logs in their own
// systems"]. Lives in rx_core (not rx_material) because it is a pure
// spdlog extension with zero rx_material/ABI-type knowledge: rx_api.h's
// RxLogCallback is `void (*)(RxLogSeverity, const char*, const char*,
// void*)` where RxLogSeverity is itself just a plain `int32_t` typedef
// (not a distinct enum type -- see rx_api.h's own comment on it), so it
// is byte-for-byte, TYPE-for-type identical to ForwardCallback below
// (`int32_t` in that exact slot). rx_material's api_impl.cpp therefore
// passes an RxLogCallback straight through to LogForwardSink::set() with
// no cast/trampoline at all -- see that file's own rxSetLogCallback() for
// the one call site.
//
// Thread-affinity (D5/D23, Phase 4): the callback installed via set() may
// be invoked from ANY thread that logs through the logger this sink is
// attached to -- worker threads included. This is the one deliberate
// exception to docs/threading.md's otherwise-universal main-thread-only
// default (see that file's own note that the public ABI surface Phase 4
// adds is exactly this log sink); a caller's callback must be safe to run
// concurrently from multiple threads and must not assume render/main-
// thread affinity -- see docs/threading.md.

#include <cstdint>
#include <memory>
#include <mutex>

#include <spdlog/sinks/base_sink.h>

namespace rx::core::log {

// Callback signature. Severity values mirror spdlog::level::level_enum's
// own Trace..Error values 1:1 (spdlog/common.h: trace=0, debug=1, info=2,
// warn=3, err=4) -- spdlog::level::critical folds down to 4 (Error) too,
// since this engine's own severity scale stops at Error (matching
// RX_LOG_ERROR's own ceiling in log.h; there is no RX_LOG_CRITICAL).
// `category`/`message` are valid ONLY for the duration of one invocation
// -- both point at buffers this sink frees the moment the callback
// returns (rx_api.h's RxLogCallback documents the identical contract for
// any caller reaching this through rxSetLogCallback()).
using ForwardCallback = void (*)(int32_t severity, const char* category, const char* message, void* userData);

// An spdlog sink that forwards every record it receives to a single,
// process-wide installed callback -- or does nothing at all when none is
// installed (the common, steady-state case: one mutex lock plus a null
// check, no string formatting/allocation performed on that path -- the
// "near-zero cost otherwise" this sink is required to hold to).
//
// Never constructed directly by a consumer -- see forwardSink() below,
// the one accessor that lazily creates AND registers this into the
// current default logger's sink list exactly once per process, however
// many times or from wherever it is called.
class LogForwardSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    // Installs `callback`/`userData` as the active forwarding target.
    // `callback == nullptr` uninstalls -- no further delivery until a new
    // one is installed (restores "console-only": the console sink
    // spdlog::default_logger() already carries is never touched/removed
    // by this class either way, so console output and forwarding are
    // always independent, additive concerns). Also clears any prior
    // throwing-callback disable state -- installing a callback is always
    // a fresh start, even right after a previous one was permanently
    // disabled for throwing (see disabledByException()'s own comment).
    //
    // Thread-safe against concurrent sink_it_()/set() calls from any
    // thread: guarded by callbackMutex_ below, a mutex-guarded {callback,
    // userData} PAIR rather than two independent atomics (or an atomic
    // pointer to a heap-allocated pair). Chosen because `callback` and
    // `userData` must always be observed TOGETHER -- a torn read that saw
    // a new callback paired with the old userData (or vice versa) would
    // silently hand a caller's callback the wrong context on the very
    // next log record racing a set() call -- and the critical section on
    // both the read side (sink_it_, which copies the pair out and
    // releases the lock BEFORE invoking the callback -- see the .cpp) and
    // the write side (set()) is a plain two-pointer copy, cheap enough
    // that a mutex here is immaterial next to spdlog's own formatting/IO
    // work. There is no real perf incentive to reach for a lock-free
    // alternative for something this rarely written and this cheap to
    // read.
    void set(ForwardCallback callback, void* userData);

    // True once a throwing callback has been permanently disabled (see
    // sink_it_()'s catch-all in the .cpp) -- lets tests assert the
    // disable path fired deterministically rather than scraping stderr
    // for the one console warning it also emits.
    [[nodiscard]] bool disabledByException() const;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    // Disables `callback`/`userData` ONLY if they are still the
    // currently-installed pair (guards against a narrow, accepted race:
    // callback A throws on thread 1 while a legitimate set() on thread 2
    // has already replaced it with callback B in between -- this must
    // never clobber B).
    void disableIfStillInstalled(ForwardCallback callback, void* userData);

    mutable std::mutex callbackMutex_;
    ForwardCallback callback_ = nullptr;
    void* userData_ = nullptr;
    bool disabledByException_ = false;
};

// Returns the process-wide LogForwardSink singleton, constructing it and
// appending it to spdlog::default_logger()'s own sink list the first time
// this is called from anywhere in the process (idempotent regardless of
// caller or call count -- both rx::core::log::init() and rx_material's
// rxSetLogCallback() call this unconditionally, so whichever runs first
// does the real work; every later call just returns the same instance).
// This ADDS a sink; it never replaces or removes the console sink
// spdlog::default_logger() already carries.
std::shared_ptr<LogForwardSink> forwardSink();

}  // namespace rx::core::log
