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
//
// [Fix round 1, task-5-review.md Critical] Lifetime contract: set()
// (hence rxSetLogCallback()) does not return until any invocation of the
// PREVIOUS callback already in flight on another thread has fully
// completed -- see set()'s own doc comment for the mechanism and why
// this is the fix, not merely a documentation patch, for a real,
// reproduced use-after-free (the review's probe: install -> log from a
// worker -> uninstall -> immediately free userData -> the worker's
// still-running invocation dereferences the freed pointer).

#include <atomic>
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
//
// The callback must be QUICK and must not assume delivery is re-entrant:
// a callback that itself logs anything (through RX_LOG_*/spdlog on the
// SAME thread while still running) will not receive that inner record
// through THIS sink -- it is silently dropped for forwarding purposes
// only (the console/other sinks on the same logger still receive and
// print it normally; see sink_it_()'s own comment in the .cpp for why
// this is a deliberate drop, not a bug). The callback must also NEVER
// call set() (hence rxSetLogCallback()) on itself, directly or
// indirectly, from within its own invocation -- see set()'s own comment.
using ForwardCallback = void (*)(int32_t severity, const char* category, const char* message, void* userData);

// An spdlog sink that forwards every record it receives to a single,
// process-wide installed callback -- or does nothing at all when none is
// installed (the common, steady-state case: one thread_local check plus
// one relaxed-enough atomic load and a return -- no mutex acquired on
// this sink's OWN side at all on that path; see installed_'s own comment
// for the "near-zero cost" claim this restores after fix round 1's Low
// finding). This is layered UNDER spdlog's own `base_sink<Mutex>::log()`,
// which unconditionally locks ITS OWN `mutex_` around every call into
// sink_it_() below regardless of whether anything is installed here --
// that outer lock is spdlog's own unavoidable per-sink cost, paid by
// every sink (including the console one) on every log record, not
// something this class adds or can skip.
//
// Never constructed directly by a consumer -- see forwardSink() below,
// the one accessor that lazily creates AND registers this into the
// current default logger's sink list exactly once per process, however
// many times or from wherever it is called.
class LogForwardSink final : public spdlog::sinks::base_sink<std::recursive_mutex> {
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
    // [Fix round 1, task-5-review.md Critical] BLOCKS until any
    // invocation of the callback/userData pair being REPLACED that is
    // already in flight (on another thread) has fully returned, by
    // taking the SAME callbackMutex_ sink_it_() holds for the entire
    // duration of its own callback invocation (see sink_it_()'s comment
    // in the .cpp) -- not merely across a copy of the pair, as an
    // earlier revision of this class did (that revision released the
    // lock before invoking, which is exactly what let set(nullptr, ...)
    // return while the old callback was still running with the old
    // userData: a real, reproduced use-after-free once a caller freed
    // userData right after the call returned, per the review's own
    // probe). The guarantee this now provides: once THIS call returns,
    // no further invocation of the callback/userData pair it just
    // replaced (or cleared) is running or will ever start -- userData is
    // safe to free immediately afterward.
    //
    // [Fix round 1, task-5-review.md item (c)] Must NEVER be called (nor
    // may rxSetLogCallback(), which forwards to this) from inside the
    // currently-installed callback's own invocation, directly or
    // indirectly (e.g. a callback that logs, whose message happens to be
    // handled by code that itself calls rxSetLogCallback) -- that thread
    // already holds callbackMutex_ for the duration of the invocation
    // (see above), and a plain (non-recursive) std::mutex self-deadlocks
    // if the SAME thread tries to lock it again. Detected via the same
    // thread_local reentrancy flag sink_it_() uses (see the .cpp): this
    // is caught with a debug-build `assert()` (compiled out in this
    // project's own RelWithDebInfo/-DNDEBUG builds, both presets -- so
    // treat it as documentation-and-defense-in-depth for a Debug build
    // elsewhere, NOT as this project's own real safety net) AND, always
    // active regardless of NDEBUG, by returning `false` WITHOUT touching
    // any state or ever attempting the lock -- rxSetLogCallback() maps
    // that to RX_E_FAIL. Returns `true` for every normal (non-reentrant)
    // call, which always succeeds (there is no other invalid input: see
    // rxSetLogCallback()'s own comment on why `userData` is never
    // validated).
    [[nodiscard]] bool set(ForwardCallback callback, void* userData);

    // True once a throwing callback has been permanently disabled (see
    // sink_it_()'s catch-all in the .cpp) -- lets tests assert the
    // disable path fired deterministically rather than scraping stderr
    // for the one console warning it also emits.
    [[nodiscard]] bool disabledByException() const;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    // Guarded by callbackMutex_ below (held for the FULL duration of a
    // callback invocation in sink_it_(), not just while copying it out --
    // see that method's own comment for why this is the Critical fix).
    // `mutable`: disabledByException() is logically read-only (const) but
    // still needs to lock this same mutex for a consistent read.
    mutable std::mutex callbackMutex_;
    ForwardCallback callback_ = nullptr;
    void* userData_ = nullptr;
    bool disabledByException_ = false;

    // Fast-path hint, NOT the authoritative state (callback_ under
    // callbackMutex_ is): sink_it_() checks this FIRST, with no lock at
    // all, and returns immediately when it reads false, restoring the
    // near-zero uninstalled-path cost fix round 1's Low finding asked
    // for. Written only inside set() while callbackMutex_ is held (kept
    // consistent with callback_ by construction: both are written in the
    // same critical section), so a load here can be momentarily stale by
    // at most one in-flight set() call -- the slow path re-checks
    // callback_ under the real lock regardless, so a stale `true` costs
    // one avoidable lock acquisition and a stale `false` is impossible
    // to observe as a false negative for a callback that is ACTUALLY
    // still installed (the write here always happens-after the write to
    // callback_ in program order within the same critical section, and
    // both this sink's own reads/writes of installed_ use the default
    // (strongest) memory ordering deliberately -- correctness over a
    // relaxed micro-optimization on a variable this rarely written).
    std::atomic<bool> installed_{false};
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
