// DOCTEST_CONFIG_IMPLEMENT (not _WITH_MAIN): main() below needs to run code
// before any TEST_CASE executes, so it constructs and drives the
// doctest::Context itself instead of using doctest's generated main() --
// the exact same warm-up pattern src/rx_rhi_vk/tests/doctest_main.cpp
// establishes, copied here (not shared -- rx_graph_tests, the existing
// device-free target, keeps its own plain DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// doctest_main.cpp unchanged; this file is rx_graph_gpu_tests' own, since
// only THIS binary's tests ever touch a real VkDevice) because this
// binary's tests build their own headless/windowed rx::rhi::Context+Device
// instances directly, and are exposed to the exact same vk-bootstrap
// process-wide instance-function-pointer-caching hazard documented on that
// original file: see rx::rhi::Context::create()'s own WARNING (rx_rhi_vk/
// context.h) for the full defect this warm-up exists to avoid.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>
#include <rx_platform/window.h>

#include <vector>

namespace {

void warmUpVkBootstrapInstanceFunctionCache() {
    std::vector<const char*> extensions;
    auto window = rx::platform::Window::create("rx_graph_gpu_tests_warmup", 64, 64, /*visible=*/false);
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
    if (context.shouldExit()) {
        return res;
    }
    return res;
}
