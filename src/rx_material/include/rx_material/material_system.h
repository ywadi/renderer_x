#pragma once
// volk.h (not vulkan/vulkan.h) -- matching every other rx_rhi_vk public
// header's own convention (pipeline_layout.h, bindless.h, device.h):
// VkPipeline/VkPipelineLayout/VkDevice below are opaque handles, not
// live volk-loaded function pointers, so this header itself never calls a
// raw vkFoo -- only material_system.cpp does.
#include <volk.h>

#include <rx_core/handle.h>
#include <rx_graph/pass_signature.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_shader/shader_layout_info.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace rx::rhi {
class Device;
}  // namespace rx::rhi

namespace rx::material {

// PassSignature genuinely lives in rx::graph (pass_signature.h) -- see that
// header's own comment for why (it is derived purely from compiled
// RenderGraph pass state, and rx_material already depends on rx_graph,
// never the reverse; putting it here would create the exact dependency
// cycle that comment describes). This alias exists so every rx_material
// caller (and this header's own PipelineRequest below) can spell it
// without reaching into rx::graph directly [Task 5 brief].
using PassSignature = rx::graph::PassSignature;

// A generational handle into MaterialSystem's own internal material
// registry -- `rx::core::Handle<Tag>`'s established per-resource-kind-tag
// pattern (see rx_rhi_vk/bindless.h's BindlessHandle for the same idiom
// applied to BindlessTable's three resource classes), here with exactly
// one tag/one registry since MaterialSystem tracks only one kind of thing.
// Default-constructed handles are invalid (`isValid() == false`); every
// handle actually returned by loadMaterial() has a real generation.
using MaterialHandle = rx::core::Handle<struct MaterialTag>;

// The full key identifying one lazily-compiled `VkPipeline` variant [spec
// Phase 3 design, D7]: which material, which render-graph pass shape, and
// (reserved -- always 0 in Phase 3 core; see D8) which orthogonal
// specialization axis bits. `getPipeline()` derives the actual cache key
// from `material`'s own content hash (not the handle's index/generation,
// which is process-local and meaningless across a hot-reload rebuild) +
// `pass.hash()` + `specializationBits` verbatim.
struct PipelineRequest {
    MaterialHandle material;
    PassSignature pass;

    // D8: reserved for a future orthogonal specialization-constant bitmask
    // (Filament-style independent shading axes -- skinning, fog, ...) --
    // always 0 in Phase 3's material core, but already part of the cache
    // key so a future axis doesn't need an ABI/cache-format break.
    uint32_t specializationBits = 0;
};

class MaterialSystem;

namespace detail {

// Test-only seam -- NOT part of the stable public contract, exactly the
// same carve-out convention rx_graph's own `detail::debugLastFrameFinalStages()`
// establishes (executor.h): exposed purely so
// test_material_system.cpp's "cache-hit" case can assert that a SECOND
// getPipeline() call for an already-cached (material, pass, specialization)
// key performs no additional Slang compile+link work -- there is no other
// way to observe that from outside MaterialSystem's own implementation
// (this codebase enables no automated way to intercept a slang::ISession
// call from a test, and spdlog capture would be a strictly noisier,
// string-matching-fragile substitute for the same fact). Counts every
// composite+link ATTEMPT this MaterialSystem has issued (loadMaterial()
// calls that reached the link step, successful or not) -- never
// incremented by getPipeline(), which only ever builds a VkPipeline from
// SPIR-V a prior loadMaterial() call already produced and cached.
[[nodiscard]] uint64_t debugCompileCount(const MaterialSystem& system);

}  // namespace detail

// Materials as Slang modules, reflection-driven pipeline layouts, and the
// lazy content-hash-keyed `VkPipeline` variant cache [spec Phase 3 design,
// D6-D8]. Owns one `slang::ISession` (created once in create(), reused for
// every loadMaterial() call on this instance -- see material_system.cpp's
// own header comment for why this drives Slang's raw session/module/
// composite/link API directly rather than through rx_shader::Compiler's
// narrower, single-module-shaped public surface) and one `VkPipelineCache`,
// loaded from / saved to `pipelineCachePath` across runs [D7].
//
// Not copyable or movable (owns live VkDevice-scoped Vulkan objects --
// shader modules, descriptor/pipeline layouts, pipelines, a pipeline
// cache -- a shallow copy/move could not safely duplicate or hand off);
// always held via the std::unique_ptr create() returns, matching
// rx::graph::Executor's and rx::graph::RenderGraph's own established
// pattern in this codebase.
//
// Not internally synchronized -- like rx::graph::RenderGraph/Executor,
// callers using one MaterialSystem instance from more than one thread must
// serialize their own access to it. This is a deliberate, explicit scoping
// decision consistent with this codebase's existing device-object
// wrappers, not an oversight.
class MaterialSystem {
public:
    // Saves this instance's VkPipelineCache to the path passed to create()
    // (best-effort -- a write failure is logged, never fatal, mirroring
    // create()'s own best-effort LOAD of that same file), then destroys
    // every VkPipeline/VkShaderModule/VkDescriptorSetLayout/VkPipelineLayout/
    // VkPipelineCache this instance owns. Waits for the device to go idle
    // first (matching rx::graph::Executor::~Executor()'s own
    // vkDeviceWaitIdle-before-destroying-anything discipline).
    ~MaterialSystem();
    MaterialSystem(const MaterialSystem&) = delete;
    MaterialSystem& operator=(const MaterialSystem&) = delete;
    MaterialSystem(MaterialSystem&&) = delete;
    MaterialSystem& operator=(MaterialSystem&&) = delete;

