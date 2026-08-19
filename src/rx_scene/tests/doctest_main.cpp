// Device-free -- no VkDevice, no rx::platform::Window (matching
// src/rx_task/tests/doctest_main.cpp's/src/rx_graph/tests/doctest_main.cpp's
// identical plain-main convention for their own device-free binaries).
// rx_scene has no GPU-touching code at all -- Scene's SoA columns are
// plain std::vectors and Camera's projection helpers are pure GLM math --
// so this binary needs no warm-up before its first TEST_CASE runs.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
