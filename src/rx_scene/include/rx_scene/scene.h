#pragma once

// rx_scene/scene.h -- rx::scene::Scene: the engine's no-ECS render-proxy
// submission layer [spec D19; Phase 4 Stage 2 Task 18; gate
// matrix-issue05-scene-proxies.md as amended by
// gate/rulings-2026-08-18.md's #5 section + RC5]. See issue #5's own
// "consumer-boundary contract" text: this is NOT an ECS and imposes none
// -- RenderableHandle/LightHandle are plain values a HOST stores inside
// its OWN representation (an ECS component, a scene-graph node, a flat
// array); the create/set/destroy surface below is exactly what a host's
// own per-frame sync system (e.g. an ECS RenderSyncSystem iterating dirty
// transforms) drives. The internal SoA storage is ECS-SHAPED for
// cache-friendly batch iteration (DrawListBuilder's culling loop, Task 19
// -- D15 reads AABBs/layers/etc. directly, contiguous, never per-object),
// never exposed as an ECS framework itself.
//
// Thread-affinity (D5): every public method on Scene is main-thread-only
// (RX_ASSERT_MAIN_THREAD-guarded) -- this class's SoA columns are plain
// `std::vector`s with no internal synchronization, matching every other
// GPU-adjacent-state-owning type in this codebase (Registry/GeometryPool/
// MaterialSystem -- see docs/threading.md). Scene itself never touches the
// GPU directly, but its data feeds DrawListBuilder's per-frame build(),
// which does, so the same single-writer-thread discipline applies from
// day one rather than being retrofitted once a real multi-threaded host
// shows up.
//
// HANDLES [gate ruling, matrix "Handle lifecycle" + "SoA storage vs
// HandlePool" rows]: RenderableHandle/LightHandle reuse
// `rx::core::Handle<Tag>` -- the TYPE (generational index+generation pair,
// `handle.h:1-24`) -- but NOT `rx::core::HandlePool<Tag,T>` (the AoS
// container, `handle.h:26-97`, which stores one whole `T` per slot in a
// single `std::vector<Slot>`). Reusing HandlePool wholesale here would
// force an AoS `RenderableDesc`-shaped row per slot, directly
// contradicting D19's SoA requirement (30k+-object contiguous-column
// iteration) -- so Scene hand-rolls the identical generational
// index/freelist bookkeeping HandlePool already implements (acquire:
// reuse-from-freelist-with-bumped-generation, or grow; release: mark dead,
// push the freed index), just applied once per SLOT and shared across
// every SoA column, instead of once per boxed value.
//
// MESH BOUNDS RESOLUTION -- MeshBoundsFn [necessary, documented deviation
// from the plan's illustrative interface sketch, matching this codebase's
// own established convention for such gaps]: the gate's "per-instance
// bounding-box override" row requires createRenderable()/setTransform() to
// populate/refresh a renderable's world-space AABB from "the mesh's stored
// bounds" -- i.e. `asset::MeshAsset::bounds`, which only `asset::Registry`
// (via its own D24 residency-tolerant `mesh()` accessor) can resolve a
// `MeshHandle` to. Scene does NOT take a hard `asset::Registry&`
// dependency for this: `asset::Registry::registerMesh()` (the only way to
// get NON-fallback `MeshAsset` content into a Registry) is private,
// friend-scoped to import_gltf.cpp's own two orchestration functions, and
// is only ever reachable through a GPU-backed `importGltf()` call (a real
// `GeometryPool`/`Device` is required) -- meaning a hard Registry
// dependency would make this library's own device-free unit tests unable
// to exercise real, nonzero-volume AABB transform math at all (only the
// registry's always-empty/invalid fallback mesh would be reachable
// device-free). `MeshBoundsFn` is the minimal seam that avoids that:
// Scene depends on "a function from MeshHandle to that mesh's local-space
// AABB", not on the concrete Registry type -- production callers bind one
// via meshBoundsFromRegistry() below (a one-line forward to
// `asset::Registry::mesh(handle).bounds`, inheriting that method's own
// D24 residency-tolerant contract with zero duplicated logic: a
// dead/evicted handle already resolves to the registry's fallback mesh's
// zero-volume bounds THROUGH Registry itself); this library's own tests
// bind a trivial in-test callable instead. This mirrors this codebase's
// existing `std::function`-as-injected-callback precedent
// (`rx::asset::ImportCompletionFn`, registry.h) rather than inventing a
// new abstraction style.
//
// D24 AT THE PROXY LEVEL: because setTransform() re-invokes the SAME
// MeshBoundsFn every call (never caches a `MeshAsset*`/`AABB*` across
// calls), a mesh that becomes nonresident BETWEEN two setTransform() calls
// is reflected automatically the next time setTransform() runs (the
// callback's own residency-tolerant behavior propagates through with zero
// extra Scene-side logic) -- see scene_test.cpp's own eviction-proxy test.
//
// NO DIRTY TRACKING [gate ruling, matrix "Dirty-tracking / update cost"
// row]: D12 flattens the whole node hierarchy at import time, so every
// stored transform is already a WORLD transform with nothing above it to
// recompose against (there is no parent to propagate through) --
// setTransform() is therefore a single O(1) direct write into a dense SoA
// slot plus one AABB recompute, full stop. No dirty bit, no two-pass
// update API exists anywhere on this class ON PURPOSE (its absence IS the
// criterion, per the gate ruling) -- see the task report for the
// published 30k-call wall-clock benchmark this criterion also requires.

