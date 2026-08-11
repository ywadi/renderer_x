#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>

#include <mutex>

namespace rx::core::log {

void init() {
    // std::call_once/std::once_flag, not a bare `static bool` guarded by a
    // plain `if` -- that pattern's read-then-write of `initialized` is not
    // atomic, so two threads racing to call init() for the first time can
    // both observe `initialized == false` and both proceed past the check
    // (a real bug found when rx_shader's Compiler::create() called this
    // before taking its own global-session mutex: two threads racing to
    // create the first Compiler in a process raced here too). call_once
    // guarantees the callable runs exactly once and that every caller --
    // whichever thread wins, and every thread that merely blocks waiting
    // for it -- only returns after that one run completes.
    static std::once_flag initFlag;
    std::call_once(initFlag, [] {
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        // [spec D23, seed 13] Registers the public-log-sink forwarding
        // sink into the default logger, once, alongside the console sink
        // above -- see log_forward_sink.h's own comment. forwardSink()
        // is independently idempotent (its own call_once), so this call
        // is defensive/eager, not the only place it can ever run: every
        // process that reaches rx_material's rxSetLogCallback() calls it
        // too, in case that happens before anything else ever called
        // rx::core::log::init().
        forwardSink();
    });
}

}  // namespace rx::core::log
