# Matrix — P5 T23 (issue #59): Thin-surface transmission (real glass)

**Plan task:** Task 23 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:673-693`), Stage 3.
**Charter binding:** "Glass is REAL transmission, never alpha blending"
(`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:403-413`);
priority order item (6) (`:452-458`); frame-pipeline target (`:447-451`,
glass/transmission sits AFTER the scene-color mip chain, BEFORE
particles/transparency); import-side "no importer rework" claim (`:459-463`).
**Depends on:** Task 22 (scene-color mip chain — screen-space refraction
falls back through it) and this same gate round's T22 matrix (mip-chain
infrastructure does not exist yet; see that matrix's rows 1-5).

**Sources consulted:**
- Ticket body: `gh issue view 59`.
- Plan Task 23 + Global Constraints (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:55-134, 673-693`).
- Charter block (cited above).
- Delivered code, read first-hand at HEAD (`bf5b853`):
  `src/rx_asset/include/rx_asset/mesh_asset.h:115-175` (`MaterialAsset`,
  `MaterialDisposition{StandardPBR, Unlit}`, `AlphaMode{Opaque,Mask,Blend}`),
  `src/rx_asset/import_gltf.cpp:1060-1064` (transmission/ior presence
  checks — WARN-only, no field extraction), `src/rx_asset/import_pipeline.h:100-103`
  (fixed 5-slot `MaterialTextureSlot`), `src/rx_scene/include/rx_scene/draw_list.h:148-157`
  (`ViewLists` — exactly TWO partitions: `[0,opaqueCommandCount)` = OPAQUE+MASK,
  `[opaqueCommandCount, commands.size())` = BLEND; no third category),
  `:266-277` (BLEND sort-key shape, `reservedBlendOrder` always zero).
- Real-content acceptance asset, inspected first-hand (not taken on
  faith): `assets/fetched/Workshop/workshop_render_scene.glb` — parsed
  its embedded glTF JSON chunk directly (Python `struct`/`json`, this
  session): `extensionsUsed: ["KHR_materials_clearcoat",
  "KHR_materials_transmission"]`, `extensionsRequired: []`, 31 materials
  total, 2 using `KHR_materials_transmission` (`glass`,
  `transmissionFactor: 0.9730974678953782`; `Material.039`,
  `transmissionFactor: 1.0`), 0 using `KHR_materials_volume` or
  `KHR_materials_ior`. `assets/test/ASSET-NOTES.md:1` — the asset's
  ACTUAL committed provenance text: "Workshop (Render Scene) by 3DHaupt
  ... complex-scene test asset (531k tris, 4k textures, GLB) ... Test
  asset only; cleaned out pre-go-live" — **no "tagged 'refraction'"
  string exists anywhere in this file or elsewhere in the repo** (grepped
  `ASSET-NOTES.md`, `docs/`, `.superpowers/` for "refraction" tag
  language; only hits were unrelated plan-doc prose, not an asset tag).
- Google Filament @ commit `721ec800093de984cbee155e459298b6b2dbb855`
  (fetched 2026-08-20), Apache-2.0: `shaders/src/surface_light_indirect.fs`
  (`refractionThinSphere`, `evaluateRefraction` — both overloads, fetched
  verbatim), `shaders/src/surface_shading_lit.fs`
  (`REFRACTION_TYPE_THIN`/`REFRACTION_TYPE_SOLID`,
  `REFRACTION_MODE_CUBEMAP`/`REFRACTION_MODE_SCREEN_SPACE`
  conditional-compilation axes, `pixel.etaIR`/`etaRI` IOR-ratio
  computation, fetched verbatim).
- Khronos glTF Sample Renderer @ commit `863b981fb755359063e370ff7b6e956bda0716e2`
  (fetched 2026-08-20), Apache-2.0: `source/Renderer/shaders/ibl.glsl`
  (`getIBLVolumeRefraction`, `getVolumeTransmissionRay`, fetched partially
  verbatim), `source/Renderer/shaders/material_info.glsl`
  (`getTransmissionInfo`/`getIorInfo`, fetched verbatim).
- Khronos `glTF-Sample-Assets` `Models/` listing, `gh api` fetch
  2026-08-20 — direct enumeration.
