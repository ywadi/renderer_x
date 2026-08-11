# Device-free (no VkDevice, no rx::platform::Window) -- exactly like
# rx_graph_tests' own doctest_main.cpp (src/rx_graph/tests/CMakeLists.txt),
# this binary needs no warm-up before its first TEST_CASE runs.
add_executable(rx_task_tests
    doctest_main.cpp
    scheduler_test.cpp
)
target_link_libraries(rx_task_tests PRIVATE rx_task doctest::doctest)
add_test(NAME rx_task_tests COMMAND rx_task_tests)
# Hang guard: a wedged test fails in minutes with output, instead of stalling CI for hours
# (windows-cross Wine hang, run 31512073559).
set_tests_properties(rx_task_tests PROPERTIES TIMEOUT 120)
