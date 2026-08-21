#include <rx_ibl/bake.h>

#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_rhi_vk/device.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <rx_task/scheduler.h>

#include <volk.h>

#include <array>
#include <chrono>
#include <cstring>
#include <vector>

namespace rx::ibl {
namespace {

using rx::graph::AttachmentDesc;
using rx::graph::CompileInfo;
using rx::graph::ImageDesc;
using rx::graph::PassContext;
using rx::graph::QueueClass;
using rx::graph::RenderGraph;
using rx::graph::SizeClass;
using rx::graph::Subresource;

constexpr VkFormat kCubeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDfgFormat = VK_FORMAT_R16G16_SFLOAT;

// --- Offscreen "backbuffer" -- identical shape/rationale to
// rx_graph/tests/test_compute_gpu.cpp's own OffscreenImage: RenderGraph::
// compile() requires a real backbuffer even though this bake never
// presents anything. -----------------------------------------------------
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

VkSampler createSampler(VkDevice device, VkSamplerAddressMode addressU, VkSamplerAddressMode addressV) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = addressU;
    info.addressModeV = addressV;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.minLod = 0.0F;
    info.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return sampler;
}

// One compiled+reflected compute kernel plus its cached PSO and an
// empty (zero-binding) set-0 descriptor set -- every shader in this
// chain declares only [[vk::binding(N, 1)]] bindings (no bindless set-0
// use), so PipelineLayoutBuilder auto-builds a placeholder empty set-0
// layout per pipeline [rx_graph/tests/test_compute_gpu.cpp's own "set 0
// is the auto-built EMPTY placeholder" precedent] -- ALLOCATED PER
// PIPELINE (not shared across the 4 kernels), since a VkDescriptorSet is
// only valid against the EXACT VkDescriptorSetLayout it was allocated
// from, and each of the 4 kernels gets its own distinct (though
// structurally identical) empty layout.
struct Kernel {
    rx::rhi::ComputePipelineCache::Pipeline pso;
    // Kept alongside `pso` (not discarded after building it) so dispatch
    // sites use the REAL reflected push-constant range (stage/offset/
    // size) rather than assuming offset=0/stage=COMPUTE_BIT/size=sizeof(
    // push) -- every push-constant struct in this bake's own shaders is
    // an all-scalar-uint/float sequence (no padding either side would
    // predict), but this codebase's own established precedent
    // (rx_graph/tests/test_compute_gpu.cpp) always dispatches through the
    // reflected range rather than a caller-side assumption, and this
    // module follows it rather than re-deriving a (probably-but-not-
    // certainly-correct) shortcut.
    rx::shader::ShaderLayoutInfo layoutInfo;
    VkDescriptorSet emptySet0 = VK_NULL_HANDLE;
};

std::optional<Kernel> buildKernel(rx::shader::Compiler& compiler, rx::rhi::ComputePipelineCache& cache,
                                    VkDevice device, VkDescriptorPool pool, const std::filesystem::path& path) {
    auto compiled = compiler.compileFromFile(path.string(), {"csMain"});
    if (!compiled.ok || compiled.entryPointCode.empty()) {
        RX_LOG_ERROR("rx_ibl: failed to compile {}: {}", path.string(), compiled.diagnostics);
        return std::nullopt;
    }
    auto layoutInfo = rx::shader::reflect(compiled);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("rx_ibl: failed to reflect {}", path.string());
        return std::nullopt;
    }
    if (layoutInfo->pushRanges.empty()) {
        RX_LOG_ERROR("rx_ibl: {} reflected no push-constant range", path.string());
        return std::nullopt;
    }
    auto pso = cache.getOrCreate(compiled.entryPointCode[0].code, *layoutInfo);
    if (!pso.has_value()) {
        RX_LOG_ERROR("rx_ibl: failed to build pipeline for {}", path.string());
        return std::nullopt;
    }
    if (pso->setLayouts.empty()) {
        RX_LOG_ERROR("rx_ibl: {} produced no descriptor set layouts", path.string());
        return std::nullopt;
    }
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &pso->setLayouts[0];
    VkDescriptorSet emptySet0 = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &allocInfo, &emptySet0) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_ibl: failed to allocate empty set-0 for {}", path.string());
        return std::nullopt;
    }
    return Kernel{*pso, *layoutInfo, emptySet0};
}

