#include <rx_core/log.h>

namespace rx::core::log {

void init() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    initialized = true;
}

}  // namespace rx::core::log
