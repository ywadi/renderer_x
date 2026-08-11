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

public struct MaterialVertex {
    public float3 worldPos;
    public float3 normal;
    public float2 uv;
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

// DEFAULT SAMPLER INDEX -- `rx_sampleTexture` below takes only a texture
// index (its signature is fixed by this task's brief: `rx_sampleTexture
// (uint textureIndex, float2 uv)`, no sampler-index parameter), so the
// bindless SAMPLER slot it reads has to come from somewhere else. A
// compile-time-fixed slot number (e.g. "sampler 0") was deliberately
// rejected: `rx::rhi::BindlessTable` is this engine's single PROCESS-WIDE
// bindless set (bindless.h's own class comment), so nothing guarantees
// MaterialSystem's own default sampler is the first one ever registered
// against a table other subsystems (present/future: shadow maps, a KTX2
// sampler cache, ImGui, ...) also register samplers into -- baking in
// index 0 would be a correctness landmine the instant two subsystems share
// one table in any order other than "MaterialSystem first".
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
// chosen default for ordinary material albedo/color-map sampling -- the
// conventional choice for a general-purpose texture lookup. CLAMP_TO_EDGE
// (not REPEAT): this is one process-wide default shared by every material,
// and a material sampling right up to UV 0/1 at a mesh seam gets a well-
// defined edge-duplicated result under CLAMP_TO_EDGE rather than
// wrong-neighbor bleed from REPEAT's wraparound (empirically confirmed via
// this task's own GPU test -- see material_system.cpp's own comment on
// `defaultSamplerInfo` for the full reasoning). There is exactly one such
// sampler per MaterialSystem instance for this phase (no per-material
// sampler/wrap-mode selection yet -- a real, supportable future case for a
// material that genuinely wants tiling, not exercised by any test this
// task writes, so not built speculatively here).
struct RxMaterialGlobals {
    uint defaultSamplerIndex;
}

[[vk::push_constant]]
ConstantBuffer<RxMaterialGlobals> gMaterialGlobals;

// Public entry point every material's `evaluate()` calls to sample a
// bindless-indexed texture (`gParams`'s own `uint` field, per this engine's
// D8 bindless-index convention -- see e.g. test_textured.slang) with this
// engine's single default LINEAR sampler. `uv` is passed straight through
// to `Texture2D.Sample()` with no wrapping/clamping decision of this
// function's own -- that is the SAMPLER's job (fixed at MaterialSystem
// construction time), not a per-call concern here.
public float4 rx_sampleTexture(uint textureIndex, float2 uv) {
    return gTextures[textureIndex].Sample(gSamplers[gMaterialGlobals.defaultSamplerIndex], uv);
}
