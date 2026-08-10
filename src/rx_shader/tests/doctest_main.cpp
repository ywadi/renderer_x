// rx_shader's tests never construct a VkDevice/VkInstance (compilation is
// pure host-side Slang work), so none of the vk-bootstrap process-wide
// function-pointer-caching hazard documented on rx_rhi_vk's doctest_main.cpp
// applies here -- doctest's own generated main is sufficient, with no
// warm-up step needed before any TEST_CASE runs.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
