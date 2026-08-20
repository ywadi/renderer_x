# Matrix — P5 T24 (issue #60): Thick-volume transmission + frosted glass

**Plan task:** Task 24 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:695-711`), Stage 3.
**Charter binding:** "thick-volume mode (thickness map + Beer-Lambert
absorption `T = exp(-absorption·distance)` via attenuationColor/Distance
— bottles, liquids; per the Khronos thickness-approximation design for
raster)" (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:403-413`);
frosted glass via the T22 mip chain (same block).
**Depends on:** T22 (mip chain — not yet built, see that matrix) and T23
(thin-surface transmission — the shared `transmission.slang` module and
IOR/roughness plumbing this ticket grows).

**Sources consulted:**
- Ticket body: `gh issue view 60`.
- Plan Task 24 + Global Constraints (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:55-134, 695-711`).
- Charter block (cited above).
- Delivered code, read first-hand at HEAD (`bf5b853`):
  `src/rx_asset/include/rx_asset/mesh_asset.h:115-175` (`MaterialAsset` —
  no volume fields), `src/rx_asset/import_gltf.cpp:1063` (volume
  presence-check, WARN-only).
- Real-content asset audit: `assets/fetched/Workshop/workshop_render_scene.glb`
  parsed directly this session (Python `struct`+`json`) — `extensionsUsed`
  contains `KHR_materials_clearcoat` and `KHR_materials_transmission`
  ONLY; **zero materials use `KHR_materials_volume`** (31/31 materials
  checked). Cross-referenced against the T23 matrix's identical finding.
- Google Filament @ commit `721ec800093de984cbee155e459298b6b2dbb855`
  (fetched 2026-08-20), Apache-2.0: `shaders/src/surface_light_indirect.fs`
  (`refractionSolidSphere`, `refractionSolidBox`, absorption term
  `vec3 T = saturate(exp(-pixel.absorption * ray.d)); t *= T;`, fetched
  verbatim).
- Khronos glTF extension registry, `KHR_materials_volume/README.md`
  (`KhronosGroup/glTF` `main` branch, fetched verbatim 2026-08-20):
  exact Beer-Lambert derivation quoted below.
- Khronos glTF Sample Renderer @ commit `863b981fb755359063e370ff7b6e956bda0716e2`:
  `source/Renderer/shaders/ibl.glsl` (`applyVolumeAttenuation`,
  `getVolumeTransmissionRay` call sites — body not independently
  confirmed, see Verification health), `material_info.glsl`
  (`getVolumeInfo`, fetched verbatim).
- Khronos `glTF-Sample-Assets` `Models/` listing, `gh api` fetch
  2026-08-20.

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/code support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------------|-------------------------------|
| 1 | Beer-Lambert absorption — exact formula | **Khronos `KHR_materials_volume` README, fetched verbatim 2026-08-20** (the extension's own normative math, the authoritative source over any port): attenuation coefficient `σ_t = -log(c) / d` (c = `attenuationColor`, d = `attenuationDistance`); transmittance `T(x) = e^(-σ_t·x)`, which simplifies to the closed form `T(x) = c^(x/d)` — applied PER COLOR CHANNEL. Filament's independent implementation matches structurally: `T = saturate(exp(-pixel.absorption * ray.d))` (`surface_light_indirect.fs`, fetched verbatim) — Filament stores a precomputed `absorption` coefficient (presumably derived from `attenuationColor`/`Distance` upstream, not independently traced in this pass) rather than re-deriving `σ_t` per-pixel. | consume-now | VERIFIED — two independent sources (the normative Khronos spec text AND Filament's shipped shader code) agree on the underlying Beer-Lambert law; the Khronos form gives the EXACT closed-form testable relationship the ticket's own acceptance sketch needs. | **This is the ticket's own named test, now pinned to an exact formula:** doubling thickness `x → 2x` gives `T(2x) = c^(2x/d) = (c^(x/d))^2 = T(x)^2` — i.e. transmittance SQUARES, precisely and only because the underlying law is exponential in `x`. Test: measure transmittance at thickness `x` and `2x` for a fixed `attenuationColor`/`Distance`, assert `T(2x) ≈ T(x)^2` within float tolerance, per-channel (attenuationColor is a 3-vector, so this must hold independently per R/G/B — a channel-coupling bug would only surface if tested per-channel, not on a luminance scalar). |
| 2 | `thicknessFactor`/`thicknessTexture` | Khronos spec (`KHR_materials_volume`): thickness is a per-fragment quantity (a texture is the normative encoding for non-trivial geometry, since raster has no real ray-traced exit-distance). Filament: `refractionSolidSphere`/`refractionSolidBox` both take an explicit `thickness` parameter used to compute the exit ray/`ray.d` (the distance fed into the absorption term). | consume-now | Confirms `thickness` is not just a shading-time scalar but a GEOMETRIC quantity feeding the refraction-ray exit-point computation, not merely the absorption term alone — a common under-scoping mistake would treat thickness as ONLY an absorption-distance input and miss its role in `refractionSolidSphere`'s own ray geometry. | Two independent value checks: (a) absorption scales per row 1's formula using the SAME thickness value the ray-exit geometry used (no silent divergence between two copies of "thickness" in the shader), (b) a `thicknessTexture`-driven per-fragment thickness produces spatially-varying transmittance across a single draw (not a per-material constant only). |
| 3 | `attenuationColor`/`attenuationDistance` defaults | fastgltf 0.9.0 (via the parallel importer-research pass): `thicknessFactor=0.0f`, `attenuationDistance=inf`, `attenuationColor=(1,1,1)` (`types.hpp:2478-2483`) — i.e. the glTF-spec-correct DEFAULT is "no attenuation at all" (infinite distance, white color → `T=1` everywhere), not an arbitrary zero. | consume-now | VERIFIED against fastgltf's own default values (the upstream library already encodes the spec-correct defaults; RendererX's own hardcoded fallback, once the field exists, must reproduce EXACTLY this "attenuation off" default for authored assets that omit the extension's volume sub-object, not an approximation). | Regression test: a transmissive material WITHOUT `KHR_materials_volume` present (only `KHR_materials_transmission`) shows `T=1` (no absorption) at any thickness — proves the "absent volume = no attenuation" default holds, distinguishing "material has no volume data" from "material has volume data with a coincidentally-white attenuationColor" (same visual result, different code path, both must be correct). |
| 4 | Solid-sphere vs. solid-box thickness-approximation choice | Filament ships BOTH `refractionSolidSphere` and `refractionSolidBox` as distinct, named functions (fetched verbatim) — i.e. Filament itself treats "which convex-shape approximation models this object's thickness" as an authored/configured choice, not a single universal formula. Khronos's own README is cited by the charter as "the Khronos thickness-approximation design for raster" (`:406-407`) — the SAME underlying problem (raster has no real inside-geometry ray tracing, so thickness must be approximated from a convex-shape model, not measured). | **Genuinely open — flagged, not resolved by this gate** | VERIFIED both Filament functions exist as named, distinct choices; NOT independently verified in this pass which one (or whether both) the Khronos Sample Renderer's `getVolumeTransmissionRay` implements — that function's body was not successfully fetched to depth in this pass (see Verification health). | The Task-1 spec must rule which approximation(s) RendererX ships (sphere-only, matching the more common "thickness ≈ sphere diameter" convention in most glTF viewers, is the lower-risk default absent contrary evidence) — this gate flags the choice exists, does not make it. |
| 5 | Frosted-glass roughness→blur monotonicity | Ticket's own acceptance sketch: "increasing transmission roughness strictly increases measured blur (mip-selection discrimination)." Pinned formula: the T22 matrix's row 8 (Filament's `evaluateRefraction` screen-space LOD: `lod = max(0, (2*log2(perceptualRoughness) + refractionLodOffset) * invLog2sqrt5)`). | consume-now (blocked on T22) | The T22 matrix (this same gate round) verifies the mip-chain infrastructure this criterion depends on does NOT exist yet, and the formula itself is monotonically increasing in `perceptualRoughness` for the domain where `log2` is defined (roughness > 0) — a closed-form property, not something to merely observe empirically. | Monotonicity test per the ticket's own text: measured blur (mip level actually sampled, or the resulting pixel-neighborhood variance) strictly increases across ≥4 roughness values; PLUS a closed-form check that the sampled LOD matches the pinned formula's output exactly (not just "trends the right direction"), catching an off-by-one or inverted-sign bug the trend-only check would miss. |
| 6 | KHR_materials_volume import-time consumption | Charter text, `:459-463`, "no importer rework needed." | **Contradicted — see Conflicts** | **VERIFIED FALSE.** `import_gltf.cpp:1063` only checks `src.volume != nullptr` to fire a generic WARN; `MaterialAsset` has zero volume fields. Confirmed by the parallel importer-research pass: **zero test coverage exists anywhere in `src/rx_asset/tests/` for volume** (no grep hits for "volume"/"Volume"), and **no committed fixture** carries `KHR_materials_volume` at all (only `cube_transmission.gltf` exists, and it has no volume sub-object). This is the LEAST-covered of the seven Stage-3-relevant extensions checked this round — worse than clearcoat/anisotropy (T21), which at least have live Khronos fixtures reachable, since volume additionally has zero RendererX-side test scaffolding to extend. | This ticket's scope must add: `MaterialAsset::thicknessFactor`/`attenuationColor`/`attenuationDistance` fields + a thickness-texture slot, a NEW test file (none to extend), and a NEW fixture (none exists) — strictly more ground-up work than T21/T23's equivalent gaps. |
| 7 | Real-content acceptance asset | Parallel to the T23 matrix's Workshop-scene audit. | **Workshop is NOT usable for this ticket** | VERIFIED first-hand (same direct glTF-JSON parse as the T23 matrix): Workshop's `extensionsUsed` list contains NO `KHR_materials_volume` — 0 of its 31 materials carry volume data. The scene's 2 transmission materials are thin-surface-only content; using Workshop as a T24 acceptance asset would be certifying nothing (the "reference-vs-ground-truth" gate rule — a gate that can't discriminate the feature under test proves nothing about it). | This ticket's acceptance MUST use the synthetic Khronos conformance fixtures (row 8) or a purpose-authored fixture, not Workshop. If a real-content thick-volume asset is wanted for the Task 11 conformance suite or the Stage-3-exit sample (T28), `DragonAttenuation` (row 8) is the correct choice — a purpose-built Khronos volume-attenuation showcase model, not Workshop. |
| 8 | Conformance fixtures | Ticket's own acceptance sketch: "Sample-viewer DragonAttenuation / AttenuationTest gated via Task 11." | log-don't-drop until Task 11 lands, consume-now for fixture acquisition | VERIFIED LIVE in `glTF-Sample-Assets` (`gh api`, fetched 2026-08-20): `AttenuationTest`, `DragonAttenuation` — both exist exactly as the ticket names them (not invented/misremembered names), confirming the ticket text is accurate here. Neither is in `tools/fetch_assets.sh` today. | Both models fetched with checksums; `AttenuationTest` (the parametric conformance grid — almost certainly the row-1 doubling-thickness test's real-content analogue) and `DragonAttenuation` (a complex-geometry showcase, closer to what T28's sample 11 would want for a "storefront vignette... thick" demonstration). |
| 9 | Frosted-glass sequencing dependency | Plan's own execution notes (`:987`): "T22→T23→T24 sequential." | N/A-Phase-5 (correctly sequenced, not this ticket's gap) | VERIFIED as the plan's own explicit, already-correct sequencing — flagged here only to cross-reference the T22 matrix's finding that the mip-chain infrastructure this ticket's frosted-glass criterion depends on does not exist as of this gate round, so T24 cannot productively start its frosted-glass sub-scope before T22 lands, exactly as already sequenced. | No new acceptance criterion; confirms the plan's sequencing is correctly load-bearing, not merely a suggestion. |

---

## Conflicts

- **Charter's "no importer rework needed" claim is FALSE for
  KHR_materials_volume, and this is the WORST-COVERED of the three
  Stage-3-relevant extensions checked across T21/T23/T24** (row 6) — zero
  test scaffolding and zero fixtures to extend, unlike clearcoat/
  anisotropy/transmission which at least have SOME existing test/fixture
  presence to build on. Not resolving; flagged prominently since this
  ticket's actual ground-up cost is understated even relative to its
  sibling tickets' same-shaped gaps.
- **The task brief's framing that a single real-content asset
  (Workshop) covers "refraction" broadly does not extend to this
  ticket's specific thick-volume/Beer-Lambert acceptance criteria**
  (row 7) — Workshop is a valid T23 asset and an invalid T24 asset; this
  is a precision correction, not a contradiction of any written plan/
  ticket text (neither names Workshop for T24 specifically).

## New gaps

- **Thickness-approximation-model choice (sphere vs. box vs. both)**
  (row 4) is not named anywhere in the plan/charter/ticket text as an
  explicit decision point — it is implicit in "per the Khronos
  thickness-approximation design for raster" without specifying WHICH
  Khronos/Filament approximation. Proposed fit: Task 1's spec should rule
  this explicitly (this gate's row 4 recommends sphere-only as the
  lower-risk default) rather than leaving it for the implementer to
  discover Filament ships two distinct functions.
- Cross-reference: the `MaterialTextureSlot` fixed-5-slot ceiling
  (registered as a New gap in the T21 matrix) is hit again here by
  `thicknessTexture` — not re-registering, noted for the coordinator's
  visibility that THREE Stage-3 tickets (T21, T23, T24) independently
  hit the same structural ceiling.

## Verification health

- **Verified first-hand:** the Beer-Lambert formula (row 1) was fetched
  verbatim from the Khronos extension's own normative README text, the
  strongest possible citation tier for a spec-mandated formula (not a
  secondary description of the spec). Filament's independent
  `exp(-absorption*d)` implementation corroborates it.
- **Verified first-hand:** the Workshop-scene volume-extension absence
  (row 7) via direct glTF JSON parsing of the actual committed binary,
  same method as the T23 matrix's transmission-presence finding.
- **Verified first-hand:** importer-code gap (row 6) via the parallel
  same-round importer-research pass, cross-checked against a repo-wide
  test-file grep confirming zero volume-related test coverage.
- **NOT independently verified, flagged explicitly:** the Khronos Sample
  Renderer's `getVolumeTransmissionRay()` and `applyVolumeAttenuation()`
  function BODIES (row 4) — the fetch tool reported them as "defined
  elsewhere, not shown in this excerpt" across two attempts; only their
  CALL SITES and argument lists were confirmed. Do not treat this gate's
  characterization of the Khronos implementation's exact ray/attenuation
  math as verified to the same tier as the Khronos README's formula
  (row 1) or Filament's code (rows 1-2, 4) — an implementer porting from
  Khronos specifically (rather than Filament, the charter's named primary
  source) should re-fetch `ibl.glsl` directly rather than rely on this
  matrix's secondary characterization.
- No dead links encountered.
