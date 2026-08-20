// [Task 2 (#38), gate rulings: per-ticket ruling + RC2] The ticket's own
// primary gate: a real compute PSO (rx::rhi::ComputePipelineCache, built
// from a Slang compute shader compiled + reflected through the existing
// rx_shader path), dispatched through the render graph's own Compute-class
// pass machinery, writing BOTH a storage buffer and a storage image;
// bindless set 0 accessible from compute (a live read, not just a
// layout-compatibility check); a downstream pass reading both via the
// render graph's own StorageBufferInput/StorageImageInput declarations
// (graph-derived barriers, never a hand-written vkCmdPipelineBarrier2 in
// this file); exact VALUE assertions via host readback, not "it didn't
// crash". A second TEST_CASE proves RC2's per-mip/per-layer subresource
// primitives (cube storage image, two disjoint per-face writes/reads) end
// to end on real hardware, extending test_barriers.cpp's own device-free
// proof of the same independence property.
#include <doctest/doctest.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>

#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <rx_task/scheduler.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace rx::graph;

namespace {

// Same skip-guarded windowed-device fixture pattern as test_execute_gpu.cpp
// (rx::rhi::Device::create() needs a real VkSurfaceKHR even though nothing
// here ever presents to it) -- an independent copy rather than a shared
// header, matching this codebase's own established per-file-helper
// precedent (see e.g. barriers.cpp's own comment on why its small local
// copies of shared logic are deliberate, not accidental duplication).
struct ComputeGpuFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    std::unique_ptr<rx::task::Scheduler> scheduler;
    std::unique_ptr<Executor> executor;
};

std::optional<ComputeGpuFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping rx_graph compute GPU test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping rx_graph compute "
                 "GPU test");
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

    auto executor = Executor::create(*device, *scheduler);
    REQUIRE(executor != nullptr);

    return ComputeGpuFixture{std::move(*window),    std::move(*context),  std::move(*device),
                              std::move(*allocator), std::move(scheduler), std::move(executor)};
}

// --- Offscreen "backbuffer" -- identical shape/rationale to
// test_execute_gpu.cpp's own OffscreenImage: every graph here needs a real
// backbuffer to satisfy RenderGraph::compile()'s own requirement, even
// though neither TEST_CASE below cares about its contents. -----------------
struct OffscreenImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

constexpr uint32_t kBbExtent = 4;
constexpr VkFormat kBbFormat = VK_FORMAT_R8G8B8A8_UNORM;

std::optional<OffscreenImage> createOffscreenImage(VkDevice device, VkPhysicalDevice physicalDevice) {
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

void destroyOffscreenImage(VkDevice device, OffscreenImage& img) {
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

AttachmentDesc bbDesc() {
    AttachmentDesc desc;
    desc.format = kBbFormat;
    desc.sizeClass = SizeClass::Absolute;
    desc.width = static_cast<float>(kBbExtent);
    desc.height = static_cast<float>(kBbExtent);
    return desc;
}

// A small dedicated descriptor pool/set for a compute shader's own set-1
// bindings (a storage buffer + a storage image) -- deliberately NOT routed
// through rx::rhi::DescriptorArena (sized/shaped for MaterialSystem's own
// one-UBO-per-instance pattern, buffer.h) or BindlessTable (set 0's own,
// separate global table): set 1 here is a small, ordinary, NON-bindless
// descriptor set, exactly the shape a real compute consumer with its own
// kernel-specific bindings would build.
struct Set1Pool {
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

std::optional<Set1Pool> allocateSet1(VkDevice device, VkDescriptorSetLayout layout) {
    std::array<VkDescriptorPoolSize, 2> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();

    Set1Pool result;
    result.device = device;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &result.pool) != VK_SUCCESS) {
        return std::nullopt;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = result.pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;
    if (vkAllocateDescriptorSets(device, &allocInfo, &result.set) != VK_SUCCESS) {
        vkDestroyDescriptorPool(device, result.pool, nullptr);
        return std::nullopt;
    }
    return result;
}

void destroySet1(Set1Pool& p) {
    if (p.pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(p.device, p.pool, nullptr);
    }
    p = Set1Pool{};
}

std::optional<rx::shader::CompileResult> compileCompute(const char* moduleName, const char* source) {
    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        return std::nullopt;
    }
    rx::shader::CompileResult result = compiler->compileFromSource(moduleName, source, {"csMain"});
    if (!result.ok) {
        MESSAGE("compute shader compile failed: ", result.diagnostics);
        return std::nullopt;
    }
    return result;
}

}  // namespace

