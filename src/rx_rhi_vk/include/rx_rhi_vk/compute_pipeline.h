#pragma once
#include <volk.h>
#include <rx_rhi_vk/pipeline_layout.h>
// The ONLY rx_shader header this file includes -- ShaderLayoutInfo alone,
// exactly like pipeline_layout.h's own header-hygiene rule (no Slang type,
// no slang::slang link dependency; see that header's own comment and
// rx_shader/CMakeLists.txt's `rx_shader_layout_types` INTERFACE target).
#include <rx_shader/shader_layout_info.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace rx::rhi {

// [Task 2 (#38), gate rulings: per-ticket ruling "compute as a parallel
// rx::rhi::ComputePipeline facility in rx_rhi_vk (NOT inside
// MaterialSystem)"; matrix row 8's decisive recommendation and Open
// Question 1's rationale] Builds + caches VkPipelines for a single compute
// entry point's already-compiled SPIR-V + reflected layout -- the compute
// counterpart to rx_material::MaterialSystem::getPipeline(), but living at
// rx_rhi_vk's own layer (BELOW rx_material, a peer every non-material
// compute consumer named in this plan -- e.g. Task 9's IBL bake, Task 14's
// froxel clustering -- can reach without depending on MaterialSystem's
// graphics/material-instance-specific machinery: a fixed 2-stage VS+FS
// array, blend/cull/depth-from-alphaMode fixed-function state, one
// hardcoded vertex-input layout, a material-instance param-arena bind --
// none of which a general compute kernel has any use for).
//
// Owns its own VkPipelineCache, using the SAME load-at-construction/
// save-at-destruction persistence discipline rx_material::MaterialSystem
// already establishes (material_system.cpp's create()/~MaterialSystem()):
// pattern reuse, not a literally shared cache instance/file [gate matrix
// row 9: the ticket's own "cached in the existing VkPipelineCache" is
// false as a literal claim -- no shared/accessible VkPipelineCache exists
// outside MaterialSystem's own private instance -- corrected here to "a
// VkPipelineCache using the same persistence discipline"].
//
// Cache key: an FNV-1a hash of the compiled SPIR-V's own words alone
// [gate matrix Open Question 4's decisive recommendation: "no PassSignature
// in the compute cache key... the entire reason PassSignature exists
// (keying MaterialSystem's graphics-pipeline cache on attachment SHAPE)
// does not apply -- a compute pipeline has no attachment state to vary
// over at all"] -- for a fixed compiled shader, its reflected
// ShaderLayoutInfo (and therefore its whole VkPipelineLayout shape) is a
// deterministic function of that same SPIR-V, so hashing the SPIR-V alone
// is sufficient; structurally SIMPLER than rx_material::PipelineKey, not a
// reuse of it.
//
// Thread-affinity (D5, Phase 4): create()/getOrCreate() are main-thread-only
// -- same GPU-object-mutation convention as MaterialSystem's own
// getPipeline() (RX_ASSERT_MAIN_THREAD) and every rx_rhi_vk factory method
// -- see docs/threading.md.
class ComputePipelineCache {
public:
    ~ComputePipelineCache();
    ComputePipelineCache(ComputePipelineCache&&) noexcept;
    ComputePipelineCache& operator=(ComputePipelineCache&&) noexcept;
    ComputePipelineCache(const ComputePipelineCache&) = delete;
    ComputePipelineCache& operator=(const ComputePipelineCache&) = delete;

    // Loads `pipelineCachePath` if it exists (best-effort -- an unreadable
    // or malformed file is a logged warning, never fatal, matching
    // MaterialSystem::create()'s identical policy: Vulkan itself guarantees
    // vkCreatePipelineCache never rejects malformed initialData outright),
    // otherwise starts fresh. Returns std::nullopt (logged) only on a real
    // vkCreatePipelineCache failure.
    static std::optional<ComputePipelineCache> create(VkDevice device, std::filesystem::path pipelineCachePath);

    struct Pipeline {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        // Borrowed -- owned by this cache's own Entry, not the caller;
        // valid for as long as this ComputePipelineCache is (mirrors
        // PipelineLayoutBundle::setLayouts' own "callers must not destroy
        // this" contract, pipeline_layout.h).
        std::span<const VkDescriptorSetLayout> setLayouts;
    };

    // Builds (compiling a fresh VkShaderModule + VkPipelineLayout via
    // rx::rhi::PipelineLayoutBuilder::build(), then vkCreateComputePipelines
    // against this cache's own VkPipelineCache) or returns the
    // already-cached VkPipeline for `spirv`'s content hash. `spirv` is a
    // single compute entry point's own compiled SPIR-V words (rx::shader::
    // CompileResult::entryPointCode[i].code for an entry point whose
    // `.stage == VK_SHADER_STAGE_COMPUTE_BIT`); `layoutInfo` is rx::shader::
    // reflect()'s output for that SAME compiled module. `externalSet0`
    // mirrors PipelineLayoutBuilder::build()'s own parameter -- pass the
    // engine's rx::rhi::BindlessTable::descriptorSetLayout() for a compute
    // shader that declares set 0 with the bindless shape (the RC-compatible
    // rationale this ticket's own GPU test proves end to end).
    //
    // Returns std::nullopt (logged) on any layout-build or
    // vkCreateComputePipelines failure -- nothing is cached on failure, so a
    // later retry (e.g. after fixing a shader) is not poisoned by a stale
    // failed entry.
    std::optional<Pipeline> getOrCreate(std::span<const uint32_t> spirv,
                                         const rx::shader::ShaderLayoutInfo& layoutInfo,
                                         VkDescriptorSetLayout externalSet0 = VK_NULL_HANDLE);

    VkPipelineCache pipelineCache() const { return pipelineCache_; }

private:
    ComputePipelineCache() = default;
    void destroyAll();

    struct Entry {
        VkShaderModule module = VK_NULL_HANDLE;
        PipelineLayoutBundle layoutBundle;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    VkDevice device_ = VK_NULL_HANDLE;
    std::filesystem::path pipelineCachePath_;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace rx::rhi
