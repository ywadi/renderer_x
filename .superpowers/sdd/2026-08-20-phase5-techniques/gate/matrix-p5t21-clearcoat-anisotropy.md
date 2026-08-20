# Matrix — P5 T21 (issue #57): Clearcoat + anisotropy

**Plan task:** Task 21 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:632-651`), Stage 3.
**Charter binding:** techniques-phase charter block,
`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:371-402`
(Filament as canonical core-PBR/clearcoat/anisotropy source, ported from
CURRENT shader code "never from its documentation prose" — the June-2026
clearcoat doc discrepancy is explicitly named as resolved correctly only
in shader code, :377-379); priority order item (5), :452-458; import-side
note ":459-463" claiming every listed extension is "already parsed and
preserved/logged by the Phase 4 importer... consumption here requires no
importer rework."
**Spec decisions binding this ticket (Phase 4, still governing):** D22
(material fixed-function/specialization split, superseded in mechanism by
D28), D28 (fixed-function pipeline-state axis — alphaMode/doubleSided are
`VkPipeline` fields on `MaterialRecord`, NOT specialization constants;
clearcoat/anisotropy are **not** fixed-function state, so they route
through the specialization-bit/generics axis Task 8 is chartered to grow,
design doc `:492-512`).

**Sources consulted:**
- Ticket body: `gh issue view 57`.
- Plan Task 21 + Global Constraints (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:55-134, 342-370, 632-651, 980-992`).
- Charter block (cited above) and Phase 4 exit-review/registry text
  (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:492-512`).
- Delivered code, read first-hand at HEAD (`bf5b853`):
  `src/rx_asset/include/rx_asset/mesh_asset.h:115-175` (`MaterialAsset`,
  `MaterialDisposition`, `AlphaMode`), `src/rx_asset/import_gltf.cpp`
  (extension-presence checks at lines 1055-1067), `src/rx_asset/import_pipeline.h:100-103`
  (`MaterialTextureSlot`, fixed 5-slot enum).
- Google Filament, `google/filament` @ commit `721ec800093de984cbee155e459298b6b2dbb855`
  (`main`, fetched 2026-08-20 via `gh api repos/google/filament/commits/main`),
  license `Apache-2.0` (`gh api repos/google/filament/license`):
  `shaders/src/surface_brdf.fs` (`distributionClearCoat`/`visibilityClearCoat`,
  `D_GGX_Anisotropic`/`V_SmithGGXCorrelated_Anisotropic`, fetched directly),
  `shaders/src/surface_shading_model_standard.fs` (clearcoat energy-compensation
  block, fetched directly), `shaders/src/surface_shading_model_standard.fs`
  (`MATERIAL_HAS_CLEAR_COAT_NORMAL` remap, fetched directly).
- Khronos glTF Sample Renderer, `KhronosGroup/glTF-Sample-Renderer` @ commit
  `863b981fb755359063e370ff7b6e956bda0716e2` (`main`, fetched 2026-08-20),
  license `Apache-2.0`: `source/Renderer/shaders/material_info.glsl`
  (`getClearCoatInfo`/`getAnisotropyInfo`, fetched directly).
- Khronos `glTF-Sample-Assets` repo, `Models/` directory listing via
  `gh api repos/KhronosGroup/glTF-Sample-Assets/contents/Models`, fetched
  2026-08-20 — direct enumeration, not a search digest.
- fastgltf 0.9.0 (`.deps-cache/fastgltf-cc4cdc2b3f9750ff/`, version-pinned
  per its own `fastgltfConfigVersion.cmake`) — `types.hpp:2437-2496`
  (`MaterialClearcoat`, `MaterialAnisotropy` struct fields), read via a
  parallel in-repo research pass in this same gate round.

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/code support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------------|-------------------------------|
| 1 | Clearcoat D/V terms (second GGX lobe) | Filament: `distributionClearCoat(roughness, NoH, h)` calling `D_GGX`, `visibilityClearCoat(LoH)` calling `V_Kelemen(LoH)` — both guarded by `#define BRDF_CLEAR_COAT_D SPECULAR_D_GGX` / `#define BRDF_CLEAR_COAT_V SPECULAR_V_KELEMEN` (`shaders/src/surface_brdf.fs`, fetched 2026-08-20, quoted verbatim). | consume-now | VERIFIED present at the pinned commit — Kelemen visibility (not the base layer's Smith-correlated visibility) is the clearcoat-specific choice; this is a load-bearing detail an implementer porting "GGX + Smith" from memory would get wrong. | Compute-harness value test (Task 7's numerical harness): `distributionClearCoat`/`visibilityClearCoat` output matches the ported Filament formula at a table of (roughness, NoH, LoH) points, exact tolerance — same discipline as Task 7's own BRDF port-parity rows. |
| 2 | Clearcoat energy-compensation ("the docs got wrong" interaction) | Filament `shaders/src/surface_shading_model_standard.fs` (fetched verbatim 2026-08-20): `float Fcc = F_Schlick(0.04, 1.0, LoH) * pixel.clearCoat;` (Fresnel scaled by the clearcoat factor), `float attenuation = 1.0 - Fcc;` applied as `color *= attenuation` (with the `* NoL` variant when a normal map/clearcoat normal is present) to the **base layer's already-summed** diffuse+specular color, THEN `color += clearCoat` (or `clearCoat * clearCoatNoL`) is added — i.e. the attenuation happens to the base layer's contribution, computed from the CLEARCOAT's own Fresnel term, not the base layer's own Fresnel. All guarded by `#if defined(MATERIAL_HAS_CLEAR_COAT)`. | consume-now | VERIFIED first-hand from the current shader source (not the docs) — this is precisely the charter's own named concern ("a clearcoat documentation discrepancy identified June 2026 is resolved correctly only in the shader code"). No independent verification was done in this pass of what the DOCUMENTATION says (out of scope — the point is to port from code, not to characterize the doc bug), so the "discrepancy" itself is reported as the charter states it, not independently re-derived. | Discrimination value test: a base-layer-only probe (clearcoat factor 0) vs. the same material with clearcoat factor 1.0 shows the base layer's specular measurably DIMMED by `(1 - Fcc)` at grazing angle (where Fcc is largest) — proves the energy-compensation term is live, not just the additive clearcoat lobe. |
| 3 | Clearcoat normal remap | Filament: `#if defined(MATERIAL_HAS_CLEAR_COAT_NORMAL) shading_clearCoatNormal = normalize(shading_tangentToWorld * material.clearCoatNormal); #else shading_clearCoatNormal = getWorldGeometricNormalVector();` (fetched verbatim). Khronos Sample Renderer: `getClearCoatInfo()` (`material_info.glsl`) populates a separate `clearCoatNormal` distinct from the base normal. | log-don't-drop (Phase 5, this ticket) | VERIFIED both reference implementations treat the clearcoat normal as an independently-bindable input (`KHR_materials_clearcoat`'s `clearcoatNormalTexture`), defaulting to the geometric/base normal when absent. RendererX's importer parses NEITHER today (see row 8/Conflicts). | If the importer gains `clearcoatNormalTexture` support in this ticket's scope (see Conflicts), a GPU probe on a flat-shaded clearcoat surface with a perturbing clearcoat normal map shows the clearcoat highlight direction shift independently of the base-layer highlight (two-lobe discrimination). If deferred to log-don't-drop, the shader falls back to the geometric normal and the deferral is logged once per material load. |
| 4 | Anisotropic D/V terms | Filament: `D_GGX_Anisotropic(float at, float ab, float ToH, float BoH, float NoH)`, `V_SmithGGXCorrelated_Anisotropic(float at, float ab, float ToV, float BoV, float ToL, float BoL, float NoV, float NoL)` (`shaders/src/surface_brdf.fs`, fetched verbatim). | consume-now | VERIFIED present. The `at`/`ab` (roughness stretched along tangent/bitangent) computation and the tangent-frame (`ToH`/`BoH`/etc.) feeding these two functions were **not** located in `surface_brdf.fs` itself in this pass — `surface_material.fs` was checked and found to NOT contain it; the exact file (likely `surface_shading_parameters.fs` or `surface_getters.fs`, both unread in full this pass) is UNVERIFIED. Port the whole call chain (tangent-frame construction → `at`/`ab` derivation → the two functions above), not the two isolated D/V functions alone — porting only the cited functions without their upstream frame construction risks a sign/axis-convention bug that would only surface as an anisotropic-highlight-orientation defect, exactly the failure mode the ticket's own acceptance sketch names. | Same compute-harness value-table test as row 1, extended to the anisotropic axes; PLUS the ticket's own named orientation probe: rotating the tangent frame rotates the measured highlight (sign/axis-convention discrimination). |
| 5 | Anisotropic tangent-frame derivation from `anisotropyRotation` | Khronos Sample Renderer: `getAnisotropyInfo()` — "loads direction, strength, and computes tangent/bitangent" (`material_info.glsl`, fetched 2026-08-20, function present and confirmed to compute `anisotropicT`/`anisotropicB` from `anisotropyStrength` + rotation). | consume-now | Confirms the KHR_materials_anisotropy vocabulary (`anisotropyStrength`, `anisotropyRotation`, `anisotropyTexture`) as the reference-conformance shape Task 8's declared-but-gated slot (per the plan's own text, `:349`) must accept. | Unit test: a synthetic anisotropy texture encoding a rotated direction produces a measurably rotated `at`/`ab` pair vs. the unrotated case (closed-form check against the rotation-matrix formula, not just "looks different"). |
| 6 | KHR_materials_clearcoat import-time consumption | Charter text, `:459-463`: "every listed material extension is already parsed and preserved/logged by the Phase 4 importer... consumption here requires no importer rework." | **Contradicted — see Conflicts** | **VERIFIED FALSE for this extension.** `import_gltf.cpp` only presence-checks `src.clearcoat != nullptr` (line 1066) to fire a generic WARN — no `clearcoatFactor`/`clearcoatRoughnessFactor`/texture data reaches `MaterialAsset` (`mesh_asset.h:153-175` has no clearcoat fields at all). fastgltf 0.9.0 itself already parses the full typed `MaterialClearcoat` struct (`types.hpp:2490-2496`) — the gap is 100% RendererX importer plumbing, not upstream glTF-library capability. | This ticket's own scope must add: `MaterialAsset::clearcoatFactor`/`clearcoatRoughnessFactor` fields (+ 2-3 new `TextureRef` slots: clearcoat, clearcoatRoughness, clearcoatNormal, growing `MaterialTextureSlot` past its current fixed 5), a worker-safe decode path for the new textures, and a committed test fixture (none exists today — see row 8). Acceptance: an imported material's clearcoat fields match a hand-authored glTF fixture's JSON values exactly (decoded-value discipline, not import-success-only). |
| 7 | KHR_materials_anisotropy import-time consumption | Same charter claim as row 6. | **Contradicted — see Conflicts** | **VERIFIED FALSE.** Identical pattern: `src.anisotropy != nullptr` (import_gltf.cpp:1067) triggers only a generic WARN; no `anisotropyStrength`/`anisotropyRotation`/texture field exists on `MaterialAsset`. fastgltf already parses `MaterialAnisotropy` (`types.hpp:2437-2441`). | Same shape as row 6: new `MaterialAsset` fields + 1 new texture slot + fixture + decoded-value test. |
| 8 | Conformance fixtures | Ticket's own acceptance sketch: "Sample-viewer conformance models for both extensions gated via the Task 11 harness." | log-don't-drop until Task 11 lands, consume-now for fixture acquisition | VERIFIED LIVE in `KhronosGroup/glTF-Sample-Assets` (`Models/` directory, fetched via GitHub API 2026-08-20, not a search digest): `ClearCoatTest`, `ClearCoatCarPaint`, `ClearcoatWicker`, `CompareClearcoat`; `AnisotropyBarnLamp`, `AnisotropyDiscTest`, `AnisotropyRotationTest`, `AnisotropyStrengthTest`, `CompareAnisotropy` — 9 real fixtures across both extensions, all currently unfetched (`tools/fetch_assets.sh` has zero references to any of these names, confirmed by the same repo-wide grep the importer-research pass ran). No RendererX-committed clearcoat/anisotropy fixture exists at all today (only `cube_transmission.gltf` and `cube_emissive_strength.gltf` exist among extension-specific fixtures). | This ticket (or a tightly-coupled Task 11 dependency) must add at least one clearcoat + one anisotropy model to `tools/fetch_assets.sh`'s fetch manifest with checksums, per the Phase 4 fetch-script discipline. |
| 9 | D28 fixed-function axis interaction | D28 (design doc `:492-512`): alphaMode/doubleSided are `VkPipeline` fixed-function fields, NOT specialization constants. | N/A-Phase-5 (correctly out of scope for this ticket) | VERIFIED clearcoat/anisotropy have zero fixed-function-state surface (no blend/cull/depth-write change) — they are pure shader-code additions gated by the specialization-bit/generics axis Task 8 is chartered to build (plan `:349-350`). D28's mechanism does not need touching by this ticket. | No acceptance criterion owned by this ticket; a code-review checklist item confirming no new `PipelineRequest`/fixed-function field was added for these two features. |
| 10 | Variant-gating cost proof (unused → free) | Ticket's own acceptance sketch. | consume-now | This is the same proof shape Task 8's own acceptance sketch already commits to (plan `:359-362`, "Variant discrimination... produces SPIR-V free of that feature's code path... pays no measured cost vs. the Phase 4 baseline") — Task 21 is simply the second consumer of a mechanism Task 8 is chartered to deliver first. | A material using neither clearcoat nor anisotropy shows zero measured cost delta vs. the pre-Task-21 baseline (re-run of Task 8's own harness on the composed material, per that task's acceptance sketch). |

