#include <doctest/doctest.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/mesh_buffers.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace {

// Same skip-guarded windowed-device fixture pattern as buffer_test.cpp/
// device_test.cpp -- Uploader::create() takes a real rx::rhi::Device&
// (it needs a real graphics queue + queue family), which only exists
// against a real VkSurfaceKHR per device.h, so (unlike
// deletion_queue_test.cpp's pure-headless fixture, which never touches
// rx::rhi::Device at all) this file's tests need a window.
struct UploadTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
};

std::optional<UploadTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping Uploader test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping Uploader test");
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

    return UploadTestFixture{std::move(*window), std::move(*context), std::move(*device), std::move(*allocator)};
}

// Copies `size` bytes out of `src` (assumed device-local, TRANSFER_SRC_BIT)
// into a freshly allocated, invalidated, host-visible buffer -- the
// readback half of every round-trip test below.
std::vector<uint8_t> readBackBuffer(rx::rhi::Allocator& allocator, VkDevice device, VkQueue queue,
                                     uint32_t queueFamily, VkBuffer src, VkDeviceSize size) {
    auto readback = allocator.createHostVisibleBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    auto cmdCtx = rx::rhi::CommandContext::create(device, queue, queueFamily);
    REQUIRE(cmdCtx.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, src, readback->handle(), 1, &region);
    });

    // GPU write (the copy above) happened after this host-visible buffer
    // was mapped -- invalidate() before reading, per Buffer's own
    // contract (this task's own Buffer API addition).
    readback->invalidate();

    std::vector<uint8_t> result(static_cast<size_t>(size));
    std::memcpy(result.data(), readback->mappedData(), result.size());
    return result;
}

}  // namespace