- fastgltf 0.9.0 `types.hpp` (via the parallel importer-research pass
  this same round).

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/code support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------------|-------------------------------|
| 1 | Never alpha blending — real BTDF | Charter's own headline text (`:403-405`, quoted above). Filament: `refractionThinSphere(etaIR, uThickness, n, r, out Refraction ray)` — computes a real refracted ray direction/exit point, not a blend-factor. | consume-now | VERIFIED — Filament's thin-surface path (`REFRACTION_TYPE_THIN`) is a genuine geometric-refraction computation, structurally incompatible with `AlphaMode::Blend`'s simple over-compositing. RendererX's CURRENT `AlphaMode` enum (`mesh_asset.h:121`) has exactly `{Opaque, Mask, Blend}` — no `Transmission`/`Refractive` value; using `Blend` for glass would be the exact anti-pattern the charter names. | Discrimination probe per the ticket's own acceptance sketch: a glass surface shows BOTH the refracted background AND the Fresnel specular highlight in one frame (two independent value probes) — an alpha-blend impostor would show only ONE lobe (background attenuated by alpha, no separate specular reflection term), so this probe positively discriminates against the anti-pattern. |
| 2 | Fresnel surface reflection retained alongside transmission | Filament `evaluateRefraction()` (outer overload, fetched verbatim): `Ft *= 1.0 - E;` where `E` is the energy already reflected (the specular/IBL reflection term computed elsewhere in the same pixel's shading) — i.e. transmission is explicitly `(1 - reflected energy) * refracted color`, not transmission OR reflection. | consume-now | VERIFIED, exact formula cited. | Value test: total outgoing radiance (reflected + transmitted) does not exceed the furnace-test energy bound already established by Task 7's white-furnace test (re-run on a transmissive material) — proves the `1 - E` term is live, not a separately-tuned/ungrounded blend weight. |
| 3 | Dual mode: `REFRACTION_MODE_CUBEMAP` vs `REFRACTION_MODE_SCREEN_SPACE` | Filament, `surface_shading_lit.fs`/`surface_light_indirect.fs`, both fetched verbatim — these are DISTINCT compile-time axes: `REFRACTION_TYPE` (thin/solid — this ticket vs. T24) is orthogonal to `REFRACTION_MODE` (cubemap/screenspace — where the refracted color comes FROM). | consume-now | VERIFIED as two independent axes, not one. The ticket's own text ("screen-space refraction first, environment/probe fallback on miss") describes exactly the `REFRACTION_MODE_SCREEN_SPACE`-with-fallback pattern; Filament's own screen-space branch has NO explicit environment fallback coded in the cited function (the `#else` branch samples `sampler0_ssr` unconditionally) — Filament relies on a SEPARATE mechanism (its SSR pass's own miss handling, or the material author choosing `REFRACTION_MODE_CUBEMAP` outright for off-screen-heavy scenes) rather than a per-pixel runtime fallback INSIDE `evaluateRefraction()` itself. RendererX's own fallback-on-miss requirement (this ticket's own acceptance sketch: "miss regions provably fall back to the environment") is therefore NOT a direct port of Filament's function as-is — it needs an explicit miss-detection branch (screen-space UV out of `[0,1]`, or a depth-mismatch heuristic) added around the ported core, feeding Task 10's environment/probe path (T26/SSR's own probe-fallback mechanism is the natural sibling to reuse — see Conflicts). | Refraction geometry probe per the ticket's own acceptance sketch: a known background feature appears at the IOR-predicted screen-space offset (closed-form check against Snell's-law geometry, not eyeballed); a SEPARATE probe forces an off-screen/miss condition and asserts the environment/probe path is used instead of a garbage/clamped sample. |
| 4 | IOR-driven Fresnel + refraction ratio | Filament, `surface_shading_lit.fs`: `float materialIor = f0ToIor(pixel.f0.g); pixel.etaIR = airIor / materialIor; pixel.etaRI = materialIor / airIor;` (fetched verbatim) — IOR round-trips through F0 rather than being stored as a raw scalar directly consumed by the refraction ray functions. | consume-now | VERIFIED. RendererX's importer does not extract the `ior` scalar today (see row 6) — StandardPBR's EXISTING fixed dielectric F0 (0.04, glTF/Disney default per the Phase 4 gate's own finding) is what a per-material IOR override would need to replace. | Unit test: an IOR override of e.g. 1.33 (water) vs. the default 1.5 produces a measurably different refraction angle at the SAME viewing geometry (closed-form Snell's-law comparison). |
| 5 | Roughness on a transmissive surface (frosted-thin variant) | Filament's `evaluateRefraction()` outer overload computes `perceptualRoughness` and feeds it to a mip-selecting LOD (see T22 matrix rows 7-8) even in the THIN case — frosted-thin glass (e.g. etched window glass) is not exclusive to thick/solid mode. | consume-now (this ticket) / depends on T22 | The T22 matrix (this same gate round) verifies NO mip-chain infrastructure exists yet — this ticket's "roughness" acceptance criterion is therefore blocked on T22 landing first, exactly as the plan's own sequencing states (`:987`, "T22→T23→T24 sequential"). | Deferred to T22's own delivery; THIS ticket's test only needs to prove the roughness VALUE reaches the LOD-selection call correctly (an interface-level test), not the full blur — the actual blur-monotonicity probe belongs to T24's acceptance sketch (which explicitly owns "frosted monotonicity probe"), consistent with the plan's own task split. |
| 6 | KHR_materials_transmission import-time consumption | Charter text, `:459-463`, "no importer rework needed." | **Contradicted — see Conflicts** | **VERIFIED FALSE.** `import_gltf.cpp:1064` only checks `src.transmission != nullptr` to fire a WARN ("material will render OPAQUE (transmission ignored)"); no `transmissionFactor`/`transmissionTexture` field exists on `MaterialAsset`. Existing test (`import_gltf_gpu_test.cpp:1225-1242`, confirmed by the parallel importer-research pass) explicitly asserts "the material parameter set carries no transmission field at all" — i.e. Phase 4 deliberately built AND TESTED the absence of this data path; this ticket must reverse that, not merely add to it. fastgltf 0.9.0 already parses the full `MaterialTransmission` struct. | This ticket's scope must add `MaterialAsset::transmissionFactor` (+ a transmission-texture slot growing `MaterialTextureSlot` past its fixed 5, same structural gap the T21 matrix's New-gaps section names) and REPLACE the existing Phase-4 test's "no transmission field" assertion with a positive decoded-value assertion — a deliberate, documented behavior change to an existing test, not a silent overwrite. |
| 7 | KHR_materials_ior import-time consumption | Same charter claim. | **Contradicted — see Conflicts** | **VERIFIED FALSE.** `import_gltf.cpp:1061` only checks `src.ior != 1.5F` to gate a WARN; no `ior` field reaches `MaterialAsset`. fastgltf already exposes `Material::ior` (default 1.5, matching the glTF spec default — confirmed correct as a hardcoded fallback CONSTANT, but there is no per-material override path). | `MaterialAsset::ior` field addition (defaulting to 1.5 when the extension is absent, matching current shader-constant behavior exactly for non-IOR-authored assets — a regression guard). |
| 8 | Architecture gap: transmission draws have no partition to render in | Charter's frame-pipeline target (`:447-451`): glass/transmission is its OWN pipeline stage, positioned BEFORE "particles/transparency" (the ordinary alpha-BLEND partition) and AFTER the scene-color mip chain — i.e. structurally DISTINCT from, and sequenced BEFORE, standard alpha blending. | **Genuinely open — flagged, not resolved by this gate** | VERIFIED: `ViewLists` (`draw_list.h:148-157`) has exactly TWO command partitions today — OPAQUE+MASK and BLEND — with `MaterialDisposition` (`mesh_asset.h:115-119`) offering only `{StandardPBR, Unlit}`. Nothing in `DrawListBuilder` recognizes a transmission/glass category. Simply routing transmissive materials into the existing BLEND partition would be semantically wrong for two independent reasons: (a) BLEND's own sort key (`draw_list.h:275-277`) is depth-dominant back-to-front ordering for alpha-compositing correctness — transmission draws read the SCENE COLOR BUFFER as a texture and are not depth-order-sensitive the same way (they need the opaque+SSR+mip-chain pass to have ALREADY completed, a PIPELINE-STAGE ordering constraint, not a per-draw depth-sort constraint); (b) the charter's own frame-pipeline text places glass/transmission and particles/transparency as TWO SEPARATE stages, meaning a scene with both glass AND alpha-blended particles needs them rendered in two distinct passes, not interleaved by one shared depth-sorted partition. | This is a genuine open design question for the coordinator/Task-1-spec, not something this gate resolves: either (a) a THIRD `ViewLists` partition (`transmissionCommandCount` boundary, its own sort-key shape) is added to `draw_list.h`, or (b) transmissive materials get a NEW `MaterialDisposition` value and are filtered out of/into a dedicated pass at `recordDrawList`/graph-wiring time rather than at the sort-key level. Recommendation: (a) is more consistent with the existing three-partition-shape precedent (`sortkey` namespace already documents OPAQUE/BLEND/SHADOW as three distinct 64-bit shapes) and keeps the D26.3 instancing-collapse rule (opaque-only today) unambiguous — transmission draws, like blend draws, should NOT be instancing-collapsed by default (world-position-dependent refraction), matching BLEND's existing `never collapsed [D14]` rule; a new partition inherits that correctly, whereas silently special-casing BLEND would risk it collapsing transmission draws that happen to share geometry identity, which is wrong. |
| 9 | Real-content acceptance asset | Task brief's framing: "the Workshop scene ... tagged 'refraction'." | **Partially confirmed, tag-language corrected** | VERIFIED first-hand by direct glTF JSON inspection (not asserted from the brief or from `ASSET-NOTES.md`'s prose, which does not use the word "refraction" or any tag mechanism at all): Workshop DOES carry 2 real `KHR_materials_transmission` materials (`glass`, `Material.039`) usable as a THIN-SURFACE transmission acceptance asset for this ticket. It carries ZERO `KHR_materials_volume`/`ior` data, so it is NOT usable for T24's thick-volume Beer-Lambert acceptance criterion (see T24 matrix). The "tagged 'refraction'" framing does not correspond to any literal tag/label in the repo — the asset is committed under the generic label "complex-scene test asset" in `assets/test/ASSET-NOTES.md:1`. | Workshop's `glass`/`Material.039` materials render with visibly refracted background geometry (not flat/opaque) once this ticket + Task 11's conformance harness land — a real-content regression guard alongside the synthetic TransmissionTest fixture (row 10). |
| 10 | Conformance fixtures | Ticket's own acceptance sketch: "Sample-viewer TransmissionTest models gated via Task 11." | log-don't-drop until Task 11 lands, consume-now for fixture acquisition | VERIFIED LIVE in `glTF-Sample-Assets` (`gh api`, fetched 2026-08-20): `TransmissionTest`, `TransmissionThinwallTestGrid`, `TransmissionRoughnessTest`, `TransmissionOrderTest`, `CompareTransmission` — 5 fixtures, none currently in `tools/fetch_assets.sh`. RendererX's own committed fixture, `assets/test/cube_transmission.gltf` (material `glass_mat`, `transmissionFactor: 0.9`, NO texture), exists but is minimal (no transmission texture, no roughness variation) — insufficient alone for the ticket's roughness/geometry-offset acceptance criteria. | This ticket adds at least `TransmissionTest` (the base conformance model) and `TransmissionThinwallTestGrid` (thin-mode-specific) to the fetch manifest with checksums. |

