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

# [Issue #76 teardown-hang fix] Linux-only subprocess reproduction harness
# for Scheduler::~Scheduler()'s bounded-shutdown path -- see
# shutdown_hang_probe.cpp's own header comment and scheduler_test.cpp's
# own "[Issue #76 teardown-hang fix, subprocess-based]" TEST_CASE, the
# only consumer of the compile definition below. Guarded the identical way
# src/rx_shader/CMakeLists.txt's own `if(NOT CMAKE_SYSTEM_NAME STREQUAL
# "Windows")` precedent already is: fork()/execv()/waitpid() (used by that
# TEST_CASE, not this file) have no portable Windows equivalent, and
# windows-cross-zig is a cross-compile-only CI target this repo never runs
# natively -- not worth a second, Windows-specific process-spawn
# implementation for one diagnostic-path test.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
  add_executable(rx_task_shutdown_hang_probe shutdown_hang_probe.cpp)
  target_link_libraries(rx_task_shutdown_hang_probe PRIVATE rx_task)
  # Deliberately NOT add_test(...) -- this binary is EXPECTED to
  # std::abort() (SIGABRT) every time it runs correctly; ctest would
  # otherwise report that as a failed test. scheduler_test.cpp's own
  # TEST_CASE spawns it directly and asserts on ITS exit behavior instead.
  target_compile_definitions(rx_task_tests PRIVATE
      RX_TASK_SHUTDOWN_HANG_PROBE_PATH="$<TARGET_FILE:rx_task_shutdown_hang_probe>")
endif()
