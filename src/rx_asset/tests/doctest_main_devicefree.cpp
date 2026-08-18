// Device-free -- no rx::platform::Window, no VkSurfaceKHR, no vk-bootstrap
// instance (matching src/rx_graph/tests/doctest_main.cpp's identical
// plain-main convention for its own device-free binary). Task 13's
// gltf_pipeline/mikktspace_bridge/byte_source coverage needs neither a
// GPU nor fastgltf itself -- see gltf_pipeline_test.cpp's own comment.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
