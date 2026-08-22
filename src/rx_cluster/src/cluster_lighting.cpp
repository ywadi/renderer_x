#include <rx_cluster/cluster_lighting.h>

#include <rx_core/log.h>
#include <rx_graph/executor.h>
#include <rx_scene/light_math.h>
#include <rx_shader/reflection.h>

#include <array>
#include <cstring>
#include <utility>

namespace rx::cluster {
namespace {

using rx::graph::BufferDesc;
using rx::graph::PassContext;
using rx::graph::QueueClass;
using rx::graph::RenderGraph;

// GPU push-constant mirror of shaders/cluster/froxel_common.slang's own
// FroxelGridParamsGpu -- kept manually in sync (see this module's own
// header top comment). All-scalar, 48 bytes, no padding ambiguity.
struct FroxelGridParamsGpu {
    uint32_t countX;
    uint32_t countY;
    uint32_t countZ;
    uint32_t totalLightCount;
    float tanHalfFovY;
    float aspectRatio;
    float zLightNear;
    float zLightFar;
    float linearizer;
    float invLinearizer;
    uint32_t maxLightsPerFroxel;
    uint32_t maxTotalLightIndices;
};

std::optional<ClusterPipelines::Kernel> buildKernel(rx::shader::Compiler& compiler, rx::rhi::ComputePipelineCache& cache,
                                                      const std::filesystem::path& path, const std::string& entryName) {
    auto compiled = compiler.compileFromFile(path.string(), {entryName});
    if (!compiled.ok || compiled.entryPointCode.empty()) {
        RX_LOG_ERROR("rx_cluster: failed to compile {}: {}", path.string(), compiled.diagnostics);
        return std::nullopt;
    }
    auto layoutInfo = rx::shader::reflect(compiled);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("rx_cluster: failed to reflect {}", path.string());
        return std::nullopt;
    }
    if (layoutInfo->pushRanges.empty()) {
        RX_LOG_ERROR("rx_cluster: {} reflected no push-constant range", path.string());
        return std::nullopt;
    }
    auto pso = cache.getOrCreate(compiled.entryPointCode[0].code, *layoutInfo);
    if (!pso.has_value()) {
        RX_LOG_ERROR("rx_cluster: failed to build pipeline for {}", path.string());
        return std::nullopt;
    }
    if (pso->setLayouts.size() < 2) {
        RX_LOG_ERROR("rx_cluster: {} produced fewer than 2 descriptor set layouts", path.string());
        return std::nullopt;
    }
    return ClusterPipelines::Kernel{*pso, *layoutInfo};
}

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

VkWriteDescriptorSet storageWrite(VkDescriptorSet set, uint32_t binding, const VkDescriptorBufferInfo& info) {
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &info;
    return w;
}

uint32_t groupCount1D(uint32_t elementCount, uint32_t groupSize) {
    return (elementCount + groupSize - 1) / groupSize;
}

}  // namespace

ClusterPipelines::~ClusterPipelines() {
    for (auto& slot : frameSlots_) {
        if (slot.pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, slot.pool, nullptr);
        }
    }
}

ClusterPipelines::ClusterPipelines(ClusterPipelines&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      countKernel_(std::move(other.countKernel_)),
      prefixSumKernel_(std::move(other.prefixSumKernel_)),
      scatterKernel_(std::move(other.scatterKernel_)),
      frameSlots_(std::move(other.frameSlots_)) {
    other.frameSlots_.clear();
}

ClusterPipelines& ClusterPipelines::operator=(ClusterPipelines&& other) noexcept {
    if (this != &other) {
        for (auto& slot : frameSlots_) {
            if (slot.pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, slot.pool, nullptr);
            }
        }
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        countKernel_ = std::move(other.countKernel_);
        prefixSumKernel_ = std::move(other.prefixSumKernel_);
        scatterKernel_ = std::move(other.scatterKernel_);
        frameSlots_ = std::move(other.frameSlots_);
        other.frameSlots_.clear();
    }
    return *this;
}

