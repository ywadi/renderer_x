// tools/rx_ibl_bench/main.cpp -- Phase 5 Stage 1 Task 9 [#45]: a small,
// standalone (NOT ctest-registered -- a full production-scale bake is too
// slow to pay on every CI push, and this tool's own job is publishing
// honest numbers for the report, not gating a merge; T36/#72, RC8, is the
// later task that builds the permanent CI perf-regression GATE mechanism)
// benchmark that runs rx::ibl::bakeEnvironment() at PRODUCTION-scale
// parameters and prints its BakeTimings -- the phase's own binding "bake
// timing measured (Tracy zones) and reported... publish the numbers
// honestly" requirement (docs/superpowers/plans/2026-08-20-phase5-
// techniques.md, Task 9).
//
// Content is synthetic (a small procedurally-generated gradient equirect)
// rather than a real HDR asset deliberately: every bake stage's cost is a
// FIXED function of resolution/sample-count parameters alone, never of
// the source texture's actual pixel VALUES (every kernel in shaders/ibl/
// does the same fixed number of texture reads/ALU ops regardless of what
// they return) -- a synthetic source gives IDENTICAL timing to a real
// HDR environment at the same resolution, with zero test-asset
// provenance/licensing overhead.

#include <rx_ibl/bake.h>

#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/texture.h>
#include <rx_task/scheduler.h>

#include <volk.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr VkFormat kSourceFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

std::vector<float> buildGradientEquirect(uint32_t w, uint32_t h) {
    std::vector<float> px(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            px[idx + 0] = static_cast<float>(x) / static_cast<float>(w);
            px[idx + 1] = static_cast<float>(y) / static_cast<float>(h);
            px[idx + 2] = 0.5F;
            px[idx + 3] = 1.0F;
        }
    }
    return px;
}

bool uploadEquirect(rx::rhi::Device& device, rx::rhi::Allocator& allocator, rx::rhi::CommandContext& cmdCtx,
                     rx::rhi::Texture2D& tex, uint32_t width, uint32_t height, const std::vector<float>& pixels) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(pixels.size()) * sizeof(float);
    auto staging = allocator.createHostVisibleBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!staging.has_value()) {
        return false;
    }
    std::memcpy(staging->mappedData(), pixels.data(), static_cast<size_t>(bytes));
    staging->flush();

    cmdCtx.runOnce([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toDst{};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = tex.image();
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toDst;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging->handle(), tex.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* shaderDir = argc > 1 ? argv[1] : RX_IBL_SHADER_DIR;

    auto window = rx::platform::Window::create("rx_ibl_bench", 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        std::fprintf(stderr, "rx_ibl_bench: no display backend available\n");
        return 1;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        std::fprintf(stderr, "rx_ibl_bench: no Vulkan surface extensions reported\n");
        return 1;
    }
    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/false);
    if (!context.has_value()) {
        std::fprintf(stderr, "rx_ibl_bench: Context::create failed\n");
        return 1;
    }
    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    auto device = rx::rhi::Device::create(*context, surface);
    if (!device.has_value()) {
        std::fprintf(stderr, "rx_ibl_bench: Device::create failed\n");
        return 1;
    }
    auto allocator = rx::rhi::Allocator::create(*context, *device);
    if (!allocator.has_value()) {
        std::fprintf(stderr, "rx_ibl_bench: Allocator::create failed\n");
        return 1;
    }
    auto scheduler = rx::task::Scheduler::create();
    auto cmdCtx = rx::rhi::CommandContext::create(device->device(), device->graphicsQueue(), device->graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        std::fprintf(stderr, "rx_ibl_bench: CommandContext::create failed\n");
        return 1;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device->physicalDevice(), &props);
    std::printf("rx_ibl_bench: device = %s\n", props.deviceName);

    constexpr uint32_t kEquirectW = 512;
    constexpr uint32_t kEquirectH = 256;
    auto pixels = buildGradientEquirect(kEquirectW, kEquirectH);
    auto source = rx::rhi::Texture2D::create(device->physicalDevice(), device->device(), *allocator,
                                               VkExtent2D{kEquirectW, kEquirectH}, kSourceFormat,
                                               VK_IMAGE_USAGE_SAMPLED_BIT, /*requestedMipLevels=*/1);
    if (!source.has_value() || !uploadEquirect(*device, *allocator, *cmdCtx, *source, kEquirectW, kEquirectH, pixels)) {
        std::fprintf(stderr, "rx_ibl_bench: source upload failed\n");
        return 1;
    }

    // Production-scale parameters -- comparable to common real-time IBL
    // bake conventions (Filament's own cmgen defaults sit in this same
    // ballpark): a 256-px base cubemap, 7 prefiltered mips down to 4px,
    // a 128-px DFG LUT.
    rx::ibl::BakeParams params;
    params.baseCubemapFaceSize = 256;
    params.irradianceFaceSize = 32;
    params.irradianceSamples = 1024;
    params.prefilteredMipCount = 7;
    params.prefilteredBaseFaceSize = 256;
    params.specularSamples = 256;
    params.dfgLutSize = 128;
    params.dfgSamples = 1024;

    rx::ibl::BakeTimings timings;
    auto result = rx::ibl::bakeEnvironment(*device, *allocator, *cmdCtx, *scheduler, *source, /*sourceIsCube=*/false,
                                             shaderDir, params, &timings);
    if (!result.has_value()) {
        std::fprintf(stderr, "rx_ibl_bench: bakeEnvironment failed\n");
        return 1;
    }

    std::printf("rx_ibl_bench: params baseCubemapFaceSize=%u irradianceFaceSize=%u(x%u samples) "
                "prefilteredMipCount=%u(base=%u, x%u samples/mip) dfgLutSize=%u(x%u samples)\n",
                params.baseCubemapFaceSize, params.irradianceFaceSize, params.irradianceSamples,
                params.prefilteredMipCount, params.prefilteredBaseFaceSize, params.specularSamples, params.dfgLutSize,
                params.dfgSamples);
    std::printf("rx_ibl_bench: equirect->cubemap = %.3f ms\n", timings.equirectToCubemapMs);
    std::printf("rx_ibl_bench: irradiance convolution = %.3f ms\n", timings.irradianceMs);
    std::printf("rx_ibl_bench: prefiltered specular = %.3f ms\n", timings.prefilterMs);
    std::printf("rx_ibl_bench: DFG LUT = %.3f ms\n", timings.dfgMs);
    std::printf("rx_ibl_bench: total (incl. setup + final copy) = %.3f ms\n", timings.totalMs);

    vkDeviceWaitIdle(device->device());
    return 0;
}
