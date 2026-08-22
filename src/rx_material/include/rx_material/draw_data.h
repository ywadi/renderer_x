#pragma once
// rx_material/draw_data.h -- D26.1's C++ mirror of the GPU-side per-draw
// addressing shapes `shaders/material/material.slang` declares (`RxDrawData`,
// `RxMaterialGlobals`). Thread-affinity: none (plain POD; every field is a
// value type with no synchronization of its own).
//
// WHY THIS HEADER EXISTS (not just duplicated inline where each caller needs
// it): this project has already been bitten once by exactly this class of
// duplication drifting out of sync -- see shaders/multipass/scene_types.slang's
// own header comment ("ObjectTransform used to be declared [in two files]...
// they drifted"). D26.1 needs the IDENTICAL byte layout known in (at least)
// three places -- MaterialSystem's own default per-draw buffer
// (material_system.cpp), samples/08_gltf_viewer's real per-submesh buffer,
// and rx_material's own D26.1 GPU tests -- so this header is the ONE place
// that layout is spelled out in C++, matching material.slang's own
// `RxDrawData`/`RxMaterialGlobals` struct declarations field-for-field.
// EVERY field ordering/type below must be kept in exact lockstep with
// material.slang's copy; there is no automated cross-check (Slang structs
// and C++ structs are two different compilers), so any future edit to
// EITHER side must edit both, in the same commit, and re-run the D26.1
// GPU tests (they read pixels rendered FROM this exact byte layout, so a
// drift shows up as a wrong-pixel failure, not a silent corruption).
//
// LAYOUT CONVENTION: RxDrawData is read back via Slang's `StructuredBuffer<T>`
// (std430-like layout: scalars/vectors pack at their own natural alignment,
// mat4 is 4 consecutive 16-byte-aligned float4 "rows" -- see the MATRIX
// LAYOUT paragraph below -- no surprise padding unlike a push-constant
// block; see rx_rhi_vk's PipelineLayoutBuilder header comment on why THIS
// project avoids float3/mat4 fields inside a push-constant struct instead,
// and lit.vert.slang's own header comment for the empirical finding that
// motivated it).
//
// MATRIX LAYOUT -- read before writing any code that populates a
// DrawDataGpu row: MaterialSystem's own `slang::ISession` is built via a
// plain, default-constructed `slang::SessionDesc{}` (material_system.cpp's
// createSession(), verified directly -- no `defaultMatrixLayoutMode`
// override anywhere in that file), which leaves that field at Slang's own
// API default -- ROW-major -- EXACTLY the same session-construction shape
// (and therefore the same matrix-layout default) `rx_shader::Compiler::
// create()`'s own documented MATRIX LAYOUT paragraph (compiler.h) describes
// for the identical reason. That default applies uniformly to every
// `float4x4` a material.slang-importing shader reads from memory, a
// StructuredBuffer<RxDrawData> element included -- NOT just push-constant
// blocks. glm::mat4 defaults to COLUMN-major storage (this engine already
// depends on GLM, `rx_core` links `glm::glm` PUBLIC) -- the OPPOSITE
// convention. Every writer of `model`/`normalMatrix`/`viewProj` below MUST
// `glm::transpose()` each matrix immediately before writing its bytes into
// a real DrawDataGpu row, exactly matching `samples/03_bindless_mesh/
// main.cpp`'s own `updateTransforms()` precedent (`glm::transpose(viewProj
// * model)` right before upload) -- skipping this produces a silently
// garbled (transposed) transform, not a validation error or a crash.

#include <glm/glm.hpp>
#include <cstdint>