#include <rx_asset/mesh_asset.h>
#include <rx_core/handle.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace rx::asset {
class Registry;
}  // namespace rx::asset

namespace rx::scene {

using RenderableHandle = rx::core::Handle<struct RenderableTag>;
using LightHandle = rx::core::Handle<struct LightTag>;

// A `MeshHandle -> that mesh's LOCAL-space AABB` resolver -- see this
// header's own "MESH BOUNDS RESOLUTION" comment above for the full
// rationale. Returned BY VALUE (asset::AABB is 24 bytes, two glm::vec3) --
// deliberately not `const AABB&`, so a caller's own lambda/adapter never
// has to reason about a returned reference's lifetime.
using MeshBoundsFn = std::function<asset::AABB(asset::MeshHandle)>;

// Production adapter: binds a live `asset::Registry&` (must outlive every
// call through the returned callable -- the same by-reference lifetime
// discipline this codebase's other injected dependencies already follow,
// e.g. GeometryPool::create()'s Allocator&/Uploader&). Forwards verbatim
// to `registry.mesh(handle).bounds` -- see this header's own top comment
// for why this inherits Registry::mesh()'s D24 residency-tolerant
// contract with no additional logic. Defined in scene.cpp (the one
// translation unit in this library that includes the real,
// heavier-weight <rx_asset/registry.h>; this public header only
// forward-declares `asset::Registry`, matching this codebase's own
// header-hygiene convention of not pulling a dependency's full surface
// into a public header that only needs one type name).
[[nodiscard]] MeshBoundsFn meshBoundsFromRegistry(const asset::Registry& registry);

// One renderable's creation-time description [D19; gate ruling #5].
struct RenderableDesc {
    asset::MeshHandle mesh;

    // [Gate ruling, "Per-submesh material override field" row] A concrete,
    // typed override list -- Filament's `setMaterialInstanceAt(instance,
    // primitiveIndex, materialInstance)` precedent, not a doc comment.
    // Indexed by submesh; `std::nullopt` at index i means "use submesh i's
    // own MeshAsset::Submesh::material verbatim" (no override). Length is
    // NOT validated against the mesh's real submesh count here (Scene has
    // no submesh-count-bearing dependency -- only MeshBoundsFn's AABB;
    // Registry itself is the source of truth for submesh count, so that
    // validation belongs to whichever consumer actually enumerates
    // submeshes, i.e. DrawListBuilder, Task 19) -- copied verbatim into
    // this renderable's own storage at createRenderable() time (the span
    // itself need not outlive this call).
    std::span<const std::optional<asset::MaterialHandle>> submeshMaterialOverrides = {};

    glm::mat4 transform{1.0F};