// ===========================================================================
// TEST_CASE 1 -- the ticket's own primary gate.
// ===========================================================================
TEST_CASE("Executor dispatches a real compute pass writing a storage buffer AND a storage image, bindless set 0 "
          "read live, a downstream pass reads both, exact values readback") {
    auto fixture = makeFixture("rx_graph_compute_primary");
    if (!fixture.has_value()) {
        return;
    }
    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    // --- Bindless table + the ONE resource this test registers into it: a
    // storage buffer holding a single known "multiplier" value, read live
    // by the compute shader through set 0 -- the ticket's own "RT-
    // compatibility rationale" proof, not just a layout-compatibility
    // check. -------------------------------------------------------------
    auto bindlessTable = rx::rhi::BindlessTable::create(physicalDevice, device, rx::rhi::BindlessTable::Capacities{4, 2, 4});
    REQUIRE(bindlessTable.has_value());

    constexpr uint32_t kMultiplier = 100;
    auto multiplierBuffer = fixture->allocator.createHostVisibleBuffer(sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(multiplierBuffer.has_value());
    std::memcpy(multiplierBuffer->mappedData(), &kMultiplier, sizeof(kMultiplier));
    multiplierBuffer->flush();
    auto multiplierHandle = bindlessTable->registerStorageBuffer(multiplierBuffer->handle(), sizeof(uint32_t));
    REQUIRE(multiplierHandle.isValid());

    // --- The compute shader: set 0 = bindless (external), set 1 = this
    // pass's own gOutBuffer/gOutImage. 8x8 dispatch (one thread per texel/
    // buffer element), deterministic pattern folding in the bindless-read
    // multiplier so a wrong/never-executed bindless read is directly
    // observable in the readback values, not just "didn't crash". -------
    constexpr const char* kSource = R"(
struct PushConstants {
    uint multiplierBufferIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 0)]]
Texture2D gBindlessTextures[];

[[vk::binding(1, 0)]]
SamplerState gBindlessSamplers[];

[[vk::binding(2, 0)]]
RWStructuredBuffer<uint> gBindlessBuffers[];

[[vk::binding(0, 1)]]
RWStructuredBuffer<uint> gOutBuffer;

