// Device-free -- no rx::platform::Window, no VkSurfaceKHR, no vk-bootstrap
// instance -- unlike rx_material_gpu_tests' doctest_main.cpp, this binary
// needs no warm-up before its first TEST_CASE runs, so this is the plain
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN translation unit rx_graph_tests'
// own doctest_main.cpp establishes (copied, not shared, matching that
// file's own precedent). Task 6's rx_api.h ABI contract tests
// (test_api_contract.cpp, test_api_header_self_contained.cpp) only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
