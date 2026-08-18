# Completeness matrix — issue #8: Default material library: StandardPBR + Unlit

**Plan task:** Task 16, "StandardPBR + Unlit + sample 08_gltf_viewer (D22)"
(`docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:316-327`).

**Spec decisions binding this ticket:** D22 (materials: StandardPBR +
Unlit with alpha modes, design doc:322-336) is the primary decision.
D26.1 (per-draw addressing via firstInstance/gl_InstanceIndex into a
bindless storage buffer, never push constants — design doc's D26 point 1,
:396-401) binds both material shaders per the ticket's 2026-08-18
amendment. D17 (tolerance pixel gates, :272-281) binds sample 08's gate.
D8 (Phase-4 pooled vertex format, :167-175) binds the vertex layout
StandardPBR's normal mapping must consume. D10 (textures: KTX2-first,
role-typed formats, :191-206) binds the sRGB/linear and BC5/BC7 choices.
D23 (public ABI, :337-343) bounds what can change on the ABI surface.
D24/D25/D26/D27 (memory-budget/eviction, UploadTicket, GPU-driven
readiness, main-thread pre-resolution) are the binding-rule invariants
this gate must enforce as acceptance-criterion rows wherever they touch
this ticket (rows below).

**Ticket body + amendments (`gh issue view 8 --comments`):** base ticket
text (StandardPBR/Unlit + sample 08, D22); comment 1 adds a
materials-authoring guide requirement (engine sampler conventions,
`rx_sampleTexture` usage); comment 2 (FG1) adds the interim flat
ambient/environment term (uniform ambient color × occlusion, "metals
never render black pre-IBL"); the issue-body amendment (2026-08-18)
adds D26.1's firstInstance/bindless-storage-buffer requirement and names
sample 07's push-constant-per-draw loop as the anti-pattern.

**Sources consulted (in-repo):**
- `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:1-24`
  (Global Constraints), `:316-327` (Task 16).
- `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md` D7,
  D8, D10, D17, D22, D23, D24-D27 (full text read).
- `docs/superpowers/specs/2026-08-10-phase3-render-graph-materials-design.md`
  D5 (ABI rules), D8 ("Parameters vs. specialization split", :190-199).
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/feature-gap-audit.md:46`
  (FG1, environment lighting gap — already ruled V1-blocking, interim
  ambient term amendment lands via this ticket per the issue comment).
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/claim-validation-2026-08-18.md:20,26,57-62`
  (GPU-driven/indirect deferral → D26.1 origin; PSO-warmup/main-thread
  collision → D27 origin).
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/research-p4-assets.md`
  (fastgltf capability survey, KHR_lights_punctual mention only).
- Delivered code (full files read, verified 2026-08-18): `src/rx_material/include/rx_material/material_system.h`,
  `src/rx_material/material_system.cpp` (`getPipeline()` :1694-1824,
  `MaterialRecord` :610-635, `PipelineRequest`/`PipelineKey`), `src/rx_material/include/rx_material/rx_api.h`
  (public ABI — `IRxMaterialInstance::setFloat/setFloat4/setTexture`),
  `shaders/material/material.slang`, `shaders/material/forward_entry.slang`,
  `shaders/multipass/tonemap.frag.slang`+`tonemap.vert.slang`,
  `shaders/multipass/lit.frag.slang` (existing constant-ambient
  precedent), `src/rx_graph/include/rx_graph/pass_signature.h`
  (fixed-function-state exclusion, header comment :33-37),
  `src/rx_platform/include/rx_platform/window.h`+`src/window.cpp`
  (no mouse-delta API exists today), `samples/06_materials/main.cpp`
  (existing orbit is auto-rotating azimuth-only, not drag-driven).
  `src/rx_material/tests/data/test_unlit.slang` (current minimal Unlit
  test material).

**Sources consulted (external, fetched/searched 2026-08-18):**
- glTF 2.0 core spec: `github.com/KhronosGroup/glTF` `main` branch,
  `specification/2.0/Specification.adoc` and
  `specification/2.0/schema/material*.schema.json` (raw content fetched
  directly).
- Khronos glTF extension registry: `github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos`
  (directory listing) plus individual `README.md` for
  `KHR_materials_unlit` and `KHR_texture_transform` (fetched directly).
- `github.com/KhronosGroup/glTF-Sample-Renderer` `source/Renderer/shaders/pbr.frag`
  (current name of the former glTF-Sample-Viewer repo — fetched
  directly).
- Filament: `google.github.io/filament/Filament.md.html` ("Physically
  Based Rendering" chapter, fetched directly).
- Slang docs: `docs.shader-slang.org/.../a2-01-spirv-target-specific.html`
  (SPIR-V-target-specific semantics, fetched directly) and
  `github.com/shader-slang/slang` issues #6876/#6457 (search-surfaced,
  not independently opened).
- Vulkan: `docs.vulkan.org/refpages/.../InstanceIndex.html` (search
  digest), `docs.vulkan.org/spec/latest/chapters/textures.html`
  (depth-compare-operation section, fetched directly),
  `docs.vulkan.org/spec/latest/chapters/primsrast.html`
  (`depthClampEnable`, fetched directly — the literal depth-bias
  equation text could not be retrieved through the fetch tool despite
  three attempts; see Verification health).
- Microsoft HLSL specs: `microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/`
  (SV_InstanceID/StartInstanceLocation semantics, search digest).
- three.js (`mrdoob/three.js`) `BRDF_Lambert`/`BRDF_GGX` source, MikkTSpace
  tangent/glTF exporter convention (`tangent.w` sign) — search digests,
  not primary-source line citations (see Verification health).
- General prevalence evidence: Sketchfab ORM (occlusion-roughness-metal)
  channel-packing convention (search digest).
- **Quantified prevalence (added in a later verification pass, 2026-08-18):**
  all 148 model directories under `KhronosGroup/glTF-Sample-Assets`
  (`main` branch) listed via the GitHub contents API, then each model's
  primary `.gltf` fetched directly (`raw.githubusercontent.com`) and
  grepped/parsed for `TEXCOORD_1`, `COLOR_0`, and `KHR_texture_transform`
  — real counts (not search-digest estimates), plus `SheenChair.gltf`
  parsed in full to confirm its occlusion-on-UV1 pattern. This same pass
  also directly listed `extensions/2.0/Khronos/` vs `extensions/2.0/Archived/`
  to correct an earlier ratification-status error on
  `KHR_materials_pbrSpecularGlossiness` (see Verification health).

---

## The matrix

| Feature | First-tier precedent (named, cited) | Phase-4 disposition | Library support (verified, cited) | Proposed acceptance criterion |
|---|---|---|---|---|
| `baseColorFactor` / `baseColorTexture` | Universal across every glTF-conformant renderer (Sample-Renderer, three.js, Filament, Babylon). | consume-now | VERIFIED — glTF 2.0 core spec, `pbrMetallicRoughness` object (`specification/2.0/Specification.adoc`, "Metallic-Roughness Material" section, fetched 2026-08-18); default `[1,1,1,1]`. | Unit test: a material instance with `baseColorFactor=(0.2,0.4,0.6,1)` and no texture renders that exact linear color (±tolerance) in a flat-lit probe; with a texture bound, factor and texture multiply per-texel. |
| `metallicFactor`/`roughnessFactor` + combined `metallicRoughnessTexture` | Same universal precedent; Sample-Renderer's `pbr.frag` reads the identical channel layout (`getMetallicRoughness()`/`BRDF_specularGGX` call sites, fetched 2026-08-18). | consume-now | VERIFIED — core spec, quoted directly: *"Its green channel contains roughness values and its blue channel contains metalness values"* (metallicRoughnessTexture). Defaults: `metallicFactor=1.0`, `roughnessFactor=1.0`. | Unit test: a synthetic 1×1 MR texture with G=128/255, B=64/255 produces roughness≈0.5, metallic≈0.25 read back in the shader (probe via a solid-color render + readback), independent of `metallicFactor`/`roughnessFactor` (both default 1.0 so the texture value passes through unscaled) and multiplicatively combined when factors ≠1. |
| `normalTexture` + `scale` | Universal (tangent-space normal mapping is baseline PBR). | consume-now | VERIFIED — core spec `normalTextureInfo`, `scale` field, JSON schema `material.normalTextureInfo.schema.json` (fetched 2026-08-18): `"default": 1.0`. Scale linearly multiplies the tangent-space X/Y components before renormalizing (standard convention, not separately spec-quoted here — see BC5 row below for this project's own reconstruction formula). | GPU test: a flat-shaded quad with a normal map perturbing X only, `scale=2.0` vs `scale=1.0`, produces a measurably different lighting response at a fixed probe pixel (larger perturbation at scale=2.0); `scale` omitted defaults to 1.0 (unit test on the reflected default). |
| `occlusionTexture` + `strength` | Universal; the FG1 ambient term (row below) is exactly what this multiplies against in Phase 4's no-IBL interim. | consume-now | VERIFIED — core spec `occlusionTextureInfo`: red channel only (*"The red channel of the texture encodes the occlusion value"*), formula quoted directly: *"it affects the occlusion value as `1.0 + strength * (occlusionTexture - 1.0)`"*; `material.occlusionTextureInfo.schema.json` confirms `"strength": {"default": 1.0}` (both fetched 2026-08-18). | Unit test: occlusion texel R=0 (fully occluded) with `strength=1.0` zeroes the ambient contribution at that texel exactly (per the quoted formula); `strength=0.5` on the same texel yields exactly 0.5× ambient (formula evaluates to `1.0 + 0.5*(0-1) = 0.5`) — this is a closed-form, exactly-checkable pixel value, not a fuzzy tolerance case. |
| `emissiveFactor` / `emissiveTexture` | Universal. | consume-now | VERIFIED — core spec; default `emissiveFactor=[0,0,0]` (no emission). `KHR_materials_emissive_strength` (extension row below) is the only way core glTF's emissive can exceed the `[0,1]` LDR range per-channel. | Unit test: an emissive-only material (baseColor black, no light) renders exactly `emissiveFactor` (pre-tonemap) at every probe pixel, confirming emissive bypasses the lighting term entirely. |
| `alphaMode = OPAQUE` | Universal; the default. | consume-now | VERIFIED — core spec enum values `OPAQUE`/`MASK`/`BLEND`, default `"OPAQUE"` (`material.schema.json`, fetched 2026-08-18). No pipeline-state change from the current fixed `blendEnable=VK_FALSE` (`material_system.cpp:1775`). | Covered by every other pixel test in this matrix (OPAQUE is the tested default path). |
| `alphaMode = MASK` + `alphaCutoff` | Universal (foliage/fences/chain-link — the canonical MASK use case in every engine). | consume-now | VERIFIED default `alphaCutoff = 0.5` (`material.schema.json`, fetched 2026-08-18). No existing mechanism in this codebase evaluates a cutoff or discards a fragment — `getPipeline()`'s fixed depth/blend state (`material_system.cpp:1755-1783`) has no branch for it, and no material module in the test corpus discards. | GPU test: a 2×1 checker alpha texture with `alphaCutoff=0.5` — texel A=0.2 (below cutoff) is fully discarded (depth NOT written, background shows through); texel A=0.8 (above cutoff) renders fully opaque with depth written. Distinct pipeline is NOT required for MASK vs OPAQUE (same fixed-function state; only the shader's discard/branch differs) — see the specialization-vs-pipeline-state row below for why this changes the *material's own* shader logic, not `getPipeline()`'s fixed-function arguments. |
| `alphaMode = BLEND` (pipeline variant: blend on, depth-write off, cull off) | Universal; D22's own text names this exact triple of state changes. | consume-now | **UNVERIFIED as buildable today without new plumbing** — `getPipeline()` hardcodes `blendAttachment.blendEnable = VK_FALSE` (`material_system.cpp:1775`), `depthStencilState.depthWriteEnable = hasDepth` (derived from attachment presence only, `:1770`), and `rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT` (`:1758`) — none of these three are parameterized by anything in `PipelineRequest` today (`material_system.h:52-61`: only `material`, `pass`, `specializationBits`). `pass_signature.h`'s own header comment (`:33-37`) explicitly disclaims covering this: *"Deliberately NOT the full fixed-function pipeline state a real variant key could in principle include (blend state, rasterization state, ...) — Phase 3's material pipelines fix every other piece of state themselves."* This is a real, not-yet-built mechanism, not a config flip. | GPU test: a BLEND-mode quad drawn back-to-front over an opaque background shows correct alpha compositing (readback matches the analytic over-blend formula) and does NOT occlude geometry behind it in the depth buffer on a subsequent opaque draw. See "Conflicts" below for the architecture gap this row's acceptance criterion depends on closing. |
| `doubleSided` → cull variant | Universal (`cullMode = NONE` when true, matching core spec text: *"back-face culling is disabled and double sided lighting is enabled"*, quoted from `Specification.adoc`, fetched 2026-08-18). | consume-now | Same gap as the BLEND row: `cullMode` is hardcoded `VK_CULL_MODE_BACK_BIT` at `material_system.cpp:1758`, not keyed by anything per-material. | GPU test: a single-sided plane viewed from its back face renders nothing (culled) when `doubleSided=false`; the identical geometry+viewpoint renders correctly lit when `doubleSided=true`. |
| **Architecture gap: fixed-function pipeline-state as a variant-cache axis** | Every first-tier engine's material system distinguishes "shader permutation" (specialization constants) from "pipeline fixed-function state" (blend/cull/depth-write) as two independent axes of a PSO cache key — Filament's own `RenderableManager`/`MaterialInstance` split and Unity SRP's `RenderStateBlock` both keep these separate from shader variants. | consume-now (foundational — blocks the two rows above) | VERIFIED as absent: `PipelineRequest`'s only axes are `material` (content hash), `pass` (attachment shape, format/count/samples only per `pass_signature.h:55-58`), and `specializationBits` (Phase-3 D8, *"Filament-style independent shading axes... skinning, fog"* — `phase3-design.md:190-199` — a shader-recompile-triggering bitmask, not a fixed-function-state carrier). No existing field anywhere in this call graph can vary blend/cull/depth-write per material. | Proposed concrete fix (for the coordinator, not a ruling): store `alphaMode`/`doubleSided`/`alphaCutoff` on `MaterialRecord` itself at `loadMaterial()` time (reflected from the module, or supplied via a small POD the importer passes alongside the `.slang` path) so `getPipeline()` reads `record->blendState`/`record->cullMode` directly instead of the current hardcoded literals — this needs no new `PipelineRequest` field and no cache-key change beyond what `record->contentHash` already provides (two materials with different alphaMode already get different content hashes if the mode is baked into generated/selected shader source, or the record's own blend/cull fields simply need including in the `PipelineKey` alongside `specializationBits` if a single `.slang` module can express multiple alphaModes at instance level). Acceptance criterion: two `MaterialInstance`s of the *same* loaded `.slang` module with different `alphaMode` values (if the design allows per-instance alphaMode) or two *different* modules with different `alphaMode` (if alphaMode is module-level) produce two distinct, independently cached `VkPipeline`s verified via `getPipeline()`'s existing cache-hit/miss counters. |
| Per-texture `texCoord` index (`textureInfo.texCoord` + `TEXCOORD_n`) | Universal; core spec: *"A texture binding is defined by an `index`... and an optional index of texture coordinates"* (`Specification.adoc`, fetched 2026-08-18); default `texCoord=0` (reads `TEXCOORD_0`). Client `SHOULD` support at least two sets (spec text, same source). | consume-now for `TEXCOORD_0` (the default, used by every texture reference in the test corpus) | VERIFIED at the format level: D8's fixed Phase-4 vertex layout is `position f32x3 \| normal f32x3 \| tangent f32x4 \| uv0 f32x2` (design doc:170-171) — **one** UV set only. `MaterialVertex` (`material.slang:21-25`) carries `worldPos/normal/uv` — also one UV set. Both are consistent with `texCoord=0`-only support. | Unit test: every texture reference in the two shipped materials (StandardPBR, Unlit) samples `uv0` unconditionally; the importer/material-parameter path is not required to reject a glTF `texCoord` value ≠0 outright but must resolve it to `uv0` with a logged notice (see TEXCOORD_1 row below — this is the same underlying constraint, stated at the texCoord-index-selection level rather than the second-UV-set level). |
| `TEXCOORD_1` (a second UV set) | **Directly quantified, not just anecdotal:** a full scan of all 148 model directories in `KhronosGroup/glTF-Sample-Assets` (`main` branch, fetched+parsed 2026-08-18 via the GitHub API + raw `.gltf` JSON for each) finds **9/148 (6.1%)** reference `TEXCOORD_1` at all: `CarConcept`, `ChairDamaskPurplegold`, `ChronographWatch`, `MorphStressTest`, `MosquitoInAmber`, `MultiUVTest`, `SheenChair`, `SheenWoodLeatherSofa`, `TextureTransformMultiTest`. Critically, `SheenChair`'s actual material JSON (fetched+parsed directly) confirms the EXACT pattern the ticket's own research prompt names as a concern: every one of its 6 materials sets `occlusionTexture: {texCoord: 1, ...}` while `normalTexture`/`baseColorTexture` stay on `texCoord: 0` (default) — occlusion genuinely lives on a second UV set distinct from albedo/normal, in a real (non-synthetic-test-named) Khronos sample asset, not a hypothetical. Several of the 9 hits ARE purpose-built conformance tests (`MultiUVTest`, `TextureTransformMultiTest`) rather than "organic" content, so 6.1% should be read as an upper-bound-inclusive-of-test-fixtures figure, not a pure real-content prevalence rate — still, 5 of the 9 (`CarConcept`, `ChairDamaskPurplegold`, `ChronographWatch`, `SheenChair`, `SheenWoodLeatherSofa`) are ordinary product-style demonstration assets, not named test cases. | **N/A-Phase-4 at this ticket's layer** (shader/material consumption) — bound by the already-ruled D7 import-side disposition (design doc:163-165: *"COLOR_0 and TEXCOORD_1 are explicitly deferred (recorded, not silently dropped: importer logs when present)"*), which is Task 13's scope, not this ticket's, and is not being re-litigated here. | UNVERIFIED beyond the design doc's own text whether "log-don't-drop" at import time is sufficient for a *credible viewer* claim — the retrofit-economics test this gate must apply: is deferring cheap? **Yes**, conditionally: D7's own logging means every imported asset that actually uses TEXCOORD_1 is discoverable (grep the log) without re-importing when a later phase adds a second UV attribute to the pooled vertex format and `MaterialVertex`; the cost is bounded to "re-run the importer," not "re-author the asset." Acceptance criterion for THIS ticket: StandardPBR/Unlit's `.slang` modules and `forward_entry.slang`/`MaterialVertex` are not required to carry a `uv1` field in Phase 4, but a code comment at `material.slang`'s `MaterialVertex` struct must reference this deferral (matching D13's own "code comment referencing Stage-2 migration" precedent for reversed-Z) so a future implementer adding `uv1` finds the exact spot to extend, rather than rediscovering the gap. See Conflicts below for whether DamagedHelmet (the sample 08 gate asset) itself uses TEXCOORD_1 anywhere. |
| `COLOR_0` vertex color | CORE SPEC BEHAVIOR, not a renderer convention: `Specification.adoc` (fetched 2026-08-18) states directly: *"If a primitive specifies a vertex color using attribute semantic COLOR_0, then this value acts as an additional linear multiplier to base color."* This is a materially different finding from treating COLOR_0 as optional polish — skipping it is skipping literal core-spec-mandated output for any asset using vertex-painted color (terrain, foliage, and vertex-color-baked assets are the common real-world case). **Directly quantified:** the same full 148-model `glTF-Sample-Assets` scan (2026-08-18) finds **8/148 (5.4%)** reference `COLOR_0`: `BoxVertexColors`, `CompareBaseColor`, `IridescentDishWithOlives`, `MeshoptCubeTest`, `PrimitiveModeNormalsTest`, `RecursiveSkeletons`, `SheenWoodLeatherSofa`, `VertexColorTest` — a similar mix of dedicated test fixtures (`VertexColorTest`, `BoxVertexColors`) and ordinary content assets (`IridescentDishWithOlives`, `SheenWoodLeatherSofa`). | **N/A-Phase-4 at this ticket's layer**, same D7-bound reasoning as TEXCOORD_1 immediately above. | VERIFIED absent from the current vertex format: D8's pooled layout has no color attribute at all (design doc:170-171); `MaterialVertex` likewise. Same "importer logs presence, cheap to extend later" retrofit-economics argument applies (D7 text, quoted above, covers both attributes identically). | Same acceptance criterion shape as TEXCOORD_1: a code comment at the deferral point, not a shader implementation, is this ticket's obligation. Flagged in Conflicts below as a place where the ticket's own framing ("currently logged-and-skipped — is that viable for a credible viewer?") invites re-litigating an already-ruled D7 decision — this gate deepens the ruling with evidence rather than reopening it, per the research brief's own instruction not to re-litigate already-ruled items. |
| `KHR_texture_transform` (UV offset/rotation/scale) | Extremely common in practice: the extension's own README states its motivation is texture atlasing (*"many engines encourage packing many objects' low-resolution textures into a single large texture atlas... defined by vertical and horizontal offsets"*, quoted from `KHR_texture_transform/README.md`, fetched 2026-08-18); status **"Complete, Ratified by the Khronos Group"** (same source), explicitly equated there to "Unity's `Material#SetTextureOffset`/`SetTextureScale`" and "Three.js's `Texture#offset`/`#repeat`" — i.e. the extension exists specifically to match conventions two other first-tier engines already ship. **Directly quantified:** the same full 148-model `glTF-Sample-Assets` scan (2026-08-18) finds **15/148 (10.1%)** use `KHR_texture_transform` — the single most common of the three attributes/extensions checked in this row-group, and notably co-occurring with TEXCOORD_1 in `SheenChair`'s materials specifically to relocate occlusion onto UV1 AND scale/offset it independently (`occlusionTexture.extensions.KHR_texture_transform: {texCoord: 1}`, fetched+parsed directly from `SheenChair.gltf`) — real evidence the two features compound in practice, not just in Khronos's own dedicated `TextureTransformMultiTest`/`TextureTransformTest` conformance fixtures (also among the 15 hits). | log-don't-drop | UNVERIFIED whether this codebase's importer (Task 13, not this ticket) currently detects/logs `KHR_texture_transform` at all — not checked by this gate (out of this ticket's scope; the importer's own gate covers Task 13). At the shader layer (this ticket's scope), applying a UV offset/rotation/scale is a cheap per-material uniform (a 2x2 matrix + offset, or the equivalent affine terms) evaluated once per `evaluate()` call — no pipeline-state or vertex-format impact, unlike TEXCOORD_1/COLOR_0 above. | Because the shader-side cost is genuinely negligible (a few ALU ops against an existing UV read, no new attribute, no new pipeline variant) and the extension is ratified + evidenced-common, the retrofit-economics argument favors **reconsidering log-don't-drop toward consume-now** for the UV-transform math itself (independent of whether the importer parses the extension's JSON this phase) — flagged as a Conflict below rather than asserted as this gate's ruling, since Task 13's importer-side disposition is out of this ticket's scope and the coordinator should adjudicate the two tickets together. |
| `KHR_materials_unlit` → this project's `Unlit` material | Direct 1:1 mapping is the extension's own intent. | consume-now | VERIFIED semantics — `KHR_materials_unlit/README.md` (fetched 2026-08-18), quoted: color = *"the product of `baseColorFactor`, `baseColorTexture`, and vertex color (if any), as defined by the core glTF material specification"*, rendered as *"a constantly shaded surface that is independent of lighting"*; all PBR fields (metallic/roughness/normal/occlusion/emissive) are explicitly ignored, present only as fallback data for non-supporting clients. Current `test_unlit.slang` (`src/rx_material/tests/data/test_unlit.slang:17-19,41-67`) implements only a flat `tint` parameter — no `baseColorTexture` sampling, no vertex-color multiply (consistent with the COLOR_0 N/A-Phase-4 disposition above) — this is a **test fixture**, not the shipped `shaders/material/unlit.slang` Task 16 must author. | Unit test: the shipped Unlit material's `evaluate()` output equals `baseColorFactor × baseColorTexture(uv0)` exactly, with **zero** dependency on any light direction, occlusion, or ambient term in its data flow (verifiable by a probe pair — flip the scene's light direction, confirm zero pixel delta on an Unlit-shaded probe). |
| `KHR_materials_emissive_strength` | Ratified Khronos extension; description: *"supplies a new emissiveStrength scalar factor that governs the upper limit of emissive strength... allowing for stronger emission effects in HDR environments"* (search digest, `KhronosGroup/glTF` extension README, fetched 2026-08-18). | log-don't-drop | UNVERIFIED ratification badge for this specific extension was not independently re-confirmed beyond its presence under the `Khronos/` (not vendor-prefixed) extension-registry path, which by glTF's own registry convention indicates multi-vendor/Khronos-owned status. | A material with this extension present but unimplemented logs once per load (`RX_LOG_WARN`, naming the extension) and falls back to core `emissiveFactor` clamped to `[0,1]` — never silently dimmer/brighter than the source data without a diagnostic. Cheap to promote to consume-now later (a single scalar multiply on the existing emissive term, no new vertex/texture data). |
| `KHR_materials_ior` | Ratified; sets a material's index of refraction, affecting Fresnel reflectance at normal incidence (search digest, fetched 2026-08-18) — feeds `KHR_materials_specular`/`transmission` below. | log-don't-drop | Same registry-path verification note as emissive_strength. | Logged once per load; falls back to the default IOR (1.5, the glTF-defined default dielectric F0) baked into StandardPBR's existing Schlick-F0 constant — no visual regression for assets that omit it (the common case), a visible-but-diagnosed approximation for assets that set a non-default IOR. |
| `KHR_materials_transmission` | Ratified; models light passing through a surface (glass, thin plastic) preserving specular reflection (search digest, fetched 2026-08-18) — a genuinely new BRDF layer, not a factor tweak. | log-don't-drop | Same registry-path note. | Logged once per load; StandardPBR renders the surface fully opaque (transmission ignored) rather than crashing or silently rendering a hole — the log entry is the diagnostic that distinguishes "opaque by design" from "transmissive material misrendered as opaque." |
| `KHR_materials_volume` | Ratified; pairs with transmission for colored-glass attenuation (`attenuationColor`/`attenuationDistance`) and a `thicknessTexture` (search digest, fetched 2026-08-18). | log-don't-drop | Same registry-path note; meaningless without transmission also being implemented, so its disposition is coupled to the transmission row above. | Same pattern: logged once, ignored, no crash. |
| `KHR_materials_specular` | Ratified; extends the metallic-roughness model with a separate specular-color/specular-factor control independent of `baseColor` (search digest, fetched 2026-08-18). | log-don't-drop | Same registry-path note. | Logged once; StandardPBR uses its existing fixed dielectric F0 (0.04, the glTF/Disney-convention default) rather than the extension's per-material override. |
| `KHR_materials_sheen` | Ratified; adds a fabric/cloth-style retroreflective sheen lobe, documented as designed to work alongside clearcoat (search digest, fetched 2026-08-18). | log-don't-drop | Same registry-path note. | Logged once; no sheen lobe added — base layer renders as if sheen were absent (not an error state, a scope limitation). |
| `KHR_materials_clearcoat` | Ratified; adds a second specular lobe over the base layer with its own normal/roughness — description notes it as *"a critical graphical element typically used in the automotive industry"* (search digest, fetched 2026-08-18) — i.e. a named, evidenced real-world use case (car paint), not hypothetical. | log-don't-drop | Same registry-path note. | Logged once; base-layer-only rendering (no second specular lobe) — visually flatter than authored intent but not incorrect for the base layer itself. |
| `KHR_materials_iridescence` | Ratified; thin-film interference (soap-bubble/oil-film/insect-wing effect), described as view-dependent (search digest, fetched 2026-08-18). | log-don't-drop | Same registry-path note. | Logged once; no iridescent color shift — base material color renders unshifted. |
| `KHR_materials_anisotropy` | Ratified; asymmetric specular lobe for brushed-metal-style materials (search digest, fetched 2026-08-18). | log-don't-drop | Same registry-path note. | Logged once; isotropic GGX lobe used regardless of the extension's anisotropy direction/strength data. |
| `KHR_materials_dispersion` | Ratified; chromatic aberration through transmissive volumes (search digest, fetched 2026-08-18) — depends entirely on transmission already being implemented, so it is strictly downstream of that row. | log-don't-drop | Same registry-path note. | Logged once, coupled to the transmission row's disposition. |
| `KHR_materials_variants` | Ratified; defines alternative material *sets* selectable at runtime for a single mesh (a scene-authoring feature, not a shading-model feature) (search digest, fetched 2026-08-18). | log-don't-drop | Same registry-path note. Distinct in kind from every other row above: this is an asset/scene-structure concern (multiple material assignments per primitive), not a shading feature — its natural home is the importer/asset-registry layer (Task 13/`rx_asset`), not this ticket's shaders. | Logged once at import if present (Task 13's scope, noted here for completeness since it appeared in the ticket's own extension list); StandardPBR/Unlit need no changes to support it later (variants just select which already-supported material a primitive binds to). |
| Additional extensions found but not in the ticket's named list | Two corrections/additions found via a direct GitHub API directory listing (`api.github.com/repos/KhronosGroup/glTF/contents/extensions/2.0/{Khronos,Archived}`, fetched 2026-08-18, superseding an earlier less-precise pass): `KHR_materials_diffuse_transmission` (thin, one-sided transmissive materials — leaves, thin fabric) IS a live, currently-ratified extension under `extensions/2.0/Khronos/` and is genuinely absent from the ticket's named list. `KHR_materials_pbrSpecularGlossiness` (the pre-metallic-roughness legacy workflow) is **NOT** in the live `Khronos/` directory at all — it lives under `extensions/2.0/Archived/` (confirmed directly, alongside `KHR_techniques_webgl` and `KHR_xmp`), meaning Khronos has formally archived/deprecated it, not merely left it out of this ticket's list. It remains real-world-relevant only insofar as OLD assets exported before/around the metallic-roughness transition may still carry it, which is a different (weaker) prevalence argument than "currently ratified." | log-don't-drop for `diffuse_transmission` (new-gap candidate, see below); `N/A-Phase-4` for `pbrSpecularGlossiness` specifically, on the grounds that an archived extension has no forward-looking prevalence claim to make for new content, though the importer's generic unknown-extension handling should still not silently drop it if it appears in a legacy asset | VERIFIED via direct GitHub API directory listing of both `extensions/2.0/Khronos/` (26 entries, `diffuse_transmission` present) and `extensions/2.0/Archived/` (`pbrSpecularGlossiness` present there specifically), fetched 2026-08-18 — not inferred from a registry-path heuristic. | Not this ticket's acceptance criterion to define; flagged as a New gap below for the coordinator to decide whether the ticket's extension list should add `diffuse_transmission` (live) before dispatch, and whether the importer's (Task 13's) generic extension-detection sweep should separately log archived-but-encountered extensions like `pbrSpecularGlossiness` with a distinct "legacy/archived" diagnostic rather than the same log line as a currently-ratified-but-unimplemented one. |
| BRDF baseline: specular D/G/F | Filament: *"a Cook-Torrance specular microfacet model, with a GGX normal distribution function, a Smith-GGX height-correlated visibility function, and a Schlick Fresnel function"* (`Filament.md.html`, fetched 2026-08-18). glTF-Sample-Renderer's `pbr.frag`: `BRDF_specularGGX()` + `F_Schlick()` calls confirm the same GGX+Schlick baseline (fetched 2026-08-18). three.js `MeshStandardMaterial`: `D_GGX`/`V_GGX_SmithCorrelated`/`F_Schlick` (search digest, fetched 2026-08-18). | consume-now | All three first-tier references converge on GGX distribution + Smith(-correlated) visibility + Schlick Fresnel — this is the uncontested 2026 baseline, not a judgment call between competing schools. | Unit/GPU test: a grazing-angle probe on a smooth (low-roughness) metal surface shows Fresnel edge brightening consistent with Schlick's formula at a known angle (closed-form comparison, not just "looks plausible"). |
| BRDF baseline: diffuse term | Filament explicitly weighs Lambertian vs Burley/Disney diffuse and **chooses Lambertian**, quoted directly: *"The Lambertian BRDF is obviously extremely efficient and delivers results close enough to more complex models"*, and on Burley: *"the extra runtime cost does not justify the slight increase in quality"* (`Filament.md.html`, fetched 2026-08-18). glTF-Sample-Renderer's `pbr.frag` uses `BRDF_lambertian(baseColor.rgb)` (fetched 2026-08-18) — the reference conformance viewer itself is Lambertian, not Burley. three.js is also `BRDF_Lambert` (search digest). | consume-now (Lambertian) | Three independent first-tier references (the reference conformance viewer, a AAA mobile-and-desktop shipping engine, and the most widely deployed web 3D engine) all ship Lambertian diffuse, with Filament's own docs giving the explicit cost/benefit reasoning for choosing it over Burley. This is a credible 2026 baseline, not a corner-cut. | Unit test: a diffuse-only (roughness=1, metallic=0) probe under a single directional light matches `NdotL × baseColor / π` (Lambertian normalization) within tolerance, confirming the `1/π` normalization is present (a common omission bug) rather than an un-normalized `NdotL × baseColor`. |
| FG1 interim flat ambient term (uniform color × occlusion) | Already-ruled per the issue's own comment 2 (FG1 amendment) — not re-litigated here; this row deepens it into a concrete criterion. | consume-now | VERIFIED existing in-repo precedent for the pattern: `shaders/multipass/lit.frag.slang:75-80` already implements `kAmbient + (1.0-kAmbient) * lambert * shadow` — a constant-ambient-plus-lit-term shape, though that sample's ambient is NOT occlusion-modulated (no occlusion texture in that sample's material). StandardPBR's version must additionally multiply by the occlusion term (the occlusion-texture row above's exact formula). | GPU test named directly by the issue comment: "sample 08's gate asserts a non-black metal probe" — a `metallic=1.0, roughness≈0` probe pixel with zero direct light visible (shadowed or facing away) still reads a non-black RGB value equal to `ambientColor × occlusionValue × baseColor` (metals have no diffuse term, so this is the ONLY contribution — an exact closed-form check, not a fuzzy "not pure black" threshold). |
| Tonemap/exposure interaction | Universal: every first-tier renderer applies exposure as a pre-tonemap HDR multiply, then a tonemap curve, then (optionally) sRGB encode. | consume-now (exposure is new work; tonemap curve itself is existing) | VERIFIED what exists: `shaders/multipass/tonemap.frag.slang:12-18` implements plain Reinhard (`c/(1+c)`) with **no exposure term at all** in its push constants (`tonemap.vert.slang:10-13`: only `hdrTextureIndex`/`hdrSamplerIndex`) — confirmed by reading both files in full. sRGB encode is delegated to the swapchain's own `_SRGB`-format fixed-function attachment write (comment at `tonemap.frag.slang:6-11`), not done in-shader. | Sample 08's `--exposure` CLI flag (plan text) must multiply the HDR color by `2^exposure` (or an equivalent linear exposure scalar) BEFORE the existing Reinhard formula — this is a genuinely new push-constant field and shader line, not a config toggle on existing code. GPU test: two renders of the identical scene differing only in `--exposure` produce measurably different (not identical) tonemapped output at a mid-gray probe, and `--exposure 0` (or the flag's documented neutral value) reproduces the pre-existing Reinhard-only output within float tolerance (regression guard against silently changing sample 05/07's shared tonemap shaders, which this ticket must not touch per the file list). |
| Normal-map BC5 two-channel Z-reconstruction | Standard practice (BC5/ATI2 stores only X/Y; Z is reconstructed as `sqrt(1 - x² - y²)` in-shader) — universal across engines using BC5 normal compression (Unreal, Unity, Filament all document this exact reconstruction). | consume-now | VERIFIED format choice: D10 commits normal maps to `BC5_UNORM` explicitly for this reason (design doc:198: *"normal → BC5_UNORM (two-channel, Z reconstructed in shader)"*). No existing shader in this repo currently performs this reconstruction (`lit.frag.slang`/`material.slang` sample normals but do not tangent-space-map or BC5-reconstruct) — this is new StandardPBR shader logic, not a reused pattern. | Unit test: a synthetic BC5 texel encoding X=0.6, Y=0.8 (a valid unit-circle point, Z=0 at the equator) reconstructs Z≈0 (not NaN, not clamped-wrong-sign); a texel at X=Y=0 (tangent-space up) reconstructs Z=1.0 exactly. Must also clamp the radicand to ≥0 for slightly-out-of-range compressed data (a known BC5 artifact at extreme angles) rather than producing NaN. |
| sRGB-vs-linear per texture role | Universal glTF convention: baseColor/emissive are sRGB-encoded; metallic-roughness/normal/occlusion are linear data, never sRGB-decoded. | consume-now | VERIFIED as already decided at the format-policy level: D10 assigns `BC7_SRGB` to baseColor/emissive and `BC5_UNORM`/`BC7_UNORM` (or BC4) — explicitly UNORM, not SRGB — to normal/MR/occlusion (design doc:197-199). This is a texture-cache/import-time format decision (D10, Task 14's scope), not a StandardPBR shader decision — the shader simply samples whatever format the texture was created with; no in-shader sRGB decode is needed if the *format* already carries the right interpretation (VK_FORMAT `_SRGB` variants decode automatically on sample). | Unit test (belongs to Task 14's gate, cross-referenced here for completeness): a baseColor texture created as `BC7_SRGB` and sampled in the shader returns linear-space values matching a known sRGB-encoded input's linearized equivalent; a normal-map texture created as `BC5_UNORM` returns the raw encoded bytes unconverted (no accidental double-linearization). |
| MikkTSpace-consistent tangent-space evaluation (sign convention) | glTF spec + MikkTSpace reference: bitangent = `cross(normal.xyz, tangent.xyz) * tangent.w` (search digest of the glTF spec's own tangent-space text and MikkTSpace documentation, fetched 2026-08-18) — `tangent.w` is a handedness sign (±1), not a magnitude. Exporters commonly flip this sign relative to raw MikkTSpace output to match glTF's own UV-space convention (search-surfaced GitHub issue discussion, `KhronosGroup/glTF-Sample-Models#174`) — a documented, known gotcha, not this project's own invention. | consume-now (D8 already provisioned the data; the shader math is new) | VERIFIED the vertex FORMAT is ready: D8's pooled layout already carries `tangent f32x4 (w = handedness)` (design doc:170) — the data pipeline (MikkTSpace generation, D7) is provisioned. **NOT verified/present**: `MaterialVertex` (`material.slang:21-25`) and `forward_entry.slang`'s `vertexMain` (`:69-87`) currently declare only `worldPos/normal/uv` — **no tangent field at all**. StandardPBR cannot normal-map without this being added first; it is a blocking prerequisite this ticket's own file list does not name (see Conflicts below). | GPU test: `bitangent = cross(normal, tangent.xyz) * tangent.w` evaluated in-shader on a known asset (or a synthetic tangent-space test grid) produces a consistent right-handed (or glTF-convention) tangent basis matching a reference computed off-line the same way — a sign-flip bug shows up as inverted normal-map lighting on one axis, the classic symptom this test must catch. |
| Specialization-constant axes (alphaMode/doubleSided) vs pipeline-state axes | See the "Architecture gap" row above — this row is the direct answer to the brief's explicit question. | consume-now (clarification, not new scope) | VERIFIED via `phase3-design.md:190-199` (D8): `specializationBits` is documented as a *shader-recompile-triggering* axis ("module choice, specialization bitmask... nothing on the instance can trigger a shader recompile mid-frame" is the OTHER side of that same split — i.e. specialization bits ARE allowed to trigger recompilation, unlike bound parameters). `alphaMode=BLEND` and `doubleSided` are **not** shader-recompile concerns at all — blend state and cull mode are `VkPipeline` fixed-function fields with zero SPIR-V representation, so encoding them as "specialization constant bits" (as the plan Task 16 text literally says: *"specialization bits gain alphaMode/doubleSided axes"*) is imprecise; D22's OWN text is more accurate (*"BLEND (pipeline variant...)"*). `alphaMode=MASK`'s cutoff-discard branch is the one sub-case that genuinely COULD be a specialization constant (it changes shader logic, not fixed-function state) — but doesn't need to be, since a per-instance uniform cutoff value with an always-present conditional discard is simpler and costs one branch, negligible on any GPU this project targets. | See Conflicts below — this is a direct textual disagreement between the plan Task 16 line and D22's own more precise phrasing, worth the coordinator's attention before dispatch rather than left for the implementer to discover mid-task. |
| D26.1: `firstInstance`/`gl_InstanceIndex` bindless addressing — does the index include `firstInstance` under Vulkan? | The ticket names this explicitly as "the classic pitfall vs D3D." | consume-now (this IS the ticket's D26.1 scope) | **VERIFIED, with a critical Slang-specific nuance the ticket did not anticipate:** Vulkan spec (`docs.vulkan.org/refpages/.../InstanceIndex.html`, search digest, fetched 2026-08-18) confirms *"InstanceIndex begins at the firstInstance parameter to vkCmdDraw or vkCmdDrawIndexed... rather than always starting from 0"* — i.e. raw SPIR-V `InstanceIndex` DOES include `firstInstance`. But this project's shaders are written in **Slang**, not raw GLSL/SPIR-V, and Slang's own docs (`a2-01-spirv-target-specific.html`, fetched 2026-08-18) state directly: *"SV_InstanceID and SV_VertexID start from zero for each draw call, while in SPIR-V, InstanceIndex and VertexIndex include the base instance... SV_InstanceID in Slang targeting SPIR-V equals (InstanceIndex - BaseInstance)"* — Slang DELIBERATELY SUBTRACTS `firstInstance` back out of `SV_InstanceID` to preserve D3D-compatible semantics (Microsoft HLSL specs proposal 0015, "Extended Command Information", search digest, fetched 2026-08-18, confirms D3D's pre-SM6.8 `SV_InstanceID` never includes `StartInstanceLocation`). A raw, `firstInstance`-inclusive index is available under a **different** semantic name: `SV_VulkanInstanceID` (Slang docs, same source). | Using ordinary `SV_InstanceID` in StandardPBR/Unlit's vertex stage would silently give a per-draw-relative index (0-based), NOT the absolute bindless-buffer index D26.1 requires — a correctness bug that would only surface once a draw's `firstInstance` is ever non-zero (i.e. never in isolated single-draw testing, only once the real scene path with sorted/collapsed instanced draws lands in Task 19). Acceptance criterion: both material shaders' vertex stage explicitly use `SV_VulkanInstanceID` (not `SV_InstanceID`) to index the bindless per-draw storage buffer, with a code comment citing this exact Slang/Vulkan/D3D semantic mismatch so a future maintainer porting a shader from an HLSL/D3D reference doesn't reintroduce the bug. Unit/GPU test: two draws in one command buffer, the second submitted with `firstInstance>0`, must read distinct, correct per-draw data — a test that would pass under a naive `SV_InstanceID` read on the FIRST draw (firstInstance=0) but fail on the second, which is exactly why single-draw tests are an insufficient regression guard here. |
| D26.1: existing bindless/per-draw mechanism readiness | D26.1 requires "never per-draw push constants." | consume-now | VERIFIED current mechanism is a HYBRID that partially violates this already: `gMaterialGlobals` (`material.slang:127-132`) IS a per-draw push constant today (carrying the default sampler's bindless index), and `bindInstance()` (`material_system.h:364-391`) binds a **fresh descriptor set 1** per draw from a per-frame arena (not a push constant, but also not a `firstInstance`-indexed storage-buffer read — it's a third mechanism, "rebind a descriptor set per draw"). Neither of today's two per-draw mechanisms is what D26.1 specifies. | This is new plumbing, not a reuse of `bindInstance()`'s existing path as-is — the scene path's `recordDrawList` (Task 19, another ticket) needs materials to read per-draw data from a bindless storage buffer indexed by `SV_VulkanInstanceID` instead of relying on a set-1 descriptor bind per draw. Acceptance criterion: StandardPBR/Unlit declare and read from a bindless `StructuredBuffer` of per-draw records (material index + instance/transform index, matching Task 19's `DrawPayload` shape) indexed by `SV_VulkanInstanceID`, with the existing `bindInstance()`/set-1 path either removed from the scene-path usage or explicitly scoped as a legacy path retained only for non-scene samples (06/07) that don't yet drive through `recordDrawList`. |
| Orbit camera (drag) | Universal viewer UX (Sample-Renderer's web viewer, three.js `OrbitControls`, Filament's `viewer` sample app all implement mouse-drag orbit as table stakes). | consume-now (blocked on a missing prerequisite — see Conflicts) | **VERIFIED absent**: `rx::platform::Window` (`window.h:9-28`, full file read) exposes only `create/sdlWindow/pumpEvents/requiredVulkanInstanceExtensions/createVulkanSurface` — no mouse position/delta API at all; `pumpEvents()` (`window.cpp:52-58`) is an empty `SDL_PollEvent` drain with a comment stating event handling policy belongs to the embedder. The existing "orbit" precedent (`samples/06_materials/main.cpp:277,313-333`) is a **timer-driven auto-rotating azimuth**, not mouse-drag-driven — there is no existing drag-orbit precedent anywhere in this codebase to reuse. | Sample 08's `main.cpp` must either (a) call raw SDL3 mouse APIs directly (bypassing `rx_platform::Window`'s abstraction, since it exposes the raw `SDL_Window*` via `sdlWindow()`), or (b) this ticket's own scope must grow to add a minimal mouse-delta accessor to `rx_platform::Window` ahead of Task 20 (which is what SPEC-level Task 20, Stage 2, is chartered to build properly, but Task 20 is SEQUENCED AFTER Task 16 despite Task 16 needing SOME mouse input now). See Conflicts below — this is a real sequencing gap between Stage 1 (Task 16) and Stage 2 (Task 20), not a design nuance. |
| Manual `--exposure` control | Universal (every viewer ships an exposure slider/flag; glTF-Sample-Renderer's own viewer has one). | consume-now | Covered by the Tonemap/exposure row above — restated here because the plan text specifically calls it out as a sample-08 CLI surface, not just a shader capability. | CLI test: `08_gltf_viewer --exposure -1.0` vs `--exposure 1.0` on the same headless scene produce two DIFFERENT committed reference PNGs (both gated under D17's tolerance), proving the flag actually reaches the shader rather than being parsed-and-ignored. |
| Loading state (async import) | Universal for any viewer that imports on a background thread (every modern engine's asset browser/level-load shows a spinner/progress bar rather than a frozen frame). | consume-now | VERIFIED the async import pipeline this depends on is Task 15 (`plans/2026-08-11-phase4-scene-assets.md:311-315`, "Async import pipeline (D5 contract in action)") — a DIFFERENT, earlier-numbered task in the same Stage 1, so the dependency is same-stage and sequenced correctly (unlike the orbit-camera/Task-20 gap above). Not independently re-verified beyond confirming Task 15 exists and precedes Task 16 in the plan's own ordering. | GPU test (headless): while an async import is in flight, sample 08 renders a distinct, recognizable "loading" frame (not a black screen, not the previous frame frozen, not a validation error from sampling an unready resource) — the pixel gate for this state is a separate committed reference from the final-loaded-scene gate. |
| Tolerance pixel-gate methodology (D17) + `regen_references.sh` | D17's own text (design doc:272-281) is the specification; this row verifies buildability against current repo state. | consume-now | VERIFIED no existing precedent to reuse: `grep` for `tolerance\|reference.*png\|pixel.*gate` across `samples/` and `tools/` finds no prior tolerance-gate or reference-regeneration script anywhere in this repo (checked 2026-08-18) — this is genuinely new CI infrastructure, not an extension of an existing pattern, despite D17 being phrased as if the mechanism already exists ("compare against committed reference PNGs"). | Per D17's own stated parameters: ±4/255 per-channel tolerance, <0.5% failing-pixel budget, 256×256 lavapipe-rendered references, CI-only enforcement (local GPU divergence reported as info). Acceptance criterion: `tools/regen_references.sh` exists, is documented as an explicit manual step (never auto-run), and sample 08's CI job fails loudly (named mismatch, pixel-diff count) rather than silently passing on a broken comparison. |
| D24 (memory budget/eviction invariant) | Binding rule — must appear on every ticket it touches. | consume-now (as a residency-tolerance requirement, not new accounting work) | Applies to StandardPBR/Unlit via their **texture bindings**: `MaterialSystem::createTexture2D()` already returns handles into a `BindlessTable` (existing mechanism); D24 requires handle resolution to be residency-tolerant (fallback substitution, never a crash) — this ticket's shaders must tolerate a fallback texture index being substituted for an evicted/not-yet-resident texture without any special-case shader logic (the bindless read is opaque to residency state by construction, since `rx_sampleTexture` just indexes `gTextures[]` — whatever index is bound IS what gets sampled). | Not a new test for this ticket specifically — the invariant is satisfied by construction as long as StandardPBR/Unlit never bypass the existing bindless-index indirection (e.g. never cache a raw `VkImageView` or assume an index stays valid across a frame boundary). Acceptance criterion: a code-review checklist item, not a runtime probe unique to this ticket (the eviction mechanism itself is Task 10/19's scope). |
| D26 points 2-4 (draw-list SoA layout, instancing collapse, BDA enablement) | Binding rule. | N/A-Phase-4 for this ticket specifically | These three sub-points are `ViewLists`/`DrawListBuilder` (Task 19) and `Device::create` (already-delivered, Task 12-adjacent) concerns — StandardPBR/Unlit's shader code has no `ViewLists` struct to lay out and doesn't enable `bufferDeviceAddress` itself. Retrofit economics: correctly N/A because nothing at THIS ticket's call sites needs revisiting once Task 19 lands — D26.1 (per-draw addressing, already a full row above) is the only D26 sub-point with a direct shader-side surface. | No acceptance criterion owned by this ticket. |
| D27 (main-thread pipeline pre-resolution) | Binding rule. | consume-now (as a design constraint on how `getPipeline()` may be called, not new work) | StandardPBR/Unlit's `getPipeline()` calls happen through `MaterialSystem`, which is already `RX_ASSERT_MAIN_THREAD`-guarded (`material_system.cpp:1695`). D27's actual mechanism (pre-resolving pipelines before worker fan-out) is Task 19's `DrawListBuilder` responsibility, not this ticket's — this ticket's obligation is simply to not introduce any NEW way to call `getPipeline()`/`bindInstance()` off the main thread (e.g. sample 08's async import must not trigger a pipeline build from its background import thread). | Code-review checklist item: sample 08's async-import worker thread (Task 15's mechanism) never directly or indirectly calls into `MaterialSystem::getPipeline()`/`bindInstance()` — pipeline warm-up, if any, happens on the main thread after import data lands via `postToMain()`. |

