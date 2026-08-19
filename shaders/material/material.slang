// rx_material's engine-defined Slang interface every material module
// conforms to [spec Phase 3 design, D6]. Follows the Shader Components
// lineage (He et al. 2017) and its production evolution in Slang --
// Falcor's material system is the direct shipping precedent for
// "material = Slang module implementing an engine interface"
// (.superpowers/sdd/2026-08-10-phase3-render-graph-materials/
// research-abi-materials.md S2.2).
//
// `public` on every declaration here is load-bearing, not decorative: this
// file is a Slang MODULE (imported via `import material;` from both
// forward_entry.slang and every material module), and Slang's access
// control defaults an unmodified top-level declaration to `internal`
// (module-local) visibility -- an importing module cannot see IMaterialShader
// or MaterialVertex at all without the explicit `public` on each
// declaration (and, for MaterialVertex, on each field: a struct's default
// member visibility only inherits its enclosing type's own visibility
// starting at Slang language version 2026, and this file does not opt into
// `#language slang 2026`, so fields need their own explicit `public` too)
// [Slang user guide, "Modules and Access Control" -- Access Control].

// [Phase 4 Task 16, D8/D26.1] Gained `tangent` (the D8 pooled-vertex tangent
// field, w = handedness -- see forward_entry.slang's own "VERTEX INPUT
// LAYOUT" comment: this was a prerequisite normal mapping needs that no
// earlier task's file list named) and the "lighting surface"
// (`lightDirWorld`/`lightColor`/`ambientColor`) -- per-pass values resolved
// once in forward_entry.slang's vertexMain (from the D26.1 bindless
// DrawData row, see RxDrawData below) and carried through as plain
// (constant-per-triangle, so interpolation is a no-op) fragment-stage
// inputs, rather than re-fetched by every material's own evaluate(). This
// mirrors shaders/multipass/lit.vert.slang's own established "per-pass
// constant travels via the vertex-output struct" idiom.
//
// [D7/matrix-issue08 deferral] Deliberately NOT carrying a second UV set
// (TEXCOORD_1) or a vertex-color (COLOR_0) field -- D7 defers both at
// IMPORT time (log-don't-drop; the importer's own diagnostic is the
// extension point), and this Phase-4 vertex format has nowhere to carry
// them even if a material wanted to consume one (D8's pooled vertex layout
// itself has no uv1/color0 attribute). This comment is the code-location a
// future implementer adding either attribute needs to extend, matching
// D13's own "code comment referencing the Stage-2 migration" precedent for
// reversed-Z.
public struct MaterialVertex {
    public float3 worldPos;
    public float3 normal;
    public float4 tangent;   // xyz = tangent direction (world space), w = handedness sign (+-1).
    public float2 uv;
    public float3 lightDirWorld;  // unit vector FROM the surface TOWARD the light.
    public float3 lightColor;     // rgb = light color * intensity.
    public float3 ambientColor;   // [FG1] interim flat ambient/environment term.

    // [BRDF baseline] World-space camera/eye position -- the one piece of
    // per-pass (not per-vertex) data StandardPBR's specular term genuinely
    // needs and neither `worldPos` nor any field above supplies: Schlick
    // Fresnel/GGX both need a view direction (`normalize(cameraPosWorld -
    // worldPos)`), which cannot be reconstructed from `normal`/`worldPos`
    // alone. Resolved once in vertexMain from the SAME RxDrawData row as
    // lightDirWorld/lightColor/ambientColor above (see that struct's own
    // `cameraPosWorld` field) and carried through identically.
    public float3 cameraPosWorld;
}

// Every material module (e.g. a material's own .slang file, loaded via
// MaterialSystem::loadMaterial()) declares a struct conforming to this
// interface plus a `ParameterBlock<TParams> gParams` global holding its
// bound parameters [D6, D8] -- see forward_entry.slang's own header
// comment for the exact link-time-specialization contract a conforming
// module must follow, and shaders/material/tests-equivalent
// src/rx_material/tests/data/test_unlit.slang for a worked minimal
// example.
public interface IMaterialShader {
    float4 evaluate(MaterialVertex v);
}

