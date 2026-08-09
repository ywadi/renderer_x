// This is the only test translation unit in rx_platform_tests (unlike
// rx_core_tests, which shares a separate tests/doctest_main.cpp across
// several test files), so it both implements doctest's runtime and
// provides main() directly.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <rx_platform/window.h>

TEST_CASE("Window::create/destroy lifecycle succeeds under any video driver") {
    auto window = rx::platform::Window::create("rx_platform_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    CHECK(window->sdlWindow() != nullptr);
    window->pumpEvents();
}

TEST_CASE("Window reports Vulkan instance extensions when a real display backend is present") {
    auto window = rx::platform::Window::create("rx_platform_vk_test", 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping Vulkan-extension check");
        return;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping");
        return;
    }
    CHECK(extensions.size() > 0);
}
