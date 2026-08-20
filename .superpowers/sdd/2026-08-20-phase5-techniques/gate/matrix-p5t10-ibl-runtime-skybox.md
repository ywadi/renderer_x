# Completeness matrix — ticket #46: [P5 T10] IBL runtime integration + skybox (FG1 closure)

**Plan task:** Task 10, "IBL runtime integration + skybox (FG1 closure)"
(`docs/superpowers/plans/2026-08-20-phase5-techniques.md:397-418`), Stage 1.
Consumes Task 9's bake chain (ticket #45,
`gate/matrix-p5t09-ibl-bake-chain.md`) and Task 4's exposure API (Stage 0,
not in this dispatch's scope — cross-referenced only).

**Binding sources:** techniques charter environment paragraph
(`toolchain-platform-rhi-design.md:422-427`, "each lobe fed by both direct
lighting and IBL"); D22's FG1 amendment (Phase-4 interim flat-ambient term,
`phase4-scene-assets-design.md:360-373`, "the full skybox+IBL environment
path is registered for the techniques phase") — **this ticket is that
registered closure, named explicitly in its own title.**

**Ticket body (`gh issue view 46`):** Scene-level environment binding
(skybox pass + IBL diffuse/specular feeding every lit-path lobe),
replacing the Phase-4 flat ambient term; exposure-aware via Task 4;
environment intensity in physical units. Acceptance sketch: discrimination
against the old ambient (regenerated references must FAIL against the
Phase-4 flat-ambient renderer); mirror-metal sphere reproduces the
environment; rough-metal energy sane via furnace test on the full path;
skybox pass gated; sponza/workshop sustained real-driver runs clean.

**Sources consulted (in-repo, read in full this session):**
`shaders/material/material.slang` (`MaterialVertex::ambientColor`,
`RxDrawData::ambientColor` — the FG1 term's exact current carriage
mechanism, per-DRAW not per-material), `shaders/material/
standard_pbr.slang` (:233-238, the FG1 ambient composition:
`ambient = v.ambientColor * occlusion * baseColor.rgb` — independent of
metallic, the literal "metals never render black" mechanism this ticket
retires), `shaders/material/forward_entry.slang` (`vertexMain`'s
`output.ambientColor = draw.ambientColor.xyz` passthrough), `src/rx_scene/
include/rx_scene/scene.h` (`Scene` class surface — `RenderableHandle`/
`LightHandle` patterns, `createDirectionalLight()`/`DirectionalLightDesc`
as the established "one typed desc struct + one handle-returning factory
method" idiom this ticket's `Environment` API should likely follow; grepped
for `Environment`/`Skybox`/`IBL` — zero hits, confirming no existing
surface).

**Sources consulted (external, fetched 2026-08-20):**
`shaders/src/surface_light_indirect.fs` (`google/filament`, same pinned
commit as the T7/T9 matrices — see T7 matrix for the resolved release SHA
`0e58877c09afb1aacd09ff640f74d2adcd2a7e80`): `evaluateIBL()` (:717, the
diffuse+specular IBL composition entry point), `prefilteredRadiance()`
(:122-126, mip-selected cubemap sample), `specularDFG()` (:135, DFG-LUT-
driven `E` term), `frameUniforms.iblLuminance` (a FRAME-level scalar,
comment at :834 quoted: *"iblLuminance is already premultiplied by the
exposure"* — the frame-level pre-exposure convention).

---

## The matrix

| Requirement | Filament precedent (cited) | Current shipped state | Disposition | Proposed acceptance criterion |
|---|---|---|---|---|
| Scene-level environment API | `Scene`'s established handle/desc pattern (`DirectionalLightDesc`/`LightHandle`/`createDirectionalLight()`, `scene.h:221-238`) — the ticket's own file list names `src/rx_scene` for the "environment API." | Absent — no `Environment`/`Skybox`/`EnvironmentHandle` symbol anywhere in `rx_scene` (grep-verified). | consume-now, new API surface | `Scene::setEnvironment(EnvironmentDesc)` (or an `EnvironmentHandle`-returning factory, matching the light-creation precedent) taking a bindless cubemap/SH-coefficients/prefiltered-mip/DFG-LUT bundle (Task 9's bake outputs) plus an intensity scalar in PHYSICAL units (the ticket's own text). Test: a `Scene` with no environment set behaves byte-identically to today's flat-ambient path is NOT required (this ticket explicitly REPLACES that path, see the discrimination row below) — the regression bar is "existing 08/09 GATES regenerate with new, provably-more-correct references," not "old behavior survives unchanged." |
| Discrimination against the Phase-4 flat ambient | The plan's own "reference-vs-ground-truth discipline" (Global Constraints, plan:74-81 — "gates that bake a bug certify the bug... every reference regeneration carries provenance") applied reflexively to this project's OWN prior output. | `standard_pbr.slang:233-238`'s flat term (`v.ambientColor * occlusion * baseColor.rgb`, uniform color, no directionality, no roughness/metallic response beyond the diffuse-only `baseColor` multiply) is the thing being retired. | consume-now (the ticket's own headline acceptance line) | The exact test the ticket names: regenerate the DamagedHelmet/sponza/workshop references under the NEW IBL path, then run those SAME scenes through the OLD flat-ambient code path (still compilable — a git-tagged prior commit, or a compile-time-gated fallback kept only long enough to run this ONE comparison) and assert the new references FAIL the old renderer's own tolerance (a real gate-flip, not just "the images look different" — quantified pixel-diff exceeding whatever tolerance the old gate used). This is a one-time proof, not a permanent dual-path requirement — the old flat-ambient code is retired after, per the ticket's own "replacing" language (not "adding a toggle"). |
| Diffuse IBL feeding the diffuse lobe | `evaluateIBL()`: `Fd = pixel.diffuseColor * diffuseIrradiance * (1-E) * diffuseBRDF` (surface_light_indirect.fs, grep-located at :801-805) — irradiance FROM Task 9's SH/cubemap output, `(1-E)` an energy-conservation correction (`E` = specular DFG "reflected" energy, so diffuse gets what specular didn't reflect — a real coupling between the two lobes, not two independent additions). | Absent (StandardPBR has no irradiance consumption at all today). | consume-now | GPU test: a Lambertian (roughness=1, metallic=0) sphere under a KNOWN synthetic environment (e.g. a uniform-radiance environment, reusing Task 9's own analytic-ground-truth fixture) reproduces the closed-form Lambertian-under-uniform-environment irradiance value (`irradiance = L * PI` for a uniform environment of radiance `L`, standard hemispherical integral) at a matched-pose probe pixel — this is the SAME "known coefficients" ground truth Task 9's own SH test establishes, reused here as an END-TO-END check rather than a bake-only one. |
| Specular IBL feeding the specular lobe | `evaluateIBL()`: `Fr = E * prefilteredRadiance(r, perceptualRoughnessToLod(pixel.perceptualRoughness))` (:755-758) — mip selected by roughness, `E` the specular-DFG scale. | Absent. | consume-now | The ticket's own acceptance line, quoted: "Mirror-metal sphere under a known environment reproduces the environment (matched-pose value probes)." Concrete test: `roughness≈0` (clamped to `kMinRoughness` per the existing shipped constant), `metallic=1` sphere reflects mip-0 of the prefiltered cubemap at a normal-incidence probe pixel — a DIRECT value match against the source environment texel at the reflected direction (not just "looks shiny"), matching the ticket's own "matched-pose value probes" wording precisely. |
| Skybox pass | Standard precedent (every first-tier engine renders a background skybox as a full-screen/far-plane pass sampling the SAME environment cubemap the IBL terms consume — not independently cited from Filament's own skybox shader this session, a light-touch item relative to the lobe-feeding rows above). | Absent — no skybox pass anywhere in the render graph or samples. | consume-now | GPU test: pixels NOT covered by any opaque geometry sample the environment cubemap directly at the camera ray's direction (a value match against the source cubemap, same discipline as the specular-IBL row) — gated with provenance per the ticket's own "Skybox pass gated (reference + provenance)" line. |
| Exposure-aware IBL (Task 4 integration) | `frameUniforms.iblLuminance`, quoted comment: *"iblLuminance is already premultiplied by the exposure"* — Filament applies exposure to the ENVIRONMENT's intensity at the SAME frame-level point it applies exposure to direct light intensities, not as a separate per-material step. | Task 4 is Stage 0 (outside this dispatch's ticket range, T7-T12) — not independently re-verified this session; its OWN plan text (`plan:232-254`) establishes "pre-exposure convention... binds Stages 1-2" as a ruling Stage 0 makes, which this ticket then must consume. | consume-now, CROSS-TICKET dependency (Stage 0 → Stage 1, already correctly sequenced by the plan) | Whatever pre-exposure convention Task 4's spec ruling adopts (pre-exposed at the light/environment source vs. full-float-then-exposed-at-tonemap), this ticket's environment-intensity value must be threaded through the SAME single point Task 4 establishes for direct lights — a discrimination test: changing `--exposure` by a stop (2x) changes the SKYBOX's own rendered radiance by the same 2x factor as it changes a directly-lit surface's radiance (both terms pass through exactly one exposure multiply, matching `forward_entry.slang`'s own existing "applied here... BEFORE the value ever reaches the... tonemap pass" discipline for the current exposure mechanism). |
| Environment intensity in physical units | Charter language, "physical light intensities/units" (priority 3's own framing, extended here to environments) — Filament's own IBL intensity is expressed in lux/lumens-consistent units matching its punctual-light model (not independently re-derived this session; Task 13, Stage 2, is where physical light UNITS land for punctual lights per the plan's own sequencing — this ticket is EARLIER, Stage 1). | Absent (no physical-units concept anywhere in the current lighting path — `RxDrawData::lightColor` is an opaque "color * intensity" product per its own comment, `material.slang:241`). | **Sequencing tension, not a hard blocker — see Open Questions** | — |

## Open Questions

- **"Environment intensity in physical units" (T10, Stage 1) vs
  "physical light units" landing at Task 13 (Stage 2) — RECOMMEND T10
  defines its OWN environment-intensity unit convention now (a
  documented lux-equivalent or nits-equivalent scalar, Filament's own
  IBL-intensity convention is the citable precedent even though not
  independently re-derived this session) rather than waiting for Task
  13, with an explicit code comment flagging the exact point Task 13
  must reconcile it against punctual-light units.** The ticket's own
  text commits to "physical units" NOW, but the plan's own Stage
  ordering puts the broader physical-light-units system (Task 13,
  KHR_lights_punctual, lux/lumens/candela) a full stage later. Two
  readings: (a) T10 invents a standalone environment-intensity unit
  now, independent of Task 13's later system, risking a UNIT MISMATCH
  between "environment intensity" and "light intensity" that Task 13
  then has to reconcile (a real but bounded retrofit — one conversion
  constant, not a redesign); (b) T10 uses a placeholder unscaled
  intensity (matching the CURRENT `lightColor`'s own "opaque color *
  intensity product, no physical unit" framing) and defers "physical
  units" to Task 13, treating the ticket's own "physical units" phrase
  as aspirational/Task-13-scoped rather than literally T10's own
  obligation. Recommend (a): this matches the ticket's own explicit
  acceptance language more literally, and a single documented
  conversion constant is cheap to add later (the SAME "cheap now,
  retrofit-expensive later" logic CLAUDE.md's performance rule applies
  elsewhere in this plan) — but this is a real judgment call the
  coordinator should rule on explicitly rather than leave implicit,
  since it directly affects whether Task 13 later needs a migration
  step for every environment asset's stored intensity value.
- **Does the skybox pass's camera-ray reconstruction need a NEW render-
  graph primitive, or does it reuse the existing forward pass's
  camera/view-proj plumbing?** Not resolved this session (light-touch
  item per the matrix row above) — the skybox pass needs the INVERSE
  view-projection to reconstruct a world-space ray per pixel, which no
  existing pass in this codebase currently does (every existing pass
  consumes `RxDrawData::viewProj` forward, never inverted) — flagged as
  a possible small new utility (CPU-side inverse-matrix push constant,
  or GPU-side `inverse()` — cheap either way) the coordinator should
  assign to this ticket's own file list explicitly rather than
  discovering it as scope-creep mid-implementation (the SAME pattern
  the Phase-4 gate matrices flagged repeatedly for this codebase —
  matrix-issue08's own "tangent field... hard blocker... no artifact
  currently names as a file to modify" precedent).

## New gaps

- **No skybox-pass precedent anywhere in this codebase's render graph**
  (full-screen pass reading a cubemap by camera-ray direction) — the
  closest existing precedent is the tonemap pass (`shaders/multipass/
  tonemap.frag.slang`, a full-screen pass reading an HDR COLOR
  attachment, not a cubemap-by-ray-direction) — structurally similar
  (full-screen triangle + fragment sampling) but not a direct template
  for the camera-ray reconstruction this ticket needs (see the Open
  Question above).

## Verification health

- `MaterialVertex`/`RxDrawData`'s `ambientColor` field and
  `standard_pbr.slang`'s exact FG1 composition formula are read directly
  from the current working tree this session (not inherited from the
  Phase-4 gate matrix's OWN citation of the same code, which was current
  as of 2026-08-18 — re-verified fresh here, unchanged since).
- `Scene`'s handle/desc API pattern is read directly from `scene.h`
  this session; the "zero Environment/Skybox/IBL symbols exist" claim is
  grep-verified, not inferred.
- Filament's `evaluateIBL()`/`prefilteredRadiance()`/`specularDFG()`
  citations are from the SAME fetched `surface_light_indirect.fs` file
  the T9 matrix already cites (not re-fetched independently — same
  session, same pinned commit) — read for the DIFFERENT purpose of
  runtime consumption shape here vs. bake-chain shape there.
- The Lambertian-under-uniform-environment closed form (`irradiance =
  L*PI`) is a standard, well-known radiometric identity, not itself
  independently re-derived or fetched from a citable source this
  session — flagged as textbook-level confidence, not primary-source-
  verified, consistent with how this project's OWN prior gate matrices
  (matrix-issue08) have flagged similar "standard convention, not
  separately spec-quoted" items.
- Task 4's exposure/pre-exposure ruling was NOT independently
  re-verified this session (it is Stage 0, outside this dispatch's
  ticket range) — this matrix's "cross-ticket dependency" row treats
  Task 4's plan text (`plan:232-254`, read in full earlier this session
  as part of general Stage 0/1 context-gathering) as the binding input,
  but the ACTUAL ruling only exists once Task 1's spec lands — flagged
  as a forward reference, not a verified-complete dependency.