// --- Bindless texture sampling for materials [Task 4, seed-10 carried] --
// Phase 3's Task 8 review (.superpowers/sdd/2026-08-10-phase3-render-graph-
// materials/task-8-review.md, "The central question: texture path vs.
// texture sampling") found that a material's `gParams` could already carry
// a real bindless texture INDEX end to end (create -> setTexture -> GPU-
// visible blob) but no material had any way to actually SAMPLE it: neither
// this file nor forward_entry.slang declared a single bindless resource of
// any kind. This section closes exactly that gap.
//
// Declared HERE, not forward_entry.slang, because every material module
// already `import material;` (see src/rx_material/tests/data/
// test_textured.slang) -- forward_entry.slang itself never samples a
// texture, only material modules do, so the globals a material calls must
// live in the module materials actually import. `public` is omitted on
// `gTextures`/`gSamplers`/`gMaterialGlobals` themselves (unlike
// MaterialVertex/IMaterialShader above) because nothing outside this
// module ever refers to them by name -- only `rx_sampleTexture` below
// (which IS `public`) touches them, and Slang's per-declaration access
// control governs cross-module NAME lookup, not same-module use.
//
// SET 0 -- same three-binding global bindless descriptor set every other
// shader in this codebase declares against the real rx::rhi::BindlessTable
// (binding 0 = unbounded `Texture2D[]` sampled images, binding 1 =
// unbounded `SamplerState[]` samplers -- see e.g.
// shaders/multipass/lit.vert.slang's identical declaration and
// rx_rhi_vk/bindless.h's own `kSampledImageBinding`/`kSamplerBinding`
// constants). `MaterialSystem::reflectMaterialLayout()`
// (src/rx_material/material_system.cpp) recognizes exactly these two
// bindings at set 0 as `rx::rhi::PipelineLayoutBuilder`'s already-existing
// "external set-0 substitution" shape (Phase 3) and folds them into the
// SAME `BindlessTable` every other set-0 binding in this engine already
// binds -- this does not create a new descriptor set.
//
// PRESENT IN EVERY MATERIAL, NOT JUST TEXTURE-SAMPLING ONES -- verified
// directly against this project's shipped Slang build before relying on
// it, not assumed (see task-4-report.md): `import material;` pulls these
// globals into a material's own linked program regardless of whether that
// material's `evaluate()` ever calls `rx_sampleTexture` -- Slang does NOT
// dead-code-eliminate an explicitly `[[vk::binding(...)]]`-decorated global
// (nor `gMaterialGlobals` below) just because nothing in the composite's
// reachable entry-point code happens to read it. So `reflectMaterialLayout()`
// treats these as present (not merely "possibly present, tolerated if so")
// for every material -- test_material_system.cpp's own reflection test
// asserts exactly this for test_unlit.slang, which never references any of
// them. This is harmless either way: the same external BindlessTable is
// substituted into set 0 regardless of which of its three slots a given
// material's own SPIR-V happens to touch.
[[vk::binding(0, 0)]]
Texture2D gTextures[];

[[vk::binding(1, 0)]]
SamplerState gSamplers[];