namespace rx::material {

// One row of the bindless per-draw StructuredBuffer StandardPBR/Unlit's
// vertexMain reads via `gDrawData[gMaterialGlobals.drawDataBufferIndex]
// [SV_VulkanInstanceID]` [D26.1, gate ruling RC1/#8: SV_VulkanInstanceID, NOT
// SV_InstanceID -- Slang subtracts firstInstance back out of SV_InstanceID
// to preserve D3D-compatible semantics, so an ordinary SV_InstanceID read
// would silently give a per-draw-relative (always-0-on-a-single-draw) index
// instead of the absolute bindless-buffer row D26.1 requires]. One row per
// DRAW (one vkCmdDrawIndexed call, addressed by that draw's own
// `firstInstance` argument), never per hardware-instanced-copy (Phase 4 never
// issues instanceCount > 1 -- see recordDrawList's own future "instancing
// collapse" note, D26 point 3, registry).
//
// `viewProj`/`lightDirWorld`/`lightColor`/`ambientColor`/`cameraPosWorld`
// are genuinely PER-PASS (not per-draw) values, redundantly repeated on
// every row -- the
// SAME "simpler than a second one-row bindless buffer just for it" precedent
// shaders/multipass/lit.vert.slang's own header comment already establishes
// for its own per-pass `lightDirWorld`. This sidesteps needing a SECOND
// StructuredBuffer<T> binding (Slang/SPIR-V requires one binding per
// distinct element TYPE, and material.slang already spends its one bindless
// storage-buffer slot -- BindlessTable::kStorageBufferBinding -- on this
// exact type) at the cost of a few redundant bytes per draw, which is
// negligible at Phase 4's per-frame draw counts.
struct DrawDataGpu {
    glm::mat4 model{1.0F};         // object -> world.
    glm::mat4 normalMatrix{1.0F};  // transpose(inverse(mat3(model))), widened to mat4 (upper-left 3x3 used; see
                                    // this header's own header comment on why mat4 not mat3 -- alignment simplicity).
    glm::mat4 viewProj{1.0F};      // world -> clip. Per-pass, repeated per row (see this struct's own comment).

    // xyz = unit vector FROM the shaded surface TOWARD the light (i.e. the
    // negated light-travel direction for a directional light); w unused
    // (padding -- keeps this a plain float4 for predictable std430 packing,
    // matching every other vector field in this struct).
    glm::vec4 lightDirWorld{0.0F, 1.0F, 0.0F, 0.0F};
    glm::vec4 lightColor{1.0F, 1.0F, 1.0F, 0.0F};    // rgb = color * intensity; w unused.
    // [FG1, RETIRED by Phase 5 Task 10, #46] StandardPbr's evaluate() no
    // longer reads this field -- see standard_pbr.slang's own header
    // comment. Kept, unconsumed, rather than removed/renamed -- see
    // material.slang's own `MaterialVertex::ambientColor` header comment
    // for why (avoids an unrelated mass edit across every existing
    // DrawDataGpu producer/test that still populates it).
    glm::vec4 ambientColor{0.03F, 0.03F, 0.03F, 0.0F};

    // [BRDF baseline] World-space camera/eye position, xyz (w unused) --
    // per-pass, repeated per row like the three fields above. Genuinely
    // required, not a convenience: Schlick Fresnel/GGX specular need a real
    // view direction (`normalize(cameraPosWorld - worldPos)`), which cannot
    // be reconstructed from `worldPos`/`normal` alone -- see
    // material.slang's own `MaterialVertex::cameraPosWorld` comment.
    glm::vec4 cameraPosWorld{0.0F, 0.0F, 0.0F, 0.0F};

    // Informational (matches Task 19's future DrawPayload shape, which also
    // carries a material index alongside the instance/transform index) --
    // StandardPBR/Unlit's own evaluate() does not read this field in Phase
    // 4 (a draw's material is already fixed by which VkPipeline/descriptor
    // set the CPU bound before issuing it); kept so the D26.1 two-draw test
    // has a second, independently-varying field to assert on beyond the
    // transform, and so a future GPU-driven consumer sorting/grouping by
    // material does not need to widen this row's layout to add it.
    uint32_t materialIndex = 0;
    uint32_t _pad0 = 0;
    uint32_t _pad1 = 0;
    uint32_t _pad2 = 0;

    // [Phase 4 Stage 2 Task 22 fix round, F1, spec D21] Mirrors
    // material.slang's RxDrawData shadow fields EXACTLY (added at the same
    // struct-tail position on both sides). `shadowMapTextureIndex`
    // defaults to the "no shadow map bound" sentinel (0xFFFFFFFF) --
    // every EXISTING producer of a DrawDataGpu row (MaterialSystem's own
    // default single-row buffer, samples 06/08's own per-draw buffers,
    // this library's own GPU test fixtures) that never sets this field
    // gets BYTE-IDENTICAL pre-Task-22 shading (rx_sampleShadowPCF()
    // returns 1.0 unconditionally for the sentinel -- see material.slang's
    // own comment).
    glm::mat4 lightViewProj{1.0F};  // world -> light clip, STANDARD-Z [D13]. Transposed before upload, same MATRIX LAYOUT convention as every other matrix field above.
    uint32_t shadowMapTextureIndex = 0xFFFFFFFFu;  // bindless SAMPLED_IMAGE index of the shadow map; sentinel = shadows disabled for this draw.
    uint32_t shadowCompareSamplerIndex = 0;        // bindless COMPARISON-SAMPLER index (BindlessTable::kComparisonSamplerBinding).
    float shadowTexelSize = 0.0F;                  // UV-space PCF tap step (1.0 / shadow map resolution).
    uint32_t _padShadow = 0;

