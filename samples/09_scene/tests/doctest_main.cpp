// Device-free -- no VkDevice, no rx::platform::Window (matching
// src/rx_scene/tests/doctest_main.cpp's identical plain-main convention).
// splitByBlockAndGroup() (draw_recording.h) is pure data transformation
// over already-built rx::scene types, so this binary needs no warm-up
// before its first TEST_CASE runs.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
