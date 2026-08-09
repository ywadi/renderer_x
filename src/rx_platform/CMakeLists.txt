find_package(Vulkan REQUIRED)

add_library(rx_platform STATIC
    src/window.cpp
)
target_include_directories(rx_platform PUBLIC include)
# SDL3::SDL3 is an ALIAS (to SDL3::SDL3-static, since third_party builds
# SDL3 static-only) created inside third_party/CMakeLists.txt's directory
# scope. Promoting the real underlying imported target to IMPORTED_GLOBAL
# (done in third_party/CMakeLists.txt) does not make the ALIAS name itself
# resolvable from this sibling directory -- CMake ALIAS targets are not
# global just because the target they point to is. Link the real global
# target name directly instead.
target_link_libraries(rx_platform PUBLIC SDL3::SDL3-static Vulkan::Headers rx_core)

add_executable(rx_platform_tests
    tests/window_test.cpp
)
target_link_libraries(rx_platform_tests PRIVATE rx_platform doctest::doctest)
add_test(NAME rx_platform_tests COMMAND rx_platform_tests)
