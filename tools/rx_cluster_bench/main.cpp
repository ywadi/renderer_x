// tools/rx_cluster_bench/main.cpp -- Phase 5 Stage 2 Task 14 [#50]: a
// small, standalone (NOT ctest-registered -- same rationale as
// tools/rx_ibl_bench/main.cpp's own header comment: a stress-scale run is
// too slow/noisy to gate every CI push on; T36/#72 owns the permanent CI
// perf-regression GATE mechanism) benchmark that runs the count/prefix-
// sum/scatter clustered light-assignment chain at synthetic HIGH-LIGHT-
// COUNT scale and prints real, GPU-inclusive wall-clock timings -- this
// project's own standing performance-first mandate ("the engine builds
// its own stress cases... proactively", CLAUDE.md): no sample in this
// repo has anywhere near enough lights to stress clustered assignment on
// its own, so this tool builds a synthetic scene that does.
//
// Content is synthetic point lights scattered inside (and a little
// outside, to also exercise the culled/rejected path) the benchmark
// camera's own view frustum -- every light's own cost is a function of
// its VIEW-SPACE position/radius alone (the compute kernels perform the
// identical fixed ALU/test sequence per light regardless of where it
// sits), so a synthetic distribution gives representative timing with
// zero test-asset provenance/licensing overhead.
#include <rx_cluster/cluster_lighting.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_scene/camera.h>
#include <rx_shader/compiler.h>
#include <rx_task/scheduler.h>

#include <volk.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <system_error>
#include <vector>

using namespace rx::cluster;

namespace {

constexpr uint32_t kBbExtent = 4;
constexpr VkFormat kBbFormat = VK_FORMAT_R8G8B8A8_UNORM;

struct OffscreenImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

bool createOffscreenImage(VkDevice device, VkPhysicalDevice physicalDevice, OffscreenImage& out) {
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
    if (vkCreateImage(device, &imageInfo, nullptr, &out.image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, out.image, &memReq);
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
        return false;
    }
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &out.memory) != VK_SUCCESS) {
        return false;
    }
    if (vkBindImageMemory(device, out.image, out.memory, 0) != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kBbFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(device, &viewInfo, nullptr, &out.view) == VK_SUCCESS;
}

rx::graph::AttachmentDesc bbDesc() {
    rx::graph::AttachmentDesc desc;
    desc.format = kBbFormat;
    desc.sizeClass = rx::graph::SizeClass::Absolute;
    desc.width = static_cast<float>(kBbExtent);
    desc.height = static_cast<float>(kBbExtent);
    return desc;
}

std::vector<ClusterLightGpu> buildSyntheticLights(uint32_t count, uint32_t seed) {
    std::mt19937 rng(seed);
    // Positions spread through roughly [-40,40]x[-25,25] laterally and
    // [5,90] in depth (matching the bench camera's own zLightNear/Far
    // below) -- a mix of small and larger-radius lights so both the
    // "handful of froxels" and "spans many froxels" cases are represented
    // at scale, matching real game-lighting distributions more closely
    // than a single fixed radius would.
    std::uniform_real_distribution<float> distX(-40.0F, 40.0F);
    std::uniform_real_distribution<float> distY(-25.0F, 25.0F);
    std::uniform_real_distribution<float> distZ(-90.0F, -5.0F);
    std::uniform_real_distribution<float> distRadius(1.0F, 8.0F);

    std::vector<ClusterLightGpu> lights;
    lights.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ClusterLightGpu l;
        l.viewPositionRadius = glm::vec4(distX(rng), distY(rng), distZ(rng), distRadius(rng));
        l.viewAxisSinInverse = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);
        l.cosSquaredFlags = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);
        lights.push_back(l);
    }
    return lights;
}

}  // namespace