    // [D19/matrix-issue07] All-ones default -- opt-out, not opt-in
    // (Unity/Godot precedent; matrix-issue07's own "all-ones defaults
    // CONFIRMED as deliberate" ruling). ANDed against Camera::cullMask by
    // DrawListBuilder (Task 19).
    uint32_t layers = ~0u;

    // [D15/RC5] Coupled to shadow-casting per RC5 ("Light channels stay
    // COUPLED... D15 stands"): a renderable whose channels don't overlap a
    // light's own channels is neither lit NOR shadow-tested by that light.
    uint8_t channels = 0xFF;

    // [RC5, NEW field] Filament-precedent per-object shadow opt-out,
    // independent of the channels mask above -- "the channels mask is not
    // the only way to exclude a caster" (RC5's own text).
    bool castsShadows = true;

    // [Gate ruling, "Priority" row, Filament `Builder::priority(uint8_t)`
    // precedent] Coarse draw-order override, orthogonal to the
    // pipeline/material/depth sort key -- clamped to [0,7] by
    // createRenderable() below (a value outside that range is clamped and
    // logged, never silently truncated by integer overflow). Default 4
    // (Filament's own default, "the middle tier").
    uint8_t priority = 4;

    // [preserve-later, seed 14/Filament `Builder::skinning()` precedent]
    // Reserved skinning attach point -- `std::nullopt` ("none") is the
    // only value any Phase 4 code path produces or consumes; present so
    // the animation phase adds population, not a struct/ABI reshape (the
    // same retrofit-economics argument the issue's own LightManager
    // comment already accepts for point/spot lights).
    std::optional<uint32_t> skinningBufferIndex;

    // [preserve-later, Filament `Builder::morphing()` precedent] Reserved
    // morph-target attach point -- same reasoning as skinningBufferIndex
    // above.
    std::optional<uint32_t> morphTargetBufferIndex;
};

// [FG2, preserve-later] Punctual-light forward-compatible type tag --
// `Directional` is the only value any Phase 4 code path produces; `Point`/
// `Spot` exist so LightManager's own SoA columns are sized/typed for
// punctual fields (position, range, cone angles) from day one, per issue
// #5's 2026-08-11 comment: "LightManager's storage admits future
// point/spot types structurally... even though only directional is
// consumed until the techniques phase."
enum class LightType : uint8_t { Directional, Point, Spot };

// Internal light-row shape -- NOT constructible through any public Scene
// API in Phase 4 (createDirectionalLight() below is the only public
// producer, and it always emits `type == Directional` with
// position/range/cone left at their inert defaults). Declared here, not
// hidden inside scene.cpp, purely so the detail:: test-only seam further
// below can construct/inspect a full Point/Spot-shaped row directly --
// proving LightManager's storage does not truncate or reinterpret
// punctual fields nothing yet populates for real (FG2's own acceptance
// criterion) -- mirroring rx_material's `detail::debugCompileCount()`
// carve-out convention (material_system.h) for "a fact only reachable
// through a deliberately non-public seam."
struct LightRecord {
    LightType type = LightType::Directional;
    glm::vec3 direction{0.0F, -1.0F, 0.0F};  // Directional, Spot
    glm::vec3 position{0.0F, 0.0F, 0.0F};    // Point, Spot [reserved, Phase 4 inert]
    glm::vec3 colorLux{1.0F, 1.0F, 1.0F};
    float range = 0.0F;             // Point, Spot falloff radius [reserved, Phase 4 inert]
    float innerConeAngle = 0.0F;    // Spot [reserved, Phase 4 inert]
    float outerConeAngle = 0.0F;    // Spot [reserved, Phase 4 inert]
    bool castsShadows = true;
    uint8_t channels = 0xFF;

