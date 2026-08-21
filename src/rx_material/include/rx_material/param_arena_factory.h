#pragma once
// rx_material/param_arena_factory.h -- [Phase 5 Task 5, ticket #41 row 1]
// createDemandSizedMaterialParamArena(): the demand-sized "material set-1
// params" descriptor-set-LAYOUT + rx::rhi::DescriptorArena bundle promoted
// from samples/08_gltf_viewer's and samples/09_scene's own byte-identical
// hand-rolled `createMaterialParamArena()` free functions (both halves --
// the VkDescriptorSetLayoutBinding/vkCreateDescriptorSetLayout block AND
// the DescriptorArena::Capacities sizing -- were duplicated verbatim, 09's
// own comment pointing back at 08's "for the full rationale").
//
// NAMING (matrix-p5t05 Open Questions #1): deliberately NOT spelled with
// "ParamArena" anywhere in it -- `rx::material::ParamArena` (instance.h) is
// a DIFFERENT abstraction (an instance-blob-to-descriptor-set BINDER, with
// its own per-frame bump-allocated byte arena) that this factory shares no
// code with; sharing a name would invite a future reader to conflate the
// two. This is a much smaller thing: a raw, demand-sized
// `rx::rhi::DescriptorArena` (rx_rhi_vk/descriptor_arena.h) plus the ONE
// set-layout shape every caller of it in this codebase needs (binding 0,
// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count 1, VERTEX|FRAGMENT).
//
// SCOPE: this is deliberately narrower than a general-purpose descriptor-
// layout builder -- it exists to close exactly the one duplicated pattern
// the gate matrix found (a single-UBO-binding, materialCount-sized set-1
// pool), not to become a generic arena factory. A future caller needing a
// genuinely different set-1 shape should build its own layout +
// DescriptorArena::create() call directly, exactly as this file's own
// implementation does.
#include <volk.h>

#include <rx_rhi_vk/descriptor_arena.h>

#include <cstdint>
#include <optional>

namespace rx::material {

// The bundle createDemandSizedMaterialParamArena() returns. Move-only
// (DescriptorArena itself is move-only -- see descriptor_arena.h).
//
// TEARDOWN CONTRACT: this struct owns NEITHER handle via RAII -- `arena`'s
// own destructor (rx::rhi::DescriptorArena::~DescriptorArena()) DOES
// destroy its underlying VkDescriptorPool(s) when this bundle (or the
// std::optional<DemandSizedMaterialParamArena> wrapping it) is destroyed/
// reset, exactly like every existing caller's std::optional<DescriptorArena>
// field already relied on -- but `setLayout` is a bare VkDescriptorSetLayout
// handle the caller must destroy itself (vkDestroyDescriptorSetLayout)
// before the owning VkDevice is destroyed, matching every existing
// VkDescriptorSetLayout field this codebase's App structs already manage
// the same way (there is no RAII wrapper for a lone VkDescriptorSetLayout
// anywhere in rx_rhi_vk, and inventing one is out of this factory's scope).
struct DemandSizedMaterialParamArena {
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    rx::rhi::DescriptorArena arena;
};

// Builds the material set-1 param descriptor-set LAYOUT (one
// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER binding at binding 0, visible to
// VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT -- the ONE
// shape every real caller in this codebase needs) and a
// `rx::rhi::DescriptorArena` sized EXACTLY from `materialCount` -- the real
// number of set-1 VkDescriptorSets the caller will ever allocate from it
// (built at framesInFlight=1: this pool is meant to be allocated once per
// run and never reset, since set-1 material params are static for a
// material's lifetime, unlike a per-instance, per-frame arena such as
// rx::material::ParamArena above). `materialCount == 0` is clamped to 1 --
// `rx::rhi::DescriptorArena::create()` rejects a zero capacity outright,
// and a caller with an empty-material scene still needs a valid (if
// never-used) arena for its own unconditional teardown path.
//
// Returns std::nullopt (logged via RX_LOG_ERROR, naming `materialCount`) on
// either the `vkCreateDescriptorSetLayout` call or the underlying
// `DescriptorArena::create()` call failing -- on the latter failure, the
// set layout this call already created is destroyed before returning, so a
// caller checking `has_value()` never leaks it.
[[nodiscard]] std::optional<DemandSizedMaterialParamArena> createDemandSizedMaterialParamArena(VkDevice device,
                                                                                                  uint32_t materialCount);

}  // namespace rx::material
