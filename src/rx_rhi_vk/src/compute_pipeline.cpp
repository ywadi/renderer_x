#include <rx_rhi_vk/compute_pipeline.h>

#include <rx_core/log.h>

#include <fstream>
#include <utility>

namespace rx::rhi {

namespace {

// FNV-1a 64-bit over `spirv`'s own words -- same algorithm/rationale as
// rx::graph::PassSignature::hash()'s own inline implementation
// (rx_graph/include/rx_graph/pass_signature.h): no existing shared utility
// for this in rx_core (checked before writing this), and the algorithm is
// small, canonical, and fully specified by its two well-known 64-bit
// constants -- not the kind of "nontrivial subsystem" this repo's
// ready-made-library-first policy targets.
uint64_t hashSpirv(std::span<const uint32_t> spirv) {
    constexpr uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kPrime = 0x100000001b3ULL;

    uint64_t h = kOffsetBasis;
    for (uint32_t word : spirv) {
        for (int byteIndex = 0; byteIndex < 4; ++byteIndex) {
            h ^= static_cast<uint64_t>((word >> (byteIndex * 8)) & 0xFF);
            h *= kPrime;
        }
    }
    return h;
}

}  // namespace

std::optional<ComputePipelineCache> ComputePipelineCache::create(VkDevice device,
                                                                    std::filesystem::path pipelineCachePath) {
    // --- Load if present, fresh+empty otherwise -- identical policy to
    // rx_material::MaterialSystem::create()'s own pipeline-cache load: an
    // unreadable/corrupt file is a logged warning, never fatal. ------------
    std::vector<char> initialCacheData;
    std::error_code existsError;
    if (std::filesystem::exists(pipelineCachePath, existsError) && !existsError) {
        std::ifstream in(pipelineCachePath, std::ios::binary | std::ios::ate);
        if (!in) {
            RX_LOG_WARN("rx_rhi_vk: ComputePipelineCache: could not open existing pipeline cache '{}'; starting "
                        "with a fresh cache",
                        pipelineCachePath.string());
        } else {
            const std::streamoff size = in.tellg();
            in.seekg(0, std::ios::beg);
            if (size > 0) {
                initialCacheData.resize(static_cast<size_t>(size));
                in.read(initialCacheData.data(), size);
                if (!in) {
                    RX_LOG_WARN("rx_rhi_vk: ComputePipelineCache: failed reading existing pipeline cache '{}'; "
                                "starting with a fresh cache",
                                pipelineCachePath.string());
                    initialCacheData.clear();
                } else {
                    RX_LOG_INFO("rx_rhi_vk: ComputePipelineCache: loading {} bytes of pipeline cache data from '{}'",
                                initialCacheData.size(), pipelineCachePath.string());
                }
            }
        }
    } else {
        RX_LOG_INFO("rx_rhi_vk: ComputePipelineCache: no existing pipeline cache at '{}'; starting fresh",
                    pipelineCachePath.string());
    }

    VkPipelineCacheCreateInfo cacheCreateInfo{};
    cacheCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cacheCreateInfo.initialDataSize = initialCacheData.size();
    cacheCreateInfo.pInitialData = initialCacheData.empty() ? nullptr : initialCacheData.data();

    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(device, &cacheCreateInfo, nullptr, &pipelineCache) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_rhi_vk: ComputePipelineCache: vkCreatePipelineCache failed");
        return std::nullopt;
    }

    ComputePipelineCache result;
    result.device_ = device;
    result.pipelineCachePath_ = std::move(pipelineCachePath);
    result.pipelineCache_ = pipelineCache;
    return result;
}