std::vector<ClusterLightGpu> buildClusterLightList(const rx::scene::Scene& scene, const glm::mat4& viewMatrix,
                                                     float cullCutoffLux) {
    std::vector<ClusterLightGpu> result;
    const auto records = scene.lightRecordsSpan();
    const auto alive = scene.lightAliveSpan();
    result.reserve(records.size());

    const glm::mat3 rot(viewMatrix);
    for (size_t i = 0; i < records.size(); ++i) {
        if (i >= alive.size() || alive[i] == 0) {
            continue;
        }
        const rx::scene::LightRecord& rec = records[i];
        if (rec.type != rx::scene::LightType::Point && rec.type != rx::scene::LightType::Spot) {
            continue;
        }

        const glm::vec3 viewPos(viewMatrix * glm::vec4(rec.position, 1.0F));
        const float cullRadius = rx::scene::froxel::pointLightCullRadius(rec.colorLux, rec.range, cullCutoffLux);

        ClusterLightGpu gpu;
        gpu.viewPositionRadius = glm::vec4(viewPos, cullRadius);
        if (rec.type == rx::scene::LightType::Spot) {
            const glm::vec3 axisView = rot * rec.direction;
            const auto cone = rx::scene::froxel::spotConeViewSpace(axisView, rec.outerConeAngle);
            gpu.viewAxisSinInverse = glm::vec4(cone.axis, cone.sinInverse);
            gpu.cosSquaredFlags = glm::vec4(cone.cosSquared, 1.0F, 0.0F, 0.0F);
        } else {
            gpu.viewAxisSinInverse = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);
            gpu.cosSquaredFlags = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);
        }

        // [Phase 5 Task 15, #51] SHADING-side fields -- the SAME world-
        // space/physical-unit data standard_pbr.slang's existing single-
        // slot Point/Spot punctual term already consumes off a DrawDataGpu
        // row (see draw_data.h's own Task 13 field comments), here indexed
        // by this SAME light's own row instead. `lightAngleScale`/
        // `lightAngleOffset` reuse `rx::scene::lightmath::
        // spotAngleScaleOffset()` -- the IDENTICAL formula/derivation the
        // single-slot path already uses (light_math.h/.cpp), computed once
        // per light here rather than re-derived per-fragment.
        const float lightTypeGpu = rec.type == rx::scene::LightType::Spot ? 2.0F : 1.0F;
        float angleScale = 0.0F;
        float angleOffset = 0.0F;
        if (rec.type == rx::scene::LightType::Spot) {
            const rx::scene::lightmath::SpotAngleScaleOffset angles =
                rx::scene::lightmath::spotAngleScaleOffset(rec.innerConeAngle, rec.outerConeAngle);
            angleScale = angles.scale;
            angleOffset = angles.offset;
        }
        gpu.shadingTypeRangeAngle = glm::vec4(lightTypeGpu, rec.range, angleScale, angleOffset);
        gpu.shadingPositionWorld = glm::vec4(rec.position, 0.0F);
        gpu.shadingColorCandela = glm::vec4(rec.colorLux, 0.0F);
        gpu.shadingSpotDirWorld = glm::vec4(rec.direction, 0.0F);

        result.push_back(gpu);
    }
    return result;
}

