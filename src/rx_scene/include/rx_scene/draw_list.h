#pragma once

// rx_scene/draw_list.h -- rx::scene::DrawListBuilder: parallel frustum +
// shadow-caster culling and u64 sort-key draw-list construction [spec
// D14/D15/D24/D26/D27; Phase 4 Stage 2 Task 19; gate
// matrix-issue06-drawlists-culling.md + matrix-issue07-layer-masks.md as
// amended by gate/rulings-2026-08-18.md's #6/#7 sections + RC3/RC5]. Entirely
// device-free: no VkDevice, no rx_rhi_vk -- like rx_scene's own
// Scene/Camera, every algorithm here is plain CPU-side math + std::sort over
// `rx::task::Scheduler`-parallelized culling. Mesh/material CONTENT is never
// touched directly: `MeshSubmeshesFn`/`MaterialResolveFn` are the SAME kind
// of injected-callback seam Task 18's `MeshBoundsFn` established (see
// scene.h's own "MESH BOUNDS RESOLUTION" comment) -- production callers bind
// `meshSubmeshesFromRegistry()`/`materialResolveFromRegistry()` below (both
// one-line forwards inheriting `asset::Registry::mesh()`/`material()`'s own
// D24 residency-tolerant contract with zero duplicated logic); this
// library's own tests bind trivial in-test callables instead.
//
// Thread-affinity (D5): `DrawListBuilder::build()`/`buildShadow()` are
// MAIN-THREAD-ONLY (they read `Scene`'s own main-thread-only accessors, and
// mutate this builder's private reused scratch storage with no internal
// synchronization) -- the PARALLEL portion (AABB-vs-frustum culling) fans
// out via `scheduler.parallelFor()` over data already snapshotted from
// `Scene` on the calling thread before fan-out, so worker chunks never call
// a `Scene`/`MaterialResolveFn`/`MeshSubmeshesFn` accessor themselves, only
// index into already-captured spans -- see this header's own "D27" section
// below for the analogous, EXPLICIT guard covering `recordDrawList()`'s own
// pre-resolution step.
//
// ZERO-ALLOC, CALLER-OWNED STORAGE [D26 amendment]: `ViewLists`/`ShadowLists`
// are caller-owned -- `build()`/`buildShadow()` `.clear()` (never
// reassign/`shrink_to_fit`) their vectors and write into them, so repeated
// calls against a STEADY scene (same or fewer visible objects than any
// prior call) perform zero net heap allocations after a warm-up call
// establishes peak capacity -- see draw_list_test.cpp's own
// capacity-snapshot test. `DrawListBuilder` itself owns identically-reused
// private scratch buffers (cull reasons, visible-index list, pending
// records) for the exact same reason -- these are per-BUILDER, not
// per-ViewLists, state.

#include <rx_asset/mesh_asset.h>
#include <rx_scene/camera.h>
#include <rx_scene/scene.h>
#include <rx_task/scheduler.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace rx::asset {
class Registry;
}  // namespace rx::asset

namespace rx::scene {

// ---------------------------------------------------------------------
// D26.2: SoA, VkDrawIndexedIndirectCommand-compatible command array (a
// packed upload target, not an AoS record struct) + a parallel per-draw
// payload array. Field names/order/types match Vulkan's own
// VkDrawIndexedIndirectCommand exactly (vulkan_core.h: `{uint32_t
// indexCount; uint32_t instanceCount; uint32_t firstIndex; int32_t
// vertexOffset; uint32_t firstInstance;}`) -- draw_list_test.cpp's own
// static_assert(sizeof(DrawCommand) == sizeof(VkDrawIndexedIndirectCommand))
// plus a reinterpret-cast upload test proves this is a real, exercised
// invariant, not merely structural coincidence (that test lives in the
// TEST binary, not this header, so this device-free library never itself
// names a Vulkan type -- see this header's own top comment).
// ---------------------------------------------------------------------
struct DrawCommand {
    uint32_t indexCount = 0;
    uint32_t instanceCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t firstInstance = 0;

    bool operator==(const DrawCommand&) const = default;
};

// [D26.1] Per-draw payload, indexed by `firstInstance + gl_InstanceIndex`
// (Slang/`SV_VulkanInstanceID` specifics are Task 16's own concern; this
// struct only defines the CPU-side shape) -- `materialIndex` is resolved
// from `asset::MaterialHandle` via `MaterialResolveFn` (D24
// residency-tolerant: a nonresident handle resolves through the SAME
// fallback-substitution `asset::Registry::material()` already performs, see
// `materialResolveFromRegistry()` below), `instanceDataIndex` is the
// renderable's own `Scene` SLOT INDEX -- directly usable to index
// `Scene::transformsSpan()`/a future per-instance transform buffer built in
// the same index space, with zero extra bookkeeping on this class's part.
struct DrawPayload {
    uint32_t materialIndex = 0;
    uint32_t instanceDataIndex = 0;

