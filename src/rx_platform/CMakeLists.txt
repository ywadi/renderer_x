# Vulkan::Headers comes from third_party/CMakeLists.txt's dependency-cache
# install of KhronosGroup/Vulkan-Headers, found via find_package(VulkanHeaders
# CONFIG ...) and promoted to IMPORTED_GLOBAL there so it's visible here.
# Deliberately NOT found via find_package(Vulkan): that would additionally
# require locating the platform's Vulkan loader import library, which this
# project never links against (see comment in third_party/CMakeLists.txt)
# and which does not exist when cross-compiling to Windows on a Linux host.

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
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_platform_tests PROPERTIES TIMEOUT 120)