    bool operator==(const LightRecord&) const = default;
};

// The only light creation surface Phase 4 exposes publicly [D19; brief:
// "struct DirectionalLightDesc { glm::vec3 dir; glm::vec3 colorLux; bool
// castsShadows; uint8_t channels = 0xFF; }"].
struct DirectionalLightDesc {
    glm::vec3 dir{0.0F, -1.0F, 0.0F};
    glm::vec3 colorLux{1.0F, 1.0F, 1.0F};
    bool castsShadows = true;
    uint8_t channels = 0xFF;
};

// [Phase 5 Task 10, #46, FG1 closure] The scene-level environment binding
// -- skybox pass sampling the base cubemap + IBL diffuse/specular feeding
// every lit-path lobe (the ticket's own text). rx::scene stays DEVICE-FREE
// (no VkDevice/rx_rhi_vk dependency anywhere in this library -- see this
// header's own top comment), so `EnvironmentDesc` carries plain D11-style
// BINDLESS INDICES (uint32_t), never a `rx::rhi::Texture2D`/`BindlessHandle`
// -- the SAME "engine-defined interface consumed by index, never by
// concrete GPU type" convention `asset::TextureRef`/`RenderableDesc`'s own
// texture-index fields already establish. A production caller (samples/
// 08_gltf_viewer) registers `rx::ibl::bakeEnvironment()`'s own BakeResult
// textures into its `rx::rhi::BindlessTable` (the CUBE-typed
// `registerCubeSampledImage()` for base/irradiance/prefiltered, the
// ordinary `registerSampledImage()` for the 2D DFG LUT) and passes the
// resulting indices here.
//
// SINGLETON, NOT A HANDLE POOL [implementer decision, matching Filament's
// own `Scene::setIndirectLight()`/`setSkybox()` precedent -- a single
// setter, not a handle-returning factory]: a Scene has AT MOST ONE active
// environment (the charter's own "Scene-level environment binding" framing,
// singular) -- `createDirectionalLight()`'s own handle-pool idiom exists
// because a scene legitimately has MANY lights; there is no equivalent
// multiplicity requirement for environments in this phase's own scope
// (multiple simultaneous environments/reflection-probe volumes are a later,
// unscoped feature), so a handle pool here would be unused generality, not
// a real requirement -- matching the gate matrix's own "or an
// EnvironmentHandle-returning factory" ALTERNATIVE phrasing explicitly, not
// its only option.
struct EnvironmentDesc {
    // Bindless CUBE indices (BindlessTable::kCubeSampledImageBinding) --
    // rx::ibl::BakeResult::{baseCubemap,irradianceCubemap,prefilteredCubemap}
    // registered by the caller. `baseCubemapIndex` feeds the skybox pass
    // ONLY (standard_pbr.slang's IBL lobes never sample it); the other two
    // feed StandardPbr's diffuse/specular IBL terms.
    uint32_t baseCubemapIndex = 0;
    uint32_t irradianceCubemapIndex = 0;
    uint32_t prefilteredCubemapIndex = 0;
    // Bindless SAMPLED_IMAGE (2D, NOT cube) index -- rx::ibl::BakeResult::
    // dfgLut.
    uint32_t dfgLutIndex = 0;
    // Bindless SAMPLER indices -- a trilinear-across-mips CLAMP_TO_EDGE
    // sampler for the three cube reads above, a bilinear CLAMP_TO_EDGE
    // sampler for the 2D DFG LUT (see material.slang's own RxDrawData
    // header comment for why these are two DISTINCT samplers, not one
    // shared index).
    uint32_t cubeSamplerIndex = 0;
    uint32_t dfgSamplerIndex = 0;
    // rx::ibl::BakeResult::prefilteredMipCount - 1 -- the roughness-to-LOD
    // remap's own natural unit (standard_pbr.slang's own `roughness *
    // maxPrefilteredLod`).
    float maxPrefilteredLod = 0.0F;
    // [T10's own "physical units" ruling -- see material.slang's own
    // RxDrawData::envIntensity header comment for the full unit-convention
    // rationale] Environment radiance/luminance in this scene's own
    // documented physical-ish unit, PRE-EXPOSURE (i.e. NOT yet multiplied
    // by `rx::scene::Camera::exposure()` -- a DrawDataGpu/RxSkyboxData
    // PRODUCER applies that multiply once, per Task 4's own pre-exposure
    // convention, the same point `lightColor`/`ambientColor` already
    // apply it at). Default 1.0 -- a neutral, unscaled environment.
    float intensity = 1.0F;
};

class Scene;

namespace detail {

// Test-only seam -- see LightRecord's own comment above for why this
// exists. NOT part of the stable public contract.
[[nodiscard]] LightHandle createLightRecordForTesting(Scene& scene, const LightRecord& record);
[[nodiscard]] const LightRecord& lightRecordForTesting(const Scene& scene, LightHandle handle);

}  // namespace detail

// SoA render-proxy submission layer [D19]. See this header's own top
// comment for the full thread-affinity/handle/MeshBoundsFn/no-dirty-
// tracking rationale.
class Scene {
public:
    // `meshBounds` is stored and invoked on every createRenderable()/
    // setTransform() call for as long as this Scene exists -- whatever it
    // closes over (typically a `const asset::Registry&` via
    // meshBoundsFromRegistry() above) must outlive this Scene.
    explicit Scene(MeshBoundsFn meshBounds);
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    // --- Renderables ----------------------------------------------------

