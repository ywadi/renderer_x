#pragma once
// volk.h (not vulkan/vulkan.h) -- matching every other rx_rhi_vk public
// header's own convention (pipeline_layout.h, bindless.h, device.h):
// VkPipeline/VkPipelineLayout/VkDevice below are opaque handles, not
// live volk-loaded function pointers, so this header itself never calls a
// raw vkFoo -- only material_system.cpp does.
#include <volk.h>

#include <rx_core/handle.h>
#include <rx_graph/executor.h>
#include <rx_graph/pass_signature.h>
#include <rx_material/instance.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_shader/shader_layout_info.h>

#include <cstddef>
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
// `pass.hash()` + `specializationBits` verbatim + (D28, below) the
// material's own OWN fixed-function state, read from its MaterialRecord --
// PipelineRequest ITSELF gains no new field for this: see D28's own
// comment on AlphaMode/MaterialFixedFunctionState just below for why the
// fixed-function axis lives on the loaded material, not on this request.
struct PipelineRequest {
    MaterialHandle material;
    PassSignature pass;

    // D8: reserved for a future orthogonal specialization-constant bitmask
    // (Filament-style independent shading axes -- skinning, fog, ...) --
    // always 0 in Phase 3's material core, but already part of the cache
    // key so a future axis doesn't need an ABI/cache-format break.
    uint32_t specializationBits = 0;
};

// --- D28 (gate ruling RC1, 2026-08-18): the fixed-function pipeline-state
// axis -----------------------------------------------------------------
// glTF's `alphaMode`/`doubleSided` are `VkPipeline` FIXED-FUNCTION fields
// (blend enable, depth-write enable, cull mode) with zero SPIR-V
// representation -- NOT specialization constants (the plan's earlier
// "specialization bits gain alphaMode/doubleSided axes" phrasing is
// corrected by D22/D28/this gate ruling). This is the reusable mechanism
// future fixed-function-state needs (wireframe, stencil effects, ...)
// extends: a loaded material (MaterialRecord, material_system.cpp) carries
// this state, and it is folded into getPipeline()'s own pipeline-variant
// cache key alongside the module's content hash/pass hash/specialization
// bits -- so two materials built from the byte-identical .slang module but
// different AlphaMode/doubleSided values are two independent MaterialHandles
// (two loadMaterial() calls -- see the AlphaMode-taking overload below) that
// yield two independently-cached VkPipelines. `PipelineRequest` above is
// deliberately UNCHANGED: the state travels with the MaterialHandle, not
// the per-getPipeline()-call request, matching how the module's own content
// hash already travels with the handle rather than being passed at every
// getPipeline() call site.
//
// MASK's `alphaCutoff` is explicitly NOT part of this fixed-function state
// (it has no VkPipeline representation at all): it is an ordinary
// per-instance TParams uniform (ParameterBlock<TParams> gParams, D6/D8),
// read by an ALWAYS-PRESENT conditional discard in a material's own
// fragment shader code -- the same shader code path runs for every
// AlphaMode; only the DATA differs (OPAQUE/BLEND materials are expected to
// bind a cutoff of 0.0, which never trips the discard since alpha is never
// negative -- see standard_pbr.slang's own header comment). This keeps the
// discard branch itself out of MaterialSystem/D28 entirely.
enum class AlphaMode : uint8_t { Opaque, Mask, Blend };

// The fixed-function state D28 attaches to a loaded material. Default
// value (Opaque, single-sided) is BYTE-IDENTICAL to every getPipeline()
// call's pre-D28 hardcoded behavior (`material_system.cpp`'s old fixed
// `blendEnable=VK_FALSE`/`depthWriteEnable=hasDepth`/
// `cullMode=VK_CULL_MODE_BACK_BIT` literals) -- so every existing
// loadMaterial() call site (test fixtures, sample 06, the public ABI
// bridge) that does not pass one keeps behaving exactly as before.
struct MaterialFixedFunctionState {
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool doubleSided = false;

    bool operator==(const MaterialFixedFunctionState&) const = default;
};

// A generational handle into MaterialSystem's own internal texture
// registry [Task 7 coordinator addition 4] -- the same
// `rx::core::Handle<Tag>` idiom MaterialHandle above and BindlessHandle
// (rx_rhi_vk/bindless.h) both already use, here for the "one real,
// renderer-owned rx::rhi::Texture2D + BindlessTable registration" this
// engine's IRxTexture (rx_api.h) always wraps.
using TextureHandle = rx::core::Handle<struct TextureTag>;

