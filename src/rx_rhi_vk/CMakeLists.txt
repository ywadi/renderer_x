# volk_SOURCE_DIR/vma_SOURCE_DIR are populated (and PARENT_SCOPE-propagated
# up to the root) by third_party/CMakeLists.txt's FetchContent_Populate
# calls -- see the comments there for why it's Populate (source only)
# rather than MakeAvailable (which would also add_subdirectory each
# library's own CMakeLists.txt: volk's problematic `volk` target, and VMA's
# sample-application target with its unwanted GLFW dependency).
#
# src/vma_impl.cpp is the sole VMA_IMPLEMENTATION translation unit for this
# entire program (see that file); every other consumer of VMA
# (src/buffer.cpp, include/rx_rhi_vk/buffer.h) only ever declares against
# vk_mem_alloc.h, never redefines VMA_IMPLEMENTATION.
add_library(rx_rhi_vk STATIC
    src/context.cpp
    src/device.cpp
    src/buffer.cpp
    src/command.cpp
    src/vma_impl.cpp
    ${volk_SOURCE_DIR}/volk.c
)
target_include_directories(rx_rhi_vk PUBLIC include ${volk_SOURCE_DIR} ${vma_SOURCE_DIR}/include)
target_link_libraries(rx_rhi_vk PUBLIC rx_core vk-bootstrap::vk-bootstrap)
target_compile_definitions(rx_rhi_vk PUBLIC VK_NO_PROTOTYPES)

# Four test translation units now (context_test.cpp, device_test.cpp,
# buffer_test.cpp, clear_color_test.cpp), so -- like rx_core_tests, and
# unlike this target before device_test.cpp existed -- doctest's
# runtime/main is implemented once in a dedicated tests/doctest_main.cpp
# shared by all four, rather than in any one test file. doctest_main.cpp
# also links rx_platform: it uses rx::platform::Window to warm up a real
# vk-bootstrap hazard before any TEST_CASE runs (see the comment there) --
# so test order here is not load-bearing, deliberately; no --order-by is
# needed on the add_test line below. clear_color_test.cpp in particular is
# pure headless (no window/surface/swapchain) and relies on that warm-up to
# be safe regardless of where doctest schedules it relative to the windowed
# tests.
add_executable(rx_rhi_vk_tests
    tests/doctest_main.cpp
    tests/context_test.cpp
    tests/device_test.cpp
    tests/buffer_test.cpp
    tests/clear_color_test.cpp
)
target_link_libraries(rx_rhi_vk_tests PRIVATE rx_rhi_vk rx_platform doctest::doctest)
add_test(NAME rx_rhi_vk_tests COMMAND rx_rhi_vk_tests)