// DEFAULT SAMPLER INDEX -- the bindless SAMPLER slot `rx_sampleTexture`'s
// two-argument overload (below) reads has to come from somewhere at
// runtime, since a compile-time-fixed slot number (e.g. "sampler 0") was
// deliberately rejected: `rx::rhi::BindlessTable` is this engine's single
// PROCESS-WIDE bindless set (bindless.h's own class comment), so nothing
// guarantees MaterialSystem's own default sampler is the first one ever
// registered against a table other subsystems (present/future: shadow
// maps, a KTX2 sampler cache, ImGui, ...) also register samplers into --
// baking in index 0 would be a correctness landmine the instant two
// subsystems share one table in any order other than "MaterialSystem
// first".
//
// Instead this follows the SAME idiom shaders/multipass/lit.vert.slang and
// tonemap.vert.slang already use for a bindless index that is not known
// until runtime: a push constant. `MaterialSystem` (material_system.cpp)
// creates and registers exactly one default LINEAR-filtering sampler
// against the real `BindlessTable` once, in `create()`, and
// `bindInstance()` writes that sampler's REAL registered bindless index
// into this push-constant range on every draw-time bind -- so this works
// correctly no matter which index the table actually assigns. LINEAR (not
// NEAREST, unlike sample 05's shadow/tonemap sampler, which has its own
// depth-comparison/1:1-copy reasons to use NEAREST) is this engine's
// chosen default for ordinary material albedo/color-map sampling.
//
// [Fix round, sampler-wrap P0] THIS IS NO LONGER "the one sampler every
// material's every texture slot samples through" -- that was exactly the
// bug (a real, shipped defect, not a hypothetical): DamagedHelmet's own
// TEXCOORD_0 V legally lies in [1.0005, 1.9987] (glTF-spec-legal, relies
// on the glTF-default REPEAT wrap), and this process-wide default was
// hardcoded CLAMP_TO_EDGE, so every fragment's V clamped to 1.0 and the
// whole mesh sampled each texture's bottom edge row. `rx_sampleTexture`
// now has a THREE-argument overload taking an explicit per-slot sampler
// index (see below) -- StandardPbrParams/UnlitParams (standard_pbr.slang/
// unlit.slang) carry one bindless sampler index per texture slot,
// populated at material-resolution time from `TextureCache::
// getOrCreateSamplerBindlessIndex()` (src/rx_asset/texture_cache.h),
// which builds the REAL per-texture VkSampler honoring that texture's own
// glTF wrap/filter state -- exactly the mechanism this comment used to say
// was "real, supportable future work... not built speculatively here".
// `MaterialSystem::create()`'s own default sampler (this push-constant
// field) is now REPEAT by default -- glTF's OWN documented default
// ("sampler unspecified" -> repeat wrapping, auto filtering) -- and
// exists only as (a) the two-argument overload's sampler for a material
// that does not wire per-slot sampling at all, and (b) resolveSamplerIndex
// ()'s own belt-and-suspenders fallback (samples/08_gltf_viewer/main.cpp)
// if a real per-texture sampler somehow fails to build. A caller that
// genuinely wants a NON-default wrap mode for this fallback (Task 4's own
// seam-bleed GPU test, which samples a deliberately non-tiled 2x2 test
// texture right up to its UV 0/1 edge and needs CLAMP_TO_EDGE to avoid
// wrong-neighbor bleed) now requests it EXPLICITLY via `MaterialSystem::
// create()`'s own `defaultSamplerAddressMode` parameter -- see that
// method's header comment and test_api_factory.cpp's own seam-bleed
// TEST_CASEs.
// --- D26.1 per-draw addressing [Phase 4 Task 16, gate ruling RC1/#8] -----
// One bindless StructuredBuffer<RxDrawData> array at
// BindlessTable::kStorageBufferBinding (binding 2, the SAME external set-0
// slot every other bindless storage-buffer consumer in this engine already
// uses -- shaders/stress/instanced.vert.slang's `gInstances`/shaders/
// multipass/lit.vert.slang's `gTransforms` are the exact same shape, just
// declared per-pass in THOSE samples' own shader files instead of here,
// since neither of them shares an entry point with any other material the
// way forward_entry.slang's vertexMain does). `gDrawData[bufferIndex][row]`
// -- the OUTER index selects WHICH registered storage buffer (this
// engine's `defaultSamplerIndex`-style "read the real registered bindless
// index from a push constant, never assume slot 0" discipline --
// rx_sampleTexture's own header comment explains why a hardcoded outer
// index would be a landmine the instant two subsystems share one
// BindlessTable instance); the INNER index is `SV_VulkanInstanceID`
// (forward_entry.slang's vertexMain) -- NOT `SV_InstanceID`: Slang
// SUBTRACTS `firstInstance` back out of `SV_InstanceID` to preserve
// D3D-compatible semantics (verified against Slang's own SPIR-V-target
// docs before relying on this -- gate matrix-issue08's own verified
// finding), so an ordinary SV_InstanceID read would silently give a
// per-draw-RELATIVE (always-0-on-a-single-draw) index instead of the
// absolute row D26.1's bindless addressing requires. This is the
// "never per-draw push constants" mechanism D26.1 mandates -- see
// samples/07_stress/main.cpp's own `gPush.instanceIndex` for the anti-
// pattern this replaces (a push-constant-carried per-draw INDEX). Field
// layout mirrors `rx::material::DrawDataGpu`
// (rx_material/include/rx_material/draw_data.h) EXACTLY -- see that
// header's own comment for the cross-file drift risk and why both sides
// must be edited together.
// `public` on both the struct and the global is load-bearing here (unlike
// gTextures/gSamplers/gMaterialGlobals below, which stayed module-internal
// under the pre-Task-16 design since only THIS module's own
// rx_sampleTexture() ever named them): forward_entry.slang's vertexMain
// now reads `gDrawData`/`RxDrawData` BY NAME directly, from a DIFFERENT
// Slang module -- Slang's access control defaults an unmodified top-level
// declaration to `internal` (module-local) visibility, exactly the same
// rule MaterialVertex/IMaterialShader's own header comment documents.
public struct RxDrawData {
    public float4x4 model;
    public float4x4 normalMatrix;
    public float4x4 viewProj;
    public float4 lightDirWorld;
    public float4 lightColor;
    public float4 ambientColor;