    bool operator==(const DrawPayload&) const = default;
};

// [D26.2] One contiguous run of `ViewLists::commands` sharing the same
// GeometryPool `blockId` -- the recorded per-block indirect submission
// bound (one future MDI call per block; `GeometryPool::bind()`'s own
// contract requires every draw between two `bind()` calls to share one
// block, so this contiguity is a REAL correctness requirement of the
// eventual GPU-driven submission, not a diagnostic nicety). `blockId` is
// the OUTERMOST sort tier `build()`/`buildShadow()` apply (see this
// header's own "SORT KEY" section below for why) specifically so this
// invariant holds by construction -- draw_list_test.cpp asserts it directly
// for a synthetic multi-block scene.
struct BlockRange {
    uint32_t blockId = 0;
    uint32_t firstCommand = 0;
    uint32_t commandCount = 0;

    bool operator==(const BlockRange&) const = default;
};

// [D18/matrix-issue06 "Exact counter definitions" row, gate ruling #6]
// Exact, CI-gateable per-`build()`/`buildShadow()` call counters -- every
// field below is unambiguous enough for a unit test to assert an EXACT
// integer, never a range. ONE shared struct serves both `ViewLists` (view
// culling: every field except `shadowCasters*` is populated,
// `shadowCastersConsidered`/`shadowCastersVisible` stay 0) and
// `ShadowLists` (shadow culling: every field is populated using the
// shadow-appropriate mapping documented on `DrawListBuilder::buildShadow()`
// below -- `culledByLayerMask`/`culledByFrustum` are reused, DOCUMENTED as
// "excluded by the caster-eligibility test (castsShadows/channel coupling,
// RC5)"/"excluded by the light's extruded ortho frustum" respectively,
// rather than inventing a second, parallel counter struct for a
// single-partition variant of the exact same culling shape).
struct CullCounters {
    uint32_t totalCandidates = 0;    // alive renderables considered, before any filter
    uint32_t culledByLayerMask = 0;  // view: camera cullMask AND-test failed. shadow: caster-eligibility test failed (castsShadows==false or channel mismatch, RC5)
    uint32_t culledByFrustum = 0;    // view: AABB-vs-camera-frustum failed. shadow: AABB-vs-light's-extruded-ortho-box failed
    uint32_t visible = 0;            // totalCandidates - culledByLayerMask - culledByFrustum
    uint32_t recordsIn = 0;          // draw records generated from `visible`, pre-collapse (one per submesh-instance pairing)
    uint32_t drawsSubmitted = 0;     // post-D26.3-collapse DrawCommand count (== ViewLists::commands.size())
    uint32_t shadowCastersConsidered = 0;  // shadow-eligible candidates (== visible + culledByFrustum, shadow build only)
    uint32_t shadowCastersVisible = 0;     // == visible, shadow build only (mirrored here per the gate's named-field requirement)