[[vk::binding(1, 1)]]
RWTexture2D<uint4> gOutImage;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    uint multiplier = gBindlessBuffers[gPush.multiplierBufferIndex][0];
    uint idx = id.y * 8 + id.x;
    gOutBuffer[idx] = idx * 7u + 3u + multiplier;
    gOutImage[id.xy] = uint4(id.x, id.y, multiplier, 1u);
}
)";
    auto compileResult = compileCompute("RxComputeGpuPrimary", kSource);
    REQUIRE(compileResult.has_value());
    REQUIRE(compileResult->entryPointCode.size() == 1);
    CHECK(compileResult->entryPointCode[0].stage == VK_SHADER_STAGE_COMPUTE_BIT);

    auto layoutInfo = rx::shader::reflect(*compileResult);
    REQUIRE(layoutInfo.has_value());
    REQUIRE(layoutInfo->pushRanges.size() == 1);

    auto pipelineCache = rx::rhi::ComputePipelineCache::create(device, "rx_compute_gpu_primary.cache");
    REQUIRE(pipelineCache.has_value());
    auto pso = pipelineCache->getOrCreate(compileResult->entryPointCode[0].code, *layoutInfo,
                                            bindlessTable->descriptorSetLayout());
    REQUIRE(pso.has_value());
    REQUIRE(pso->setLayouts.size() >= 2);
    CHECK(pso->setLayouts[0] == bindlessTable->descriptorSetLayout());

    auto set1 = allocateSet1(device, pso->setLayouts[1]);
    REQUIRE(set1.has_value());

    // --- The graph itself. "produce" is Compute-class (no attachment
    // output at all) -- the render-graph pass classification this ticket's
    // "attachment-free PassSignature accepted end-to-end" criterion is
    // about: it never touches MaterialSystem::getPipeline() at all (which
    // would reject it), routing through rx::rhi::ComputePipelineCache
    // instead. -------------------------------------------------------------
    constexpr uint32_t kImgExtent = 8;
    BufferDesc outBufDesc;
    outBufDesc.size = 8 * 8 * sizeof(uint32_t);
    outBufDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    ImageDesc outImgDesc;
    outImgDesc.format = VK_FORMAT_R32G32B32A32_UINT;
    outImgDesc.sizeClass = SizeClass::Absolute;
    outImgDesc.width = static_cast<float>(kImgExtent);
    outImgDesc.height = static_cast<float>(kImgExtent);

    VkBuffer capturedOutBuffer = VK_NULL_HANDLE;
    VkImage capturedOutImage = VK_NULL_HANDLE;

    RenderGraph graph;
    graph.addPass("produce", QueueClass::AsyncCompute)
        .addStorageBufferOutput("outBuffer", outBufDesc)
        .addStorageImageOutput("outImage", outImgDesc)
        .setSideEffect()
        .setExecute([&](PassContext& ctx) {
            VkDescriptorBufferInfo bufInfo{ctx.buffer("outBuffer"), 0, VK_WHOLE_SIZE};
            VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, ctx.storageImageView("outImage"), VK_IMAGE_LAYOUT_GENERAL};

            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = set1->set;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[0].pBufferInfo = &bufInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = set1->set;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo = &imgInfo;
            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso->pipeline);
            std::array<VkDescriptorSet, 2> sets{bindlessTable->descriptorSet(), set1->set};
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso->layout, 0,
                                     static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);

            struct {
                uint32_t multiplierBufferIndex;
            } push{multiplierHandle.index()};
            vkCmdPushConstants(ctx.cmd, pso->layout, layoutInfo->pushRanges[0].stages,
                                layoutInfo->pushRanges[0].offset, layoutInfo->pushRanges[0].size, &push);

            vkCmdDispatch(ctx.cmd, 1, 1, 1);

            capturedOutBuffer = ctx.buffer("outBuffer");
            capturedOutImage = ctx.image("outImage");
        });
    // "consume": a REAL downstream pass declaring reads of both resources
    // -- this is what makes the render graph derive the produce->consume
    // barrier (COMPUTE_SHADER/SHADER_STORAGE_READ, VK_IMAGE_LAYOUT_GENERAL
    // for the image) this test's own zero-validation-errors assertion
    // below is proof of. No side-effect-free reader of ITS OWN outputs
    // exists (it produces none), so it needs setSideEffect() to survive
    // culling.
    graph.addPass("consume", QueueClass::AsyncCompute)
        .addStorageBufferInput("outBuffer")
        .addStorageImageInput("outImage")
        .setSideEffect();
    graph.addPass("present").addColorOutput("bb", bbDesc());
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kBbExtent;
    info.swapchainHeight = kBbExtent;
    info.swapchainFormat = kBbFormat;
    // Deliberately left at its default (VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) --
    // neither TEST_CASE in this file ever reads "bb" back (it exists purely
    // to satisfy RenderGraph::compile()'s own backbuffer requirement), so
    // there is no reason to ask finalBarriers() for TRANSFER_SRC_OPTIMAL,
    // which createOffscreenImage() above's own VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    // -only usage does not support.
    graph.compile(info);

    fixture->executor->realize(graph);

    auto offscreen = createOffscreenImage(device, physicalDevice);
    REQUIRE(offscreen.has_value());

    auto cmdCtx = rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kBbExtent, kBbExtent});
    });

    REQUIRE(capturedOutBuffer != VK_NULL_HANDLE);
    REQUIRE(capturedOutImage != VK_NULL_HANDLE);

    // --- Host readback -- no AVAILABILITY barrier needed here (see this
    // file's own top comment / the task report for the reasoning): the
    // PRECEDING runOnce() call already vkQueueWaitIdle()'d, so every write
    // the graph performed is already fully visible to this SEPARATE, LATER
    // submission. A LAYOUT transition is still needed: the storage image's
    // real current layout is VK_IMAGE_LAYOUT_GENERAL (StorageImageInput's
    // own resolved layout, "consume"'s read never changes it further) --
    // legal but sub-optimal as a vkCmdCopyImageToBuffer source, so this
    // test transitions it to TRANSFER_SRC_OPTIMAL first (StorageImage
    // unconditionally carries VK_IMAGE_USAGE_TRANSFER_SRC_BIT -- see that
    // class's own header comment), matching what a real caller wanting to
    // copy a compute-written image out would do. Hand-written here (not
    // graph-derived) deliberately: this copy is TEST-ONLY verification
    // work outside the render graph's own tracked resource lifecycle, the
    // same category the offscreen "bb" readback in test_execute_gpu.cpp's
    // own tests already is. ------------------------------------------------
    const VkDeviceSize bufBytes = 8 * 8 * sizeof(uint32_t);
    auto bufReadback = fixture->allocator.createHostVisibleBuffer(bufBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(bufReadback.has_value());
    const VkDeviceSize imgBytes = static_cast<VkDeviceSize>(kImgExtent) * kImgExtent * 4 * sizeof(uint32_t);
    auto imgReadback = fixture->allocator.createHostVisibleBuffer(imgBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(imgReadback.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferCopy bufRegion{0, 0, bufBytes};
        vkCmdCopyBuffer(cmd, capturedOutBuffer, bufReadback->handle(), 1, &bufRegion);

        VkImageMemoryBarrier2 toTransferSrc{};
        toTransferSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toTransferSrc.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toTransferSrc.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        toTransferSrc.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toTransferSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransferSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransferSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferSrc.image = capturedOutImage;
        toTransferSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toTransferSrc;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy imgRegion{};
        imgRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imgRegion.imageSubresource.layerCount = 1;
        imgRegion.imageExtent = {kImgExtent, kImgExtent, 1};
        vkCmdCopyImageToBuffer(cmd, capturedOutImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgReadback->handle(), 1,
                                &imgRegion);
    });
    bufReadback->invalidate();
    imgReadback->invalidate();

    std::vector<uint32_t> bufValues(64);
    std::memcpy(bufValues.data(), bufReadback->mappedData(), bufBytes);
    std::vector<uint32_t> imgValues(static_cast<size_t>(kImgExtent) * kImgExtent * 4);
    std::memcpy(imgValues.data(), imgReadback->mappedData(), imgBytes);

    bool allBufferValuesCorrect = true;
    for (uint32_t idx = 0; idx < 64; ++idx) {
        if (bufValues[idx] != idx * 7u + 3u + kMultiplier) {
            allBufferValuesCorrect = false;
            break;
        }
    }
    CHECK(allBufferValuesCorrect);

    bool allImageValuesCorrect = true;
    for (uint32_t y = 0; y < kImgExtent && allImageValuesCorrect; ++y) {
        for (uint32_t x = 0; x < kImgExtent; ++x) {
            const size_t texel = (static_cast<size_t>(y) * kImgExtent + x) * 4;
            if (imgValues[texel + 0] != x || imgValues[texel + 1] != y || imgValues[texel + 2] != kMultiplier ||
                imgValues[texel + 3] != 1u) {
                allImageValuesCorrect = false;
                break;
            }
        }
    }
    CHECK(allImageValuesCorrect);

    vkDeviceWaitIdle(device);
    bindlessTable->release(multiplierHandle);
    destroySet1(*set1);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===========================================================================
// TEST_CASE 2 -- RC2's per-mip/per-layer subresource primitives, end to
// end: a cube-compatible storage image, two SEPARATE compute passes each
// writing exactly one face (a disjoint Subresource, per-subresource
// barrier-tracked independently -- test_barriers.cpp's own device-free
// proof of this, extended here to real dispatches/real readback), a
// downstream pass per face reading it back. Proves cube/array views +
// per-layer subresource addressing are genuinely usable by a real compute
// consumer, not just declarable.
// ===========================================================================
TEST_CASE("Executor dispatches two compute passes writing DISJOINT faces of a cube storage image; each face reads "
          "back its own independent values") {
    auto fixture = makeFixture("rx_graph_compute_cube_faces");
    if (!fixture.has_value()) {
        return;
    }
    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    constexpr const char* kSource = R"(
struct PushConstants {
    uint faceIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 1)]]