---

## Conflicts

- **Charter's "no importer rework needed" claim is FALSE for both
  KHR_materials_clearcoat and KHR_materials_anisotropy** (rows 6-7).
  Verified first-hand: `import_gltf.cpp` performs only a presence-check +
  generic WARN for both extensions (lines 1066-1067); `MaterialAsset`
  (`mesh_asset.h:153-175`) has zero fields for either extension's data.
  This is not a Task-21-adjacent nuance — clearcoat/anisotropy shading
  code has literally nothing to read without this importer work landing
  first, in THIS ticket's own scope (the charter names no separate
  importer-rework ticket for Stage 3 extensions). The charter's blanket
  claim was true only for `KHR_materials_emissive_strength` among the
  seven extensions checked across this gate round's Stage-3 tickets
  (confirmed by the parallel importer-research pass feeding all of T21/
  T23/T24) — clearcoat and anisotropy need the same treatment. Not
  resolving; the coordinator should either fold this importer work
  explicitly into Task 21's file list or split it into a shared
  prerequisite the Stage-3 tickets share (T21/T23/T24 all hit the same
  gap independently for their own extensions).

## New gaps

- **`MaterialTextureSlot`'s fixed 5-slot enum** (`import_pipeline.h:100-103`,
  documented as mirroring `MaterialAsset`'s exact 5 `TextureRef` fields)
  is a structural ceiling every Stage-3 extension with its own texture
  (clearcoat, clearcoatRoughness, clearcoatNormal, anisotropy — 4 new
  slots from this ticket alone, before T23/T24's transmission/volume/
  thickness textures are even counted) will hit. Not this ticket's gap
  alone to register — flagged here as the FIRST ticket to need it, since
  Task 21 is scheduled independent/first among the Stage 3 material
  tickets per the plan's own sequencing note (`:987`, "T21 independent").
  Proposed fit: this ticket grows the enum/slot mechanism generically
  (e.g. a `std::vector<TextureRef>` or an extensible slot registry)
  rather than each subsequent ticket adding a fixed handful more.

