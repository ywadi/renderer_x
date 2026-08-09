# volk_SOURCE_DIR is populated (and PARENT_SCOPE-propagated up to the root)
# by third_party/CMakeLists.txt's FetchContent_Populate(volk) call -- see the
# comment there for why it's Populate (source only) rather than
# MakeAvailable (which would also add_subdirectory volk's own CMakeLists.txt
# and its problematic `volk` target).
add_library(rx_rhi_vk STATIC
    src/context.cpp
    ${volk_SOURCE_DIR}/volk.c
)
target_include_directories(rx_rhi_vk PUBLIC include ${volk_SOURCE_DIR})
target_link_libraries(rx_rhi_vk PUBLIC rx_core vk-bootstrap::vk-bootstrap)
target_compile_definitions(rx_rhi_vk PUBLIC VK_NO_PROTOTYPES)

add_executable(rx_rhi_vk_tests
    tests/context_test.cpp
)
target_link_libraries(rx_rhi_vk_tests PRIVATE rx_rhi_vk doctest::doctest)
add_test(NAME rx_rhi_vk_tests COMMAND rx_rhi_vk_tests)