    // Acquires a slot (reusing the LIFO freelist's most-recently-freed
    // entry with a bumped generation, or growing every SoA column by one
    // row) and populates every column from `desc`. `priority` is clamped
    // to [0,7] (logged if `desc.priority` was out of range).
    // `worldBounds(handle)` is populated immediately, from
    // `meshBoundsFn(desc.mesh).transformed(desc.transform)` (D24
    // residency-tolerant by construction -- see this header's own top
    // comment).
    //
    // Main-thread-only (D5).
    [[nodiscard]] RenderableHandle createRenderable(const RenderableDesc& desc);

    // Marks `handle`'s slot dead and returns its index to the freelist for
    // reuse (bumped generation on the next createRenderable()). A no-op,
    // logged, for an already-dead or never-live handle -- mirrors
    // `rx::core::HandlePool::release()`'s own forgiving double-destroy
    // convention (GeometryPool::free()'s "a double-free... is a logged
    // error, not UB" posture), not `MaterialSystem`'s throwing convention,
    // since a destroy-style API being idempotent is the more usable
    // contract for a host that may race its own bookkeeping.
    //
    // Main-thread-only (D5).
    void destroyRenderable(RenderableHandle handle);

    // O(1) direct SoA write [no dirty tracking -- see this header's own
    // top comment] -- also recomputes worldBounds(handle) from the SAME
    // MeshBoundsFn every call (the D24-at-the-proxy-level mechanism this
    // header's own top comment documents). Throws std::out_of_range for a
    // dead/unknown/stale `handle` (loud failure, matching
    // MaterialSystem/Registry's own convention for a mutator that expects
    // an already-live handle -- see the "Handle lifecycle" gate row: "a
    // stale handle... fails every accessor loudly").
    //
    // Main-thread-only (D5).
    void setTransform(RenderableHandle handle, const glm::mat4& transform);
    void setLayers(RenderableHandle handle, uint32_t layers);
    // [matrix-issue07 API-parity ruling: "getLayers/getChannels/
    // setChannels added for API parity"]
    void setChannels(RenderableHandle handle, uint8_t channels);

    [[nodiscard]] bool isRenderableAlive(RenderableHandle handle) const;
    [[nodiscard]] const glm::mat4& transform(RenderableHandle handle) const;
    [[nodiscard]] const asset::AABB& worldBounds(RenderableHandle handle) const;
    [[nodiscard]] uint32_t layers(RenderableHandle handle) const;
    [[nodiscard]] uint8_t channels(RenderableHandle handle) const;
    [[nodiscard]] bool castsShadows(RenderableHandle handle) const;
    [[nodiscard]] uint8_t priority(RenderableHandle handle) const;
    [[nodiscard]] asset::MeshHandle mesh(RenderableHandle handle) const;
    [[nodiscard]] std::span<const std::optional<asset::MaterialHandle>> submeshMaterialOverrides(
        RenderableHandle handle) const;
    [[nodiscard]] std::optional<uint32_t> skinningBufferIndex(RenderableHandle handle) const;
    [[nodiscard]] std::optional<uint32_t> morphTargetBufferIndex(RenderableHandle handle) const;

