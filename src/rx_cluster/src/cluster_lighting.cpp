#include <rx_cluster/cluster_lighting.h>

#include <rx_core/log.h>
#include <rx_graph/executor.h>
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
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, pool_, nullptr);
    }
}

ClusterPipelines::ClusterPipelines(ClusterPipelines&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
      countKernel_(std::move(other.countKernel_)),
      prefixSumKernel_(std::move(other.prefixSumKernel_)),
      scatterKernel_(std::move(other.scatterKernel_)),
      countSet0_(std::exchange(other.countSet0_, VK_NULL_HANDLE)),
      prefixSumSet0_(std::exchange(other.prefixSumSet0_, VK_NULL_HANDLE)),
      scatterSet0_(std::exchange(other.scatterSet0_, VK_NULL_HANDLE)),
      countSet1_(std::exchange(other.countSet1_, VK_NULL_HANDLE)),
      prefixSumSet1_(std::exchange(other.prefixSumSet1_, VK_NULL_HANDLE)),
      scatterSet1_(std::exchange(other.scatterSet1_, VK_NULL_HANDLE)) {}

ClusterPipelines& ClusterPipelines::operator=(ClusterPipelines&& other) noexcept {
    if (this != &other) {
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        }
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
        countKernel_ = std::move(other.countKernel_);
        prefixSumKernel_ = std::move(other.prefixSumKernel_);
        scatterKernel_ = std::move(other.scatterKernel_);
        countSet0_ = std::exchange(other.countSet0_, VK_NULL_HANDLE);
        prefixSumSet0_ = std::exchange(other.prefixSumSet0_, VK_NULL_HANDLE);
        scatterSet0_ = std::exchange(other.scatterSet0_, VK_NULL_HANDLE);
        countSet1_ = std::exchange(other.countSet1_, VK_NULL_HANDLE);
        prefixSumSet1_ = std::exchange(other.prefixSumSet1_, VK_NULL_HANDLE);
        scatterSet1_ = std::exchange(other.scatterSet1_, VK_NULL_HANDLE);
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
        result.push_back(gpu);
    }
    return result;
}

std::optional<ClusterPipelines> ClusterPipelines::create(VkDevice device, rx::rhi::ComputePipelineCache& cache,
                                                           rx::shader::Compiler& compiler,
                                                           const std::filesystem::path& shaderDir) {
    auto countKernel = buildKernel(compiler, cache, shaderDir / "froxel_light_count.slang", "csCount");
    auto prefixSumKernel = buildKernel(compiler, cache, shaderDir / "froxel_prefix_sum.slang", "csPrefixSum");
    auto scatterKernel = buildKernel(compiler, cache, shaderDir / "froxel_light_scatter.slang", "csScatter");
    if (!countKernel.has_value() || !prefixSumKernel.has_value() || !scatterKernel.has_value()) {
        return std::nullopt;
    }

    // One generously-sized pool, reset (not individually freed) at the
    // start of every addClusterPasses() call -- see this class's own
    // header comment for the frames-in-flight scope boundary this implies.
    // 3 empty set-0s (allocated once, below) + up to 3 real set-1s per
    // call, sized with headroom for repeated calls between resets.
    constexpr uint32_t kMaxSets = 16;
    constexpr uint32_t kMaxStorageBuffers = 64;
    std::array<VkDescriptorPoolSize, 1> poolSizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxStorageBuffers}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxSets;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_cluster: failed to create descriptor pool");
        return std::nullopt;
    }

    ClusterPipelines result;
    result.device_ = device;
    result.pool_ = pool;
    result.countKernel_ = std::move(countKernel);
    result.prefixSumKernel_ = std::move(prefixSumKernel);
    result.scatterKernel_ = std::move(scatterKernel);

    // Each kernel's own empty set-0 (no bindless use in any of these
    // shaders -- PipelineLayoutBuilder auto-builds a placeholder empty
    // layout, same "set 0 is the auto-built EMPTY placeholder" precedent
    // test_compute_gpu.cpp's TEST_CASE 2 establishes) -- allocated ONCE
    // here (not per addClusterPasses() call/reset) since it is never
    // written to and every call binds the SAME empty set.
    result.countSet0_ = allocateSet1(device, pool, result.countKernel_->pso.setLayouts[0]);
    result.prefixSumSet0_ = allocateSet1(device, pool, result.prefixSumKernel_->pso.setLayouts[0]);
    result.scatterSet0_ = allocateSet1(device, pool, result.scatterKernel_->pso.setLayouts[0]);
    if (result.countSet0_ == VK_NULL_HANDLE || result.prefixSumSet0_ == VK_NULL_HANDLE ||
        result.scatterSet0_ == VK_NULL_HANDLE) {
        RX_LOG_ERROR("rx_cluster: failed to allocate an empty set-0");
        return std::nullopt;
    }

    return result;
}