---

## Conflicts

- **Plan Task 16 text vs D22's own text — "specialization bits" vs
  "pipeline variant."** Task 16's file list says *"specialization bits
  gain alphaMode/doubleSided axes; BLEND pipeline-state variant... wired
  through PassSignature/pipeline build"*
  (`docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:317`) — this
  sentence itself contains the contradiction: it calls alphaMode/
  doubleSided "specialization bits" in its first half, then treats BLEND
  (an alphaMode value) as a "pipeline-state variant" in its second half.
  D22's own text is more precise (design doc:326-327: *"alphaMode...
  BLEND (pipeline variant: blending on, depth-write off, cull off)...
  doubleSided → cull variant"* — no mention of specialization bits for
  either). Per the "Specialization-constant axes vs pipeline-state axes"
  and "Architecture gap" rows above, blend/cull are NOT expressible as
  SPIR-V specialization constants at all — they are `VkPipeline`
  fixed-function fields. Not resolving; the coordinator should confirm
  Task 16's own text is corrected to match D22 (and `PassSignature`'s
  header comment, which explicitly disclaims carrying this state) before
  dispatch, since an implementer reading Task 16 literally would first
  try (and fail) to route this through `specializationBits`.
- **TEXCOORD_1/COLOR_0 "credible viewer" framing vs the already-ruled D7
  deferral.** The gate research brief's own prompt for this ticket asks
  "is [logged-and-skipped] viable for a credible viewer?" — but D7
  (design doc:163-165) already rules this disposition, and the brief's
  own binding rules say already-ruled items should be deepened with
  evidence, not re-litigated. This gate's finding (both rows above) is
  that the ruling IS defensible on retrofit-economics grounds (import-
  time logging bounds the future cost to "re-run the importer," not
  "re-author assets"), but flags that D7's scope is the IMPORTER
  (Task 13), while this ticket's shaders have their own, narrower,
  correctly-scoped obligation (a documented extension point, not
  silence). Not a disagreement to resolve — noted so the coordinator
  sees both tickets' framing is consistent, not contradictory.