    // Builds a MaterialSystem against `device`'s already-created VkDevice
    // (Device::create() has already enabled every descriptor-indexing
    // feature BindlessTable/this class's own external-set-0 pipelines
    // need -- see rx_rhi_vk/device.cpp) and `bindless`, which the returned
    // MaterialSystem does NOT take ownership of: `bindless` must outlive
    // this MaterialSystem and every VkPipeline it builds (its
    // `descriptorSetLayout()` becomes set 0 of every material pipeline
    // layout, exactly like PipelineLayoutBuilder's own externalSet0
    // contract -- pipeline_layout.h). Loads an existing VkPipelineCache
    // from `pipelineCachePath` if the file exists and is readable;
    // an unreadable or corrupt file is logged as a warning and this still
    // succeeds with a fresh, empty cache [Task 5 ambiguity resolution] --
    // Vulkan itself guarantees vkCreatePipelineCache never rejects
    // malformed initial data outright (a driver must fall back to
    // treating it as absent), so the only failure this needs to guard is
    // the file READ step, not the blob's own internal structure.
    //
    // Returns nullptr (logged via RX_LOG_ERROR) if Slang session creation
    // fails, if `shaders/material/material.slang`/`forward_entry.slang`
    // cannot be read or fail to compile (an engine-authoring bug, not a
    // per-material one -- these two files are fixed and shared across
    // every material this MaterialSystem will ever load), or if any
    // underlying Vulkan object creation fails.
    static std::unique_ptr<MaterialSystem> create(rx::rhi::Device& device, rx::rhi::BindlessTable& bindless,
                                                    const std::filesystem::path& pipelineCachePath);

    // Loads the Slang module at `slangModulePath` as a material: reads its
    // bytes (moduleHash() below), compiles+composes+links it against the
    // shared forward_entry.slang entry points [D6, and see
    // shaders/material/forward_entry.slang's own header comment for the
    // exact link-time-specialization mechanism], reflects its
    // `ParameterBlock<TParams> gParams` global into a
    // `rx::shader::ShaderLayoutInfo` (layoutInfo() below), and builds this
    // material's `VkPipelineLayout` (pipelineLayout() below) against
    // `bindless`'s set-0 layout -- all of this happens here, eagerly, not
    // lazily deferred to getPipeline(): a material's own layout/reflection
    // never depends on which render-graph pass will later request a
    // pipeline for it, only on the module's own content, so there is
    // nothing to gain by deferring it, and layoutInfo()/pipelineLayout()
    // below must already have a real answer immediately after this
    // returns (both are exercised that way by
    // test_material_system.cpp's "load-reflect" case).
    //
    // Throws std::runtime_error, with Slang's own diagnostic text folded
    // into the exception message, on: a module load/compile/link failure
    // (the module has a syntax error, or fails to conform to
    // IMaterialShader); a module that does not declare exactly one
    // top-level `ParameterBlock<...>` global at descriptor set 1 (this
    // engine's fixed material-parameters convention -- see
    // material_system.cpp's own comment on exactly which Slang reflection
    // call actually reports that parameter's real descriptor set, and why
    // it is not the one rx_shader::reflect() itself relies on for an
    // ordinary flat resource global); or a
    // PipelineLayoutBuilder::build()/vkCreateShaderModule failure.
    MaterialHandle loadMaterial(const std::filesystem::path& slangModulePath);