int main(int argc, char** argv) {
    const char* shaderDir = argc > 1 ? argv[1] : RX_CLUSTER_SHADER_DIR;

    auto window = rx::platform::Window::create("rx_cluster_bench", 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        std::fprintf(stderr, "rx_cluster_bench: no display backend available\n");
        return 1;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        std::fprintf(stderr, "rx_cluster_bench: no Vulkan surface extensions reported\n");
        return 1;
    }
    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/false);
    if (!context.has_value()) {
        std::fprintf(stderr, "rx_cluster_bench: Context::create failed\n");
        return 1;
    }
    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    auto device = rx::rhi::Device::create(*context, surface);
    if (!device.has_value()) {
        std::fprintf(stderr, "rx_cluster_bench: Device::create failed\n");
        return 1;
    }
    auto allocator = rx::rhi::Allocator::create(*context, *device);
    if (!allocator.has_value()) {
        std::fprintf(stderr, "rx_cluster_bench: Allocator::create failed\n");
        return 1;
    }
    auto scheduler = rx::task::Scheduler::create();
    auto executor = rx::graph::Executor::create(*device, *scheduler);
    if (executor == nullptr) {
        std::fprintf(stderr, "rx_cluster_bench: Executor::create failed\n");
        return 1;
    }
    auto cmdCtx = rx::rhi::CommandContext::create(device->device(), device->graphicsQueue(), device->graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        std::fprintf(stderr, "rx_cluster_bench: CommandContext::create failed\n");
        return 1;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device->physicalDevice(), &props);
    std::printf("rx_cluster_bench: device = %s\n", props.deviceName);

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        std::fprintf(stderr, "rx_cluster_bench: Compiler::create failed\n");
        return 1;
    }
    const std::filesystem::path cacheDir = std::filesystem::temp_directory_path() / "rx_cluster";
    std::error_code cacheDirEc;
    std::filesystem::create_directories(cacheDir, cacheDirEc);
    auto pipelineCache =
        rx::rhi::ComputePipelineCache::create(device->device(), cacheDir / "bench.pipeline_cache");
    if (!pipelineCache.has_value()) {
        std::fprintf(stderr, "rx_cluster_bench: ComputePipelineCache::create failed\n");
        return 1;
    }
    auto pipelines = ClusterPipelines::create(device->device(), *pipelineCache, *compiler, shaderDir);
    if (!pipelines.has_value()) {
        std::fprintf(stderr, "rx_cluster_bench: ClusterPipelines::create failed\n");
        return 1;
    }

    rx::scene::Camera camera;  // default 60deg vfov, 16:9 aspect.
    constexpr uint32_t kViewportW = 1920;
    constexpr uint32_t kViewportH = 1080;

    ClusterParams params;
    params.zLightNear = 5.0F;
    params.zLightFar = 100.0F;
    params.maxLightsPerFroxel = 256;
    params.maxTotalLightIndices = 1U << 20;  // 1,048,576 -- generous headroom at 5k lights.

    // T15's own named content-scale target (plan:523, "100/1k/5k lights") --
    // reused here so this task's own stress proof lands at the SAME scale
    // the next task's own frame-pipeline integration will be judged
    // against.
    const std::vector<uint32_t> scales{100, 1000, 5000};

    for (uint32_t scale : scales) {
        auto lights = buildSyntheticLights(scale, /*seed=*/scale * 7919U + 1U);
        const VkDeviceSize lightsBytes = static_cast<VkDeviceSize>(lights.size()) * sizeof(ClusterLightGpu);
        auto lightsBuffer = allocator->createHostVisibleBuffer(lightsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!lightsBuffer.has_value()) {
            std::fprintf(stderr, "rx_cluster_bench: light buffer allocation failed at scale %u\n", scale);
            return 1;
        }
        std::memcpy(lightsBuffer->mappedData(), lights.data(), static_cast<size_t>(lightsBytes));
        lightsBuffer->flush();

        VkBuffer capturedTotalUsed = VK_NULL_HANDLE;
        rx::graph::RenderGraph graph;
        const rx::scene::froxel::FroxelGridParams grid = pipelines->addClusterPasses(
            graph, lightsBuffer->handle(), static_cast<uint32_t>(lights.size()), camera, kViewportW, kViewportH, params);
        graph.addPass("bench_capture", rx::graph::QueueClass::AsyncCompute)
            .addStorageBufferInput("clusterTotalUsed")
            .setSideEffect()
            .setExecute([&](rx::graph::PassContext& ctx) { capturedTotalUsed = ctx.buffer("clusterTotalUsed"); });
        graph.addPass("present").addColorOutput("bb", bbDesc());
        graph.setBackbufferSource("bb");

        rx::graph::CompileInfo info;
        info.swapchainWidth = kBbExtent;
        info.swapchainHeight = kBbExtent;
        info.swapchainFormat = kBbFormat;
        graph.compile(info);
        executor->realize(graph);

        OffscreenImage real{};
        if (!createOffscreenImage(device->device(), device->physicalDevice(), real)) {
            std::fprintf(stderr, "rx_cluster_bench: offscreen image creation failed\n");
            return 1;
        }

        // Warm-up dispatch (first-ever PSO use on some drivers pays a
        // one-time lazy-compile cost NOT representative of steady-state
        // per-frame cost) -- discarded, matching rx_ibl_bench's own
        // documented "publish honest, steady-state numbers" posture.
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            executor->execute(graph, cmd, real.image, real.view, VkExtent2D{kBbExtent, kBbExtent});
        });

        constexpr int kIterations = 20;
        double totalMs = 0.0;
        double minMs = 1e9;
        double maxMs = 0.0;
        for (int i = 0; i < kIterations; ++i) {
            const auto start = std::chrono::steady_clock::now();
            cmdCtx->runOnce([&](VkCommandBuffer cmd) {
                executor->execute(graph, cmd, real.image, real.view, VkExtent2D{kBbExtent, kBbExtent});
            });
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            totalMs += ms;
            minMs = std::min(minMs, ms);
            maxMs = std::max(maxMs, ms);
        }
        const double avgMs = totalMs / kIterations;

        // Read back the total assigned light-index count (a real
        // correctness/scale sanity figure alongside the timing).
        auto readback = allocator->createHostVisibleBuffer(sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        uint32_t totalUsed = 0;
        if (readback.has_value() && capturedTotalUsed != VK_NULL_HANDLE) {
            cmdCtx->runOnce([&](VkCommandBuffer cmd) {
                VkBufferCopy region{0, 0, sizeof(uint32_t)};
                vkCmdCopyBuffer(cmd, capturedTotalUsed, readback->handle(), 1, &region);
            });
            readback->invalidate();
            std::memcpy(&totalUsed, readback->mappedData(), sizeof(uint32_t));
        }

        std::printf(
            "rx_cluster_bench: scale=%u lights, grid=%ux%ux%u (%u froxels), totalAssignedIndices=%u -- "
            "avg=%.4f ms min=%.4f ms max=%.4f ms (n=%d, warm-up discarded)\n",
            scale, grid.countX, grid.countY, grid.countZ, grid.froxelCount(), totalUsed, avgMs, minMs, maxMs,
            kIterations);

        if (real.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device->device(), real.view, nullptr);
        }
        if (real.image != VK_NULL_HANDLE) {
            vkDestroyImage(device->device(), real.image, nullptr);
        }
        if (real.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device->device(), real.memory, nullptr);
        }
    }

    vkDeviceWaitIdle(device->device());
    return 0;
}