RWTexture2D<uint4> gOutImage;

[shader("compute")]
[numthreads(4, 4, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    gOutImage[id.xy] = uint4(id.x, id.y, gPush.faceIndex, 777u);
}
)";
    auto compileResult = compileCompute("RxComputeGpuCubeFaces", kSource);
    REQUIRE(compileResult.has_value());
    auto layoutInfo = rx::shader::reflect(*compileResult);
    REQUIRE(layoutInfo.has_value());
    REQUIRE(layoutInfo->pushRanges.size() == 1);

    auto pipelineCache = rx::rhi::ComputePipelineCache::create(device, "rx_compute_gpu_cube_faces.cache");
    REQUIRE(pipelineCache.has_value());
    // No externalSet0 -- this shader declares no set-0 bindings at all
    // (test 1 already proves the bindless-set-0 substitution path).
    auto pso = pipelineCache->getOrCreate(compileResult->entryPointCode[0].code, *layoutInfo);
    REQUIRE(pso.has_value());
    REQUIRE(pso->setLayouts.size() >= 2);

    // Two independent descriptor sets (one per face) from one small pool.
    VkDescriptorPoolSize imageSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &imageSize;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool) == VK_SUCCESS);

    std::array<VkDescriptorSetLayout, 2> setLayouts{pso->setLayouts[1], pso->setLayouts[1]};
    std::array<VkDescriptorSet, 2> faceSets{};
    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = descPool;
    setAllocInfo.descriptorSetCount = 2;
    setAllocInfo.pSetLayouts = setLayouts.data();
    REQUIRE(vkAllocateDescriptorSets(device, &setAllocInfo, faceSets.data()) == VK_SUCCESS);

    constexpr uint32_t kFaceExtent = 4;
    ImageDesc cubeDesc;
    cubeDesc.format = VK_FORMAT_R32G32B32A32_UINT;
    cubeDesc.sizeClass = SizeClass::Absolute;
    cubeDesc.width = static_cast<float>(kFaceExtent);
    cubeDesc.height = static_cast<float>(kFaceExtent);
    cubeDesc.arrayLayers = 6;
    cubeDesc.cube = true;

    constexpr uint32_t kFace2 = 2;
    constexpr uint32_t kFace5 = 5;
    const Subresource face2Sub{0, 1, kFace2, 1};
    const Subresource face5Sub{0, 1, kFace5, 1};

    VkImage capturedCubeImage = VK_NULL_HANDLE;

    auto writeFacePass = [&](std::string_view passName, uint32_t face, const Subresource& sub, VkDescriptorSet set) {
        return [&, passName, face, sub, set](PassContext& ctx) {
            VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, ctx.storageImageView("cube"), VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            // [[vk::binding(0, 1)]] in kSource above -- this shader's set 1
            // has exactly ONE resource (gOutImage) at binding 0, unlike
            // test 1's two-resource set 1 (gOutBuffer at 0, gOutImage at 1).
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.pImageInfo = &imgInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso->pipeline);
            // set 0 is the auto-built EMPTY placeholder (no bindless use in
            // this shader) -- still must be bound at index 0 for set 1 to
            // land at the right index.
            std::array<VkDescriptorSet, 1> sets{set};
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pso->layout, 1, 1, sets.data(), 0,
                                     nullptr);
            struct {
                uint32_t faceIndex;
            } push{face};
            vkCmdPushConstants(ctx.cmd, pso->layout, layoutInfo->pushRanges[0].stages,
                                layoutInfo->pushRanges[0].offset, layoutInfo->pushRanges[0].size, &push);
            vkCmdDispatch(ctx.cmd, 1, 1, 1);
            capturedCubeImage = ctx.image("cube");
        };
    };

    RenderGraph graph;
    graph.addPass("write_face2", QueueClass::AsyncCompute)
        .addStorageImageOutput("cube", cubeDesc, face2Sub)
        .setSideEffect()
        .setExecute(writeFacePass("write_face2", kFace2, face2Sub, faceSets[0]));
    graph.addPass("write_face5", QueueClass::AsyncCompute)
        .addStorageImageOutput("cube", cubeDesc, face5Sub)
        .setSideEffect()
        .setExecute(writeFacePass("write_face5", kFace5, face5Sub, faceSets[1]));
    graph.addPass("read_face2", QueueClass::AsyncCompute).addStorageImageInput("cube", face2Sub).setSideEffect();
    graph.addPass("read_face5", QueueClass::AsyncCompute).addStorageImageInput("cube", face5Sub).setSideEffect();
    graph.addPass("present").addColorOutput("bb", bbDesc());
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kBbExtent;
    info.swapchainHeight = kBbExtent;
    info.swapchainFormat = kBbFormat;
    // Deliberately left at its default (VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) --
    // neither TEST_CASE in this file ever reads "bb" back (it exists purely
    // to satisfy RenderGraph::compile()'s own backbuffer requirement), so
    // there is no reason to ask finalBarriers() for TRANSFER_SRC_OPTIMAL,
    // which createOffscreenImage() above's own VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    // -only usage does not support.
    graph.compile(info);

    fixture->executor->realize(graph);

    auto offscreen = createOffscreenImage(device, physicalDevice);
    REQUIRE(offscreen.has_value());
    auto cmdCtx = rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kBbExtent, kBbExtent});
    });

    REQUIRE(capturedCubeImage != VK_NULL_HANDLE);

    const VkDeviceSize faceBytes = static_cast<VkDeviceSize>(kFaceExtent) * kFaceExtent * 4 * sizeof(uint32_t);
    auto face2Readback = fixture->allocator.createHostVisibleBuffer(faceBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(face2Readback.has_value());
    auto face5Readback = fixture->allocator.createHostVisibleBuffer(faceBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(face5Readback.has_value());

    // Same GENERAL -> TRANSFER_SRC_OPTIMAL transition as test 1's own
    // readback, applied per-face -- each barrier's own subresourceRange is
    // scoped to EXACTLY the one face it covers (never the whole 6-layer
    // image): the other 4 faces were never written by this graph and are
    // still in their creation-time VK_IMAGE_LAYOUT_UNDEFINED, so a single
    // barrier spanning all 6 layers claiming oldLayout=GENERAL would be
    // FALSE for those -- real core validation tracks per-subresource
    // layout state and would flag exactly that mismatch.
    auto toTransferSrc = [](VkImage image, uint32_t layer) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
        return barrier;
    };

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        std::array<VkImageMemoryBarrier2, 2> barriers{toTransferSrc(capturedCubeImage, kFace2),
                                                        toTransferSrc(capturedCubeImage, kFace5)};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region2{};
        region2.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region2.imageSubresource.baseArrayLayer = kFace2;
        region2.imageSubresource.layerCount = 1;
        region2.imageExtent = {kFaceExtent, kFaceExtent, 1};
        vkCmdCopyImageToBuffer(cmd, capturedCubeImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, face2Readback->handle(),
                                1, &region2);

        VkBufferImageCopy region5 = region2;
        region5.imageSubresource.baseArrayLayer = kFace5;
        vkCmdCopyImageToBuffer(cmd, capturedCubeImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, face5Readback->handle(),
                                1, &region5);
    });
    face2Readback->invalidate();
    face5Readback->invalidate();

    std::vector<uint32_t> face2Values(static_cast<size_t>(kFaceExtent) * kFaceExtent * 4);
    std::memcpy(face2Values.data(), face2Readback->mappedData(), faceBytes);
    std::vector<uint32_t> face5Values(static_cast<size_t>(kFaceExtent) * kFaceExtent * 4);
    std::memcpy(face5Values.data(), face5Readback->mappedData(), faceBytes);

    auto checkFace = [&](const std::vector<uint32_t>& values, uint32_t expectedFace) {
        for (uint32_t y = 0; y < kFaceExtent; ++y) {
            for (uint32_t x = 0; x < kFaceExtent; ++x) {
                const size_t texel = (static_cast<size_t>(y) * kFaceExtent + x) * 4;
                CHECK(values[texel + 0] == x);
                CHECK(values[texel + 1] == y);
                CHECK(values[texel + 2] == expectedFace);
                CHECK(values[texel + 3] == 777u);
            }
        }
    };
    checkFace(face2Values, kFace2);
    checkFace(face5Values, kFace5);

    vkDeviceWaitIdle(device);
    vkDestroyDescriptorPool(device, descPool, nullptr);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