---

## Conflicts

- **Charter's "no importer rework needed" claim is FALSE for both
  KHR_materials_transmission and KHR_materials_ior** (rows 6-7) — same
  finding pattern as the T21 matrix, independently confirmed for this
  ticket's own two extensions. Not resolving; flagged for the coordinator
  to fold importer work explicitly into this ticket's file list (Task 23
  currently lists only `shaders/material/transmission.slang` + entry
  integration, `src/rx_scene` transmission partition wiring, `src/rx_asset`
  consumption — the LAST item is where this work belongs, but the
  ticket's acceptance sketch does not currently name the importer-side
  decoded-value test this requires).
- **No draw-list partition exists for transmission draws** (row 8) — a
  genuine, unresolved architecture question, not a contradiction of
  stated text (the charter's frame-pipeline slot IMPLIES this need but
  the plan/ticket text never states it explicitly). Recommendation given
  in row 8; the coordinator should rule on it before dispatch since
  Task 25 (blendOrder/D27 partition revisit, this same gate round's other
  matrix) touches the SAME `draw_list.h` sort-key machinery — sequencing
  T23's partition addition and T25's partition-revisit work against each
  other matters (a new partition shape landing mid-way through T25's own
  D27 resolve-once-per-key fix could reintroduce the exact inefficiency
  T25 is closing, if the new partition isn't included in T25's fix).
