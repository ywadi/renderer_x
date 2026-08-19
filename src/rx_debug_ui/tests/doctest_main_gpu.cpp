// Same warm-up pattern as src/rx_graph/tests/doctest_main_gpu.cpp (copied,
// not shared -- see that file's own comment for the full rationale): this
// binary's tests build their own headless/windowed rx::rhi::Context+Device
// instances directly, exposed to the same process-wide vk-bootstrap
// instance-function-pointer-caching hazard rx::rhi::Context::create()'s own
// WARNING documents (rx_rhi_vk/context.h).
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/context.h>

#include <cstdio>
#include <vector>

namespace {

void warmUpVkBootstrapInstanceFunctionCache() {
    std::vector<const char*> extensions;
    auto window = rx::platform::Window::create("rx_debug_ui_gpu_tests_warmup", 64, 64, /*visible=*/false);
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

    // Harness gap fix -- same rationale/mechanism as rx_rhi_vk/tests/
    // doctest_main.cpp and rx_graph/tests/doctest_main_gpu.cpp: a
    // validation error raised during a fixture's own teardown (after that
    // TEST_CASE's own CHECK_FALSE(hasValidationErrors()) already ran) must
    // still fail the run, not merely print.
    const std::size_t processErrors = rx::rhi::Context::processValidationErrorCount();
    if (processErrors > 0) {
        std::fprintf(stderr,
                      "[rx_debug_ui_gpu_tests] FAILED: %zu unfiltered Vulkan validation error(s) observed during "
                      "this run (possibly raised during a fixture's own teardown, after that TEST_CASE's "
                      "CHECK_FALSE(hasValidationErrors()) already ran) -- see the \"[vulkan validation]\" ERROR "
                      "line(s) above.\n",
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