std::optional<ClusterPipelines> ClusterPipelines::create(VkDevice device, rx::rhi::ComputePipelineCache& cache,
                                                           rx::shader::Compiler& compiler,
                                                           const std::filesystem::path& shaderDir,
                                                           uint32_t framesInFlight) {
    auto countKernel = buildKernel(compiler, cache, shaderDir / "froxel_light_count.slang", "csCount");
    auto prefixSumKernel = buildKernel(compiler, cache, shaderDir / "froxel_prefix_sum.slang", "csPrefixSum");
    auto scatterKernel = buildKernel(compiler, cache, shaderDir / "froxel_light_scatter.slang", "csScatter");
    if (!countKernel.has_value() || !prefixSumKernel.has_value() || !scatterKernel.has_value()) {
        return std::nullopt;
    }
    if (framesInFlight == 0) {
        RX_LOG_ERROR("rx_cluster: ClusterPipelines::create: framesInFlight must be > 0");
        return std::nullopt;
    }

    ClusterPipelines result;
    result.device_ = device;
    result.countKernel_ = std::move(countKernel);
    result.prefixSumKernel_ = std::move(prefixSumKernel);
    result.scatterKernel_ = std::move(scatterKernel);
    result.frameSlots_.resize(framesInFlight);

    // [Phase 5 Task 15, #51] `framesInFlight` INDEPENDENT pools, each sized
    // exactly like T14's original single pool (generous headroom: 3 empty
    // set-0s + up to 3 real set-1s per call) -- see this class's own
    // FRAMES-IN-FLIGHT DESIGN comment (header) for why one pool per slot,
    // not one shared pool, is what makes a per-frame reset safe.
    constexpr uint32_t kMaxSets = 16;
    constexpr uint32_t kMaxStorageBuffers = 64;
    for (uint32_t slotIndex = 0; slotIndex < framesInFlight; ++slotIndex) {
        std::array<VkDescriptorPoolSize, 1> poolSizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxStorageBuffers}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = kMaxSets;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            RX_LOG_ERROR("rx_cluster: failed to create descriptor pool for frame slot {}", slotIndex);
            return std::nullopt;
        }
        result.frameSlots_[slotIndex].pool = pool;

        // Each kernel's own empty set-0 (no bindless use in any of these
        // shaders -- PipelineLayoutBuilder auto-builds a placeholder empty
        // layout, same "set 0 is the auto-built EMPTY placeholder"
        // precedent test_compute_gpu.cpp's TEST_CASE 2 establishes) --
        // pre-warmed here (one per frame slot) exactly mirroring T14's own
        // original create()-time pre-warm, just replicated per slot.
        FrameSlot& slot = result.frameSlots_[slotIndex];
        slot.countSet0 = allocateSet1(device, pool, result.countKernel_->pso.setLayouts[0]);
        slot.prefixSumSet0 = allocateSet1(device, pool, result.prefixSumKernel_->pso.setLayouts[0]);
        slot.scatterSet0 = allocateSet1(device, pool, result.scatterKernel_->pso.setLayouts[0]);
        if (slot.countSet0 == VK_NULL_HANDLE || slot.prefixSumSet0 == VK_NULL_HANDLE ||
            slot.scatterSet0 == VK_NULL_HANDLE) {
            RX_LOG_ERROR("rx_cluster: failed to allocate an empty set-0 for frame slot {}", slotIndex);
            return std::nullopt;
        }
    }

    return result;
}

