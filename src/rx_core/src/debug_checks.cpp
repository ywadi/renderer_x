#include <rx_core/debug_checks.h>

// Entire body guarded, matching the header -- when RX_DEBUG_CHECKS is OFF,
// this translation unit compiles to a genuinely empty object file: zero
// symbols, verified directly (see task-7-report.md's fix-round section for
// the `nm`/`strings` evidence, mirroring Task 3's own RX_TRACY=OFF
// verification convention).
#ifdef RX_DEBUG_CHECKS

#include <rx_core/log.h>

#include <atomic>
#include <cstdlib>
#include <thread>

namespace rx::core::debug {

namespace {

// Production default: log (already done by the caller, assertMainThreadImpl()
// itself, before invoking this) then terminate immediately. A silent data
// race corrupting shared state is strictly worse than a loud, deterministic
// crash naming exactly which API was misused.
void defaultViolationHook(const char* /*context*/) {
    std::abort();
}

// The currently-installed violation hook -- std::atomic because a
// violation can be detected from any worker thread concurrently with a
// test's own main thread calling detail::setViolationHookForTests() to
// install/restore one; relaxed ordering is sufficient (this is a single
// function-pointer swap with no other state it needs to synchronize
// against -- the hook itself is responsible for whatever synchronization
// ITS OWN recording needs, per its own contract in debug_checks.h).
std::atomic<detail::ViolationHook> g_violationHook{&defaultViolationHook};

}  // namespace

void assertMainThreadImpl(const char* context) {
    // Lazily captured on the FIRST call to this function from ANYWHERE in
    // the process, by whichever thread reaches it first -- C++11's static-
    // local initialization is itself thread-safe and exactly-once (the
    // standard's own guarantee, not something this code adds), so no
    // separate registration call (e.g. a `markMainThread()` some other
    // subsystem would need to remember to invoke) exists at all: this is
    // the LEAST INTRUSIVE of the two mechanisms considered for this fix
    // round -- zero new call sites anywhere in this engine's setup path
    // beyond the guarded mutators themselves.
    //
    // LIMITATION, disclosed rather than hidden: this can only mis-learn
    // the main thread if a program's FIRST EVER call to ANY
    // RX_ASSERT_MAIN_THREAD site (across every guarded subsystem, process-
    // wide) happens from a non-main thread -- which is already the exact
    // misuse this mechanism exists to catch, just not on its own first
    // occurrence (every LATER call, including from the genuine main
    // thread, would then be incorrectly flagged as the violator instead).
    // This is not a real risk in this engine's own call graph: every real
    // caller (BindlessTable/Uploader/MaterialSystem/DeletionQueue) is only
    // ever reached after synchronous, main-thread scene/device setup that
    // itself calls one of these guarded APIs at least once (e.g. loading
    // the first material, uploading the first mesh) before any
    // `Scheduler::parallelFor()` fan-out could possibly exist to run a
    // worker at all -- so a real program's first-ever guarded call is
    // always, by construction, on the main thread. Documented here so a
    // future caller with a genuinely different startup shape knows to
    // check this before trusting a "no violation reported" result.
    static const std::thread::id kMainThreadId = std::this_thread::get_id();

    if (std::this_thread::get_id() == kMainThreadId) {
        return;
    }

    RX_LOG_ERROR("rx_core: main-thread-only API called from a non-main thread: {}", context);
    g_violationHook.load(std::memory_order_relaxed)(context);
}

namespace detail {

void setViolationHookForTests(ViolationHook hook) {
    g_violationHook.store(hook != nullptr ? hook : &defaultViolationHook, std::memory_order_relaxed);
}

}  // namespace detail

}  // namespace rx::core::debug

#endif  // RX_DEBUG_CHECKS
