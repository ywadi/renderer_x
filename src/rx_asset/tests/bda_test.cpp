#include <doctest/doctest.h>
#include <rx_asset/geometry_pool.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// bda_test.cpp -- GeometryPool's own BDA acceptance criterion [Phase 4
// Stage 1 Task 12, spec D26.4, gate matrix-issue21 row "BDA enablement --
// pool block buffers carry VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT"]:
// "test asserts a pool block yields nonzero vkGetBufferDeviceAddress on a
// supporting device". The lower-level Device/Allocator/Buffer mechanics
// (feature bit, VMA allocator flag) are covered one layer down in
// src/rx_rhi_vk/tests/bda_test.cpp; this file exercises the same
// guarantee through GeometryPool's own public surface
// (bufferDeviceAddressEnabled()/vertexBufferDeviceAddress()/
// indexBufferDeviceAddress()) against a real block created by a real
// upload() call.
//
// Empirically verified in-task (2026-08-18) via `vulkaninfo`: both
// devices available in this development environment (NVIDIA GeForce RTX
// 2080 proprietary driver, and Mesa llvmpipe/lavapipe) report
// `bufferDeviceAddress = true` -- see src/rx_rhi_vk/tests/bda_test.cpp's
// header comment for the full citation.

namespace {

struct BdaPoolFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    rx::rhi::Uploader uploader;
};

std::optional<BdaPoolFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping GeometryPool BDA test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping GeometryPool BDA test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());

    auto uploader = rx::rhi::Uploader::create(*allocator, *device);
    REQUIRE(uploader.has_value());

    return BdaPoolFixture{std::move(*window), std::move(*context), std::move(*device), std::move(*allocator),
                           std::move(*uploader)};
}

}  // namespace

TEST_CASE("GeometryPool block buffers yield a nonzero vkGetBufferDeviceAddress on a BDA-supporting device") {
    auto fixture = makeFixture("rx_asset_bda_pool_block");
    if (!fixture.has_value()) {
        return;
    }

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader);
    REQUIRE(pool != nullptr);

    CHECK(pool->bufferDeviceAddressEnabled() == fixture->device.supportsBufferDeviceAddress());

    std::vector<rx::asset::PoolVertex> verts(3);
    std::vector<uint32_t> indices{0, 1, 2};
    rx::asset::MeshRange range = pool->upload(verts, indices);
    REQUIRE(pool->blockCount() == 1);
    CHECK(range.blockId == 0);

    if (pool->bufferDeviceAddressEnabled()) {
        VkDeviceAddress vertexAddress = pool->vertexBufferDeviceAddress(range.blockId);
        VkDeviceAddress indexAddress = pool->indexBufferDeviceAddress(range.blockId);
        CHECK(vertexAddress != 0);
        CHECK(indexAddress != 0);
        CHECK(vertexAddress != indexAddress);
    } else {
        MESSAGE("this device does not support bufferDeviceAddress -- verifying the disabled branch instead");
        CHECK(pool->vertexBufferDeviceAddress(range.blockId) == 0);
        CHECK(pool->indexBufferDeviceAddress(range.blockId) == 0);
    }

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