    // Live renderable count (dead/freed slots excluded) -- test/diagnostic
    // and legitimate host-facing (e.g. HUD) use alike, mirroring
    // GeometryPool::PoolStats::blockCount's dual audience.
    [[nodiscard]] size_t renderableCount() const;

    // --- SoA span accessors [gate ruling, "SoA storage vs HandlePool" row]
    // ---------------------------------------------------------------------
    // Contiguous, per-field, index-parallel storage -- the direct proof
    // this is real SoA (one std::vector<T> per field), not one
    // std::vector<RenderableDesc>-shaped AoS row per slot. Index i in
    // every span below addresses the SAME underlying slot across every
    // span (whether that slot is currently alive or not -- a dead slot's
    // entry holds whatever its last occupant left; createRenderable()
    // overwrites every column on reuse, so nothing needs to be
    // proactively zeroed on destroy). DrawListBuilder's culling loop
    // (Task 19) is the intended high-frequency consumer of these.
    [[nodiscard]] std::span<const glm::mat4> transformsSpan() const;
    [[nodiscard]] std::span<const asset::AABB> worldBoundsSpan() const;
    [[nodiscard]] std::span<const uint32_t> layersSpan() const;
    [[nodiscard]] std::span<const uint8_t> channelsSpan() const;
    // 0/1 per slot, NOT std::vector<bool> -- std::vector<bool>'s
    // bit-packed specialization is not std::span<const T>-compatible
    // (there is no contiguous bool[] to span over), so every boolean
    // column meant to be span-accessible uses uint8_t internally instead;
    // castsShadows(handle) above converts to a real bool at the per-handle
    // accessor boundary.
    [[nodiscard]] std::span<const uint8_t> castsShadowsSpan() const;
    [[nodiscard]] std::span<const uint8_t> prioritySpan() const;
    [[nodiscard]] std::span<const asset::MeshHandle> meshSpan() const;

    // [Task 19 addition, necessary -- see the task report] `aliveSpan()`
    // (0/1 per slot, same uint8_t-not-vector<bool> convention as
    // castsShadowsSpan() above) and `generationsSpan()` are the two pieces
    // DrawListBuilder's bulk SoA culling loop needs that no existing
    // accessor provides: every OTHER accessor above requires a full,
    // correctly-generationed RenderableHandle (isRenderableAlive(handle),
    // submeshMaterialOverrides(handle), ...), but a consumer iterating the
    // span columns directly only ever has a bare SLOT INDEX, with no way
    // to (a) know whether that slot is a live renderable or a
    // destroyRenderable()d hole still holding its last occupant's stale
    // column values (destroyRenderable() marks `alive_[idx] = false` but
    // deliberately does NOT re-zero every other column -- see scene.cpp --
    // so e.g. a freed slot's `layers_[idx]` cannot be used as an implicit
    // "dead" sentinel), or (b) reconstruct a valid RenderableHandle(idx,
    // generation) to call a per-handle accessor like
    // submeshMaterialOverrides() for that slot. Both spans are read-only,
    // additive, and follow the exact same "index i addresses the same
    // underlying slot across every span" contract documented on this
    // class's own SoA span-accessor block above.
    [[nodiscard]] std::span<const uint8_t> aliveSpan() const;
    [[nodiscard]] std::span<const uint32_t> generationsSpan() const;

    // --- Lights ----------------------------------------------------------

    // Main-thread-only (D5).
    [[nodiscard]] LightHandle createDirectionalLight(const DirectionalLightDesc& desc);
    void destroyLight(LightHandle handle);
    void setLightChannels(LightHandle handle, uint8_t channels);

    [[nodiscard]] bool isLightAlive(LightHandle handle) const;
    [[nodiscard]] glm::vec3 lightDirection(LightHandle handle) const;
    [[nodiscard]] glm::vec3 lightColorLux(LightHandle handle) const;
    [[nodiscard]] bool lightCastsShadows(LightHandle handle) const;
    [[nodiscard]] uint8_t lightChannels(LightHandle handle) const;
    [[nodiscard]] size_t lightCount() const;