std::optional<ComputePipelineCache::Pipeline> ComputePipelineCache::getOrCreate(
    std::span<const uint32_t> spirv, const rx::shader::ShaderLayoutInfo& layoutInfo,
    VkDescriptorSetLayout externalSet0) {
    const uint64_t key = hashSpirv(spirv);

    auto it = entries_.find(key);
    if (it != entries_.end()) {
        return Pipeline{it->second.pipeline, it->second.layoutBundle.layout, it->second.layoutBundle.setLayouts};
    }

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirv.size() * sizeof(uint32_t);
    moduleInfo.pCode = spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_rhi_vk: ComputePipelineCache::getOrCreate: vkCreateShaderModule failed");
        return std::nullopt;
    }

    auto layoutBundle = PipelineLayoutBuilder::build(device_, layoutInfo, externalSet0);
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("rx_rhi_vk: ComputePipelineCache::getOrCreate: PipelineLayoutBuilder::build failed");
        vkDestroyShaderModule(device_, module, nullptr);
        return std::nullopt;
    }

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = module;
    // Slang's SPIR-V backend always names each entry point's OpEntryPoint
    // "main" regardless of source-level function name -- same verified
    // finding rx_material::MaterialSystem::getPipeline()'s own identical
    // "main" literal cites (material_system.cpp).
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = layoutBundle->layout;
    pipelineInfo.basePipelineIndex = -1;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_rhi_vk: ComputePipelineCache::getOrCreate: vkCreateComputePipelines failed");
        vkDestroyShaderModule(device_, module, nullptr);
        return std::nullopt;
    }

    Entry entry;
    entry.module = module;
    entry.layoutBundle = std::move(*layoutBundle);
    entry.pipeline = pipeline;

    auto [inserted, ok] = entries_.emplace(key, std::move(entry));
    (void)ok;
    return Pipeline{inserted->second.pipeline, inserted->second.layoutBundle.layout,
                     inserted->second.layoutBundle.setLayouts};
}

ComputePipelineCache::ComputePipelineCache(ComputePipelineCache&& other) noexcept : ComputePipelineCache() {
    *this = std::move(other);
}

ComputePipelineCache& ComputePipelineCache::operator=(ComputePipelineCache&& other) noexcept {
    if (this != &other) {
        destroyAll();

        device_ = other.device_;
        pipelineCachePath_ = std::move(other.pipelineCachePath_);
        pipelineCache_ = other.pipelineCache_;
        entries_ = std::move(other.entries_);

        other.device_ = VK_NULL_HANDLE;
        other.pipelineCache_ = VK_NULL_HANDLE;
        other.entries_.clear();
    }
    return *this;
}

ComputePipelineCache::~ComputePipelineCache() {
    destroyAll();
}

void ComputePipelineCache::destroyAll() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    for (auto& [key, entry] : entries_) {
        (void)key;
        if (entry.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, entry.pipeline, nullptr);
        }
        if (entry.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, entry.module, nullptr);
        }
        entry.layoutBundle = PipelineLayoutBundle{};
    }
    entries_.clear();

    // Best-effort save -- never fatal, matching MaterialSystem's own
    // equally best-effort SAVE of its own pipeline-cache file
    // [Task 5 ambiguity resolution, reused here for the identical reason].
    if (pipelineCache_ != VK_NULL_HANDLE) {
        size_t dataSize = 0;
        vkGetPipelineCacheData(device_, pipelineCache_, &dataSize, nullptr);
        if (dataSize > 0) {
            std::vector<char> data(dataSize);
            const VkResult result = vkGetPipelineCacheData(device_, pipelineCache_, &dataSize, data.data());
            if (result == VK_SUCCESS) {
                std::ofstream out(pipelineCachePath_, std::ios::binary | std::ios::trunc);
                if (out) {
                    out.write(data.data(), static_cast<std::streamsize>(dataSize));
                    if (!out) {
                        RX_LOG_WARN("rx_rhi_vk: ComputePipelineCache: failed writing pipeline cache to '{}'",
                                    pipelineCachePath_.string());
                    } else {
                        RX_LOG_INFO("rx_rhi_vk: ComputePipelineCache: saved {} bytes of pipeline cache data to '{}'",
                                    dataSize, pipelineCachePath_.string());
                    }
                } else {
                    RX_LOG_WARN("rx_rhi_vk: ComputePipelineCache: could not open '{}' for writing pipeline cache",
                                pipelineCachePath_.string());
                }
            } else {
                RX_LOG_WARN("rx_rhi_vk: ComputePipelineCache: vkGetPipelineCacheData (data fetch) failed: "
                            "VkResult={}",
                            static_cast<int>(result));
            }
        } else {
            RX_LOG_WARN(
                "rx_rhi_vk: ComputePipelineCache: vkGetPipelineCacheData reported zero bytes; not writing "
                "pipeline cache file");
        }
        vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
    }
}

}  // namespace rx::rhi
