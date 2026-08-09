#pragma once
#include <spdlog/spdlog.h>

namespace rx::core::log {

void init();

}  // namespace rx::core::log

#define RX_LOG_INFO(...)  ::spdlog::info(__VA_ARGS__)
#define RX_LOG_WARN(...)  ::spdlog::warn(__VA_ARGS__)
#define RX_LOG_ERROR(...) ::spdlog::error(__VA_ARGS__)
