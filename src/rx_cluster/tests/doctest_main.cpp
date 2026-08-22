// Device-free -- no VkDevice, no rx::platform::Window (rx::cluster::
// buildClusterLightList() touches no Vulkan type at all -- matching
// src/rx_scene/tests/doctest_main.cpp's identical plain-main convention).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
