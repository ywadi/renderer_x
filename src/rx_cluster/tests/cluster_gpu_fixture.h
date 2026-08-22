#pragma once

// src/rx_cluster/tests/cluster_gpu_fixture.h -- Phase 5 Stage 2 Task 14
// [#50]: shared GPU test fixture + harness for rx_cluster's own compute
// dispatch, mirroring rx_graph/tests/test_compute_gpu.cpp's own
// ComputeGpuFixture/OffscreenImage pattern (an independent copy, not a
// shared header, matching this codebase's own established per-file-helper
// precedent -- see e.g. barriers.cpp's own comment on why).
#include <doctest/doctest.h>
#include <rx_cluster/cluster_lighting.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_scene/froxel_grid.h>
#include <rx_shader/compiler.h>
#include <rx_task/scheduler.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace rx::cluster::test {

struct ClusterGpuFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    std::unique_ptr<rx::task::Scheduler> scheduler;
    std::unique_ptr<rx::graph::Executor> executor;
};

// A namespaced ComputePipelineCache path under the system temp directory --
// mirrors rx_ibl::bakeEnvironment()'s own `cacheNamespace` convention
// (bake.h's own header comment) exactly, and fixes the SAME plain-
// relative-filename-leaks-into-CWD anti-pattern this codebase already
// found and closed once for rx_rhi_vk's own compute-pipeline tests (P5 T6
// round micro-item: "T2's compute-pipeline tests leak
// rx_compute_pipeline_test*.cache into CWD -> redirect to temp dir").
inline std::filesystem::path pipelineCachePath(const std::string& name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "rx_cluster";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / (name + ".pipeline_cache");
}

inline std::optional<ClusterGpuFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping rx_cluster GPU test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping rx_cluster GPU test");
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
    auto executor = rx::graph::Executor::create(*device, *scheduler);
    REQUIRE(executor != nullptr);
    return ClusterGpuFixture{std::move(*window),    std::move(*context),  std::move(*device),
                              std::move(*allocator), std::move(scheduler), std::move(executor)};
}

struct OffscreenImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

inline constexpr uint32_t kBbExtent = 4;
inline constexpr VkFormat kBbFormat = VK_FORMAT_R8G8B8A8_UNORM;

inline std::optional<OffscreenImage> createOffscreenImage(VkDevice device, VkPhysicalDevice physicalDevice) {
    OffscreenImage result;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kBbFormat;
    imageInfo.extent = {kBbExtent, kBbExtent, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &result.image) != VK_SUCCESS) {
        return std::nullopt;
    }
    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, result.image, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == UINT32_MAX) {
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &result.memory) != VK_SUCCESS) {
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }
    if (vkBindImageMemory(device, result.image, result.memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, result.memory, nullptr);
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kBbFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &result.view) != VK_SUCCESS) {
        vkFreeMemory(device, result.memory, nullptr);
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }
    return result;
}

inline void destroyOffscreenImage(VkDevice device, OffscreenImage& img) {
    if (img.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, img.view, nullptr);
    }
    if (img.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, img.image, nullptr);
    }
    if (img.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, img.memory, nullptr);
    }
    img = OffscreenImage{};
}

inline rx::graph::AttachmentDesc bbDesc() {
    rx::graph::AttachmentDesc desc;
    desc.format = kBbFormat;
    desc.sizeClass = rx::graph::SizeClass::Absolute;
    desc.width = static_cast<float>(kBbExtent);
    desc.height = static_cast<float>(kBbExtent);
    return desc;
}

// Uploads `lights` into a fresh host-visible storage buffer -- the
// "externally-owned, already-populated" `lightsBuffer` ClusterPipelines::
// addClusterPasses() expects (this test harness's own stand-in for a
// production caller's rx::rhi::PerFrameStorageBuffer slot).
inline std::optional<rx::rhi::Buffer> uploadLights(rx::rhi::Allocator& allocator,
                                                     const std::vector<ClusterLightGpu>& lights) {
    const VkDeviceSize bytes =
        std::max<VkDeviceSize>(sizeof(ClusterLightGpu), static_cast<VkDeviceSize>(lights.size()) * sizeof(ClusterLightGpu));
    auto buffer = allocator.createHostVisibleBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!buffer.has_value()) {
        return std::nullopt;
    }
    if (!lights.empty()) {
        std::memcpy(buffer->mappedData(), lights.data(), lights.size() * sizeof(ClusterLightGpu));
    }
    buffer->flush();
    return buffer;
}

