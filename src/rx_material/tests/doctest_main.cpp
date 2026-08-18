// DOCTEST_CONFIG_IMPLEMENT (not _WITH_MAIN): main() below needs to run code
// before any TEST_CASE executes, so it constructs and drives the
// doctest::Context itself instead of using doctest's generated main() --
// the exact same warm-up pattern src/rx_rhi_vk/tests/doctest_main.cpp and
// src/rx_graph/tests/doctest_main_gpu.cpp both already establish (copied
// here, not shared, matching those two files' own precedent): every
// test_material_system.cpp case builds a real rx::rhi::Device (via
// rx::platform::Window), exposing it to the same vk-bootstrap process-wide
// instance-function-pointer-caching hazard documented on
// rx::rhi::Context::create()'s own WARNING (rx_rhi_vk/context.h) -- the
// pinned vk-bootstrap commit caches instance-level Vulkan function
// pointers process-wide from whichever vkb::Instance is built FIRST,
// forever, with no reset. Warming the cache with the BROADEST instance
// this binary will ever need (real window extensions when available,
// enableValidation=true unconditionally) before any TEST_CASE runs makes
// that hazard's precondition impossible to hit, regardless of test order.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/context.h>

#include <cstdio>
#include <vector>

namespace {

void warmUpVkBootstrapInstanceFunctionCache() {
    std::vector<const char*> extensions;
    auto window = rx::platform::Window::create("rx_material_gpu_tests_warmup", 64, 64, /*visible=*/false);
    if (window.has_value()) {
        extensions = window->requiredVulkanInstanceExtensions();
    }
    // enableValidation=true here is not incidental -- see
    // rx_rhi_vk/tests/doctest_main.cpp's identical warm-up for the full
    // rationale (the same process-wide vk-bootstrap cache also covers
    // fp_vkCreateDebugUtilsMessengerEXT, only non-null when the FIRST
    // instance built in this process actually requested validation).
    rx::rhi::Context::create(extensions, /*enableValidation=*/true);
}

}  // namespace

int main(int argc, char** argv) {
    warmUpVkBootstrapInstanceFunctionCache();

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    int res = context.run();

    // Harness gap fix -- see rx_rhi_vk/tests/doctest_main.cpp's identical
    // block for the full rationale: a validation error raised while a
    // fixture's OWN destructor chain tears down (after that TEST_CASE's own
    // CHECK_FALSE(hasValidationErrors()) already ran) previously printed but
    // never failed the run. Re-checking the PROCESS-lifetime tally here,
    // after context.run() has already destroyed every fixture including the
    // last one, closes that gap without touching debugCallback()'s own
    // known-false-positive filter list (context.cpp), which stays
    // authoritative.
    const std::size_t processErrors = rx::rhi::Context::processValidationErrorCount();
    if (processErrors > 0) {
        std::fprintf(stderr,
                      "[rx_material_gpu_tests] FAILED: %zu unfiltered Vulkan validation error(s) "
                      "observed during this run (possibly raised during a fixture's own "
                      "teardown, after that TEST_CASE's CHECK_FALSE(hasValidationErrors()) "
                      "already ran) -- see the \"[vulkan validation]\" ERROR line(s) above.\n",
                      processErrors);
        if (res == 0) {
            res = 1;
        }
    }

    if (context.shouldExit()) {
        return res;
    }
    return res;
}