- **The task brief's "tagged 'refraction'" framing does not match the
  repo's actual asset provenance record** (row 9) — not a contradiction
  of anything IN the repo, but worth recording precisely: the Workshop
  asset IS usable as described in substance (verified real transmission
  materials), but no literal "refraction" tag exists to point to; future
  citations of this asset should reference its verified material content
  directly (2 `KHR_materials_transmission` materials, 0
  `KHR_materials_volume`), not an assumed tag.

## New gaps

- None beyond the `MaterialTextureSlot` fixed-5-slot ceiling already
  registered as a New gap in the T21 matrix (this ticket adds at least
  one more texture — `transmissionTexture` — to the same pressure point;
  not re-registering, cross-referenced here).

## Verification health

- **Verified first-hand:** all Filament/Khronos-Sample-Renderer function
  citations fetched verbatim from pinned commits; the Workshop glTF JSON
  was parsed directly in this session (Python `struct`+`json` against the
  actual committed `.glb` binary), not taken from any secondary
  description — this is the strongest-possible verification tier for an
  asset-content claim.
- **Verified first-hand:** importer-code findings (rows 6-7) read
  directly at HEAD via the parallel same-round importer-research pass,
  cross-checked against fastgltf's `types.hpp` and the existing Phase 4
  test's explicit "no transmission field" assertion.
- **Inferred/lower-confidence:** Filament's screen-space refraction
  MISS-handling behavior (row 3) is inferred from the absence of visible
  fallback code in the cited function body, not from reading Filament's
  broader SSR/probe-fallback orchestration code (out of this ticket's
  scoped reading — that orchestration is closer to T26's own scope).
  This is flagged as an inference, not presented as a confirmed absence
  of ANY fallback mechanism in Filament's engine as a whole.
- No dead links encountered.
