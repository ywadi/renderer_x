// Same warm-up-before-any-TEST_CASE pattern every GPU test binary in this
// codebase establishes (src/rx_rhi_vk/tests/doctest_main.cpp is the
// original; src/rx_graph/tests/doctest_main_gpu.cpp/src/rx_material/
// tests/ own copies are the closest precedent for a non-rx_rhi_vk
// library) -- see either of those files' own header comment for the full
// vk-bootstrap process-wide instance-function-pointer-caching hazard this
// avoids.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>
#include <rx_platform/window.h>

#include <cstdio>
#include <vector>

namespace {

void warmUpVkBootstrapInstanceFunctionCache() {
    std::vector<const char*> extensions;
    auto window = rx::platform::Window::create("rx_shadow_gpu_tests_warmup", 64, 64, /*visible=*/false);
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

    const std::size_t processErrors = rx::rhi::Context::processValidationErrorCount();
    if (processErrors > 0) {
        std::fprintf(stderr,
                      "[rx_shadow_gpu_tests] FAILED: %zu unfiltered Vulkan validation error(s) "
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
