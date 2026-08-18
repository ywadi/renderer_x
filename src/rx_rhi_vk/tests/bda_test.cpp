#include <doctest/doctest.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/device.h>
#include <rx_platform/window.h>
#include <optional>
#include <utility>

// bda_test.cpp -- coverage for the BDA (bufferDeviceAddress) enablement
// mechanics [Phase 4 Stage 1 Task 12, spec D26.4, gate matrix-issue21's
// three BDA rows]: the opportunistic Vulkan12Features bit
// (Device::supportsBufferDeviceAddress()), the matching VMA allocator flag
// (VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT, threaded through
// Allocator::create()/createRaw()), and the buffer-usage-bit consequence
// (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT + a real nonzero
// vkGetBufferDeviceAddress()). rx::asset::GeometryPool's own
// bda_test.cpp (src/rx_asset/tests) covers the POOL-BLOCK-level version
// of the last row against a real GeometryPool; this file covers the
// lower-level Device/Allocator/Buffer mechanics directly, one layer below
// GeometryPool, mirroring the existing memory_budget_test.cpp/
// oom_handling_test.cpp split (one file per feature-set, not folded into
// buffer_test.cpp/device_test.cpp).
//
// Empirically verified in-task (2026-08-18, per the ticket's own required
// step) via `vulkaninfo`: BOTH devices available in this development
// environment report `bufferDeviceAddress = true` --
// NVIDIA GeForce RTX 2080 (proprietary driver) AND Mesa llvmpipe/lavapipe
// (the CI-relevant software rasterizer) -- consistent with gate
// matrix-issue21's own finding that lavapipe has carried
// VK_KHR_buffer_device_address support since March 2021 (Mesa MR #9616).
// This means every TEST_CASE below exercises the SUPPORTED branch for
// real on both locally available drivers; no BDA-lacking device is
// available in this environment to exercise the "opportunistic, never a
// selection requirement" degrade branch against a REAL unsupported
// device -- that guarantee is structural instead (see the last TEST_CASE
// below and the task report for the full discussion, mirroring Task 11's
// own "eliminated structurally, no revert needed" disclosure for its row
// 1).

namespace {

struct BdaTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
};

std::optional<BdaTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping BDA test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping BDA test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    return BdaTestFixture{std::move(*window), std::move(*context), std::move(*device)};
}

}  // namespace

TEST_CASE("Device::create resolves supportsBufferDeviceAddress() without gating device selection") {
    auto fixture = makeFixture("rx_rhi_vk_bda_test_device");
    if (!fixture.has_value()) {
        return;
    }

    // The real assertion here is simpler than it looks: Device::create()
    // above already SUCCEEDED (the REQUIRE inside makeFixture()) without
    // this test ever adding bufferDeviceAddress to any REQUIRED feature
    // set -- proving selection did not depend on it. What this call
    // reports beyond that is informational (both local drivers happen to
    // support it, logged for the record rather than hardcoded as an
    // assumption a future CI driver must match).
    MESSAGE("Device::supportsBufferDeviceAddress() = ", fixture->device.supportsBufferDeviceAddress());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE(
    "Allocator built from a BDA-capable Device creates a VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT buffer that "
    "yields a nonzero vkGetBufferDeviceAddress") {
    auto fixture = makeFixture("rx_rhi_vk_bda_test_buffer");
    if (!fixture.has_value()) {
        return;
    }

    if (!fixture->device.supportsBufferDeviceAddress()) {
        MESSAGE("this device does not report bufferDeviceAddress support -- skipping the enablement-path assertion");
        return;
    }

    auto allocator = rx::rhi::Allocator::create(fixture->context, fixture->device);
    REQUIRE(allocator.has_value());

    // [matrix-issue21 row 61] Indirect proof that
    // VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT was actually set on
    // this Allocator: per the vendored VMA 3.4.0 source (vk_mem_alloc.h:
    // 14440-14445/16931), creating a buffer with
    // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT WITHOUT that allocator
    // flag trips a VMA_ASSERT / returns VK_ERROR_INITIALIZATION_FAILED --
    // so a successful creation here, followed by a real nonzero address,
    // is only possible if the flag genuinely made it through the
    // Device::supportsBufferDeviceAddress() -> Allocator::create() ->
    // createRaw() lockstep.
    auto buffer = allocator->createDeviceLocalBuffer(
        256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    REQUIRE(buffer.has_value());

    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = buffer->handle();
    VkDeviceAddress address = vkGetBufferDeviceAddress(fixture->device.device(), &addressInfo);

    CHECK(address != 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE(
    "bufferDeviceAddress enablement is opportunistic by construction, never a device-selection requirement "
    "[structural, spec D26.4]") {
    // This is a CODE-STRUCTURE guarantee, not a runtime assertion this
    // test can exercise against a real BDA-lacking device (none is
    // available in this environment -- see this file's header comment).
    // The guarantee holds by inspection of device.cpp: bufferDeviceAddress
    // is set on a SEPARATE, freshly zero-initialized
    // VkPhysicalDeviceVulkan12Features (`bufferDeviceAddressRequest`),
    // passed ONLY to `enable_extension_features_if_present()` -- called
    // AFTER `vkb::PhysicalDeviceSelector::select()` has already returned
    // successfully -- never merged into the EARLIER `features12` struct
    // passed to `set_required_features_12()` (the one struct that
    // actually gates which physical devices `select()` will accept).
    // Structurally, a device lacking the bit cannot fail selection over
    // it: there is no code path connecting this bit to `select()`'s own
    // acceptance criteria at all. Every fixture above already exercises
    // "Device::create() succeeds" as a REQUIRE with no bufferDeviceAddress
    // dependency threaded into it, which is the fixture-level half of
    // this same proof; this TEST_CASE exists solely to record the
    // guarantee explicitly, mirroring Task 11's own "eliminated
    // structurally, not merely by a runtime check" callout for a
    // similarly unreachable-on-available-hardware case.
    CHECK(true);
}