    // [BRDF baseline] World-space camera/eye position, xyz (w unused
    // padding, matching every other vector field in this struct) --
    // per-pass, repeated per row, same "simpler than a second bindless
    // buffer" precedent as viewProj/light/ambient above. See
    // MaterialVertex's own `cameraPosWorld` comment (this file, above) for
    // why this is unavoidable: Fresnel/specular need a real view direction,
    // not derivable from worldPos/normal alone.
    public float4 cameraPosWorld;

    public uint materialIndex;
    public uint _pad0;
    public uint _pad1;
    public uint _pad2;
}

[[vk::binding(2, 0)]]
public StructuredBuffer<RxDrawData> gDrawData[];

// [Phase 4 Task 16] Gained `drawDataBufferIndex` (the D26.1 bindless
// STORAGE BUFFER index -- see RxDrawData/gDrawData above) and `exposure`
// (the sample 08 `--exposure` CLI flag's pre-tonemap `2^exposure`
// multiplier, applied in forward_entry.slang's fragmentMain immediately
// after a material's own evaluate() returns -- BEFORE the value ever
// reaches the separate, UNTOUCHED shared tonemap pass; exposure=0.0 is the
// neutral value, `2^0 == 1`, a byte-identical no-op regression guard for
// every sample that does not care about exposure). Kept ALL-SCALAR
// (uint/float only, no vector/matrix field) deliberately -- see
// shaders/multipass/lit.vert.slang's own header comment for the
// empirically-verified reason this project avoids vector/matrix fields
// inside a push-constant block; every genuinely vector-shaped per-pass
// value (viewProj, light/ambient) lives in RxDrawData above instead,
// reached indirectly via `drawDataBufferIndex`. Field layout mirrors
// `rx::material::MaterialGlobalsPush` (draw_data.h) EXACTLY.
//
// `public` is now load-bearing here too (grew from module-internal under
// the pre-Task-16 design): forward_entry.slang's vertexMain/fragmentMain
// both read `gMaterialGlobals` BY NAME directly now (`drawDataBufferIndex`,
// `exposure`), not just indirectly through this module's own
// rx_sampleTexture() -- same access-control rule as RxDrawData/gDrawData
// above.
public struct RxMaterialGlobals {
    public uint defaultSamplerIndex;
    public uint drawDataBufferIndex;
    public float exposure;
}

[[vk::push_constant]]
public ConstantBuffer<RxMaterialGlobals> gMaterialGlobals;

// [Fix round, sampler-wrap P0] Primary entry point every PER-SLOT-sampler-
// aware material's `evaluate()` calls (StandardPbr/Unlit -- standard_pbr.
// slang/unlit.slang) to sample a bindless-indexed texture through an
// EXPLICIT bindless-indexed sampler -- both `gParams`'s own `uint` fields,
// per this engine's D8 bindless-index convention. `uv` is passed straight
// through to `Texture2D.Sample()` with no wrapping/clamping decision of
// this function's own -- that is entirely `samplerIndex`'s own real,
// per-texture VkSampler's job (built by `TextureCache::
// getOrCreateSamplerBindlessIndex()`, honoring that texture's own glTF
// wrap/filter state), not a per-call concern here.
public float4 rx_sampleTexture(uint textureIndex, uint samplerIndex, float2 uv) {
    return gTextures[textureIndex].Sample(gSamplers[samplerIndex], uv);
}

// Two-argument overload, UNCHANGED signature from before this fix round --
// every existing caller that does not wire its own per-slot sampler (the
// test-only test_textured.slang/test_textured_sample.slang fixtures, any
// future material that genuinely wants "one shared sampler for every
// slot") keeps compiling and behaving exactly as documented: samples
// through `gMaterialGlobals.defaultSamplerIndex`, the ONE process-wide
// default this file's own header comment (above) now describes in full.
public float4 rx_sampleTexture(uint textureIndex, float2 uv) {
    return rx_sampleTexture(textureIndex, gMaterialGlobals.defaultSamplerIndex, uv);
}
