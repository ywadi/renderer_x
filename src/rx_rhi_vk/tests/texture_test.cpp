#include <doctest/doctest.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/texture.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace {

// Same skip-guarded windowed-device fixture pattern as upload_test.cpp --
// Uploader::create() needs a real rx::rhi::Device&.
struct TextureTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
};

std::optional<TextureTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping Texture2D test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping Texture2D test");
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

    return TextureTestFixture{std::move(*window), std::move(*context), std::move(*device), std::move(*allocator)};
}

// Deterministic 64x64 RGBA8 gradient -- every channel is a simple
// function of (x, y) that never overflows a uint8_t for this extent
// (x, y both in [0, 63]: x*4 and y*4 max out at 252, (x+y)*2 maxes out at
// 252), so the exact bytes are trivially reproducible for comparison
// after the round trip.
std::vector<uint8_t> makeGradientPixels(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t i = (static_cast<size_t>(y) * width + x) * 4;
            pixels[i + 0] = static_cast<uint8_t>(x * 4);
            pixels[i + 1] = static_cast<uint8_t>(y * 4);
            pixels[i + 2] = static_cast<uint8_t>((x + y) * 2);
            pixels[i + 3] = 255;
        }
    }
    return pixels;
}

}  // namespace

TEST_CASE("Texture2D + Uploader::uploadToImage: 64x64 upload with generated mips, level 0 readback exact, level "
          "count correct, validation clean") {
    auto fixture = makeFixture("rx_rhi_vk_texture_test_upload_mips");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    constexpr VkExtent2D kExtent{64, 64};
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

    auto texture = rx::rhi::Texture2D::create(fixture->device.physicalDevice(), fixture->device.device(),
                                               fixture->allocator, kExtent, kFormat, VK_IMAGE_USAGE_SAMPLED_BIT,
                                               /*requestedMipLevels=*/0);
    REQUIRE(texture.has_value());
    // floor(log2(64)) + 1 == 7 -- the full chain down to a 1x1 level.
    CHECK(texture->mipLevels() == 7);
    CHECK(texture->image() != VK_NULL_HANDLE);
    CHECK(texture->view() != VK_NULL_HANDLE);

    std::vector<uint8_t> pixels = makeGradientPixels(kExtent.width, kExtent.height);
    REQUIRE(uploader->uploadToImage(*texture, pixels.data(), pixels.size(), /*generateMips=*/true));
    // [Phase 4 Task 11, spec D25] flush() no longer blocks -- wait()
    // explicitly before this readback's own separate submission.
    uploader->wait(uploader->flush());

    // Readback: transition (back) to a copy source, then copy level 0
    // out to a host-visible buffer -- transitionImage() covers every
    // level via VK_REMAINING_MIP_LEVELS, which is harmless here since the
    // copy itself only ever reads level 0.
    auto readback = fixture->allocator.createHostVisibleBuffer(pixels.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                    fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        rx::rhi::transitionImage(cmd, texture->image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {kExtent.width, kExtent.height, 1};
        vkCmdCopyImageToBuffer(cmd, texture->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                               &region);
    });

    readback->invalidate();
    std::vector<uint8_t> readBackPixels(pixels.size());
    std::memcpy(readBackPixels.data(), readback->mappedData(), pixels.size());

    CHECK(std::memcmp(readBackPixels.data(), pixels.data(), pixels.size()) == 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Texture2D + Uploader::uploadToImage: a single-mip-level texture round-trips exactly through both "
          "uploadToImage() branches -- generateMips=true (exercises Texture2D::recordMipChainBlit()'s own "
          "early-return branch) and generateMips=false (exercises uploadToImage()'s plain-transition branch), "
          "hardware-independent") {
    auto fixture = makeFixture("rx_rhi_vk_texture_test_single_mip");
    if (!fixture.has_value()) {
        return;
    }

    auto uploader = rx::rhi::Uploader::create(fixture->allocator, fixture->device);
    REQUIRE(uploader.has_value());

    constexpr VkExtent2D kExtent{16, 16};
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
    std::vector<uint8_t> pixels = makeGradientPixels(kExtent.width, kExtent.height);

    auto readBackLevel0 = [&](rx::rhi::Texture2D& texture) {
        auto readback = fixture->allocator.createHostVisibleBuffer(pixels.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        REQUIRE(readback.has_value());

        auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                        fixture->device.graphicsQueueFamily());
        REQUIRE(cmdCtx.has_value());
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            rx::rhi::transitionImage(cmd, texture.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {kExtent.width, kExtent.height, 1};
            vkCmdCopyImageToBuffer(cmd, texture.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                                   &region);
        });
        readback->invalidate();
        std::vector<uint8_t> result(pixels.size());
        std::memcpy(result.data(), readback->mappedData(), pixels.size());
        return result;
    };

    // Sub-case 1: generateMips=true on a requestedMipLevels=1 texture --
    // uploadToImage() now calls Texture2D::recordMipChainBlit()
    // unconditionally whenever generateMips is true (see upload.cpp),
    // which for mipLevels() == 1 takes its own early-return branch (a
    // single transitionLevelRange() call, no vkCmdBlitImage at all) --
    // previously unreachable through this public API (this task's
    // review, Important 2).
    {
        // TRANSFER_SRC_BIT requested explicitly: Texture2D::create() only
        // auto-adds it when mipLevels() > 1 (a single-mip texture never
        // needs to be a blit source in production), but this test's own
        // readback helper needs it to run vkCmdCopyImageToBuffer.
        auto textureA = rx::rhi::Texture2D::create(fixture->device.physicalDevice(), fixture->device.device(),
                                                     fixture->allocator, kExtent, kFormat,
                                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                     /*requestedMipLevels=*/1);
        REQUIRE(textureA.has_value());
        REQUIRE(textureA->mipLevels() == 1);

        REQUIRE(uploader->uploadToImage(*textureA, pixels.data(), pixels.size(), /*generateMips=*/true));
        uploader->wait(uploader->flush());

        std::vector<uint8_t> readBack = readBackLevel0(*textureA);
        CHECK(std::memcmp(readBack.data(), pixels.data(), pixels.size()) == 0);
    }

    // Sub-case 2: generateMips=false on the same kind of texture --
    // uploadToImage()'s plain single-transition `else` branch (never
    // calls recordMipChainBlit() at all).
    {
        auto textureB = rx::rhi::Texture2D::create(fixture->device.physicalDevice(), fixture->device.device(),
                                                     fixture->allocator, kExtent, kFormat,
                                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                     /*requestedMipLevels=*/1);
        REQUIRE(textureB.has_value());
        REQUIRE(textureB->mipLevels() == 1);

        REQUIRE(uploader->uploadToImage(*textureB, pixels.data(), pixels.size(), /*generateMips=*/false));
        uploader->wait(uploader->flush());

        std::vector<uint8_t> readBack = readBackLevel0(*textureB);
        CHECK(std::memcmp(readBack.data(), pixels.data(), pixels.size()) == 0);
    }

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Texture2D::create honors an explicit requestedMipLevels below the auto-computed maximum") {
    auto fixture = makeFixture("rx_rhi_vk_texture_test_explicit_miplevels");
    if (!fixture.has_value()) {
        return;
    }

    auto texture =
        rx::rhi::Texture2D::create(fixture->device.physicalDevice(), fixture->device.device(), fixture->allocator,
                                    VkExtent2D{64, 64}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,
                                    /*requestedMipLevels=*/3);
    REQUIRE(texture.has_value());
    CHECK(texture->mipLevels() == 3);

    // Requesting more than the extent could ever support clamps down
    // silently rather than failing -- a 4x4 extent can only ever produce
    // 3 levels (4 -> 2 -> 1) regardless of what's requested.
    auto clamped =
        rx::rhi::Texture2D::create(fixture->device.physicalDevice(), fixture->device.device(), fixture->allocator,
                                    VkExtent2D{4, 4}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT,
                                    /*requestedMipLevels=*/10);
    REQUIRE(clamped.has_value());
    CHECK(clamped->mipLevels() == 3);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Texture2D::createCubeForPresuppliedMips builds a real 6-layer VK_IMAGE_VIEW_TYPE_CUBE image, "
          "isolated from TextureCache -- [Phase 5 Task 6, ticket #42, gate matrix-p5t06-ktx2-cubemap-hdr row 7]") {
    auto fixture = makeFixture("rx_rhi_vk_texture_test_create_cube");
    if (!fixture.has_value()) {
        return;
    }

    auto cube = rx::rhi::Texture2D::createCubeForPresuppliedMips(
        fixture->device.physicalDevice(), fixture->device.device(), fixture->allocator, VkExtent2D{16, 16},
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, /*mipLevels=*/3, rx::rhi::MemoryCategory::Texture);
    REQUIRE(cube.has_value());
    CHECK(cube->image() != VK_NULL_HANDLE);
    CHECK(cube->view() != VK_NULL_HANDLE);
    CHECK(cube->isCube());
    CHECK(cube->arrayLayers() == 6);
    CHECK(cube->mipLevels() == 3);
    // `extent()` reports ONE FACE's own extent (this factory's own
    // documented contract, texture.h) -- never a 6x-multiplied width.
    CHECK(cube->extent().width == 16);
    CHECK(cube->extent().height == 16);

    // A plain (non-cube) Texture2D built via the SIBLING factory in the
    // same test, for direct contrast: isCube()/arrayLayers() must NOT
    // have silently become "always cube" process-wide.
    auto plain = rx::rhi::Texture2D::createForPresuppliedMips(
        fixture->device.physicalDevice(), fixture->device.device(), fixture->allocator, VkExtent2D{16, 16},
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, /*mipLevels=*/1, rx::rhi::MemoryCategory::Texture);
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->isCube());
    CHECK(plain->arrayLayers() == 1);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