TEST_CASE("Uploader::uploadToBuffer round-trips bytes through the staging path byte-exact") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_roundtrip");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());
    // Not a hard assertion either way -- both the direct (ReBAR/unified
    // memory) and staging (plain HOST_VISIBLE) paths are correct;
    // reporting which one this machine took is diagnostic, matching
    // Task 3's own bindless_test.cpp precedent of reporting hardware-
    // dependent facts via MESSAGE rather than asserting a specific one.
    MESSAGE("Uploader::usesDirectPath() on this machine: ", uploader->usesDirectPath());

    constexpr VkDeviceSize kSize = 4096;
    std::array<uint8_t, kSize> pattern{};
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    }

    auto dst = fixture->allocator.createDeviceLocalBuffer(
        kSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dst.has_value());

    REQUIRE(uploader->uploadToBuffer(dst->handle(), 0, pattern.data(), kSize));
    uploader->flush();

    std::vector<uint8_t> readBack = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                     fixture->device.graphicsQueue(),
                                                     fixture->device.graphicsQueueFamily(), dst->handle(), kSize);
    CHECK(std::memcmp(readBack.data(), pattern.data(), kSize) == 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader auto-flushes mid-batch when the ring buffer runs out of room, and every upload stays "
          "byte-exact") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_ring_wrap");
    if (!fixture.has_value()) {
        return;
    }

    // Deliberately tiny (64 bytes) so two 48-byte uploads in the same
    // batch cannot both fit -- the second one forces reserveRingSpace()'s
    // internal auto-flush path (upload.cpp) before it can write anything,
    // per the class comment in upload.h. Neither upload result should be
    // affected by that internal flush happening mid-batch.
    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device, /*ringBufferSize=*/64);
    REQUIRE(uploader.has_value());

    constexpr VkDeviceSize kChunkSize = 48;
    std::array<uint8_t, kChunkSize> patternA{};
    std::array<uint8_t, kChunkSize> patternB{};
    for (size_t i = 0; i < kChunkSize; ++i) {
        patternA[i] = static_cast<uint8_t>(i);
        patternB[i] = static_cast<uint8_t>(0xFF - i);
    }

    auto dstA = fixture->allocator.createDeviceLocalBuffer(
        kChunkSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dstA.has_value());
    auto dstB = fixture->allocator.createDeviceLocalBuffer(
        kChunkSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(dstB.has_value());

    REQUIRE(uploader->uploadToBuffer(dstA->handle(), 0, patternA.data(), kChunkSize));
    REQUIRE(uploader->uploadToBuffer(dstB->handle(), 0, patternB.data(), kChunkSize));
    uploader->flush();

    std::vector<uint8_t> readA = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                  fixture->device.graphicsQueue(),
                                                  fixture->device.graphicsQueueFamily(), dstA->handle(), kChunkSize);
    std::vector<uint8_t> readB = readBackBuffer(fixture->allocator, fixture->device.device(),
                                                  fixture->device.graphicsQueue(),
                                                  fixture->device.graphicsQueueFamily(), dstB->handle(), kChunkSize);
    CHECK(std::memcmp(readA.data(), patternA.data(), kChunkSize) == 0);
    CHECK(std::memcmp(readB.data(), patternB.data(), kChunkSize) == 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Uploader::uploadToBuffer rejects a single upload larger than the ring buffer's total capacity") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_oversize");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device, /*ringBufferSize=*/64);
    REQUIRE(uploader.has_value());

    std::array<uint8_t, 128> tooBig{};
    auto dst = fixture->allocator.createDeviceLocalBuffer(128, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(dst.has_value());

    CHECK_FALSE(uploader->uploadToBuffer(dst->handle(), 0, tooBig.data(), tooBig.size()));
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MeshBuffers::create uploads vertex+index data into device-local buffers, byte-exact readback") {
    auto fixture = makeFixture("rx_rhi_vk_upload_test_meshbuffers");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    struct Vertex {
        float position[3];
        float uv[2];
    };
    constexpr std::array<Vertex, 4> kVertices{{
        {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F}},
        {{1.0F, 0.0F, 0.0F}, {1.0F, 0.0F}},
        {{1.0F, 1.0F, 0.0F}, {1.0F, 1.0F}},
        {{0.0F, 1.0F, 0.0F}, {0.0F, 1.0F}},
    }};
    constexpr std::array<uint32_t, 6> kIndices{{0, 1, 2, 2, 3, 0}};

    auto mesh = rx::rhi::MeshBuffers::create(fixture->allocator, *uploader, kVertices.data(), sizeof(kVertices),
                                              kIndices.data(), sizeof(kIndices),
                                              static_cast<uint32_t>(kIndices.size()), VK_INDEX_TYPE_UINT32);
    REQUIRE(mesh.has_value());
    CHECK(mesh->indexCount() == 6);
    CHECK(mesh->indexType() == VK_INDEX_TYPE_UINT32);
    CHECK(mesh->vertexBufferSize() == sizeof(kVertices));
    CHECK(mesh->indexBufferSize() == sizeof(kIndices));

    // MeshBuffers::create() does not add TRANSFER_SRC_BIT to either
    // buffer's usage (it only ever needs to be a transfer *destination*
    // in production) -- readBackBuffer() below needs a fresh pair of
    // TRANSFER_SRC-capable device-local buffers instead, populated
    // through the same Uploader, purely so this test can verify the
    // upload path independently of MeshBuffers' own accessors already
    // returning correct handles/sizes above.
    auto vertexCopy = fixture->allocator.createDeviceLocalBuffer(
        sizeof(kVertices), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(vertexCopy.has_value());
    REQUIRE(uploader->uploadToBuffer(vertexCopy->handle(), 0, kVertices.data(), sizeof(kVertices)));
    uploader->flush();

    std::vector<uint8_t> readBack =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                        fixture->device.graphicsQueueFamily(), vertexCopy->handle(), sizeof(kVertices));
    CHECK(std::memcmp(readBack.data(), kVertices.data(), sizeof(kVertices)) == 0);

    // The real proof that MeshBuffers' OWN vertex/index buffers actually
    // contain the right bytes (not just that Uploader in isolation works,
    // which the copy above already showed): copy straight out of
    // mesh->vertexBuffer()/indexBuffer() themselves.
    std::vector<uint8_t> meshVertexReadBack =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                        fixture->device.graphicsQueueFamily(), mesh->vertexBuffer(), sizeof(kVertices));
    CHECK(std::memcmp(meshVertexReadBack.data(), kVertices.data(), sizeof(kVertices)) == 0);

    std::vector<uint8_t> meshIndexReadBack =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                        fixture->device.graphicsQueueFamily(), mesh->indexBuffer(), sizeof(kIndices));
    CHECK(std::memcmp(meshIndexReadBack.data(), kIndices.data(), sizeof(kIndices)) == 0);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
