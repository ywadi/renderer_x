// DOCTEST_CONFIG_IMPLEMENT (not _WITH_MAIN): see rx_asset_tests'
// doctest_main.cpp for the full warm-up rationale (copied here, not
// shared -- each GPU-backed test binary in this repo owns its own copy,
// matching rx_graph_gpu_tests' own doctest_main_gpu.cpp precedent).
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>
#include <rx_platform/window.h>

#include <cstdio>
#include <vector>

namespace {

void warmUpVkBootstrapInstanceFunctionCache() {
    std::vector<const char*> extensions;
    auto window = rx::platform::Window::create("rx_asset_gltf_gpu_tests_warmup", 64, 64, /*visible=*/false);
    if (window.has_value()) {
        extensions = window->requiredVulkanInstanceExtensions();
    }
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
                      "[rx_asset_gltf_gpu_tests] FAILED: %zu unfiltered Vulkan validation error(s) "
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
