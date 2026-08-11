// rx_task's tests never construct a VkDevice/VkInstance -- Scheduler is
// entirely device-free -- so, exactly like rx_graph_tests' own
// doctest_main.cpp, doctest's own generated main is sufficient with no
// warm-up step needed before any TEST_CASE runs.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