// Allocates ONE fresh descriptor set (from `pool`) for exactly one pass
// invocation -- a fresh set per invocation is REQUIRED (not merely
// tidy): every pass in this bake's own graph is recorded into the SAME
// command buffer, then submitted ONCE (one-shot), so every
// vkUpdateDescriptorSets call across every pass happens on the host
// BEFORE the GPU ever begins executing any of it -- reusing one set
// across passes would leave every earlier pass's dispatch reading
// whichever pass's writes happened to update that set LAST. Same
// precedent as rx_graph/tests/test_compute_gpu.cpp's own two-face test
// (`faceSets[0]`/`faceSets[1]`, never reused).
VkDescriptorSet allocateSet1(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(device, &allocInfo, &set);
    return set;
}

void writeSampledPass(VkDevice device, VkDescriptorSet set, VkImageView sourceView, VkImageLayout sourceLayout,
                       VkSampler sampler, VkImageView outView) {
    VkDescriptorImageInfo srcInfo{sampler, sourceView, sourceLayout};
    VkDescriptorImageInfo samplerInfo{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, outView, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo = &srcInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[1].pImageInfo = &samplerInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &outInfo;
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void writeDfgPass(VkDevice device, VkDescriptorSet set, VkImageView outView) {
    VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, outView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &outInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void bindAndDispatch(VkCommandBuffer cmd, const Kernel& kernel, VkDescriptorSet set1, const void* pushData,
                      uint32_t pushSize, uint32_t groupsX, uint32_t groupsY) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel.pso.pipeline);
    std::array<VkDescriptorSet, 2> sets{kernel.emptySet0, set1};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel.pso.layout, 0,
                             static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    const auto& range = kernel.layoutInfo.pushRanges[0];
    if (range.size != pushSize) {
        RX_LOG_ERROR("rx_ibl: push-constant size mismatch: reflected {} vs caller struct {}", range.size, pushSize);
    }
    vkCmdPushConstants(cmd, kernel.pso.layout, range.stages, range.offset, range.size, pushData);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);
}

uint32_t groupCount(uint32_t extent, uint32_t groupSize) { return (extent + groupSize - 1) / groupSize; }

VkImageMemoryBarrier2 makeBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, uint32_t baseMip,
                                    uint32_t levelCount, uint32_t baseLayer, uint32_t layerCount) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, levelCount, baseLayer, layerCount};
    return barrier;
}

void submitBarriers(VkCommandBuffer cmd, std::vector<VkImageMemoryBarrier2>& barriers) {
    if (barriers.empty()) {
        return;
    }
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
    barriers.clear();
}

// Copies `srcImage` (already transitioned to TRANSFER_SRC_OPTIMAL by the
// caller) into `dstImage` (fresh, VK_IMAGE_LAYOUT_UNDEFINED) across every
// mip/layer, then leaves `dstImage` in SHADER_READ_ONLY_OPTIMAL --
// BakeResult's own Texture2D outputs are persistent, sampled-usage
// textures a later consumer (this task's own tests, Task 10) binds
// directly.
void copyToPersistent(VkCommandBuffer cmd, VkImage dstImage, VkImage srcImage, uint32_t mipCount, uint32_t width,
                       uint32_t height, uint32_t layerCount) {
    std::vector<VkImageMemoryBarrier2> pre;
    pre.push_back(makeBarrier(dstImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, mipCount, 0, layerCount));
    submitBarriers(cmd, pre);

    std::vector<VkImageCopy> regions;
    regions.reserve(mipCount);
    uint32_t mipW = width;
    uint32_t mipH = height;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layerCount};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layerCount};
        region.extent = {mipW, mipH, 1};
        regions.push_back(region);
        mipW = std::max(1U, mipW / 2);
        mipH = std::max(1U, mipH / 2);
    }
    vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   static_cast<uint32_t>(regions.size()), regions.data());

    std::vector<VkImageMemoryBarrier2> post;
    post.push_back(makeBarrier(dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, 0, mipCount, 0,
                                layerCount));
    submitBarriers(cmd, post);
}

double millisSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