    // [Phase 5 Task 10, #46, FG1 closure] Mirrors material.slang's
    // RxDrawData environment fields EXACTLY (added at the same struct-tail
    // position on both sides -- see this header's own top comment on why
    // both sides must move together). `envIrradianceCubeIndex ==
    // 0xFFFFFFFF` (the default below) is the "no environment configured"
    // sentinel -- every EXISTING producer of a DrawDataGpu row that never
    // sets these fields (samples 06/09, every pre-Task-10 GPU test rig)
    // gets ZERO indirect/IBL contribution, per standard_pbr.slang's own
    // header comment on why this is a real, intentional behavior change
    // from the RETIRED `ambientColor` term above, not a byte-identical
    // default.
    uint32_t envIrradianceCubeIndex = 0xFFFFFFFFu;   // bindless CUBE index (BindlessTable::kCubeSampledImageBinding).
    uint32_t envPrefilteredCubeIndex = 0xFFFFFFFFu;  // bindless CUBE index.
    uint32_t envDfgLutIndex = 0xFFFFFFFFu;           // bindless SAMPLED_IMAGE (2D) index.
    uint32_t envCubeSamplerIndex = 0;                // bindless SAMPLER index, trilinear-across-mips.
    uint32_t envDfgSamplerIndex = 0;                 // bindless SAMPLER index, bilinear clamp-to-edge.
    float envMaxPrefilteredLod = 0.0F;               // prefilteredCubemap mip count - 1.
    float envIntensity = 0.0F;                       // PRE-EXPOSED physical-units scalar (Camera::exposure() already applied).
    // [MATRIX LAYOUT / std430 ARRAY STRIDE -- LOAD-BEARING, see this
    // header's own top comment] ONE padding field, not decorative --
    // rounds the struct's size (AS OF Task 10) up to a multiple of 16
    // bytes, matching the per-element STRIDE Slang's `StructuredBuffer<
    // RxDrawData>[]` (material.slang) actually uses on the GPU side
    // (std430's array-of-struct stride-rounds-to-largest-member-alignment
    // rule, 16 bytes here because of this struct's several float4x4/
    // float4 fields). Omitting this padding reproduced a REAL bug directly
    // during Task 10's own development: row 0 of a multi-row DrawDataGpu
    // buffer still read correctly (offset 0 either way), but every row
    // after it read progressively misaligned/garbage data, because a C++
    // producer's tight `sizeof(DrawDataGpu) * rowCount` packing silently
    // disagreed with the shader's own (16-byte-rounded) real stride. KEPT
    // (not removed/reused) even though Task 13 appends more fields after
    // it below -- every subsequent std430-aligned append must land AFTER
    // this field, at the 384-byte boundary it establishes, never before
    // it (an earlier draft of this task's own diff got this backwards --
    // see this file's own git history / task-13-report.md for the
    // disclosed self-caught defect).
    float _padEnv0 = 0.0F;