// Full readback of every buffer a `ClusterPipelines::addClusterPasses()`
// call produces.
struct ClusterRunResult {
    rx::scene::froxel::FroxelGridParams grid;
    std::vector<uint32_t> trueCounts;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> writeCounts;
    std::vector<uint32_t> perFroxelOverflow;
    std::vector<uint32_t> globalOverflow;
    uint32_t totalUsed = 0;
    // Sized `params.maxTotalLightIndices` -- only [0, totalUsed) is
    // meaningful (the prefix-sum's own contiguous, gap-free packing -- see
    // shaders/cluster/froxel_prefix_sum.slang's own header comment).
    std::vector<uint32_t> lightIndices;

    // Froxel `idx`'s own light-index list, as a real std::span-like slice
    // -- froxel `idx`'s [offset,offset+writeCount) range into lightIndices.
    [[nodiscard]] std::vector<uint32_t> froxelLights(uint32_t idx) const {
        const uint32_t off = offsets[idx];
        const uint32_t count = writeCounts[idx];
        return std::vector<uint32_t>(lightIndices.begin() + off, lightIndices.begin() + off + count);
    }
};

inline std::optional<ClusterRunResult> runCluster(ClusterGpuFixture& fixture, ClusterPipelines& pipelines,
                                                    VkBuffer lightsBuffer, uint32_t lightCount,
                                                    const rx::scene::Camera& camera, uint32_t viewportWidth,
                                                    uint32_t viewportHeight, const ClusterParams& params) {
    using rx::graph::CompileInfo;
    using rx::graph::PassContext;
    using rx::graph::QueueClass;
    using rx::graph::RenderGraph;

    VkDevice device = fixture.device.device();
    VkPhysicalDevice physicalDevice = fixture.device.physicalDevice();

    VkBuffer capturedTrueCounts = VK_NULL_HANDLE;
    VkBuffer capturedOffsets = VK_NULL_HANDLE;
    VkBuffer capturedWriteCounts = VK_NULL_HANDLE;
    VkBuffer capturedPerFroxelOverflow = VK_NULL_HANDLE;
    VkBuffer capturedGlobalOverflow = VK_NULL_HANDLE;
    VkBuffer capturedTotalUsed = VK_NULL_HANDLE;
    VkBuffer capturedLightIndices = VK_NULL_HANDLE;

    RenderGraph graph;
    // [Phase 5 Task 15, #51] `frameInputs` is a LOCAL of this function --
    // stays alive through this same function's own later graph-execution
    // call below (the ClusterPipelines::addClusterPasses() lambdas capture
    // it BY REFERENCE, per that method's own header comment), matching the
    // ONE-SHOT "declare, then immediately realize+execute+GPU-wait before
    // this local goes out of scope" usage every one of this fixture's own
    // callers already follows.
    const rx::cluster::ClusterFrameInputs frameInputs{lightsBuffer, lightCount};
    const rx::scene::froxel::FroxelGridParams grid =
        pipelines.addClusterPasses(graph, frameInputs, camera, viewportWidth, viewportHeight, params);

    graph.addPass("cluster_test_capture", QueueClass::AsyncCompute)
        .addStorageBufferInput("clusterTrueCounts")
        .addStorageBufferInput("clusterOffsets")
        .addStorageBufferInput("clusterWriteCounts")
        .addStorageBufferInput("clusterPerFroxelOverflow")
        .addStorageBufferInput("clusterGlobalOverflow")
        .addStorageBufferInput("clusterTotalUsed")
        .addStorageBufferInput("clusterLightIndices")
        .setSideEffect()
        .setExecute([&](PassContext& ctx) {
            capturedTrueCounts = ctx.buffer("clusterTrueCounts");
            capturedOffsets = ctx.buffer("clusterOffsets");
            capturedWriteCounts = ctx.buffer("clusterWriteCounts");
            capturedPerFroxelOverflow = ctx.buffer("clusterPerFroxelOverflow");
            capturedGlobalOverflow = ctx.buffer("clusterGlobalOverflow");
            capturedTotalUsed = ctx.buffer("clusterTotalUsed");
            capturedLightIndices = ctx.buffer("clusterLightIndices");
        });

    graph.addPass("present").addColorOutput("bb", bbDesc());
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kBbExtent;
    info.swapchainHeight = kBbExtent;
    info.swapchainFormat = kBbFormat;
    graph.compile(info);
    fixture.executor->realize(graph);

    auto offscreen = createOffscreenImage(device, physicalDevice);
    if (!offscreen.has_value()) {
        return std::nullopt;
    }
    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture.device.graphicsQueue(), fixture.device.graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        destroyOffscreenImage(device, *offscreen);
        return std::nullopt;
    }

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture.executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kBbExtent, kBbExtent});
    });

    const uint32_t froxelCount = grid.froxelCount();
    const VkDeviceSize perFroxelBytes = static_cast<VkDeviceSize>(froxelCount) * sizeof(uint32_t);
    const VkDeviceSize indicesBytes = static_cast<VkDeviceSize>(params.maxTotalLightIndices) * sizeof(uint32_t);

    auto trueCountsRB = fixture.allocator.createHostVisibleBuffer(perFroxelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto offsetsRB = fixture.allocator.createHostVisibleBuffer(perFroxelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto writeCountsRB = fixture.allocator.createHostVisibleBuffer(perFroxelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto perFroxelOverflowRB = fixture.allocator.createHostVisibleBuffer(perFroxelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto globalOverflowRB = fixture.allocator.createHostVisibleBuffer(perFroxelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto totalUsedRB = fixture.allocator.createHostVisibleBuffer(sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto lightIndicesRB = fixture.allocator.createHostVisibleBuffer(indicesBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!trueCountsRB.has_value() || !offsetsRB.has_value() || !writeCountsRB.has_value() ||
        !perFroxelOverflowRB.has_value() || !globalOverflowRB.has_value() || !totalUsedRB.has_value() ||
        !lightIndicesRB.has_value()) {
        destroyOffscreenImage(device, *offscreen);
        return std::nullopt;
    }

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        auto copy = [&](VkBuffer src, rx::rhi::Buffer& dst, VkDeviceSize bytes) {
            VkBufferCopy region{0, 0, bytes};
            vkCmdCopyBuffer(cmd, src, dst.handle(), 1, &region);
        };
        copy(capturedTrueCounts, *trueCountsRB, perFroxelBytes);
        copy(capturedOffsets, *offsetsRB, perFroxelBytes);
        copy(capturedWriteCounts, *writeCountsRB, perFroxelBytes);
        copy(capturedPerFroxelOverflow, *perFroxelOverflowRB, perFroxelBytes);
        copy(capturedGlobalOverflow, *globalOverflowRB, perFroxelBytes);
        copy(capturedTotalUsed, *totalUsedRB, sizeof(uint32_t));
        copy(capturedLightIndices, *lightIndicesRB, indicesBytes);
    });

    auto readAll = [](rx::rhi::Buffer& buf, uint32_t count) {
        buf.invalidate();
        std::vector<uint32_t> v(count);
        if (count > 0) {
            std::memcpy(v.data(), buf.mappedData(), static_cast<size_t>(count) * sizeof(uint32_t));
        }
        return v;
    };

    ClusterRunResult result;
    result.grid = grid;
    result.trueCounts = readAll(*trueCountsRB, froxelCount);
    result.offsets = readAll(*offsetsRB, froxelCount);
    result.writeCounts = readAll(*writeCountsRB, froxelCount);
    result.perFroxelOverflow = readAll(*perFroxelOverflowRB, froxelCount);
    result.globalOverflow = readAll(*globalOverflowRB, froxelCount);
    result.totalUsed = readAll(*totalUsedRB, 1)[0];
    result.lightIndices = readAll(*lightIndicesRB, params.maxTotalLightIndices);

    destroyOffscreenImage(device, *offscreen);
    return result;
}

// CPU ORACLE -- rx::scene::froxel's own (already device-free-unit-tested,
// froxel_grid_test.cpp) intersection predicates applied directly, ascending
// light-index order (matching both compute kernels' own iteration order),
// so the returned vector is directly comparable to ClusterRunResult::
// froxelLights() with plain vector equality (no sorting needed on either side).
inline std::vector<uint32_t> oracleFroxelLights(const rx::scene::froxel::FroxelGridParams& grid, uint32_t x,
                                                  uint32_t y, uint32_t z, const std::vector<ClusterLightGpu>& lights) {
    using namespace rx::scene::froxel;
    const FroxelBounds bounds = froxelViewSpaceBounds(x, y, z, grid);
    std::vector<uint32_t> hits;
    for (uint32_t i = 0; i < lights.size(); ++i) {
        const ClusterLightGpu& l = lights[i];
        const glm::vec3 pos(l.viewPositionRadius);
        const float radius = l.viewPositionRadius.w;
        bool hit = false;
        if (l.cosSquaredFlags.y > 0.5F) {
            SpotConeViewSpace cone;
            cone.axis = glm::vec3(l.viewAxisSinInverse);
            cone.sinInverse = l.viewAxisSinInverse.w;
            cone.cosSquared = l.cosSquaredFlags.x;
            hit = spotIntersectsFroxel(pos, radius, cone, bounds);
        } else {
            hit = sphereIntersectsFroxel(pos, radius, bounds);
        }
        if (hit) {
            hits.push_back(i);
        }
    }
    return hits;
}

}  // namespace rx::cluster::test