std::optional<BakeResult> bakeEnvironment(rx::rhi::Device& device, rx::rhi::Allocator& allocator,
                                            rx::rhi::CommandContext& cmdCtx, rx::task::Scheduler& scheduler,
                                            const rx::rhi::Texture2D& source, bool sourceIsCube,
                                            const std::filesystem::path& shaderDir, const BakeParams& params,
                                            BakeTimings* outTimings) {
    RX_ZONE_NAMED("rx_ibl: bakeEnvironment");
    const auto totalStart = std::chrono::steady_clock::now();
    BakeTimings timings;

    VkDevice vkDevice = device.device();
    VkPhysicalDevice physicalDevice = device.physicalDevice();

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        RX_LOG_ERROR("rx_ibl: failed to create Slang compiler");
        return std::nullopt;
    }

    auto pipelineCache =
        rx::rhi::ComputePipelineCache::create(vkDevice, std::filesystem::temp_directory_path() / "rx_ibl_bake.cache");
    if (!pipelineCache.has_value()) {
        RX_LOG_ERROR("rx_ibl: failed to create ComputePipelineCache");
        return std::nullopt;
    }

    // One generously-sized pool for every descriptor set this bake ever
    // allocates (empty set-0s + one real set-1 per pass invocation) --
    // see allocateSet1()'s own comment for why each pass invocation needs
    // its own set, never a reused one.
    const uint32_t maxSets = 8 + 6 + 6 + 6 * params.prefilteredMipCount + 1;
    std::array<VkDescriptorPoolSize, 3> poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxSets},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, maxSets},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxSets},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = maxSets;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_ibl: failed to create descriptor pool");
        return std::nullopt;
    }

    auto equirectKernel = buildKernel(*compiler, *pipelineCache, vkDevice, pool, shaderDir / "equirect_to_cubemap.slang");
    auto irradianceKernel = buildKernel(*compiler, *pipelineCache, vkDevice, pool, shaderDir / "irradiance_convolve.slang");
    auto prefilterKernel = buildKernel(*compiler, *pipelineCache, vkDevice, pool, shaderDir / "prefilter_specular.slang");
    auto dfgKernel = buildKernel(*compiler, *pipelineCache, vkDevice, pool, shaderDir / "dfg_lut.slang");
    if (!irradianceKernel.has_value() || !prefilterKernel.has_value() || !dfgKernel.has_value() ||
        (!sourceIsCube && !equirectKernel.has_value())) {
        vkDestroyDescriptorPool(vkDevice, pool, nullptr);
        return std::nullopt;
    }

    VkSampler equirectSampler = createSampler(vkDevice, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    VkSampler cubeSampler =
        createSampler(vkDevice, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (equirectSampler == VK_NULL_HANDLE || cubeSampler == VK_NULL_HANDLE) {
        RX_LOG_ERROR("rx_ibl: failed to create samplers");
        vkDestroyDescriptorPool(vkDevice, pool, nullptr);
        return std::nullopt;
    }

    ImageDesc baseCubeDesc;
    baseCubeDesc.format = kCubeFormat;
    baseCubeDesc.sizeClass = SizeClass::Absolute;
    baseCubeDesc.width = static_cast<float>(params.baseCubemapFaceSize);
    baseCubeDesc.height = static_cast<float>(params.baseCubemapFaceSize);
    baseCubeDesc.arrayLayers = 6;
    baseCubeDesc.cube = true;

    ImageDesc irrCubeDesc;
    irrCubeDesc.format = kCubeFormat;
    irrCubeDesc.sizeClass = SizeClass::Absolute;
    irrCubeDesc.width = static_cast<float>(params.irradianceFaceSize);
    irrCubeDesc.height = static_cast<float>(params.irradianceFaceSize);
    irrCubeDesc.arrayLayers = 6;
    irrCubeDesc.cube = true;

    ImageDesc prefilterDesc;
    prefilterDesc.format = kCubeFormat;
    prefilterDesc.sizeClass = SizeClass::Absolute;
    prefilterDesc.width = static_cast<float>(params.prefilteredBaseFaceSize);
    prefilterDesc.height = static_cast<float>(params.prefilteredBaseFaceSize);
    prefilterDesc.arrayLayers = 6;
    prefilterDesc.cube = true;
    prefilterDesc.mipLevels = params.prefilteredMipCount;

    ImageDesc dfgDesc;
    dfgDesc.format = kDfgFormat;
    dfgDesc.sizeClass = SizeClass::Absolute;
    dfgDesc.width = static_cast<float>(params.dfgLutSize);
    dfgDesc.height = static_cast<float>(params.dfgLutSize);

    VkImage capturedBaseCubemap = VK_NULL_HANDLE;
    VkImage capturedIrradiance = VK_NULL_HANDLE;
    VkImage capturedPrefiltered = VK_NULL_HANDLE;
    VkImage capturedDfg = VK_NULL_HANDLE;

    auto executor = rx::graph::Executor::create(device, scheduler);
    if (executor == nullptr) {
        RX_LOG_ERROR("rx_ibl: failed to create Executor");
        vkDestroySampler(vkDevice, equirectSampler, nullptr);
        vkDestroySampler(vkDevice, cubeSampler, nullptr);
        vkDestroyDescriptorPool(vkDevice, pool, nullptr);
        return std::nullopt;
    }

    auto offscreen = createOffscreenImage(vkDevice, physicalDevice);
    if (!offscreen.has_value()) {
        RX_LOG_ERROR("rx_ibl: failed to create offscreen backbuffer");
        vkDestroySampler(vkDevice, equirectSampler, nullptr);
        vkDestroySampler(vkDevice, cubeSampler, nullptr);
        vkDestroyDescriptorPool(vkDevice, pool, nullptr);
        return std::nullopt;
    }

    CompileInfo info;
    info.swapchainWidth = kBbExtent;
    info.swapchainHeight = kBbExtent;
    info.swapchainFormat = kBbFormat;

    // [render_graph.cpp's own subresource validator: "identical or
    // fully disjoint ranges" ONLY -- a resource declared with disjoint
    // PER-FACE StorageImageOutput writes (baseCubemap's own 6 equirect
    // passes) can never ALSO be declared with a WHOLE-RESOURCE
    // addTextureInput read in the SAME graph (a whole-resource range
    // PARTIALLY overlaps each individual face's own range -- neither
    // identical nor disjoint, rejected at compile()). This is exactly
    // why this bake runs as FOUR SEPARATE single-purpose graphs (one
    // runOnce() submission each), not one -- "baseCubemap" is written
    // (per-face, disjoint) in ONE graph and consumed via a
    // DIRECTLY-CAPTURED VkImageView/VkImageLayout in the NEXT ones,
    // never re-declared as a graph resource once written. This also
    // gives genuinely separate, honest per-stage wall-clock timings
    // (each stage's own runOnce() blocks until GPU completion before
    // the next stage's std::chrono span starts) -- a real improvement
    // over a single-graph design, not merely a workaround.]
    VkImageView baseCubemapView = VK_NULL_HANDLE;
    VkImageLayout baseCubemapLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // NOTE on why the sourceIsCube path also runs a compute stage rather
    // than a raw vkCmdCopyImage from `source` directly: (1) a raw image
    // copy REQUIRES format compatibility (same texel size) between
    // `source` and this bake's own kCubeFormat (R16G16B16A16_SFLOAT) --
    // `source` is caller-owned and may be ANY sampled-float format (this
    // module's own test fixture uses R32G32B32A32_SFLOAT, a real
    // mismatch, verified directly this round -- VUID-vkCmdCopyImage-
    // srcImage-01548); (2) a raw copy also REQUIRES `source` to carry
    // VK_IMAGE_USAGE_TRANSFER_SRC_BIT, an extra precondition this bake
    // would otherwise silently impose on every caller's own texture
    // (Task 6's own createCubeForPresuppliedMips() factory does NOT add
    // that bit either, per that factory's own header comment -- a real
    // T6-loaded cubemap would hit the exact same failure in production,
    // not just in this module's own tests). A per-face COMPUTE
    // passthrough (prefilterKernel's own linearRoughness==0 special
    // case -- see prefilter_specular.slang's own header comment: a
    // literal `TextureCube.SampleLevel(dir,0)` resample, no importance
    // sampling) sidesteps both: it needs only VK_IMAGE_USAGE_SAMPLED_BIT
    // on `source` (already required for every other read this bake
    // performs of it) and is format-agnostic (the shader reads/writes
    // float4 regardless of either side's exact bit layout).
    struct BaseCubemapPush {
        uint32_t faceIndex;
        uint32_t faceDim;
        uint32_t numSamples;
        float linearRoughness;
    };
    {
        RenderGraph baseGraph;
        // `source.view()` returns whatever view type matches how the
        // CALLER actually built `source` (2D for the equirect path, Cube
        // for the sourceIsCube path) -- the branch below dispatches the
        // correspondingly-shaped kernel (equirectKernel expects
        // Texture2D, prefilterKernel expects TextureCube) either way, so
        // the SPIR-V-declared image Dim always matches the bound view.
        const VkImageView sourceView = source.view();
        const VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for (uint32_t face = 0; face < 6; ++face) {
            const Subresource sub{0, 1, face, 1};
            const std::string passName = "ibl_basecube_face" + std::to_string(face);
            // `source` is an EXTERNALLY-owned, already-populated,
            // already-SHADER_READ_ONLY_OPTIMAL resource this bake never
            // writes -- bound DIRECTLY (captured by reference), not
            // through a graph declaration: Task 1's Pass API has no
            // "externally supplied resource" input kind at all.
            baseGraph.addPass(passName, QueueClass::AsyncCompute)
                .addStorageImageOutput("baseCubemap", baseCubeDesc, sub)
                .setSideEffect()
                .setExecute([&, face](PassContext& ctx) {
                    if (sourceIsCube) {
                        VkDescriptorSet set = allocateSet1(vkDevice, pool, prefilterKernel->pso.setLayouts[1]);
                        writeSampledPass(vkDevice, set, sourceView, sourceLayout, cubeSampler,
                                          ctx.storageImageView("baseCubemap"));
                        BaseCubemapPush push{face, params.baseCubemapFaceSize, 0, 0.0F};
                        bindAndDispatch(ctx.cmd, *prefilterKernel, set, &push, sizeof(push),
                                         groupCount(params.baseCubemapFaceSize, 8),
                                         groupCount(params.baseCubemapFaceSize, 8));
                    } else {
                        VkDescriptorSet set = allocateSet1(vkDevice, pool, equirectKernel->pso.setLayouts[1]);
                        writeSampledPass(vkDevice, set, sourceView, sourceLayout, equirectSampler,
                                          ctx.storageImageView("baseCubemap"));
                        struct EquirectPush {
                            uint32_t faceIndex;
                            uint32_t faceDim;
                        };
                        EquirectPush push{face, params.baseCubemapFaceSize};
                        bindAndDispatch(ctx.cmd, *equirectKernel, set, &push, sizeof(push),
                                         groupCount(params.baseCubemapFaceSize, 8),
                                         groupCount(params.baseCubemapFaceSize, 8));
                    }
                });
        }
        // [render_graph.cpp's own subresource validator] "baseCubemap"'s
        // only declarations so far are 6 DISJOINT per-face
        // StorageImageOutput writes -- its underlying image is therefore
        // created with STORAGE_BIT|TRANSFER_SRC_BIT only (StorageImage::
        // create()'s own unconditional bits; PhysicalResource::imageUsage
        // otherwise unions SAMPLED_BIT ONLY from a TextureInput
        // declaration, per that field's own comment), which the LATER
        // irradiance/prefilter stages' TextureCube SAMPLED read of this
        // same image (via a directly-captured VkImageView, see this
        // function's own header comment on why cross-graph reads bypass
        // addTextureInput entirely) needs and does not otherwise get.
        // This dedicated capture-only pass adds exactly 6 addTextureInput
        // declarations, one per face, each IDENTICAL to its own writer
        // pass's own range (never the "whole resource" default, which
        // would PARTIALLY overlap every individual face range and be
        // rejected) -- satisfies "identical or disjoint" for every
        // declared-pair comparison across all 12 declarations, and its
        // mere presence is what causes SAMPLED_BIT to be unioned into
        // the image's real creation usage (usage is an IMAGE-level
        // property; once unioned in, it also legalizes the WHOLE-cube
        // fullView() captured inside the write loop above, not just this
        // pass's own narrower per-face reads).
        auto& capturePass = baseGraph.addPass("ibl_basecube_capture", QueueClass::AsyncCompute);
        for (uint32_t face = 0; face < 6; ++face) {
            capturePass.addTextureInput("baseCubemap", Subresource{0, 1, face, 1});
        }
        capturePass.setSideEffect().setExecute([&](PassContext& ctx) {
            capturedBaseCubemap = ctx.image("baseCubemap");
            baseCubemapView = ctx.imageView("baseCubemap");
        });

        baseGraph.addPass("ibl_present_a").addColorOutput("bb", bbDesc());
        baseGraph.setBackbufferSource("bb");
        baseGraph.compile(info);
        executor->realize(baseGraph);

        const auto stageStart = std::chrono::steady_clock::now();
        RX_ZONE_NAMED("rx_ibl: base cubemap stage");
        cmdCtx.runOnce([&](VkCommandBuffer cmd) {
            executor->execute(baseGraph, cmd, offscreen->image, offscreen->view, VkExtent2D{kBbExtent, kBbExtent});
        });
        timings.equirectToCubemapMs = millisSince(stageStart);
        // "baseCubemap" IS read within this graph now (the capture
        // pass's own per-face addTextureInput declarations, added to
        // satisfy the SAMPLED_BIT usage-flag requirement -- see that
        // pass's own comment above) -- TextureInput resolves to
        // SHADER_READ_ONLY_OPTIMAL, and the capture pass is the LAST
        // toucher of every subresource (it depends on all 6 writers),
        // so that is "baseCubemap"'s real, final layout once this
        // runOnce() call (which blocks until GPU completion) returns.
        baseCubemapLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    {
        struct IrradiancePush {
            uint32_t faceIndex;
            uint32_t faceDim;
            uint32_t numSamples;
        };
        RenderGraph irradianceGraph;
        for (uint32_t face = 0; face < 6; ++face) {
            const Subresource sub{0, 1, face, 1};
            const std::string passName = "ibl_irradiance_face" + std::to_string(face);
            irradianceGraph.addPass(passName, QueueClass::AsyncCompute)
                .addStorageImageOutput("irradianceCubemap", irrCubeDesc, sub)
                .setSideEffect()
                .setExecute([&, face](PassContext& ctx) {
                    VkDescriptorSet set = allocateSet1(vkDevice, pool, irradianceKernel->pso.setLayouts[1]);
                    writeSampledPass(vkDevice, set, baseCubemapView, baseCubemapLayout, cubeSampler,
                                      ctx.storageImageView("irradianceCubemap"));
                    IrradiancePush push{face, params.irradianceFaceSize, params.irradianceSamples};
                    bindAndDispatch(ctx.cmd, *irradianceKernel, set, &push, sizeof(push),
                                     groupCount(params.irradianceFaceSize, 8), groupCount(params.irradianceFaceSize, 8));
                    capturedIrradiance = ctx.image("irradianceCubemap");
                });
        }
        irradianceGraph.addPass("ibl_present_b").addColorOutput("bb", bbDesc());
        irradianceGraph.setBackbufferSource("bb");
        irradianceGraph.compile(info);
        executor->realize(irradianceGraph);

        const auto stageStart = std::chrono::steady_clock::now();
        RX_ZONE_NAMED("rx_ibl: irradiance convolution stage");
        cmdCtx.runOnce([&](VkCommandBuffer cmd) {
            executor->execute(irradianceGraph, cmd, offscreen->image, offscreen->view, VkExtent2D{kBbExtent, kBbExtent});
        });
        timings.irradianceMs = millisSince(stageStart);
    }

    {
        struct PrefilterPush {
            uint32_t faceIndex;
            uint32_t faceDim;
            uint32_t numSamples;
            float linearRoughness;
        };
        RenderGraph prefilterGraph;
        for (uint32_t mip = 0; mip < params.prefilteredMipCount; ++mip) {
            const uint32_t mipDim = std::max(1U, params.prefilteredBaseFaceSize >> mip);
            // [dfg_lut.slang / this task's own report: coord = mip/(N-1),
            // linearRoughness = coord*coord -- SAME roughness<->mip
            // mapping this bake's DFG LUT and prefiltered chain must
            // agree on for the "highest-roughness mip corresponds to the
            // DFG LUT's roughest row" acceptance line to mean anything.]
            const float coord = params.prefilteredMipCount > 1
                                     ? static_cast<float>(mip) / static_cast<float>(params.prefilteredMipCount - 1)
                                     : 0.0F;
            const float linearRoughness = coord * coord;
            for (uint32_t face = 0; face < 6; ++face) {
                const Subresource sub{mip, 1, face, 1};
                const std::string passName = "ibl_prefilter_mip" + std::to_string(mip) + "_face" + std::to_string(face);
                prefilterGraph.addPass(passName, QueueClass::AsyncCompute)
                    .addStorageImageOutput("prefilteredCubemap", prefilterDesc, sub)
                    .setSideEffect()
                    .setExecute([&, face, mip, mipDim, linearRoughness](PassContext& ctx) {
                        VkDescriptorSet set = allocateSet1(vkDevice, pool, prefilterKernel->pso.setLayouts[1]);
                        writeSampledPass(vkDevice, set, baseCubemapView, baseCubemapLayout, cubeSampler,
                                          ctx.storageImageView("prefilteredCubemap"));
                        PrefilterPush push{face, mipDim, params.specularSamples, linearRoughness};
                        bindAndDispatch(ctx.cmd, *prefilterKernel, set, &push, sizeof(push), groupCount(mipDim, 8),
                                         groupCount(mipDim, 8));
                        capturedPrefiltered = ctx.image("prefilteredCubemap");
                    });
            }
        }
        prefilterGraph.addPass("ibl_present_c").addColorOutput("bb", bbDesc());
        prefilterGraph.setBackbufferSource("bb");
        prefilterGraph.compile(info);
        executor->realize(prefilterGraph);

        const auto stageStart = std::chrono::steady_clock::now();
        RX_ZONE_NAMED("rx_ibl: prefiltered specular stage");
        cmdCtx.runOnce([&](VkCommandBuffer cmd) {
            executor->execute(prefilterGraph, cmd, offscreen->image, offscreen->view, VkExtent2D{kBbExtent, kBbExtent});
        });
        timings.prefilterMs = millisSince(stageStart);
    }

    {
        struct DfgPush {
            uint32_t width;
            uint32_t height;
            uint32_t numSamples;
        };
        RenderGraph dfgGraph;
        dfgGraph.addPass("ibl_dfg", QueueClass::AsyncCompute)
            .addStorageImageOutput("dfgLut", dfgDesc)
            .setSideEffect()
            .setExecute([&](PassContext& ctx) {
                VkDescriptorSet set = allocateSet1(vkDevice, pool, dfgKernel->pso.setLayouts[1]);
                writeDfgPass(vkDevice, set, ctx.storageImageView("dfgLut"));
                DfgPush push{params.dfgLutSize, params.dfgLutSize, params.dfgSamples};
                bindAndDispatch(ctx.cmd, *dfgKernel, set, &push, sizeof(push), groupCount(params.dfgLutSize, 8),
                                 groupCount(params.dfgLutSize, 8));
                capturedDfg = ctx.image("dfgLut");
            });
        dfgGraph.addPass("ibl_present_d").addColorOutput("bb", bbDesc());
        dfgGraph.setBackbufferSource("bb");
        dfgGraph.compile(info);
        executor->realize(dfgGraph);

        const auto stageStart = std::chrono::steady_clock::now();
        RX_ZONE_NAMED("rx_ibl: DFG LUT stage");
        cmdCtx.runOnce([&](VkCommandBuffer cmd) {
            executor->execute(dfgGraph, cmd, offscreen->image, offscreen->view, VkExtent2D{kBbExtent, kBbExtent});
        });
        timings.dfgMs = millisSince(stageStart);
    }

    // Texture2D has no default constructor (only its factory methods,
    // returning std::optional<Texture2D>, can produce one) -- built as 4
    // separate locals rather than default-constructing a BakeResult and
    // assigning into its fields.
    auto baseCubemapTex = rx::rhi::Texture2D::createCubeForPresuppliedMips(
        physicalDevice, vkDevice, allocator, VkExtent2D{params.baseCubemapFaceSize, params.baseCubemapFaceSize},
        kCubeFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1);
    if (!baseCubemapTex.has_value()) {
        RX_LOG_ERROR("rx_ibl: failed to create baseCubemap persistent texture");
        return std::nullopt;
    }
    auto irradianceTex = rx::rhi::Texture2D::createCubeForPresuppliedMips(
        physicalDevice, vkDevice, allocator, VkExtent2D{params.irradianceFaceSize, params.irradianceFaceSize},
        kCubeFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1);
    if (!irradianceTex.has_value()) {
        RX_LOG_ERROR("rx_ibl: failed to create irradianceCubemap persistent texture");
        return std::nullopt;
    }
    auto prefilteredTex = rx::rhi::Texture2D::createCubeForPresuppliedMips(
        physicalDevice, vkDevice, allocator, VkExtent2D{params.prefilteredBaseFaceSize, params.prefilteredBaseFaceSize},
        kCubeFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, params.prefilteredMipCount);
    if (!prefilteredTex.has_value()) {
        RX_LOG_ERROR("rx_ibl: failed to create prefilteredCubemap persistent texture");
        return std::nullopt;
    }
    auto dfgTex = rx::rhi::Texture2D::createForPresuppliedMips(physicalDevice, vkDevice, allocator,
                                                                  VkExtent2D{params.dfgLutSize, params.dfgLutSize},
                                                                  kDfgFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1);
    if (!dfgTex.has_value()) {
        RX_LOG_ERROR("rx_ibl: failed to create dfgLut persistent texture");
        return std::nullopt;
    }

    // Final copy step -- NOT counted in any per-stage bake timing above
    // (it is real GPU->GPU copy work, but it is post-processing of
    // already-baked results into this function's own persistent output
    // textures, not part of "the bake" those numbers describe; a real
    // caller keeping the transient results in place, e.g. Task 10's own
    // eventual integration, would not pay this cost at all).
    cmdCtx.runOnce([&](VkCommandBuffer cmd) {
        // baseCubemapLayout is SHADER_READ_ONLY_OPTIMAL (sourceIsCube --
        // `source` was never written by this bake) or GENERAL
        // (equirect path -- StorageImageOutput's own resolved layout,
        // nothing else touches "baseCubemap" once its own stage's
        // runOnce() call, which blocks until GPU completion, returns).
        std::vector<VkImageMemoryBarrier2> barriers;
        barriers.push_back(makeBarrier(capturedBaseCubemap, baseCubemapLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1, 0, 6));
        submitBarriers(cmd, barriers);
        copyToPersistent(cmd, baseCubemapTex->image(), capturedBaseCubemap, 1, params.baseCubemapFaceSize,
                          params.baseCubemapFaceSize, 6);

        std::vector<VkImageMemoryBarrier2> preCopy;
        preCopy.push_back(makeBarrier(capturedIrradiance, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1, 0, 6));
        preCopy.push_back(makeBarrier(capturedPrefiltered, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0,
                                       params.prefilteredMipCount, 0, 6));
        preCopy.push_back(makeBarrier(capturedDfg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                       VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0, 1, 0, 1));
        submitBarriers(cmd, preCopy);

        copyToPersistent(cmd, irradianceTex->image(), capturedIrradiance, 1, params.irradianceFaceSize,
                          params.irradianceFaceSize, 6);
        copyToPersistent(cmd, prefilteredTex->image(), capturedPrefiltered, params.prefilteredMipCount,
                          params.prefilteredBaseFaceSize, params.prefilteredBaseFaceSize, 6);
        copyToPersistent(cmd, dfgTex->image(), capturedDfg, 1, params.dfgLutSize, params.dfgLutSize, 1);
    });
    timings.totalMs = millisSince(totalStart);
    // Per-stage timings (equirectToCubemapMs/irradianceMs/prefilterMs/
    // dfgMs) are each real, independently-measured wall-clock spans --
    // every stage runs as its own RenderGraph + one-shot runOnce()
    // submission (see this function's own comment on why, above), which
    // blocks until GPU completion before the NEXT stage's std::chrono
    // span starts. totalMs additionally includes pipeline/session setup
    // (compile+reflect+PSO build, once, for all four kernels) and the
    // final copy-to-persistent-textures step, so totalMs is always >=
    // the sum of the four per-stage numbers, honestly.
    if (outTimings != nullptr) {
        *outTimings = timings;
    }

    vkDeviceWaitIdle(vkDevice);
    destroyOffscreenImage(vkDevice, *offscreen);
    vkDestroySampler(vkDevice, equirectSampler, nullptr);
    vkDestroySampler(vkDevice, cubeSampler, nullptr);
    vkDestroyDescriptorPool(vkDevice, pool, nullptr);

    return BakeResult{std::move(*baseCubemapTex), std::move(*irradianceTex), std::move(*prefilteredTex),
                       std::move(*dfgTex), params.prefilteredMipCount};
}

}  // namespace rx::ibl