    // [Phase 5 Stage 2 Task 13, #49] Generalizes the "one directional
    // light" term above (`lightDirWorld`/`lightColor`) to Directional/
    // Point/Spot -- see material.slang's own RxDrawData mirror + this
    // struct's own top comment for the full field-by-field rationale.
    // `lightDirWorld`/`lightColor` KEEP their existing semantics for
    // Directional (constant per-pass "toward light" unit vector / lux
    // color*intensity, unattenuated); for Point/Spot, `lightColor` instead
    // holds `color*intensity` in CANDELA and the true per-fragment "toward
    // light" vector is derived in-shader from `lightPositionWorld`. Every
    // field below defaults to the exact value that reproduces byte-
    // identical Directional-only (pre-Task-13) behavior for a row that
    // never sets them (lightType==0==Directional, lightRange==0==no
    // window -- irrelevant anyway since Directional never attenuates).
    // Appended starting at byte offset 384 (a clean 16-byte boundary,
    // thanks to `_padEnv0` immediately above) -- the four leading scalars
    // (16 bytes) keep the two trailing `vec4`s naturally 16-byte-aligned
    // with zero implicit compiler padding on EITHER the C++ or the Slang
    // std430 side, so this append needs no additional manual pad field.
    uint32_t lightType = 0;        // 0=Directional, 1=Point, 2=Spot [rx::scene::LightType].
    float lightRange = 0.0F;       // Point/Spot inverse-square-window radius; 0.0 == infinite range (no windowing).
    float lightAngleScale = 0.0F;  // Spot cone precomputed scale [rx::scene::lightmath::spotAngleScaleOffset()]; Directional/Point unused.
    float lightAngleOffset = 0.0F; // Spot cone precomputed offset; Directional/Point unused.
    glm::vec4 lightPositionWorld{0.0F};        // Point/Spot world position, xyz (w unused). Directional: unused.
    glm::vec4 lightSpotDirWorld{0.0F, 0.0F, -1.0F, 0.0F};  // Spot's own facing/travel direction, world space, xyz (w unused). Directional/Point: unused.