    bool operator==(const CullCounters&) const = default;
};

// [D26.2/D26.3] The sorted, (optionally) instancing-collapsed draw list for
// one view (camera). `commands`/`payloads` are ALWAYS kept in lockstep
// (D26.3's own binding lockstep criterion -- see draw_list.cpp's collapse
// pass): `commands[k].firstInstance` is the index into `payloads` where
// that command's `[firstInstance, firstInstance+instanceCount)` contiguous
// instance-payload range begins. Two partitions, concatenated:
// `[0, opaqueCommandCount)` is the OPAQUE+MASK partition (sorted
// front-to-back, instancing-collapsed); `[opaqueCommandCount,
// commands.size())` is the BLEND partition (sorted strictly
// back-to-front, NEVER collapsed -- D14). `blocks` groups `commands`
// contiguously by `blockId`, scoped per (block-run-within-a-partition) --
// see `BlockRange`'s own comment for why the SAME blockId can legitimately
// appear as two separate entries when both partitions use it (they are, by
// construction, two independent MDI submissions regardless).
struct ViewLists {
    std::vector<DrawCommand> commands;
    std::vector<DrawPayload> payloads;
    std::vector<BlockRange> blocks;
    uint32_t opaqueCommandCount = 0;
    CullCounters counters;
};

// [gate ruling #6, "ShadowLists' concrete field layout"] Same shape as
// `ViewLists` -- a single partition (`opaqueCommandCount == commands.size()`
// always; BLEND-partition casters are EXCLUDED per RC3), sorted
// (blockId-outer, then pipeline|mesh-range), never instancing-collapsed
// (Phase 4 scope: not required by the gate criteria; a documented,
// non-required optimization for a later task).
using ShadowLists = ViewLists;

// ---------------------------------------------------------------------
// Injected resolution seams [mirrors Scene's own MeshBoundsFn precedent --
// see this header's own top comment]. Both are residency-tolerant BY
// CONSTRUCTION when bound to their `xFromRegistry()` production adapter:
// `asset::Registry::mesh()`/`material()` already substitute the D11
// fallback asset for a dead/nonresident handle, and these adapters forward
// to them verbatim with zero additional logic -- exactly
// `meshBoundsFromRegistry()`'s own documented contract (scene.h).
// ---------------------------------------------------------------------

// A mesh's submesh list (geometry range + default material per submesh) --
// `std::span<const asset::Submesh>`, so the production adapter below
// returns `asset::Registry`'s own storage directly (zero copy, zero
// allocation): `asset::MeshAsset::submeshes` IS the exact shape this needs.
using MeshSubmeshesFn = std::function<std::span<const asset::Submesh>(asset::MeshHandle)>;

// Production adapter: binds a live `asset::Registry&` (must outlive every
// call through the returned callable, same by-reference lifetime discipline
// as `meshBoundsFromRegistry()`). Forwards to `registry.mesh(handle).submeshes`
// -- inherits `Registry::mesh()`'s D24 residency-tolerant contract verbatim
// (a dead/evicted handle resolves to the D11 fallback mesh's EMPTY submesh
// list -- see `rx_asset/fallbacks.cpp`'s `makeFallbackMeshAsset()` -- so a
// renderable referencing an evicted mesh contributes zero draw records,
// never a crash). Defined in draw_list.cpp (the one translation unit that
// includes the real, heavier-weight `<rx_asset/registry.h>`).
[[nodiscard]] MeshSubmeshesFn meshSubmeshesFromRegistry(const asset::Registry& registry);

// One material's pipeline-relevant content, resolved from a MaterialHandle.
// `materialIndex` is the handle's own stable slot INDEX (`handle.index()`)
// -- never re-derived to point at a different slot on eviction: the
// SUBSTITUTION itself happens at CONTENT resolution (the three fields
// below, read from `registry.material(handle)`, which is ALREADY
// D24-tolerant), not at the index -- a later, real GPU-side per-draw
// material buffer is populated from `registry.material(handle)` at that
// SAME index, so using the raw index unconditionally is itself safe (an
// integer, never a dereferenced pointer) and automatically consistent with
// whatever content ends up bound there.
struct ResolvedMaterial {
    uint32_t materialIndex = 0;
    asset::AlphaMode alphaMode = asset::AlphaMode::Opaque;
    bool doubleSided = false;
    asset::MaterialDisposition disposition = asset::MaterialDisposition::StandardPBR;
};

using MaterialResolveFn = std::function<ResolvedMaterial(asset::MaterialHandle)>;

// Production adapter -- see `ResolvedMaterial`'s own comment. Defined in
// draw_list.cpp.
[[nodiscard]] MaterialResolveFn materialResolveFromRegistry(const asset::Registry& registry);

// ---------------------------------------------------------------------
// SORT KEY [gate ruling #6; bgfx `SortKey` (bgfx_p.h) + Filament
// `RenderPass::CommandKey` (RenderPass.h) discipline: document the full bit
// layout with named constants + a comment diagram, provide a decode() that
// round-trips, exactly like bgfx's own `SortKey::decode()` exists as a
// correctness check on itself]. THREE distinct 64-bit key shapes, one per
// consumer -- bgfx precedent for "one uint64_t, several shapes depending on
// what's being sorted," not an invented pattern:
//
//   OPAQUE (+MASK) partition -- state-minimization dominant, per D14
//   ("front-to-back by pipeline|material|depth"): NO mesh-identity tier in
//   this key at all, deliberately -- see this namespace's own "COLLAPSE
//   BEFORE SORT" note below for why putting mesh-range identity ABOVE
//   depth (an earlier revision of this design did exactly that, to force
//   D26.3 collapse-adjacency) is WRONG: it would make a scattered
//   instanced group's relative position depend on an arbitrary mesh-hash
//   rather than depth, breaking D14's own front-to-back guarantee across
//   different meshes/materials. Collapse instead happens BEFORE this key
//   is ever computed, so this key only ever needs to order the resulting
//   (already-collapsed) draw GROUPS by pipeline/material/depth, exactly as
//   D14 states, no mesh field required.
//
//     bit  63........61 60............54 53..........39 38...........16 15.........0
//          [priority: 3] [pipelineKey:7] [materialKey:15][depthBucket:23][tieBreak:16]
//
//   COLLAPSE BEFORE SORT [D26.3]: `DrawListBuilder::Impl::collapseAndSort()`
//   (draw_list.cpp) runs the opaque partition through TWO passes, not one:
//   (1) sort `pending` records by their exact draw IDENTITY (blockId,
//   indexCount, firstIndex, vertexOffset, materialIndex, priority -- the
//   same tuple `sameDrawIdentity()` checks) so every collapse-eligible run
//   becomes contiguous REGARDLESS of the instances' individual depths (a
//   scattered "forest of identical trees" scene collapses into ONE
//   instanced draw exactly as D26.3 requires, even though its 1000
//   instances are at 1000 different depths); (2) fold each identity-run
//   into ONE `CollapsedGroup` (instanceCount = run length, representative
//   depth = the NEAREST member's NDC depth -- the run's own best early-Z
//   position), THEN sort the resulting (far fewer) groups by THIS key
//   (priority|pipeline|material|depth). The direction/priority-tier tests
//   below operate on scenes with no two objects sharing draw identity
//   (distinct meshes), so each is its own trivial one-member "group" and
//   this key's pure depth-ordering behavior is directly, simply
//   observable -- exactly D14's own stated contract.
//
//   BLEND partition -- depth-dominant (bgfx's own "Draw Key 1... reordered
//   so depth dominates" precedent), strict back-to-front correctness over
//   state minimization; pipeline/material/mesh are NOT sort tiers here
//   (blend draws are never instancing-collapsed, D14, so there is no
//   adjacency requirement to serve). `reservedBlendOrder` is present,
//   ALWAYS ZERO in Phase 4 (RC5/#5 ruling: "per-primitive blendOrder is NOT
//   added... reserved bits documented in the key layout") -- a future
//   per-primitive override lands here with no ABI break.
//
//     bit  63........61 60..................48 47..............16 15.........0
//          [priority: 3] [reservedBlendOrder:13] [depthBucket:32]  [tieBreak:16]
//
//   SHADOW partition (ShadowLists) -- no depth/priority tier at all (a
//   depth-only pass has no early-Z-order OR blend-order concern); sorted
//   purely for state minimization, per gate ruling #6: "sorted (pipeline,
//   mesh range, block)".
//
//     bit  63................54 53................32 31.........0
//          [pipelineKey: 10]    [meshKey: 22]         [tieBreak: 32]
//
//   `blockId` is NOT a bit-field in any shape above -- it is the OUTERMOST
//   sort tier, applied as an explicit primary comparison ABOVE the 64-bit
//   key (see draw_list.cpp's own sort comparators) so `BlockRange`'s own
//   "one contiguous run per block" invariant holds unconditionally,
//   independent of everything else in this key.
//
//   DEPTH BUCKET [gate ruling #6]: never a linear rescale of NDC depth --
//   reversed-Z's whole precision benefit comes from the raw IEEE-754 bit
//   pattern already being densely, monotonically distributed near the near
//   plane (depth->1.0); this key reinterprets the (non-negative, [0,1]
//   clamped) depth float's raw bits and keeps only the top N bits (bgfx's
//   own technique, since it stores the near-full float depth directly).
//   OPAQUE inverts the truncated bucket (`~bucket & mask`, Filament's own
//   `~distanceBits` bit-negation trick, RenderPass.h:103) so ascending key
//   order is DEcreasing depth (nearest/highest-reversed-Z-value first,
//   front-to-back); BLEND stores the bucket UNINVERTED so ascending key
//   order is INcreasing depth (farthest/lowest-reversed-Z-value first,
//   back-to-front) -- see draw_list_test.cpp's shared-fixture direction
//   test, which asserts both directions against the SAME depth values so a
//   sign-flip bug in either partition fails immediately.
//
//   TIE-BREAK [gate ruling #6]: the renderable's own Scene SLOT INDEX (its
//   "stable creation index" for as long as this exact scene state persists
//   -- see draw_list.cpp for why slot index, not a separate monotonic
//   counter, satisfies the binding requirement: `std::sort`'s output
//   becomes fully determined by the key alone, regardless of algorithm
//   internals, independent of whether that index also reflects TRUE
//   historical creation order across destroy/recreate churn).
// ---------------------------------------------------------------------
namespace sortkey {

// --- OPAQUE shape --------------------------------------------------------
inline constexpr uint32_t kOpaqueTieBreakBits = 16;
inline constexpr uint32_t kOpaqueDepthBits = 23;
inline constexpr uint32_t kOpaqueMaterialKeyBits = 15;
inline constexpr uint32_t kOpaquePipelineKeyBits = 7;
inline constexpr uint32_t kOpaquePriorityBits = 3;
static_assert(
    kOpaqueTieBreakBits + kOpaqueDepthBits + kOpaqueMaterialKeyBits + kOpaquePipelineKeyBits + kOpaquePriorityBits == 64,
    "opaque sort key must occupy exactly 64 bits");

inline constexpr uint32_t kOpaqueTieBreakShift = 0;
inline constexpr uint32_t kOpaqueDepthShift = kOpaqueTieBreakShift + kOpaqueTieBreakBits;
inline constexpr uint32_t kOpaqueMaterialKeyShift = kOpaqueDepthShift + kOpaqueDepthBits;
inline constexpr uint32_t kOpaquePipelineKeyShift = kOpaqueMaterialKeyShift + kOpaqueMaterialKeyBits;
inline constexpr uint32_t kOpaquePriorityShift = kOpaquePipelineKeyShift + kOpaquePipelineKeyBits;

// --- BLEND shape -----------------------------------------------------------
inline constexpr uint32_t kBlendTieBreakBits = 16;
inline constexpr uint32_t kBlendDepthBits = 32;
inline constexpr uint32_t kBlendReservedBlendOrderBits = 13;
inline constexpr uint32_t kBlendPriorityBits = 3;
static_assert(kBlendTieBreakBits + kBlendDepthBits + kBlendReservedBlendOrderBits + kBlendPriorityBits == 64,
              "blend sort key must occupy exactly 64 bits");

inline constexpr uint32_t kBlendTieBreakShift = 0;
inline constexpr uint32_t kBlendDepthShift = kBlendTieBreakShift + kBlendTieBreakBits;
inline constexpr uint32_t kBlendReservedBlendOrderShift = kBlendDepthShift + kBlendDepthBits;
inline constexpr uint32_t kBlendPriorityShift = kBlendReservedBlendOrderShift + kBlendReservedBlendOrderBits;

// --- SHADOW shape ------------------------------------------------------
inline constexpr uint32_t kShadowTieBreakBits = 32;
inline constexpr uint32_t kShadowMeshKeyBits = 22;
inline constexpr uint32_t kShadowPipelineKeyBits = 10;
static_assert(kShadowTieBreakBits + kShadowMeshKeyBits + kShadowPipelineKeyBits == 64,
              "shadow sort key must occupy exactly 64 bits");

inline constexpr uint32_t kShadowTieBreakShift = 0;
inline constexpr uint32_t kShadowMeshKeyShift = kShadowTieBreakShift + kShadowTieBreakBits;
inline constexpr uint32_t kShadowPipelineKeyShift = kShadowMeshKeyShift + kShadowMeshKeyBits;

// Raw depth-bucket bits from an NDC depth in [0,1] -- the top `bucketBits`
// bits of the depth float's own IEEE-754 bit pattern (monotonic for
// non-negative floats: reinterpreting preserves ordering, no multiplication
// or rescale involved). `bucketBits` must be in [1,32].
[[nodiscard]] uint32_t depthBucketBits(float ndcDepth, uint32_t bucketBits);

// Coarse, device-free pipeline-variant proxy derived from the
// fixed-function-relevant material fields available at build() time (D28's
// own axis: alphaMode -> blend/depth-write, doubleSided -> cull mode).
// This is NOT a real VkPipeline identity -- that is resolved LATER, per
// D27, by `resolveDrawGroups()`'s main-thread pre-resolution pass, keyed by
// (at minimum) materialIndex boundaries this sort already produces. It
// exists so distinct fixed-function pipeline VARIANTS sort into separate,
// contiguous groups even device-free, before any real MaterialSystem
// exists to ask.
[[nodiscard]] uint16_t pipelineKeyOf(asset::MaterialDisposition disposition, asset::AlphaMode alphaMode,
                                      bool doubleSided);

// Coarse hash of a submesh's geometry identity within its own block
// (firstIndex/vertexOffset uniquely identify a suballocation within one
// block; block identity itself is the OUTER sort tier, not part of this
// hash -- see this namespace's own top comment).
[[nodiscard]] uint16_t meshKeyOf(uint32_t firstIndex, int32_t vertexOffset);

[[nodiscard]] uint64_t encodeOpaque(uint8_t priority, uint16_t pipelineKey, uint16_t materialKey, float ndcDepth,
                                     uint32_t tieBreak);
[[nodiscard]] uint64_t encodeBlend(uint8_t priority, float ndcDepth, uint32_t tieBreak);
[[nodiscard]] uint64_t encodeShadow(uint16_t pipelineKey, uint16_t meshKey, uint32_t tieBreak);

struct DecodedOpaque {
    uint8_t priority = 0;
    uint16_t pipelineKey = 0;
    uint16_t materialKey = 0;
    uint32_t invertedDepthBucket = 0;  // the STORED bits (already inverted -- see encodeOpaque()) -- round-trip only, not un-inverted
    uint32_t tieBreak = 0;
};
struct DecodedBlend {
    uint8_t priority = 0;
    uint32_t reservedBlendOrder = 0;  // always 0 in Phase 4 -- round-trip proves nothing else leaks into these bits
    uint32_t depthBucket = 0;
    uint32_t tieBreak = 0;
};
struct DecodedShadow {
    uint16_t pipelineKey = 0;
    uint16_t meshKey = 0;
    uint32_t tieBreak = 0;
};

[[nodiscard]] DecodedOpaque decodeOpaque(uint64_t key);
[[nodiscard]] DecodedBlend decodeBlend(uint64_t key);
[[nodiscard]] DecodedShadow decodeShadow(uint64_t key);

}  // namespace sortkey

class DrawListBuilder;

namespace detail {
// Forward-declared here (full definition + doc comment near the bottom of
// this header, alongside recordDrawList()'s own detail:: seam) purely so
// DrawListBuilder's own `friend` declaration below can name it -- mirrors
// this codebase's usual forward-declare-for-friendship pattern (see
// scene.h's own detail::createLightRecordForTesting friend declarations).
struct DrawListBuilderCapacitiesForTesting;
[[nodiscard]] DrawListBuilderCapacitiesForTesting capacitiesForTesting(const DrawListBuilder& builder);
}  // namespace detail

// ---------------------------------------------------------------------
// DrawListBuilder [D14/D15/D26/D27]
// ---------------------------------------------------------------------
class DrawListBuilder {
public:
    // `meshSubmeshes`/`materialResolve` are stored and invoked on every
    // build()/buildShadow() call for as long as this builder exists --
    // whatever they close over (typically a `const asset::Registry&` via
    // the two `xFromRegistry()` adapters above) must outlive this builder,
    // matching Scene's own MeshBoundsFn lifetime discipline.
    DrawListBuilder(MeshSubmeshesFn meshSubmeshes, MaterialResolveFn materialResolve);
    ~DrawListBuilder();

