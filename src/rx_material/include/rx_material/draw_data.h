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
    glm::vec4 ambientColor{0.03F, 0.03F, 0.03F, 0.0F};  // [FG1] flat interim ambient/environment term; w unused.

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
};
static_assert(sizeof(DrawDataGpu) == 272, "DrawDataGpu must stay exactly 272 bytes -- mirrors material.slang's RxDrawData");

// Mirrors material.slang's `RxMaterialGlobals` push-constant struct
// (`[[vk::push_constant]] ConstantBuffer<RxMaterialGlobals> gMaterialGlobals;`)
// field-for-field. Deliberately ALL-SCALAR (no vec/mat fields) -- see
// lit.vert.slang's own header comment for the empirically-verified reason
// this project avoids vector/matrix fields inside a push-constant block
// (Slang's push-constant packing rules are not the predictable std430 rules
// a StructuredBuffer gets); every genuinely vector-shaped per-pass value
// (viewProj, light/ambient) lives in DrawDataGpu above instead, reached
// indirectly via `drawDataBufferIndex`.
struct MaterialGlobalsPush {
    uint32_t defaultSamplerIndex = 0;  // bindless SAMPLER index -- material.slang's pre-D26.1 field, unchanged in
                                        // shape. [Fix round, sampler-wrap P0] StandardPbr/Unlit no longer sample
                                        // through this for real texture slots (they carry their own per-slot
                                        // sampler indices in gParams) -- this is now only the two-argument
                                        // rx_sampleTexture() overload's fallback; see material.slang's own header
                                        // comment.
    uint32_t drawDataBufferIndex = 0;  // bindless STORAGE BUFFER index of the DrawDataGpu[] this draw reads.
    float exposure = 0.0F;             // pre-tonemap 2^exposure multiplier (0.0 = neutral, no-op: 2^0 == 1).
};
static_assert(sizeof(MaterialGlobalsPush) == 12,
              "MaterialGlobalsPush must stay exactly 12 bytes (3 packed 4-byte scalars) -- mirrors material.slang's "
              "RxMaterialGlobals, and bindInstance() (material_system.cpp) pushes exactly this many bytes for it");

}  // namespace rx::material
