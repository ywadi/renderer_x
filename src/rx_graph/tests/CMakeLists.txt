# Device-free (no rx::platform::Window, no VkSurfaceKHR, no vk-bootstrap
# instance) -- unlike rx_rhi_vk_tests' doctest_main.cpp, this binary needs
# no warm-up before its first TEST_CASE runs, so doctest_main.cpp here is
# the plain DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN translation unit rx_core_tests
# uses.
add_executable(rx_graph_tests
    doctest_main.cpp
    test_compile.cpp
)
target_link_libraries(rx_graph_tests PRIVATE rx_graph doctest::doctest)
add_test(NAME rx_graph_tests COMMAND rx_graph_tests)
