#pragma once
#include <volk.h>

#include <glm/glm.hpp>
#include <rx_rhi_vk/pipeline_layout.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace rx::rhi {
class Device;
class BindlessTable;
}  // namespace rx::rhi

namespace rx::shadow {

// One row of the shadow-caster vertex shader's bindless per-instance
// StructuredBuffer -- mirrors shaders/shadow/shadow_caster.vert.slang's
// `RxShadowDrawData` EXACTLY (D26.1's own cross-file-drift-risk pattern,
// matching rx_material/draw_data.h's identical convention for its own
// GPU-side counterpart). `SV_VulkanInstanceID`-addressed, NEVER a
// push-constant transformIndex [D26.1, gate ruling RC1/#8's own verified
// Slang/SPIR-V pitfall, restated here for this ticket's own shadow-caster
// shader]. Both fields are transposed (GLM column-major -> this project's
// Slang sessions' ROW-major default, see rx_shader/compiler.h's own
// MATRIX LAYOUT comment) before being written into a real row -- callers
// building a ShadowDrawData row must `glm::transpose()` both matrices
// themselves (this struct does not do it for them, matching
// rx::material::DrawDataGpu's own established convention).
struct ShadowDrawData {
    glm::mat4 model{1.0F};          // object -> world. Transposed before upload.
    glm::mat4 lightViewProj{1.0F};  // world -> light clip, STANDARD-Z [D13]. Transposed before upload; per-pass, repeated per row (same "simpler than a second buffer" precedent RxDrawData's own header comment establishes).
};
static_assert(sizeof(ShadowDrawData) == 128,
              "ShadowDrawData must stay exactly 128 bytes -- mirrors shadow_caster.vert.slang's RxShadowDrawData");

// Mirrors shadow_caster.vert.slang's `PushConstants` field-for-field.
struct ShadowCasterPushConstants {
    uint32_t drawDataBufferIndex = 0;  // bindless STORAGE BUFFER index of the ShadowDrawData[] this draw reads.
};
static_assert(sizeof(ShadowCasterPushConstants) == 4, "ShadowCasterPushConstants must stay exactly 4 bytes");

struct ShadowCasterPipelineDesc {
    // The shadow map's own depth format -- D32_SFLOAT is this project's
    // existing convention (sample 05's own kDepthFormat), kept as the
    // default rather than re-hardcoded, per the gate's own "resolution
    // exposed as an overridable parameter, not a re-hardcoded literal"
    // acceptance criterion.
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    // Test/override seam: when unset (default), depthClampEnable follows
    // `device.supportsDepthClamp()` automatically (the real, production
    // behavior). Forcing this to `true` on a device that does not
    // actually have the feature enabled is rejected (nullptr, logged) --
    // the exact VUID this class exists to avoid. Exists primarily so this
    // library's own GPU tests can build a "clamp OFF" pipeline variant on
    // a device that DOES support the feature, to demonstrate the gate's
    // own required regression contrast ("a regression variant with clamp
    // disabled demonstrates the defect this setting fixes") without
    // depending on CI happening to run on clamp-less hardware.
    std::optional<bool> depthClampEnableOverrideForTesting;
};

// The scene-path shadow-caster pipeline [D21, gate ruling RC3]: built
// OUTSIDE `rx::material::MaterialSystem` entirely -- a depth-only
// pipeline with its own minimal shader/layout, never reusing
// MaterialSystem's `getPipeline()` cache or its `RxDrawData`/`material.
// slang` machinery (RC3: "the scene-path shadow-caster pass is a
// depth-only pipeline built OUTSIDE MaterialSystem... `getPipeline()`
// serves only reversed-Z main-camera pipelines from Stage 2 on -- no
// compare-op axis is built in Phase 4").
//
// STANDARD-Z, NOT REVERSED [D13, required code comment per the gate's own
// "wrong-fix-prevention" row]: this pipeline's `depthCompareOp` is
// `VK_COMPARE_OP_LESS` -- shadow maps stay STANDARD-Z in Phase 4
// (D13's own text: "ortho depth is less precision-critical; bias tuning
// is calibrated for it; the cascades work in the techniques phase
// revisits"). An implementer aware that D13's MAIN-camera migration lands
// in this SAME task could plausibly but WRONGLY "fix" this pipeline's
// compare op to GREATER_OR_EQUAL to match -- DO NOT: only the main
// camera reverses. See create()'s own implementation for the matching
// depth-bias-sign comment (the same non-flip applies to
// `vkCmdSetDepthBias`'s constant/slope factors -- see bindAndSetDepthBias()).
//
// DYNAMIC DEPTH BIAS [D21]: `VK_DYNAMIC_STATE_DEPTH_BIAS` is part of this
// pipeline's dynamic state (the gate's own "creation-time detail the
// ticket omitted" row) -- `bindAndSetDepthBias()` issues the matching
// `vkCmdSetDepthBias()` call every time it binds this pipeline, so a
// caller never needs to (and cannot correctly) set it separately.
//
// DEPTH CLAMP [D21]: `depthClampEnable=VK_TRUE` when `device.
// supportsDepthClamp()` is true (VK_FALSE, logged, otherwise -- see that
// accessor's own header comment for the VUID this check exists to avoid)
// -- a caster whose geometry crosses the light's own near plane still
// casts its full silhouette instead of being clipped away.
//
// Not copyable or movable (owns live VkDevice-scoped Vulkan objects),
// matching rx::material::MaterialSystem's own established pattern; always
// held via the std::unique_ptr create() returns.
//
// Thread-affinity (D5): create() and destruction are main-thread-only
// (ordinary Vulkan object lifetime rules); pipeline()/pipelineLayout()/
// bindAndSetDepthBias() are read-only/record-time and may be called from
// any thread already permitted to record into `cmd` (matching every other
// record-time helper in this codebase -- no internal mutable state).
class ShadowCasterPipeline {
public:
    ~ShadowCasterPipeline();
    ShadowCasterPipeline(const ShadowCasterPipeline&) = delete;
    ShadowCasterPipeline& operator=(const ShadowCasterPipeline&) = delete;
    ShadowCasterPipeline(ShadowCasterPipeline&&) = delete;
    ShadowCasterPipeline& operator=(ShadowCasterPipeline&&) = delete;

