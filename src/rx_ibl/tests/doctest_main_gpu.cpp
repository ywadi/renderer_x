// rx_ibl/tests/doctest_main_gpu.cpp -- Phase 5 Stage 1 Task 9 [#45]. Same
// warm-up-before-any-TEST_CASE pattern as rx_graph/tests/doctest_main_gpu.cpp
// (copied, not shared -- this codebase's own established per-binary
// duplicated-warm-up idiom): every TEST_CASE in this binary builds a real
// windowed rx::rhi::Context/Device, exposed to the same vk-bootstrap
// process-wide instance-function-pointer-caching hazard documented on
// rx::rhi::Context::create()'s own WARNING.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/context.h>

#include <cstdio>
#include <vector>

namespace {

void warmUpVkBootstrapInstanceFunctionCache() {
    std::vector<const char*> extensions;
    auto window = rx::platform::Window::create("rx_ibl_gpu_tests_warmup", 64, 64, /*visible=*/false);
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
                      "[rx_ibl_gpu_tests] FAILED: %zu unfiltered Vulkan validation error(s) observed during this "
                      "run (possibly raised during a fixture's own teardown).\n",
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