    // [Phase 5 Stage 2 Task 15, #51] Clustered Forward+ per-pixel lookup
    // inputs -- matrix-p5t15's own row 2 ("extend RxDrawData, don't invent
    // a new mechanism", following the IDENTICAL Task 13 struct-tail-append
    // precedent immediately above). ADDITIVE alongside the single-slot
    // Point/Spot term above (which is left completely UNTOUCHED -- see
    // standard_pbr.slang's own header comment): `clusterEnabled==0` (the
    // default) makes every field below dead, reproducing byte-identical
    // pre-Task-15 behavior for any row that never sets them.
    //
    // Grid SHAPE constants -- the SAME `rx::scene::froxel::
    // FroxelGridParams` a `rx::cluster::ClusterPipelines::
    // addClusterPasses()` call already computed this frame (see that
    // method's own header comment: "a caller needing to interpret froxel
    // indices... uses this SAME value, never re-derives it") -- copied
    // here so `shaders/material/cluster_lighting.slang`'s shading-side
    // lookup maps a shaded fragment's own view-space position to the
    // EXACT SAME froxel `rx::cluster`'s own compute passes used to build
    // this buffer's [offset,count) ranges (a mismatched copy here would
    // silently look up the WRONG froxel's light list, not merely a
    // performance bug).
    uint32_t clusterEnabled = 0;         // 0 == no cluster grid this row's own pass has bound; every field below is dead.
    uint32_t clusterCountX = 0;
    uint32_t clusterCountY = 0;
    uint32_t clusterCountZ = 0;
    float clusterTanHalfFovY = 0.0F;
    float clusterAspectRatio = 0.0F;
    float clusterZLightFar = 0.0F;
    float clusterInvLinearizer = 0.0F;
    // Bindless indices -- `clusterLightsBufferIndex` is a
    // `kClusterLightBufferBinding`-slot index (rx::cluster::ClusterLightGpu[]);
    // the other three are `kGenericStorageBufferBinding`-slot indices
    // (plain uint[] arrays -- T14's own `clusterOffsets`/`clusterWriteCounts`/
    // `clusterLightIndices` render-graph outputs, registered into that
    // bindless slot by this row's own producer -- see BindlessTable's own
    // header comment for why these need a DIFFERENT binding than
    // `kStorageBufferBinding`, which this struct's own `gDrawData` array
    // already occupies).
    uint32_t clusterOffsetsBufferIndex = 0;
    uint32_t clusterWriteCountsBufferIndex = 0;
    uint32_t clusterLightIndicesBufferIndex = 0;
    uint32_t clusterLightsBufferIndex = 0;
    // World -> view. Per-pass, repeated per row -- same "simpler than a
    // second bindless buffer" precedent every other per-pass matrix field
    // in this struct already establishes. Needed ONLY for the clustered
    // lookup's own view-space-position derivation (`viewProj` above stays
    // the projection-included matrix every OTHER per-pass field already
    // uses; this is the plain view-space transform the froxel grid's own
    // math is defined against -- rx::cluster::buildClusterLightList()'s
    // own `viewMatrix` parameter is the SAME matrix, transposed here per
    // this header's own MATRIX LAYOUT convention).
    glm::mat4 clusterView{1.0F};
};
static_assert(sizeof(DrawDataGpu) == 544,
              "DrawDataGpu must stay exactly 544 bytes (432 + 48 grid-shape/bindless-index bytes + 64 cluster-view "
              "matrix bytes, Phase 5 Task 15 -- already a 16-byte multiple with no additional pad) -- mirrors "
              "material.slang's RxDrawData");

// Mirrors material.slang's `RxMaterialGlobals` push-constant struct
// (`[[vk::push_constant]] ConstantBuffer<RxMaterialGlobals> gMaterialGlobals;`)
// field-for-field. Deliberately ALL-SCALAR (no vec/mat fields) -- see
// lit.vert.slang's own header comment for the empirically-verified reason
// this project avoids vector/matrix fields inside a push-constant block
// (Slang's push-constant packing rules are not the predictable std430 rules
// a StructuredBuffer gets); every genuinely vector-shaped per-pass value
// (viewProj, light/ambient) lives in DrawDataGpu above instead, reached
// indirectly via `drawDataBufferIndex`.
//
// [Phase 5 Task 4/#40, gate ruling rulings-2026-08-20.md T4] LOST its
// former `exposure` field: Filament-style PRE-EXPOSURE moved exposure
// application to each RxDrawData producer's own `lightColor`/
// `ambientColor` source (multiplied there by `rx::scene::Camera::
// exposure()`, CPU-side, before upload), not a push-constant scalar this
// shared struct carries -- see material.slang's own `RxMaterialGlobals`
// header comment for the full ruling.
struct MaterialGlobalsPush {
    uint32_t defaultSamplerIndex = 0;  // bindless SAMPLER index -- material.slang's pre-D26.1 field, unchanged in
                                        // shape. [Fix round, sampler-wrap P0] StandardPbr/Unlit no longer sample
                                        // through this for real texture slots (they carry their own per-slot
                                        // sampler indices in gParams) -- this is now only the two-argument
                                        // rx_sampleTexture() overload's fallback; see material.slang's own header
                                        // comment.
    uint32_t drawDataBufferIndex = 0;  // bindless STORAGE BUFFER index of the DrawDataGpu[] this draw reads.
};
static_assert(sizeof(MaterialGlobalsPush) == 8,
              "MaterialGlobalsPush must stay exactly 8 bytes (2 packed 4-byte scalars) -- mirrors material.slang's "
              "RxMaterialGlobals, and bindInstance() (material_system.cpp) pushes exactly this many bytes for it");

// [Phase 5 Task 10, #46, FG1 closure] Mirrors shaders/ibl/skybox.slang's
// own `RxSkyboxData` struct field-for-field -- see that struct's own
// header comment and this header's own top comment (MATRIX LAYOUT/std430
// ARRAY STRIDE) for the same cross-file-drift and 16-byte-struct-stride
// discipline DrawDataGpu/RxDrawData already establish. ONE row, rebuilt
// every frame by samples/08_gltf_viewer's own recordSkybox() call site
// (the camera moves every frame in the interactive present loop).
struct SkyboxDataGpu {
    glm::mat4 invViewProj{1.0F};              // clip -> world. Transposed before upload -- see this header's own MATRIX LAYOUT paragraph.
    glm::vec4 cameraPosWorld{0.0F};           // xyz used; w unused padding.
    uint32_t baseCubeIndex = 0xFFFFFFFFu;     // bindless CUBE index (BindlessTable::kCubeSampledImageBinding).
    uint32_t cubeSamplerIndex = 0;            // bindless SAMPLER index.
    float intensity = 1.0F;                   // PRE-EXPOSED, SAME convention as DrawDataGpu::envIntensity above.
    uint32_t _pad0 = 0;
};
static_assert(sizeof(SkyboxDataGpu) == 96,
              "SkyboxDataGpu must stay exactly 96 bytes (already a multiple of 16 -- mat4[64] + vec4[16] + "
              "4 scalars[16]) -- mirrors shaders/ibl/skybox.slang's RxSkyboxData");

// Mirrors shaders/ibl/skybox.slang's own `SkyboxPush` push-constant struct.
// All-scalar, same reasoning as MaterialGlobalsPush above.
struct SkyboxPush {
    uint32_t skyboxDataBufferIndex = 0;  // bindless STORAGE BUFFER index of the one-row SkyboxDataGpu[] above.
};
static_assert(sizeof(SkyboxPush) == 4, "SkyboxPush must stay exactly 4 bytes (one scalar) -- mirrors skybox.slang's SkyboxPush");

}  // namespace rx::material