    // Builds this pipeline against `device`'s already-created VkDevice and
    // `bindless`'s own external-set-0 layout (this shader's SPIR-V
    // reflects exactly ONE set-0 binding -- the storage-buffer slot,
    // `kStorageBufferBinding` -- a strict SUBSET of BindlessTable's three
    // fixed slots, which `PipelineLayoutBuilder::build()`'s externalSet0
    // substitution already supports with no changes; `bindless` must
    // outlive this pipeline and every draw recorded against it, same
    // lifetime contract as MaterialSystem's own `bindless` parameter).
    //
    // `shaderDir` defaults to an empty path, meaning "use the compile-time
    // RX_SHADOW_SHADER_DIR" (this repository's own checked-out
    // shaders/shadow/ directory, matching MaterialSystem::create()'s own
    // `sharedShaderDir` default-resolution precedent) -- every test in
    // this library's own tests/ relies on this default, unchanged.
    //
    // Returns nullptr (logged via RX_LOG_ERROR) on any compile/reflect/
    // Vulkan-object-creation failure.
    static std::unique_ptr<ShadowCasterPipeline> create(rx::rhi::Device& device, rx::rhi::BindlessTable& bindless,
                                                          const ShadowCasterPipelineDesc& desc = {},
                                                          const std::filesystem::path& shaderDir = {});

    [[nodiscard]] VkPipeline pipeline() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout pipelineLayout() const { return layoutBundle_.layout; }

    // True iff this pipeline was built with `depthClampEnable=VK_TRUE`
    // (i.e. `device.supportsDepthClamp()` was true at create() time) --
    // exposed so a caller/test can assert WHICH of the two device-feature
    // branches actually ran, rather than inferring it indirectly.
    [[nodiscard]] bool depthClampEnabled() const { return depthClampEnabled_; }

    // Binds this pipeline and issues the matching `vkCmdSetDepthBias()`
    // call [D21's own slope-scaled bias, `constantFactor`/`slopeFactor`
    // computed by the caller from its own scene-specific tuning --
    // this class does not choose a bias VALUE, only wires the mechanism].
    // `clamp` requires the `depthBiasClamp` device feature (or must stay
    // 0.0F, its own default) per Vulkan's own documented constraint on
    // that parameter -- NOT checked/enabled by this class (out of this
    // ticket's own scope: `depthClampEnable` and `depthBiasClamp` are two
    // independent Vulkan features despite the similar name).
    //
    // Deliberately does NOT bind set 0 (the bindless table) or push the
    // per-draw buffer index -- a caller issuing multiple draws across
    // several pipelines in one pass binds set 0 once, per the same
    // set-compatibility precedent every other bindless consumer in this
    // codebase already relies on (see e.g. samples/08_gltf_viewer's own
    // recordSceneDraws()); pushing `ShadowCasterPushConstants` is the
    // caller's own per-draw-buffer-registration concern.
    void bindAndSetDepthBias(VkCommandBuffer cmd, float depthBiasConstantFactor, float depthBiasSlopeFactor,
                              float depthBiasClamp = 0.0F) const;

private:
    ShadowCasterPipeline() = default;

    VkDevice device_ = VK_NULL_HANDLE;
    VkShaderModule vertModule_ = VK_NULL_HANDLE;
    // Owns this pipeline's VkPipelineLayout (and, in principle, any
    // non-zero-set VkDescriptorSetLayouts this shader might declare --
    // this shader declares none: its only set-0 binding is externally
    // substituted from `bindless`, per create()'s own comment, so
    // `layoutBundle_.setLayouts` holds exactly one entry, NOT owned by
    // this bundle -- see PipelineLayoutBundle's own class comment).
    rx::rhi::PipelineLayoutBundle layoutBundle_;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    bool depthClampEnabled_ = false;
};

}  // namespace rx::shadow
