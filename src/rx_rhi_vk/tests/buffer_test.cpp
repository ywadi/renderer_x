#include <doctest/doctest.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/device.h>
#include <rx_platform/window.h>
#include <array>
#include <cstring>
#include <optional>
#include <utility>

namespace {

// One layer deeper than device_test.cpp's DeviceTestFixture: a hidden
// window, a validated Context, and the Device built against the surface
// that window/context produce. Allocator::create() needs a live Device
// (VkPhysicalDevice/VkDevice) and Context (VkInstance), so this fixture
// carries the Device through rather than stopping at the bare surface.
// Same skip-guard pattern as device_test.cpp/window_test.cpp -- returns an
// empty optional (with a MESSAGE explaining why) only when this machine's
// video/Vulkan backend genuinely can't support the test; on a machine with
// a real display and driver this must actually produce a value.
struct BufferTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
};

std::optional<BufferTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping Buffer test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping Buffer test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    return BufferTestFixture{std::move(*window), std::move(*context), std::move(*device)};
}

struct Vertex {
    float x;
    float y;
    float z;
};

}  // namespace

TEST_CASE("Allocator::createHostVisibleBuffer round-trips a 3-vertex pattern through mapped memory") {
    auto fixture = makeFixture("rx_rhi_vk_buffer_test_roundtrip");
    if (!fixture.has_value()) {
        return;
    }

    // Allocator (and, below, Buffer) are declared after `fixture` -- i.e.
    // after its Window/Context/Device -- purely by ordinary C++ scope
    // rules: locals are destroyed in the exact reverse order they were
    // declared, so Buffer and Allocator are guaranteed to be torn down
    // before Device/Context/Window without any manual teardown here. This
    // is the RAII property the brief calls out explicitly: "allocator/buffer
    // die before Device".
    auto allocator = rx::rhi::Allocator::create(fixture->context, fixture->device);
    REQUIRE(allocator.has_value());

    constexpr std::array<Vertex, 3> kPattern{{
        {0.0F, -0.5F, 0.0F},
        {0.5F, 0.5F, 0.0F},
        {-0.5F, 0.5F, 0.0F},
    }};
    constexpr VkDeviceSize kBufferSize = sizeof(kPattern);

    auto buffer = allocator->createHostVisibleBuffer(kBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    REQUIRE(buffer.has_value());

    CHECK(buffer->handle() != VK_NULL_HANDLE);
    CHECK(buffer->mappedData() != nullptr);
    CHECK(buffer->size() == kBufferSize);

    std::memcpy(buffer->mappedData(), kPattern.data(), kBufferSize);
    CHECK(std::memcmp(buffer->mappedData(), kPattern.data(), kBufferSize) == 0);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
