cmake_minimum_required(VERSION 3.21)
project(renderer_x LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(third_party)
add_subdirectory(src/rx_core)
add_subdirectory(src/rx_platform)
add_subdirectory(tools/dep_cache_smoketest)
add_subdirectory(tools/toolchain_check)
