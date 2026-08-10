#include <rx_core/log.h>

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
    std::call_once(initFlag, [] { spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v"); });
}

}  // namespace rx::core::log