    DrawListBuilder(const DrawListBuilder&) = delete;
    DrawListBuilder& operator=(const DrawListBuilder&) = delete;
    DrawListBuilder(DrawListBuilder&&) = delete;
    DrawListBuilder& operator=(DrawListBuilder&&) = delete;

    // Frustum-culls every alive `scene` renderable against `camera`
    // (`Camera::cullingFrustumPlanes()` -- the FINITE culling projection,
    // never the infinite-far render one, per matrix-issue05/06's shared
    // Conflict #1 resolution), applies the camera `cullMask`/renderable
    // `layers` AND-test, resolves visible renderables into draw records via
    // this builder's injected `MeshSubmeshesFn`/`MaterialResolveFn`
    // (D24-tolerant), sorts + instancing-collapses per D14/D26.3, and
    // writes the result into `out` (caller-owned, reused storage -- see
    // this header's own top comment).
    //
    // Main-thread-only (D5), [guarded] [Phase 4 exit fix wave, M1]: reads
    // `scene`'s own main-thread-only accessors before fanning any work out
    // to `scheduler`'s workers; the parallel AABB-vs-frustum pass itself
    // touches only already-captured plain spans, never `scene` again, from
    // a worker thread.
    void build(const Scene& scene, const Camera& camera, task::Scheduler& scheduler, ViewLists& out);