    // --- Environment [Phase 5 Task 10, #46] -------------------------------

    // Sets/replaces this Scene's own single active environment -- see
    // `EnvironmentDesc`'s own header comment for the singleton rationale.
    // Main-thread-only (D5), matching every other Scene mutator.
    void setEnvironment(const EnvironmentDesc& desc);
    // Clears this Scene's environment -- `hasEnvironment()` returns false
    // and every subsequent RxDrawData row a caller populates from this
    // Scene should carry the "no environment" sentinel again (a caller's
    // own responsibility -- Scene itself does not retroactively touch
    // already-uploaded GPU buffers, matching its own "no dirty tracking"
    // top-of-file convention).
    void clearEnvironment();
    [[nodiscard]] bool hasEnvironment() const;
    // Throws std::out_of_range if `hasEnvironment() == false` -- matching
    // this class's own established "a mutator/accessor that expects
    // already-live state fails loudly" convention (requireLiveRenderable()/
    // requireLiveLight()), rather than returning a silently-default-
    // constructed EnvironmentDesc a caller could mistake for a real,
    // intentionally-neutral one.
    [[nodiscard]] const EnvironmentDesc& environment() const;

private:
    friend LightHandle detail::createLightRecordForTesting(Scene&, const LightRecord&);
    friend const LightRecord& detail::lightRecordForTesting(const Scene&, LightHandle);

    [[nodiscard]] bool isLiveRenderableIndex(RenderableHandle handle) const;
    [[nodiscard]] uint32_t requireLiveRenderable(RenderableHandle handle, const char* context) const;
    [[nodiscard]] bool isLiveLightIndex(LightHandle handle) const;
    [[nodiscard]] uint32_t requireLiveLight(LightHandle handle, const char* context) const;

    // Shared acquire-a-light-slot logic between createDirectionalLight()
    // and the detail:: test-only seam (both just build a different-shaped
    // LightRecord and hand it here).
    [[nodiscard]] LightHandle insertLightRecord(const LightRecord& record);

    void recomputeWorldBounds(uint32_t index);

    MeshBoundsFn meshBounds_;

    // --- Renderable SoA columns (index-parallel with generation_/alive_)
    std::vector<uint32_t> generation_;
    // [Task 19] uint8_t, not vector<bool> -- was internal-bookkeeping-only
    // (comment now stale: aliveSpan() above spans it directly, so it needs
    // the same contiguous-bool-as-uint8_t representation every other
    // boolean column already uses, per castsShadowsSpan()'s own comment).
    std::vector<uint8_t> alive_;
    std::vector<uint32_t> freeList_;

    std::vector<asset::MeshHandle> mesh_;
    std::vector<std::vector<std::optional<asset::MaterialHandle>>> submeshOverrides_;
    std::vector<glm::mat4> transform_;
    std::vector<asset::AABB> worldBounds_;
    std::vector<uint32_t> layers_;
    std::vector<uint8_t> channels_;
    std::vector<uint8_t> castsShadows_;
    std::vector<uint8_t> priority_;
    std::vector<std::optional<uint32_t>> skinningBufferIndex_;
    std::vector<std::optional<uint32_t>> morphTargetBufferIndex_;

    // --- Light storage. Plain AoS (one std::vector<LightRecord>), not
    // column-split -- D19's SoA emphasis is specifically about the
    // 30k+-renderable culling hot path (DrawListBuilder, Task 19); Phase 4
    // has no light-count-at-scale hot loop that would benefit from
    // per-field columns (a scene has a handful of lights, not tens of
    // thousands), so this deliberately does not mirror the renderable
    // columns above.
    std::vector<uint32_t> lightGeneration_;
    std::vector<bool> lightAlive_;
    std::vector<uint32_t> lightFreeList_;
    std::vector<LightRecord> lightRecords_;

    // --- Environment [Phase 5 Task 10, #46] -- a plain optional value, not
    // a handle-pool column (see EnvironmentDesc's own singleton rationale).
    std::optional<EnvironmentDesc> environment_;
};

}  // namespace rx::scene