rx::scene::froxel::FroxelGridParams ClusterPipelines::addClusterPasses(RenderGraph& graph, VkBuffer lightsBuffer,
                                                                        uint32_t lightCount,
                                                                        const rx::scene::Camera& camera,
                                                                        uint32_t viewportWidth, uint32_t viewportHeight,
                                                                        const ClusterParams& params) {
    const rx::scene::froxel::FroxelGridParams grid = rx::scene::froxel::buildFroxelGrid(
        viewportWidth, viewportHeight, camera.verticalFovRadians, camera.aspectRatio, params.zLightNear,
        params.zLightFar, params.targetFroxelBudget, params.sliceCountZ);
    const uint32_t froxelCount = grid.froxelCount();

    // Every set-1 allocated by THIS call is freed here (the empty set-0s,
    // allocated once at create() time, are untouched by this reset -- they
    // live in the SAME pool but are never reallocated) -- see this class's
    // own header comment for the frames-in-flight scope boundary.
    vkResetDescriptorPool(device_, pool_, 0);
    countSet0_ = allocateSet1(device_, pool_, countKernel_->pso.setLayouts[0]);
    prefixSumSet0_ = allocateSet1(device_, pool_, prefixSumKernel_->pso.setLayouts[0]);
    scatterSet0_ = allocateSet1(device_, pool_, scatterKernel_->pso.setLayouts[0]);
    countSet1_ = allocateSet1(device_, pool_, countKernel_->pso.setLayouts[1]);
    prefixSumSet1_ = allocateSet1(device_, pool_, prefixSumKernel_->pso.setLayouts[1]);
    scatterSet1_ = allocateSet1(device_, pool_, scatterKernel_->pso.setLayouts[1]);

    const FroxelGridParamsGpu gpuGrid{
        grid.countX,       grid.countY,          grid.countZ,   lightCount,
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

    const VkDeviceSize lightsBytes = static_cast<VkDeviceSize>(lightCount) * sizeof(ClusterLightGpu);

    graph.addPass("cluster_count", QueueClass::AsyncCompute)
        .addStorageBufferOutput("clusterTrueCounts", perFroxelDesc)
        .setSideEffect()
        .setExecute([this, gpuGrid, lightsBuffer, lightsBytes, froxelCount](PassContext& ctx) {
            const VkDescriptorBufferInfo lightsInfo{lightsBuffer, 0, lightsBytes > 0 ? lightsBytes : VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo countsInfo{ctx.buffer("clusterTrueCounts"), 0, VK_WHOLE_SIZE};
            std::array<VkWriteDescriptorSet, 2> writes{storageWrite(countSet1_, 0, lightsInfo),
                                                          storageWrite(countSet1_, 1, countsInfo)};
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, countKernel_->pso.pipeline);
            std::array<VkDescriptorSet, 2> sets{countSet0_, countSet1_};
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
        .setExecute([this, gpuGrid](PassContext& ctx) {
            const VkDescriptorBufferInfo trueCountsInfo{ctx.buffer("clusterTrueCounts"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo offsetsInfo{ctx.buffer("clusterOffsets"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo writeCountsInfo{ctx.buffer("clusterWriteCounts"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo perFroxelOverflowInfo{ctx.buffer("clusterPerFroxelOverflow"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo globalOverflowInfo{ctx.buffer("clusterGlobalOverflow"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo totalUsedInfo{ctx.buffer("clusterTotalUsed"), 0, VK_WHOLE_SIZE};
            std::array<VkWriteDescriptorSet, 6> writes{
                storageWrite(prefixSumSet1_, 0, trueCountsInfo),         storageWrite(prefixSumSet1_, 1, offsetsInfo),
                storageWrite(prefixSumSet1_, 2, writeCountsInfo),        storageWrite(prefixSumSet1_, 3, perFroxelOverflowInfo),
                storageWrite(prefixSumSet1_, 4, globalOverflowInfo),     storageWrite(prefixSumSet1_, 5, totalUsedInfo)};
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prefixSumKernel_->pso.pipeline);
            std::array<VkDescriptorSet, 2> sets{prefixSumSet0_, prefixSumSet1_};
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
        .setExecute([this, gpuGrid, lightsBuffer, lightsBytes, froxelCount](PassContext& ctx) {
            const VkDescriptorBufferInfo lightsInfo{lightsBuffer, 0, lightsBytes > 0 ? lightsBytes : VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo offsetsInfo{ctx.buffer("clusterOffsets"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo writeCountsInfo{ctx.buffer("clusterWriteCounts"), 0, VK_WHOLE_SIZE};
            const VkDescriptorBufferInfo indicesInfo{ctx.buffer("clusterLightIndices"), 0, VK_WHOLE_SIZE};
            std::array<VkWriteDescriptorSet, 4> writes{
                storageWrite(scatterSet1_, 0, lightsInfo), storageWrite(scatterSet1_, 1, offsetsInfo),
                storageWrite(scatterSet1_, 2, writeCountsInfo), storageWrite(scatterSet1_, 3, indicesInfo)};
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatterKernel_->pso.pipeline);
            std::array<VkDescriptorSet, 2> sets{scatterSet0_, scatterSet1_};
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scatterKernel_->pso.layout, 0,
                                      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
            const auto& range = scatterKernel_->layoutInfo.pushRanges[0];
            vkCmdPushConstants(ctx.cmd, scatterKernel_->pso.layout, range.stages, range.offset, range.size, &gpuGrid);
            vkCmdDispatch(ctx.cmd, groupCount1D(froxelCount, 64), 1, 1);
        });

    return grid;
}

}  // namespace rx::cluster