    // Shadow-caster culling for one directional light [D15]: an ortho box
    // fitted to `camera`'s own visible-bounds cross-section, extruded
    // conservatively so that the side UPSTREAM of the light (where a
    // caster must sit to shadow something in the visible region) is
    // unbounded, while the downstream side stays at the visible bounds' own
    // tight extent (see draw_list.cpp's own worked-example derivation) so
    // off-screen casters whose shadow could still reach the visible region
    // are included. `light` must be a currently-alive `LightHandle` on `scene`;
    // a dead/stale handle or a light with `castsShadows == false` produces
    // an EMPTY `out` (cleared, zeroed counters), never a throw (this is a
    // per-frame query, not a mutator with a "handle must be live"
    // contract). BLEND-partition casters are EXCLUDED (RC3); MASK casters
    // are included, rendered as full opaque silhouettes in Phase 4
    // (documented limitation, RC3 -- no alpha-cutoff test in the depth-only
    // pass this feeds). Channel coupling (RC5): a renderable whose
    // `channels` do not overlap `light`'s own `channels` is excluded from
    // caster consideration entirely, same AND-test as camera cullMask.
    //
    // Main-thread-only (D5), [guarded] [Phase 4 exit fix wave, M1], same
    // rationale as build().
    void buildShadow(const Scene& scene, LightHandle light, const Camera& camera, task::Scheduler& scheduler,
                      ShadowLists& out);

private:
    friend detail::DrawListBuilderCapacitiesForTesting detail::capacitiesForTesting(const DrawListBuilder&);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------
// recordDrawList [D27] -- the chunked submit helper Task 22/24's real
// GPU-side pass recording binds to (via `PipelineResolveFn`/`RecordChunkFn`
// implementations that DO touch `rx::material::MaterialSystem`/
// `rx::graph::PassContext`, out of THIS device-free library's own scope).
// What this library delivers is the MECHANISM: a single main-thread linear
// scan over the already-sorted list that pre-resolves every distinct
// materialIndex boundary EXACTLY ONCE (D27's own "nearly free... the sorted
// draw list already enumerates every distinct pair" argument), producing
// plain `ResolvedDrawGroup` data with NO reachable path back to whatever
// main-thread-guarded resolver produced it -- `RecordChunkFn`'s own
// signature never receives `PipelineResolveFn` at all, a
// compile-time-adjacent guarantee stronger than a runtime-only check (gate
// ruling #6's own "D27 -- parameter-offset pre-computation" row).
//
// SEAM SHAPE [review round 1, finding 3 -- fixed here, not deferred]:
// mirrors `rx::material::PipelineRequest`'s own three cache-key inputs
// (`material_system.h`: `MaterialHandle material; PassSignature pass;
// uint32_t specializationBits;`) field-for-field, WITHOUT taking a hard
// dependency on `rx::material`/`rx::graph` types (this library stays
// device-free) -- `materialIndex` stands in for `MaterialHandle` (already
// this seam's own resolved index), `passSignatureHash` stands in for
// `PassSignature` via its own `hash()` method's exact return type
// (`rx_graph/pass_signature.h`: `[[nodiscard]] uint64_t hash() const`),
// and `specializationBits` is spelled identically. The CALLER of
// `recordDrawList()`/`resolveDrawGroups()` supplies
// `passSignatureHash`/`specializationBits` (whoever is recording a
// specific pass knows its own `PassSignature::hash()`) -- `ViewLists`
// itself stays pass-agnostic (one built draw list may legitimately feed
// more than one pass, e.g. a depth prepass and a forward pass, with
// different pass signatures) and never carries either field.
//
// The resolved TOKEN is a plain `uint64_t`, not `uint32_t`: `VkPipeline`
// is a `VK_DEFINE_NON_DISPATCHABLE_HANDLE` (vendored `vulkan_core.h`),
// which resolves to either a `uint64_t` or an 8-byte pointer-typed handle
// depending on `VK_USE_64_BIT_PTR_DEFINES` -- 8 bytes either way on every
// platform this project targets -- so a real `VkPipeline` bit-cast
// directly into this token (the production binding's own job; this
// device-free library never interprets it) round-trips exactly, with no
// truncation. `ResolvedDrawGroup::pipelineToken` is widened to match.
// ---------------------------------------------------------------------

// Device-free mirror of `rx::material::PipelineRequest`'s cache-key
// inputs -- see this section's own top comment for the field-by-field
// correspondence. A plain, trivially-comparable value type (matches this
// codebase's own `PassSignature`/`MaterialFixedFunctionState` convention
// for such keys).
struct PipelineRequestKey {
    uint32_t materialIndex = 0;
    uint64_t passSignatureHash = 0;
    uint32_t specializationBits = 0;