rx::scene::froxel::FroxelGridParams ClusterPipelines::addClusterPasses(RenderGraph& graph,
                                                                        const ClusterFrameInputs& frameInputs,
                                                                        const rx::scene::Camera& camera,
                                                                        uint32_t viewportWidth, uint32_t viewportHeight,
                                                                        const ClusterParams& params,
                                                                        uint32_t frameSlot) {
    const rx::scene::froxel::FroxelGridParams grid = rx::scene::froxel::buildFroxelGrid(
        viewportWidth, viewportHeight, camera.verticalFovRadians, camera.aspectRatio, params.zLightNear,
        params.zLightFar, params.targetFroxelBudget, params.sliceCountZ);
    const uint32_t froxelCount = grid.froxelCount();

    // [Phase 5 Task 15, #51] Clamp (not assert) an out-of-range frameSlot --
    // this is a per-frame-called path (unlike create()'s own one-time
    // validation), so a defensively-recoverable clamp (with a loud log)
    // beats a crash on a caller's off-by-one; see this class's own
    // FRAMES-IN-FLIGHT DESIGN comment (header) for why ONLY the SELECTED
    // slot's pool is touched below.
    if (frameSlot >= frameSlots_.size()) {
        RX_LOG_ERROR("rx_cluster: addClusterPasses: frameSlot {} out of range (framesInFlight={}); clamping",
                     frameSlot, frameSlots_.size());
        frameSlot = frameSlot % static_cast<uint32_t>(frameSlots_.size());
    }
    FrameSlot& slot = frameSlots_[frameSlot];

    // Every set-1 (and set-0, re-warmed identically) allocated by THIS call
    // is freed here -- a DIFFERENT frame slot's own pool/sets, potentially
    // still referenced by a still-in-flight GPU submission from an earlier
    // call against THAT slot, is never touched.
    vkResetDescriptorPool(device_, slot.pool, 0);
    slot.countSet0 = allocateSet1(device_, slot.pool, countKernel_->pso.setLayouts[0]);
    slot.prefixSumSet0 = allocateSet1(device_, slot.pool, prefixSumKernel_->pso.setLayouts[0]);
    slot.scatterSet0 = allocateSet1(device_, slot.pool, scatterKernel_->pso.setLayouts[0]);
    slot.countSet1 = allocateSet1(device_, slot.pool, countKernel_->pso.setLayouts[1]);
    slot.prefixSumSet1 = allocateSet1(device_, slot.pool, prefixSumKernel_->pso.setLayouts[1]);
    slot.scatterSet1 = allocateSet1(device_, slot.pool, scatterKernel_->pso.setLayouts[1]);

    // [Phase 5 Task 15, #51] `totalLightCount` is 0 here -- the grid's own
    // SHAPE (every other field) is frozen at this declare-time call, but
    // the LIGHT COUNT is read FRESH from `frameInputs` inside each pass's
    // own setExecute() lambda below (see this function's own header
    // comment on ClusterFrameInputs) -- this local copy is only ever used
    // as the shape TEMPLATE each lambda copies-and-overrides.
    const FroxelGridParamsGpu gpuGridShape{
        grid.countX,       grid.countY,          grid.countZ,   0,
        grid.tanHalfFovY,  grid.aspectRatio,      grid.zLightNear, grid.zLightFar,
        grid.linearizer,   grid.invLinearizer,    params.maxLightsPerFroxel, params.maxTotalLightIndices};

    BufferDesc perFroxelDesc;
    perFroxelDesc.size = std::max<uint32_t>(1, froxelCount) * sizeof(uint32_t);
    perFroxelDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    BufferDesc scalarDesc;
    scalarDesc.size = sizeof(uint32_t);
    scalarDesc.usage = perFroxelDesc.usage;

    BufferDesc indicesDesc;
    indicesDesc.size = std::max<uint32_t>(1, params.maxTotalLightIndices) * sizeof(uint32_t);
    indicesDesc.usage = perFroxelDesc.usage;

    graph.addPass("cluster_count", QueueClass::AsyncCompute)
        .addStorageBufferOutput("clusterTrueCounts", perFroxelDesc)
        .setSideEffect()
        .setExecute([this, gpuGridShape, &frameInputs, froxelCount, frameSlot](PassContext& ctx) {
            FroxelGridParamsGpu gpuGrid = gpuGridShape;
            gpuGrid.totalLightCount = frameInputs.lightCount;
            const VkDeviceSize lightsBytes = static_cast<VkDeviceSize>(frameInputs.lightCount) * sizeof(ClusterLightGpu);
            FrameSlot& s = frameSlots_[frameSlot];
            const VkDescriptorBufferInfo lightsInfo{frameInputs.lightsBuffer, 0,
                                                       lightsBytes > 0 ? lightsBytes : VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo countsInfo{ctx.buffer("clusterTrueCounts"), 0, VK_WHOLE_SIZE};
            std::array<VkWriteDescriptorSet, 2> writes{storageWrite(s.countSet1, 0, lightsInfo),
                                                          storageWrite(s.countSet1, 1, countsInfo)};
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, countKernel_->pso.pipeline);
            std::array<VkDescriptorSet, 2> sets{s.countSet0, s.countSet1};
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, countKernel_->pso.layout, 0,
                                      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
            const auto& range = countKernel_->layoutInfo.pushRanges[0];
            vkCmdPushConstants(ctx.cmd, countKernel_->pso.layout, range.stages, range.offset, range.size, &gpuGrid);
            vkCmdDispatch(ctx.cmd, groupCount1D(froxelCount, 64), 1, 1);
        });

    graph.addPass("cluster_prefix_sum", QueueClass::AsyncCompute)
        .addStorageBufferInput("clusterTrueCounts")
        .addStorageBufferOutput("clusterOffsets", perFroxelDesc)
        .addStorageBufferOutput("clusterWriteCounts", perFroxelDesc)
        .addStorageBufferOutput("clusterPerFroxelOverflow", perFroxelDesc)
        .addStorageBufferOutput("clusterGlobalOverflow", perFroxelDesc)
        .addStorageBufferOutput("clusterTotalUsed", scalarDesc)
        .setSideEffect()
        .setExecute([this, gpuGridShape, &frameInputs, frameSlot](PassContext& ctx) {
            FroxelGridParamsGpu gpuGrid = gpuGridShape;
            gpuGrid.totalLightCount = frameInputs.lightCount;
            FrameSlot& s = frameSlots_[frameSlot];
            const VkDescriptorBufferInfo trueCountsInfo{ctx.buffer("clusterTrueCounts"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo offsetsInfo{ctx.buffer("clusterOffsets"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo writeCountsInfo{ctx.buffer("clusterWriteCounts"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo perFroxelOverflowInfo{ctx.buffer("clusterPerFroxelOverflow"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo globalOverflowInfo{ctx.buffer("clusterGlobalOverflow"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo totalUsedInfo{ctx.buffer("clusterTotalUsed"), 0, VK_WHOLE_SIZE};
            std::array<VkWriteDescriptorSet, 6> writes{
                storageWrite(s.prefixSumSet1, 0, trueCountsInfo),     storageWrite(s.prefixSumSet1, 1, offsetsInfo),
                storageWrite(s.prefixSumSet1, 2, writeCountsInfo),    storageWrite(s.prefixSumSet1, 3, perFroxelOverflowInfo),
                storageWrite(s.prefixSumSet1, 4, globalOverflowInfo), storageWrite(s.prefixSumSet1, 5, totalUsedInfo)};
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefixSumKernel_->pso.pipeline);
            std::array<VkDescriptorSet, 2> sets{s.prefixSumSet0, s.prefixSumSet1};
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefixSumKernel_->pso.layout, 0,
                                      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
            const auto& range = prefixSumKernel_->layoutInfo.pushRanges[0];
            vkCmdPushConstants(ctx.cmd, prefixSumKernel_->pso.layout, range.stages, range.offset, range.size, &gpuGrid);
            vkCmdDispatch(ctx.cmd, 1, 1, 1);
        });

    graph.addPass("cluster_scatter", QueueClass::AsyncCompute)
        .addStorageBufferInput("clusterOffsets")
        .addStorageBufferInput("clusterWriteCounts")
        .addStorageBufferOutput("clusterLightIndices", indicesDesc)
        .setSideEffect()
        .setExecute([this, gpuGridShape, &frameInputs, froxelCount, frameSlot](PassContext& ctx) {
            FroxelGridParamsGpu gpuGrid = gpuGridShape;
            gpuGrid.totalLightCount = frameInputs.lightCount;
            const VkDeviceSize lightsBytes = static_cast<VkDeviceSize>(frameInputs.lightCount) * sizeof(ClusterLightGpu);
            FrameSlot& s = frameSlots_[frameSlot];
            const VkDescriptorBufferInfo lightsInfo{frameInputs.lightsBuffer, 0,
                                                       lightsBytes > 0 ? lightsBytes : VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo offsetsInfo{ctx.buffer("clusterOffsets"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo writeCountsInfo{ctx.buffer("clusterWriteCounts"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo indicesInfo{ctx.buffer("clusterLightIndices"), 0, VK_WHOLE_SIZE};
            std::array<VkWriteDescriptorSet, 4> writes{
                storageWrite(s.scatterSet1, 0, lightsInfo), storageWrite(s.scatterSet1, 1, offsetsInfo),
                storageWrite(s.scatterSet1, 2, writeCountsInfo), storageWrite(s.scatterSet1, 3, indicesInfo)};
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatterKernel_->pso.pipeline);
            std::array<VkDescriptorSet, 2> sets{s.scatterSet0, s.scatterSet1};
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatterKernel_->pso.layout, 0,
                                      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
            const auto& range = scatterKernel_->layoutInfo.pushRanges[0];
            vkCmdPushConstants(ctx.cmd, scatterKernel_->pso.layout, range.stages, range.offset, range.size, &gpuGrid);
            vkCmdDispatch(ctx.cmd, groupCount1D(froxelCount, 64), 1, 1);
        });

    return grid;
}

}  // namespace rx::cluster