    // Content hash of `handle`'s module source bytes, as loaded --
    // FNV-1a-64, matching pass_signature.h's own algorithm choice for the
    // same "small, canonical, no existing rx_core utility" reasons (see
    // that header's comment; checked before writing this too). This is the
    // material half of getPipeline()'s cache key [D7, D9]: a hot-reload
    // that changes a module's bytes changes this value, which is exactly
    // what scopes a future reload's cache invalidation to the (material,
    // pass) entries actually derived from the changed module (D9) --
    // though the reload path itself is not part of Task 5's scope (no
    // reload test exists in this task's test list; see material_system.cpp
    // for the fresh-Compiler/fresh-session-per-reload caveat this would
    // need to honor when that lands).
    //
    // Throws std::out_of_range for an invalid/unknown `handle`, matching
    // rx::graph::PassContext's own resolvers' contract for the same class
    // of caller error.
    [[nodiscard]] uint64_t moduleHash(MaterialHandle handle) const;

    // Lazily compiles (builds a real VkPipeline from `req.material`'s
    // already-linked SPIR-V; never re-invokes Slang -- see
    // detail::debugCompileCount()'s own comment) and caches one
    // `VkPipeline` per unique (material content hash, pass signature hash,
    // specializationBits) key [D7]; a repeated call with an
    // already-cached key returns the same handle immediately. Every
    // VkPipeline this creates uses fixed rasterization/multisample/
    // blend/depth-stencil state (opaque, no culling, depth test+write
    // enabled iff `req.pass.depthFormat != VK_FORMAT_UNDEFINED`) and the
    // fixed position/normal/uv vertex-input layout
    // forward_entry.slang's own header comment documents -- there are no
    // other fixed-function axes for a Phase 3 material pipeline to vary
    // on, so none of that state is itself part of the cache key.
    //
    // Throws std::out_of_range for an invalid/unknown `req.material`.
    // Throws std::runtime_error if `req.pass` declares neither a color nor
    // a depth attachment (colorCount == 0 && depthFormat ==
    // VK_FORMAT_UNDEFINED) -- there is no Phase 3 use case for a graphics
    // pipeline with no attachment output at all [Task 5 ambiguity
    // resolution], or if the underlying vkCreateGraphicsPipelines call
    // fails.
    VkPipeline getPipeline(const PipelineRequest& req);

    // `handle`'s VkPipelineLayout, reflection-driven from its own
    // `ParameterBlock<TParams> gParams` (set 1) plus whatever set-0
    // bindings it directly declares (if any -- see material_system.cpp;
    // Phase 3's own test material never declares any, routing textures
    // through `gParams`'s bindless-table indices instead per D8), built
    // against the external bindless set-0 layout passed to create().
    // Throws std::out_of_range for an invalid/unknown `handle`.
    [[nodiscard]] VkPipelineLayout pipelineLayout(MaterialHandle handle) const;

    // `handle`'s reflected layout -- the same `rx::shader::ShaderLayoutInfo`
    // pipelineLayout()'s VkPipelineLayout was built from, exposed for a
    // caller that needs the raw set/binding/type shape itself (e.g. to
    // allocate and populate the real set-1 VkDescriptorSet a
    // ParameterBlock needs -- IMaterialInstance's job, not this task's).
    // Throws std::out_of_range for an invalid/unknown `handle`.
    [[nodiscard]] const rx::shader::ShaderLayoutInfo& layoutInfo(MaterialHandle handle) const;

    // Forward-declared publicly, defined only in material_system.cpp --
    // the exact same "nameable but not constructible/reachable outside
    // this class's own member functions" pattern rx::graph::Executor::Impl
    // establishes (executor.h's own comment on that struct explains why:
    // C++ access control is per-translation-unit-blind, so a handful of
    // free helper functions in material_system.cpp's own anonymous
    // namespace need to be able to NAME `Impl&` even though nothing
    // outside this class can ever construct or otherwise reach a real one
    // through this public surface).
    struct Impl;

private:
    friend uint64_t detail::debugCompileCount(const MaterialSystem&);

    explicit MaterialSystem(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace rx::material