// Input to MaterialSystem::createTexture2D() -- `format`/`generateMips`
// mirror rx_api.h's own RxTextureDesc one field at a time (that ABI struct
// converts its RxFormat enum to a real VkFormat and its int32_t flag to a
// real bool before calling down to this internal type; see api_impl.cpp's
// MaterialSystemImpl::createTexture2D()). `pixels`/`pixelBytes` are read
// synchronously during createTexture2D() (uploaded via this
// MaterialSystem's own internal rx::rhi::Uploader, flushed before that call
// returns) -- never retained past it.
struct TextureCreateInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    bool generateMips = false;
    const void* pixels = nullptr;
    size_t pixelBytes = 0;
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
//
// Thread-affinity (D5, Phase 4): loadMaterial()/getPipeline() (and every
// other method here) are main-thread-only -- see docs/threading.md.
// loadMaterial()/getPipeline()/reloadChanged()/bindInstance() carry a
// dev-time RX_ASSERT_MAIN_THREAD guard [Phase 4 Task 7 fix round 1] that
// fails loudly on a chunk >= 1 violation (the one contract a chunked pass
// can actually reach mid-frame, per spec D4's chunk-0 guarantee) instead of
// corrupting this system's unsynchronized internal state silently.
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
    //
    // `sharedShaderDir` [Task 8 ledger item carried from Task 5, resolved
    // here]: the directory `material.slang`/`forward_entry.slang` are read
    // from, AND the Slang session search path `import material;` (every
    // material module's own first line) resolves against -- both were
    // previously hardcoded to RX_MATERIAL_SHADER_DIR, a compile-time
    // absolute path baked in by src/rx_material/CMakeLists.txt, anchored at
    // this checked-out repository's own CMAKE_SOURCE_DIR. That is exactly
    // right for a build-tree run (dev builds, ctest, CI -- Task 5 through
    // Task 7's own tests all rely on it, unchanged, via the default below)
    // but is meaningless for a REDISTRIBUTED binary, which has no
    // CMAKE_SOURCE_DIR at all: samples/06_materials is the first caller
    // that ships as a standalone package (binary + its own materials/ dir +
    // these two shared files, deployed flat next to it -- see that
    // sample's own CMakeLists.txt/README), so it needs a real, runtime-
    // resolved directory (typically SDL_GetBasePath()-relative) instead of
    // this compile-time one.
    //
    // Defaults to an empty path, which means "use RX_MATERIAL_SHADER_DIR"
    // (this method's ORIGINAL, unchanged behavior) -- every existing
    // caller (test_material_system.cpp, test_api_factory.cpp,
    // test_api_contract.cpp, api_impl.cpp) keeps compiling and behaving
    // identically without passing this parameter at all. A non-empty path
    // is used verbatim (not validated against RX_MATERIAL_SHADER_DIR in any
    // way) as both the shared-module read directory and the session search
    // path; a caller passing one that does not actually contain
    // material.slang/forward_entry.slang gets the exact same "file could
    // not be read" nullptr-and-RX_LOG_ERROR failure this method already
    // documents for that case.
    //
    // `defaultSamplerAddressMode` [Fix round, sampler-wrap P0]: the wrap
    // mode of this instance's ONE process-wide default sampler --
    // material.slang's own `gMaterialGlobals.defaultSamplerIndex` /
    // `rx_sampleTexture`'s two-argument overload (material.slang's own
    // header comment has the full account of what this sampler is for
    // post-fix: a fallback for a material that does not wire its own
    // per-slot sampler, NOT the one sampler every texture slot samples
    // through -- that was the shipped defect this fix round closes).
    // Defaults to `VK_SAMPLER_ADDRESS_MODE_REPEAT`, glTF's OWN documented
    // default ("sampler unspecified" -> repeat wrapping) -- every existing
    // caller keeps compiling and gets the spec-correct default without
    // passing this parameter at all. Task 4's own seam-bleed GPU test
    // (test_api_factory.cpp, a deliberately non-tiled 2x2 test texture
    // sampled right up to its UV 0/1 edge) is the one caller that passes
    // `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE` explicitly here -- see that
    // test's own header comment for why CLAMP_TO_EDGE is still the right
    // choice for THAT specific non-tiling content, now requested
    // explicitly rather than inherited from a process-wide default that no
    // longer defaults to it.
    static std::unique_ptr<MaterialSystem> create(rx::rhi::Device& device, rx::rhi::BindlessTable& bindless,
                                                    const std::filesystem::path& pipelineCachePath,
                                                    const std::filesystem::path& sharedShaderDir = {},
                                                    VkSamplerAddressMode defaultSamplerAddressMode =
                                                        VK_SAMPLER_ADDRESS_MODE_REPEAT);

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
    //
    // [D28] `fixedFunctionState` is attached to the returned handle's own
    // MaterialRecord verbatim and folded into every subsequent
    // getPipeline() call's cache key for this handle (see
    // MaterialFixedFunctionState's own comment above) -- it is NOT
    // reflected from the module, so two loadMaterial() calls against the
    // SAME `slangModulePath` bytes with two different
    // MaterialFixedFunctionState values are two fully independent
    // MaterialHandles (independent MaterialRecords, independent
    // getPipeline() cache entries), by design: this is exactly how one
    // shared StandardPBR module backs many glTF materials that differ only
    // in alphaMode/doubleSided (each becomes its own loadMaterial() call).
    // Defaults to `MaterialFixedFunctionState{}` (Opaque, single-sided) --
    // see that struct's own comment for why this preserves every existing
    // call site's behavior byte-for-byte.
    MaterialHandle loadMaterial(const std::filesystem::path& slangModulePath,
                                 MaterialFixedFunctionState fixedFunctionState = {});

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

    // [D28] `handle`'s own fixed-function state, exactly as passed to the
    // loadMaterial() call that produced it -- the value getPipeline() folds
    // into that handle's pipeline-variant cache key. Throws
    // std::out_of_range for an invalid/unknown `handle`.
    [[nodiscard]] MaterialFixedFunctionState fixedFunctionState(MaterialHandle handle) const;

    // Lazily compiles (builds a real VkPipeline from `req.material`'s
    // already-linked SPIR-V; never re-invokes Slang -- see
    // detail::debugCompileCount()'s own comment) and caches one
    // `VkPipeline` per unique (material content hash, pass signature hash,
    // specializationBits, [D28] fixed-function state) key [D7, D28]; a
    // repeated call with an already-cached key returns the same handle
    // immediately. Every VkPipeline this creates uses fixed rasterization/
    // multisample/depth-compare state (back-face culling with
    // counter-clockwise front-face UNLESS `req.material`'s own
    // MaterialFixedFunctionState::doubleSided is true, in which case
    // VK_CULL_MODE_NONE; depth test enabled iff `req.pass.depthFormat !=
    // VK_FORMAT_UNDEFINED`, depth WRITE additionally gated on
    // `fixedFunctionState.alphaMode != AlphaMode::Blend` per D22's own
    // "BLEND: blending on, depth-write off, cull off" text -- doubleSided
    // and BLEND's cull-off are independent axes that happen to compose:
    // a double-sided BLEND material is still cull-none either way) and the
    // fixed position/normal/tangent/uv vertex-input layout forward_entry.
    // slang's own header comment documents -- blend state (blendEnable +
    // standard source-alpha/one-minus-source-alpha factors) is the fourth
    // D28 axis, enabled iff `alphaMode == AlphaMode::Blend`. See D28's own
    // header comment (material_system.h) for why none of this is part of
    // `PipelineRequest` itself.
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
    //
    // CAUTION: The returned reference is invalidated by a later loadMaterial()
    // call on this same instance -- HandlePool's internal reallocation may
    // move the underlying MaterialRecord. Store the actual values of interest
    // (set/binding counts, type information, etc.) rather than caching a
    // ShaderLayoutInfo pointer/reference across a loadMaterial() call.
    [[nodiscard]] const rx::shader::ShaderLayoutInfo& layoutInfo(MaterialHandle handle) const;

    // `handle`'s reflected `TParams` fields -- name, kind, and BYTE
    // OFFSET/SIZE within the parameter block (rx_material/instance.h's
    // MaterialParamInfo) [Task 7 coordinator addition 2]: this is the
    // authoritative reflection api_impl.cpp's IRxMaterialInstance setters
    // now validate against directly (name lookup -> RX_E_NOTFOUND, kind
    // mismatch -> RX_E_INVALIDARG) instead of Task 6's own second,
    // throwaway Slang reflection session -- computed once, here, from the
    // SAME already-linked `slang::IComponentType` (MaterialRecord::
    // linkedProgram) that produced this material's real SPIR-V, not a
    // separately-composed duplicate. Throws std::out_of_range for an
    // invalid/unknown `handle`.
    [[nodiscard]] const std::vector<MaterialParamInfo>& materialParams(MaterialHandle handle) const;

    // `handle`'s whole `TParams` struct size, in bytes (the Slang Uniform
    // parameter category's own size for the ParameterBlock's element type)
    // -- exactly how many bytes a caller's instance blob (rx_api.h's
    // IRxMaterialInstance, or any other InstanceBinding producer) must
    // supply to bindInstance() below. Throws std::out_of_range for an
    // invalid/unknown `handle`.
    [[nodiscard]] uint32_t paramBlockSize(MaterialHandle handle) const;

    // Frames-in-flight this MaterialSystem's own per-frame GPU-facing state
    // (the parameter arena below) is sized for -- fixed at 2, mirroring
    // rx::rhi::FrameSync::kFramesInFlight (frame_sync.h). Not literally the
    // same constant (this header does not include frame_sync.h purely for
    // one value) but documented here as required to match it: every real
    // caller of beginFrame() below drives this MaterialSystem from the same
    // frame-in-flight loop that already uses FrameSync.
    static constexpr uint32_t kFramesInFlight = 2;

    // Advances this MaterialSystem's own per-frame GPU-facing state to a
    // new frame [binding global constraint: frames-in-flight discipline].
    // `frameInFlightIndex` (0..kFramesInFlight()-1) selects which parameter-
    // arena/descriptor-pool slot bindInstance() below writes into next --
    // same SAFETY CONTRACT as rx::rhi::DescriptorArena::beginFrame() (only
    // call once the caller has confirmed, via its own fence wait, that this
    // slot's prior GPU work is complete). `frameNumber` is a monotonically
    // increasing counter that never repeats for this MaterialSystem's
    // lifetime (mirroring rx::rhi::FrameSync::frameNumber(), NOT
    // frameInFlightIndex, which cycles) -- stashed so reloadChanged()'s
    // VkPipeline retirements and releaseTexture()'s Texture2D retirements
    // (both routed through this MaterialSystem's own internal DeletionQueue)
    // are tagged with the frame they actually happened in. Callers are
    // expected to pass the SAME monotonic source they already feed their
    // own direct rx::rhi::DeletionQueue::retire() calls (e.g.
    // rx::rhi::FrameSync::frameNumber()) so this MaterialSystem's own
    // retirements become due at exactly the point the caller's own are.
    void beginFrame(uint32_t frameInFlightIndex, uint64_t frameNumber);

    // Runs (and permanently removes) every VkPipeline/Texture2D retirement
    // this MaterialSystem's internal rx::rhi::DeletionQueue has queued with
    // a frame number <= `completedFrameNumber` -- call once per real frame,
    // after the caller has confirmed (via its own fence wait) that frame
    // `completedFrameNumber`'s GPU work is done. Mirrors DeletionQueue's own
    // documented "typical frame-loop usage" pattern (deletion_queue.h)
    // exactly, just routed through this MaterialSystem's own internal
    // queue rather than one the caller owns directly.
    void onFrameCompleted(uint64_t completedFrameNumber);

    // Record-time material-instance binding [Task 7 coordinator addition 3;
    // brief: "MaterialSystem::bindInstance(cmd, PassContext&, instance) --
    // new internal API consumed by sample 06"] -- NOT on the ABI surface
    // this phase (draw submission is not public in Phase 3, per spec
    // D5/D11). Resolves `binding.material`'s VkPipeline for
    // `passContext.passSignature()` (getPipeline() above -- lazily compiles
    // it on first use, exactly as that method already documents) and binds
    // it, then copies `binding.paramData` (exactly `paramBlockSize(binding.
    // material)` bytes, laid out per materialParams()' own offsets) into
    // this MaterialSystem's own per-frame parameter arena, allocates a
    // set-1 VkDescriptorSet from it, writes the UBO descriptor, and binds
    // that set at set index 1. Sampling stays bindless set-0 [D8] -- this
    // method never touches set 0; the caller binds the owning
    // rx::rhi::BindlessTable's own descriptor set once per frame/pass,
    // exactly the established pattern (samples/03_bindless_mesh).
    //
    // Throws std::out_of_range for an invalid/unknown `binding.material`.
    // Throws std::invalid_argument if `binding.paramData` is null or
    // `binding.paramSize` does not equal `paramBlockSize(binding.material)`
    // exactly -- every real caller already has that size from
    // paramBlockSize() itself, so a mismatch here is a caller bug, not a
    // recoverable runtime condition. Throws std::runtime_error if the
    // per-frame parameter arena is exhausted (kFramesInFlight-many
    // beginFrame() calls without an intervening onFrameCompleted() growing
    // unbounded is a caller bug, not this method's to silently paper over)
    // or if the underlying getPipeline()/vkCreateGraphicsPipelines call
    // fails (same as getPipeline() itself).
    void bindInstance(VkCommandBuffer cmd, const rx::graph::PassContext& passContext, const InstanceBinding& binding);

    // Restates the module-path list `loadMaterial()` has been called with
    // on this instance, stats each one's current on-disk mtime (the same
    // ~4 Hz-poll-friendly `std::filesystem::last_write_time` mechanism
    // samples/02_hotreload/main.cpp establishes -- this method does the
    // stat, not the polling cadence, which stays the caller's job), and for
    // every module whose mtime changed since the last call: recompiles it
    // against a FRESH `slang::ISession` (this class's own analogue of
    // compiler.h's "fresh Compiler per reload" same-module-name caveat --
    // reusing `handle`'s already-loaded session/module name would fail with
    // Slang's own "already loaded with different source" diagnostic) [D9].
    //
    // On a successful recompile: erases every getPipeline()-cached
    // VkPipeline keyed by the module's OLD content hash and retires them
    // through this MaterialSystem's own internal DeletionQueue (tagged with
    // the frame number most recently passed to beginFrame() -- actually
    // destroyed only once onFrameCompleted() confirms that frame is done);
    // subsequent getPipeline() calls for this material re-link lazily
    // against the NEW hash, exactly like a first-time cache miss. The
    // material's old VkShaderModules/VkPipelineLayout/VkDescriptorSetLayouts
    // are destroyed immediately (safe without deferring, unlike VkPipeline
    // itself -- see material_system.cpp's own comment on exactly why), and
    // `layoutInfo()`/`pipelineLayout()`/`materialParams()`/`paramBlockSize()`
    // all reflect the NEW module from the moment this call returns.
    //
    // On a failed recompile (syntax error, reflection rejection, ...): logs
    // Slang's diagnostic text and every other failure detail via
    // RX_LOG_WARN and leaves the material's existing (last-good) compiled
    // state completely untouched -- a reload failure is not this method's
    // caller's error, so this never throws for that reason (it can still
    // throw std::bad_alloc etc. on genuine allocation failure, like every
    // other method here). A module this instance cannot currently stat
    // (e.g. momentarily missing mid-save) is treated the same way: logged,
    // skipped this call, retried on the next call.
    void reloadChanged();

    // Creates a real, renderer-owned rx::rhi::Texture2D from `info` (via
    // this MaterialSystem's own internal rx::rhi::Allocator/Uploader),
    // registers its view into the SAME rx::rhi::BindlessTable passed to
    // create() as a sampled image, and returns a handle whose
    // textureBindlessIndex() below is exactly the u32 a material instance's
    // setTexture()-bound blob field stores [D8; Task 7 coordinator addition
    // 4]. Throws std::invalid_argument if `info.width`/`info.height` is 0
    // or `info.pixels`/`info.pixelBytes` is null/0. Throws
    // std::runtime_error if Texture2D creation, the upload, or the
    // BindlessTable registration (capacity exhausted) fails.
    [[nodiscard]] TextureHandle createTexture2D(const TextureCreateInfo& info);

    // `handle`'s bindless sampled-image index (rx::rhi::BindlessHandle::
    // index()) -- the value setTexture()'s blob field stores. Throws
    // std::out_of_range for an invalid/unknown `handle`.
    [[nodiscard]] uint32_t textureBindlessIndex(TextureHandle handle) const;

    // Releases `handle`'s BindlessTable slot immediately (safe per
    // bindless.h's own RELEASE-SAFETY CONTRACT: it only returns the slot to
    // the free list, never rewrites or destroys anything a pending command
    // buffer might still read) and retires the underlying rx::rhi::Texture2D
    // through this MaterialSystem's own internal DeletionQueue, tagged with
    // the frame number most recently passed to beginFrame() -- actually
    // destroyed only once onFrameCompleted() confirms that frame's GPU work
    // is done [bindless.h's own release-safety contract; Task 7 coordinator
    // addition 4]. A no-op (logs a warning, never throws or crashes) for an
    // invalid or already-released `handle` -- this is the release() path a
    // COM-lite IRxTexture::release() (api_impl.cpp) calls unconditionally
    // once its own refcount reaches zero, and D5 forbids anything reachable
    // from that boundary from ever throwing.
    void releaseTexture(TextureHandle handle);

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