- **`KHR_texture_transform` shader-side cost vs its log-don't-drop
  disposition.** See that row's own text — the shader-side UV transform
  is cheap enough that log-don't-drop may be overly conservative
  specifically for the shading math (independent of whether the
  IMPORTER parses the extension, which is Task 13's separate decision).
  Not resolving; flagged for the coordinator to weigh Task 13's and this
  ticket's dispositions together rather than in isolation.
- **Sample 08's "orbit camera (drag)" requirement vs Task 20's later
  sequencing.** Task 16 (Stage 1) needs mouse-drag input; `rx_platform`
  gains mouse-delta APIs only in Task 20 (Stage 2, plan:408-411),
  which is sequenced AFTER Task 16 in the plan's own task numbering.
  Quoting Task 16's file list directly: it creates `samples/08_gltf_viewer/`
  with "orbit camera (drag)" and lists NO changes to
  `src/rx_platform/{window.h,window.cpp}` — the only two files Task 20
  is chartered to modify for this exact capability. Not resolving; the
  coordinator must decide whether Task 16 pulls a minimal mouse-delta
  accessor forward (duplicating a small piece of Task 20's scope early),
  Task 16 reads `SDL_Window*` raw via `Window::sdlWindow()` directly in
  `samples/08_gltf_viewer/main.cpp` (bypassing `rx_platform`'s
  abstraction, consistent with how `sdlWindow()` is already exposed for
  exactly this kind of embedder-side use), or the two tasks are
  reordered.

## New gaps

- **`KHR_materials_diffuse_transmission` is a live, currently-ratified
  Khronos extension absent from this ticket's own named extension list**
  (verified present under `extensions/2.0/Khronos/` via direct GitHub
  API directory listing, fetched 2026-08-18). Proposed fit: Task 13's
  importer-extension-detection list (out of this ticket's scope) should
  add it to its log-don't-drop enumeration; no shader work implied at
  log-don't-drop disposition.
- **`KHR_materials_pbrSpecularGlossiness`, by contrast, is formally
  Archived** (verified: it lives under `extensions/2.0/Archived/`, not
  `extensions/2.0/Khronos/`, fetched 2026-08-18) — an earlier pass at
  this research incorrectly treated it as a live registry entry; corrected
  here. It still matters as a legacy-asset-compatibility case (pre-
  metallic-roughness exporters), just not as a "current extension this
  ticket's list overlooked" case — the importer's generic unknown-
  extension handling (Task 13) should ideally distinguish "archived/
  legacy, seen in old content" from "currently ratified, just not
  implemented yet" in its log line, since the two carry different
  implications for whether future support is likely to land.
- **The fixed-function pipeline-state variant-cache gap (the
  "Architecture gap" row above) is not named anywhere in the master
  registry** (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`,
  grepped for `pipeline variant\|blend state\|fixed-function`, fetched
  2026-08-18: no hit describing this specific gap) despite being exactly
  the kind of "cheap now, retrofit-expensive later" item CLAUDE.md's
  performance policy and this gate's own charter care about — every
  future material feature needing a fixed-function-state axis (blend
  modes beyond BLEND, wireframe, stencil-based effects) will hit this
  same gap. Proposed fit: this ticket (Task 16) is the natural place to
  build the mechanism, since it is the FIRST ticket that needs it — not
  a separate registry item, unless the coordinator prefers to land the
  mechanism generically ahead of Task 16.
- **`forward_entry.slang`/`MaterialVertex` lacking a tangent field is a
  hard blocker for normal mapping that no artifact currently names as a
  Task 16 file to modify.** Task 16's file list creates
  `shaders/material/standard_pbr.slang`/`unlit.slang` but does not list
  `shaders/material/forward_entry.slang` or `material.slang` as files to
  modify (`plans/2026-08-11-phase4-scene-assets.md:316`) — yet D22
  requires normal mapping, and D8's vertex format already provisions the
  tangent data those two files don't yet surface to a material's
  `evaluate()`. This is functionally identical in shape to the
  TEXCOORD_1/COLOR_0 gap (a shared-entry-point file needs a new field)
  but, unlike those two, is NOT deferrable — normal mapping is explicitly
  in D22's Phase-4 scope. Proposed fit: amend Task 16's file list to
  include `forward_entry.slang`/`material.slang` explicitly, since this
  is discovered scope, not a new capability.

## Verification health

- **Verified first-hand this session:** every in-repo file/line citation
  above was read directly from the working tree on 2026-08-18 (not
  inherited from an earlier research pass) — `material_system.{h,cpp}`,
  `rx_api.h`, `pass_signature.h`, `material.slang`, `forward_entry.slang`,
  both tonemap shader pairs, `lit.frag.slang`, `window.h`/`window.cpp`,
  `test_unlit.slang`, `samples/06_materials/main.cpp`, and the design/
  plan/issue/feature-gap-audit/claim-validation documents. The glTF core
  spec's material field defaults (`alphaCutoff=0.5`, `normalTexture.scale=1.0`,
  `occlusionTexture.strength=1.0`) were confirmed against the actual
  JSON Schema files (`material.schema.json`,
  `material.normalTextureInfo.schema.json`,
  `material.occlusionTextureInfo.schema.json`), not inferred from
  prose. The Slang `SV_InstanceID`/`SV_VulkanInstanceID` distinction and
  the Vulkan `InstanceIndex`-includes-`firstInstance` fact were each
  independently fetched from their own primary-ish source (Slang's own
  docs page; a Vulkan refpage digest) rather than taken from a single
  source and assumed to generalize.
- **Search-digest, not primary-source-quoted:** the per-extension
  descriptions for `KHR_materials_{ior,transmission,volume,specular,
  sheen,clearcoat,iridescence,anisotropy,dispersion,variants,
  emissive_strength}` come from WebSearch result digests summarizing
  each extension's own README, not from directly fetching and quoting
  each of the eleven README files verbatim (budget/scope trade-off for
  this gate pass). The ratification-status claim ("Khronos-ratified") is
  now VERIFIED at the directory-placement level, not merely inferred: a
  direct GitHub API listing of `extensions/2.0/Khronos/` (fetched
  2026-08-18, superseding the earlier per-extension inference) confirms
  all twelve extensions the ticket names are present there, and
  separately confirms `KHR_materials_pbrSpecularGlossiness` is NOT
  there — it lives under `extensions/2.0/Archived/` instead (see the
  correction in the extensions table and New gaps above; the original
  pass in this file had this wrong). `KHR_texture_transform`'s status
  string ("Complete, Ratified by the Khronos Group") remains the one
  extension with a directly-quoted status STRING (not just directory
  placement) from its own README.
- **TEXCOORD_1/COLOR_0/KHR_texture_transform prevalence is now directly
  quantified, not search-surfaced:** all 148 model directories in
  `KhronosGroup/glTF-Sample-Assets` (`main` branch) were fetched via the
  GitHub API and each model's primary `.gltf` JSON parsed directly
  (2026-08-18) for these three attributes/extensions — 9/148 (6.1%)
  TEXCOORD_1, 8/148 (5.4%) COLOR_0, 15/148 (10.1%) KHR_texture_transform
  — with `SheenChair`'s material JSON individually fetched and parsed to
  confirm the exact "occlusion on a second UV set, offset/scaled
  independently of albedo/normal" pattern this ticket's research prompt
  specifically asked about. This replaces the earlier version's
  single-model, search-surfaced evidence for these three rows with a
  full-corpus scan; the earlier evidence is superseded, not merely
  supplemented.
- **The Vulkan spec's literal depth-bias equation text could not be
  retrieved** despite three direct-fetch attempts against
  `docs.vulkan.org` (the fetch tool's HTML-to-markdown conversion
  appears to truncate before reaching that subsection on every attempt;
  `registry.khronos.org` returned HTTP 403 outright). This does not
  affect any row in THIS matrix (the depth-bias formula is issue #23's
  concern, not #8's) — noted here only because the same fetch limitation
  would recur if this matrix's Slang/Vulkan InstanceIndex citations are
  ever re-verified against the primary spec text rather than the refpage
  digest used here.
- **Three.js and MikkTSpace citations are search digests, not fetched
  primary source** (`BRDF_Lambert`/`D_GGX` function names, the
  `tangent.w` sign-flip convention) — consistent with multiple
  independent secondary sources rather than a single unverified claim,
  but not line-cited against three.js's own shader chunk files or
  Mikkelsen's own reference implementation.
- **No dead links encountered** among directly-fetched sources; the two
  `registry.khronos.org` URLs that returned 403 were replaced with
  working `raw.githubusercontent.com`/`github.com` equivalents for the
  same content, cited above.
