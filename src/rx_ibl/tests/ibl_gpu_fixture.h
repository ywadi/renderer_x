#pragma once
// rx_ibl/tests/ibl_gpu_fixture.h -- Phase 5 Stage 1 Task 9 [#45]. Shared
// windowed-device bootstrap + test-only source-texture upload/readback
// helpers for this binary's GPU tests -- an independent copy of the same
// skip-guarded fixture shape rx_graph/tests/test_compute_gpu.cpp's own
// ComputeGpuFixture establishes (this codebase's own established
// per-file-duplicated-fixture idiom, not shared as a cross-module public
// header).
//
// TEST-SOURCE FORMAT CHOICE [implementer decision]: every synthetic
// source texture this fixture builds uses VK_FORMAT_R32G32B32A32_SFLOAT
// (full fp32), NOT R16G16B16A16_SFLOAT (Task 6's own real-asset upload
// format ruling) -- this is a TEST-ONLY convenience avoiding an fp32->fp16
// packing step for synthetic CPU-generated pixel data, verified directly
// against both drivers this round (R32G32B32A32_SFLOAT's own
// VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT support, needed for
// this bake's own bilinear TextureCube/Texture2D reads, is NOT a Vulkan
// 1.0-mandatory guarantee the way it is for R16G16B16A16_SFLOAT -- see
// task-09-report.md's own verification note). rx_ibl::bakeEnvironment()
// itself is format-polymorphic on its `source` parameter (a
// `Texture2D<float4>`/`TextureCube<float4>` sampled read accepts any
// float-interpretable format), so this choice is local to this test
// binary alone and does not constrain a real caller (Task 10) from using
// Task 6's own R16G16B16A16_SFLOAT loader output directly.

#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/texture.h>
#include <rx_task/scheduler.h>

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace rx::ibl_test {

constexpr VkFormat kTestSourceFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

struct IblGpuFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    std::unique_ptr<rx::task::Scheduler> scheduler;
    std::unique_ptr<rx::rhi::CommandContext> cmdCtx;
};

inline std::optional<IblGpuFixture> makeIblFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping rx_ibl GPU test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping rx_ibl GPU test");
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
    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto cmdCtx = rx::rhi::CommandContext::create(device->device(), device->graphicsQueue(), device->graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    return IblGpuFixture{std::move(*window),
                          std::move(*context),
                          std::move(*device),
                          std::move(*allocator),
                          std::move(scheduler),
                          std::make_unique<rx::rhi::CommandContext>(std::move(*cmdCtx))};
}

// Uploads `pixels` (tightly packed, `layerCount` consecutive
// width*height*4-float blocks) into a fresh non-mipped Texture2D (cube if
// `layerCount == 6`, plain 2D otherwise), leaving it in
// SHADER_READ_ONLY_OPTIMAL.
inline std::optional<rx::rhi::Texture2D> uploadTestSource(IblGpuFixture& fx, uint32_t width, uint32_t height,
                                                             uint32_t layerCount, const std::vector<float>& pixels) {
    REQUIRE(pixels.size() == static_cast<size_t>(width) * height * 4 * layerCount);
    std::optional<rx::rhi::Texture2D> tex;
    if (layerCount == 6) {
        tex = rx::rhi::Texture2D::createCubeForPresuppliedMips(fx.device.physicalDevice(), fx.device.device(),
                                                                  fx.allocator, VkExtent2D{width, height}, kTestSourceFormat,
                                                                  VK_IMAGE_USAGE_SAMPLED_BIT, 1);
    } else {
        REQUIRE(layerCount == 1);
        tex = rx::rhi::Texture2D::create(fx.device.physicalDevice(), fx.device.device(), fx.allocator,
                                          VkExtent2D{width, height}, kTestSourceFormat, VK_IMAGE_USAGE_SAMPLED_BIT,
                                          /*requestedMipLevels=*/1);
    }
    if (!tex.has_value()) {
        return std::nullopt;
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(pixels.size()) * sizeof(float);
    auto staging = fx.allocator.createHostVisibleBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!staging.has_value()) {
        return std::nullopt;
    }
    std::memcpy(staging->mappedData(), pixels.data(), static_cast<size_t>(bytes));
    staging->flush();

    fx.cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toDst{};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toDst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toDst.srcAccessMask = VK_ACCESS_2_NONE;
        toDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = tex->image();
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layerCount};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toDst;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layerCount};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging->handle(), tex->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier2 toRead = toDst;
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &dep2);
    });
    return tex;
}

// Reads back mip `mip`, layer `layer` of `tex` (currently
// SHADER_READ_ONLY_OPTIMAL) as tightly-packed RGBA floats (R16G16B16A16_
// SFLOAT/R16G16_SFLOAT/R32G32B32A32_SFLOAT-agnostic -- `bytesPerTexel`
// names how many raw bytes to copy per texel; the caller decodes them per
// its own known format). Restores SHADER_READ_ONLY_OPTIMAL before
// returning (readback is non-destructive to the texture's own usability
// for a LATER readback call on a different mip/layer).
inline std::vector<uint8_t> readbackRaw(IblGpuFixture& fx, rx::rhi::Texture2D& tex, uint32_t mip, uint32_t layer,
                                          uint32_t width, uint32_t height, uint32_t bytesPerTexel) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * bytesPerTexel;
    auto readback = fx.allocator.createHostVisibleBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    fx.cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toSrc.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toSrc.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        toSrc.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = tex.image();
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toSrc;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, tex.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);

        VkImageMemoryBarrier2 toRead = toSrc;
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &dep2);
    });
    readback->invalidate();
    std::vector<uint8_t> out(static_cast<size_t>(bytes));
    std::memcpy(out.data(), readback->mappedData(), out.size());
    return out;
}

// fp16 (IEEE 754 binary16) -> fp32 decode -- used to interpret
// R16G16B16A16_SFLOAT/R16G16_SFLOAT readback bytes (this bake's own real
// output formats). No existing fp16 decode helper exists anywhere in this
// codebase (grepped) small/self-contained enough that hand-writing it
// here (rather than pulling in a new third-party dependency for one
// function) matches this project's own "no reasonable ready-made option
// exists for something this small" carve-out; standard bit-manipulation
// algorithm (subnormal/inf/nan cases handled), not a novel invention.
inline float halfToFloat(uint16_t h) {
    uint32_t sign = static_cast<uint32_t>(h & 0x8000U) << 16U;
    uint32_t exp = (h >> 10U) & 0x1FU;
    uint32_t mant = h & 0x3FFU;
    uint32_t bits;
    if (exp == 0U) {
        if (mant == 0U) {
            bits = sign;
        } else {
            // Subnormal half -> normalized float.
            int e = -1;
            uint32_t m = mant;
            do {
                m <<= 1U;
                ++e;
            } while ((m & 0x400U) == 0U);
            m &= 0x3FFU;
            uint32_t outExp = static_cast<uint32_t>(127 - 15 - e);
            bits = sign | (outExp << 23U) | (m << 13U);
        }
    } else if (exp == 0x1FU) {
        bits = sign | 0x7F800000U | (mant << 13U);
    } else {
        uint32_t outExp = exp - 15U + 127U;
        bits = sign | (outExp << 23U) | (mant << 13U);
    }
    float result = 0.0F;
    std::memcpy(&result, &bits, sizeof(float));
    return result;
}

}  // namespace rx::ibl_test