    bool operator==(const PipelineRequestKey&) const = default;
};

// One contiguous run of `ViewLists::commands` sharing the same resolved
// `PipelineRequestKey` (and therefore, in production, the same real
// `VkPipeline`) -- the ONLY thing a `RecordChunkFn` chunk callback ever
// sees.
struct ResolvedDrawGroup {
    uint32_t firstCommand = 0;
    uint32_t commandCount = 0;
    uint64_t pipelineToken = 0;  // opaque 64-bit; whatever PipelineResolveFn returned for this group -- wide enough for a real VkPipeline, see this section's own top comment

    bool operator==(const ResolvedDrawGroup&) const = default;
};

// MAIN-THREAD-ONLY by contract (D27/D5) -- called exactly once per distinct
// `DrawPayload::materialIndex` boundary encountered in `resolveDrawGroups()`'s
// single linear scan, NEVER from a worker chunk. A production binding
// mirrors `MaterialSystem::getPipeline(PipelineRequest)`'s own signature
// shape exactly (material+pass+specialization -> VkPipeline, encoded into
// the opaque `uint64_t` token here); this device-free library never names
// `MaterialSystem`/`PipelineRequest` itself.
using PipelineResolveFn = std::function<uint64_t(const PipelineRequestKey& key)>;

// Worker-safe: receives ONLY plain, pre-resolved data (`groups`) -- never
// `PipelineResolveFn`, never `lists.payloads` (a chunk records commands
// purely from `groups`' command-index ranges plus whatever else the
// production binding closes over that is itself worker-safe, e.g. a
// `VkCommandBuffer` slice).
using RecordChunkFn = std::function<void(uint32_t chunkIndex, uint32_t chunkCount, std::span<const ResolvedDrawGroup> groups)>;

// The single linear scan [D27]: groups `lists.commands` into contiguous
// materialIndex-identical runs, calling `resolvePipeline` exactly once per
// run -- with `passSignatureHash`/`specializationBits` (constant for this
// one call, supplied by the caller -- see this section's own top comment)
// folded into every `PipelineRequestKey` alongside that run's own
// materialIndex -- main-thread-guarded internally via
// RX_ASSERT_MAIN_THREAD (see draw_list.cpp). O(commands.size()) -- one
// pass, no second grouping sort.
//
// Main-thread-only (D5/D27).
[[nodiscard]] std::vector<ResolvedDrawGroup> resolveDrawGroups(const ViewLists& lists, uint64_t passSignatureHash,
                                                                  uint32_t specializationBits,
                                                                  const PipelineResolveFn& resolvePipeline);

// Pre-resolves (see resolveDrawGroups() above, called internally, on the
// calling/main thread, BEFORE any fan-out) then fans `recordChunk` out
// across exactly `chunkCount` chunks via `scheduler.parallelFor()` --
// `recordChunk` may run on any of `scheduler`'s worker threads (or the
// calling thread, per `Scheduler::parallelFor()`'s own contract) and never
// touches `resolvePipeline` again.
//
// Main-thread-only entry (D5) -- the pre-resolution half runs on the
// calling thread; the fan-out half is `scheduler.parallelFor()`'s own
// documented any-thread-safe-to-call-from contract (may itself be called
// from within an outer chunk, matching `parallelFor()`'s own nesting rule).
void recordDrawList(const ViewLists& lists, task::Scheduler& scheduler, uint32_t chunkCount, uint64_t passSignatureHash,
                     uint32_t specializationBits, const PipelineResolveFn& resolvePipeline,
                     const RecordChunkFn& recordChunk);

namespace detail {

// Test-only seam -- NOT part of the stable public contract, mirroring
// `rx::material::detail::debugCompileCount()`'s own carve-out convention.
// `ViewLists`'s OWN vectors are directly inspectable by any test (public
// fields); `DrawListBuilder`'s PRIVATE reused scratch storage (cull
// reasons/visible-index lists/pending draw records) is not otherwise
// observable from outside, so the zero-alloc test needs this one seam to
// cover the full `build()`/`buildShadow()` call, not just its public
// output struct.
struct DrawListBuilderCapacitiesForTesting {
    size_t cullReasons = 0;
    size_t visibleIndices = 0;
    size_t cameraVisibleIndices = 0;
    size_t pending = 0;
    size_t opaqueRecords = 0;
    size_t blendRecords = 0;
    size_t opaqueGroups = 0;
};
// (capacitiesForTesting() itself is forward-declared above, right before
// `class DrawListBuilder` -- not re-declared here, just defined in
// draw_list.cpp.)

}  // namespace detail

}  // namespace rx::scene