## Verification health

- **Verified first-hand:** every Filament/Khronos-Sample-Renderer code
  citation above was fetched directly from the pinned commit (not a
  search digest); the importer-code findings (rows 6-7) were read
  directly at HEAD by a parallel same-round research pass covering all
  seven Stage-3-relevant glTF material extensions, cross-checked against
  fastgltf's own `types.hpp` to confirm the gap is RendererX-side, not
  upstream-library-side.
- **Inferred/lower-confidence, flagged explicitly:** row 4's anisotropic
  tangent-frame construction site (which exact Filament file computes
  `at`/`ab`/`ToH`/`BoH` before calling the cited D/V functions) was NOT
  located in this pass — `surface_material.fs` was checked and ruled out;
  the likely candidates (`surface_shading_parameters.fs`,
  `surface_getters.fs`) were not read in full. This is flagged as an
  explicit implementer TODO in row 4 rather than guessed at.
- The clearcoat-documentation-discrepancy characterization is reported
  exactly as the charter states it (a named, dated concern) — this pass
  did not independently read Filament's *documentation* prose to
  characterize what specifically is wrong there, since the charter's own
  instruction is to port from code and treat the doc as untrusted, making
  that comparison out of scope for a completeness matrix.
- No dead links encountered; all GitHub API/raw fetches returned content
  on the first attempt.
