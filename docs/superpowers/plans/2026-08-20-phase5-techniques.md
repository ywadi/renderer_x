# Phase 5: Techniques — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Five stages — Foundations & Prerequisites, Core PBR + IBL, Lights + Shadows, Advanced Surfaces, Volumetrics + Post + Exit — delivering an advanced material & lighting renderer (the techniques-phase charter's priorities 1–8 in full, 9–11 as in-plan stretch), samples 10_lights / 11_surfaces / 12_bistro, the Bistro hero/benchmark scene, and release v0.5.0-phase5.

**Charter (binding):** the techniques-phase charter block in
`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`
(reference sources, shader architecture, real-transmission glass, clustered
Forward+, environment/indirect, shadows ladder, TRUE volumetrics, frame-pipeline
target, the 12-step priority order, Bistro commitment). The charter's priority
order is the plan's spine: **(1)** Filament-quality GGX PBR, **(2)** excellent
HDR IBL, **(3)** physical light units + clustered Forward+, **(4)** good shadow
filtering, **(5)** clearcoat + anisotropy, **(6)** real transmission/IOR/
thickness glass, **(7)** SSR + probe fallback, **(8)** LTC area lights,
**(9)** sheen/cloth, **(10)** iridescence + dispersion, **(11)** diffuse
transmission, **(12)** GI — with 9–11 explicitly stretch (see Task 34) and GI
staying deferred (existing layer-9 registry line).

**Spec:** authored by Stage 0 Task 1 as
`docs/superpowers/specs/2026-08-2x-phase5-techniques-design.md` (decision
D-series continues Phase 4's numbering convention). Until it lands, the
charter + this plan are the working truth; acceptance criteria below are
DIRECTIONAL and the Task 1 primary gate hardens them into binding matrices.

**Frame-pipeline target (charter):** depth → shadows → clustered light
assignment → opaque lighting → volumetrics (froxel march + apply) → SSR →
scene-color mip chain → glass/transmission → particles/transparency → bloom →
tone mapping (AgX/ACES, FG8 HDR output) → TAA. Tasks land pieces in stage
order; Task 36's hero scene runs the full pipeline.

**Architecture:** No new top-level libraries expected; growth lands in
existing modules — `src/rx_rhi_vk` + `src/rx_material` (compute pipelines),
`src/rx_scene` (lights, environment, camera exposure), `src/rx_shadow`
(cascades/atlas/PCSS), `src/rx_asset` (cubemap/HDR input), `shaders/material`
+ new `shaders/{ibl,cluster,volumetric,post}` module trees (composable Slang
modules: BRDF / StandardMaterial / ClearCoat / Sheen / Anisotropy /
Transmission / IBL / Lighting / Shadows). The spec may rule a new
`rx_techniques` module into existence if pass-orchestration code outgrows the
samples; that is a Task 1 decision, not an assumption.

**Ported-source additions (per the port-don't-reinvent rule; licenses recorded
at adoption):** Google **Filament** — canonical core-PBR/froxel/shadow/post
source; **port from its CURRENT `shaders/` implementation, never from its
documentation prose** (the June-2026 clearcoat doc discrepancy is resolved
correctly only in shader code). Khronos **glTF Sample Viewer** — material
vocabulary + reference-conformance source. NVIDIA **Falcor** —
how-to-express-it-in-Slang patterns ONLY (bundled NVIDIA SDKs like RTXDI/NRD/
DLSS carry their own licenses — not adopted without a separate license
decision). **LTC reference code** from the original authors (permissive).
**Amazon Lumberyard Bistro** (NVIDIA ORCA, CC-BY 4.0) — hero/benchmark scene.
New code dependencies beyond these ports are expected to be zero; any that
surface follow the standing pin+license+windows-cross rule.

## Global Constraints

- All Phase 1–4 global constraints remain: attribution ban; production grade
  (no stubs, no half-solutions); sync2/dynamic-rendering only; zero validation
  errors with sync validation active; both presets (`linux-native`,
  `windows-cross-zig`) build in every task; TDD; warm-up pattern for GPU test
  binaries; established style per directory; new dependencies/ports pinned
  with license + tag recorded in the vendoring commit and windows-cross
  verified in the SAME task; board mirror discipline (every dispatched
  round gets its ticket at dispatch time; coordinator moves cards).
- **Real-GPU verification (standing corrective, 2026-08-20): lavapipe-only
  verification is NOT verification.** Every GPU-facing task/round includes a
  real-driver run (default ICD, `--validate`, sustained) alongside the
  lavapipe suite; reports label each run's driver. Vendor-matrix note: the
  dev machine's NVIDIA GPU + the owner's Windows machine + the owner's Steam
  Deck ARE the vendor matrix until expanded — lavapipe forgives real-driver
  limits (descriptor pools were the proof), so real-driver rows are
  mandatory, and Deck/Windows human-hardware rows are tracked honestly in
  MANUAL_VERIFICATION, never silently assumed.
- **Reference-vs-ground-truth discipline (Phase 4 exit + Draco lessons):
  gates that bake a bug certify the bug.** Tests assert decoded/rendered
  VALUES against independent ground truth (analytic expectations, Filament/
  Khronos-sample-viewer references, CLI reference decoders) — never mere
  import/render success. Every new pixel gate ships with a discrimination
  proof (break the feature → gate fails, evidence pasted); every reference
  regeneration carries provenance (what changed, why the new image is more
  correct, verified against what ground truth).
- **Content-scale testing past declared capacities:** every capacity a task
  declares (light counts, froxel list lengths, atlas slots, descriptor
  counts, texture sizes, material counts) gets a test PAST it — behavior at
  capacity+1 is loud and defined, never corrupt. Real content (Sponza,
  Workshop, Bistro) exercises paths synthetic fixtures miss; "works on the
  committed fixture" is not "works".
- **Samples are pure consumers of engine facilities.** No sample hand-rolls
  what the engine provides (the Phase 4 pattern: descriptor arenas, per-FIF
  buffers, mouse capture, node transforms). Any facility a Phase 5 sample
  needs that the engine lacks is an API gap: promote it into the engine in
  the same task (or record an explicit ruling why sample-local stands).
  Task 5 clears the inherited backlog; the rule then binds every new task.
- **Performance is an exit criterion (CLAUDE.md):** measured claims only
  (Tracy/counters, driver-labeled); fast-path-as-default designs (no
  per-object state churn; retrofit-later designs rejected at review);
  numbers published per stage checkpoint; phase exits with published desktop
  AND Steam Deck benchmark numbers and CI perf regression gates on the
  stress/exit samples. Features above the Vulkan 1.3 baseline (HDR display
  output, etc.) are optional capabilities with a fallback engineered to the
  same performance bar.
- **No deferred fixes (standing directive, 2026-08-18):** every finding of
  every severity closes in-round. Only FEATURE phase-fits (registry) and
  genuine human-hardware MANUAL_VERIFICATION rows are legitimate deferrals.
- Threading contract (D5) binds all new code; GPU-object mutation
  main-thread-only unless a spec decision says otherwise; every new public
  header states its thread affinity in one line. Abandon/teardown paths get
  real-GPU-resource tests by default (Phase 4 durable lesson).
- Compute passes go through the render graph's compute-class pass machinery
  (barriers derive from attachment-free signatures — delivered Phase 3);
  no hand-rolled dispatch outside the graph in production paths.

## Deliberately deferred BY THIS PLAN (recorded, not dropped)

- **GPU particles / compute-driven simulation** (registry, techniques-phase
  fit): stays in the registry. Its FG3 CPU-spawn-contract dependency is
  unmet until the SDK phase designs the dynamic-content contract, and it is
  absent from the charter's binding priority list. Phase 5's compute
  capability (Task 2) makes it purely additive later; the frame-pipeline
  "particles/transparency" slot is exercised by existing transparency.
- **Visibility-buffer shading decision:** charter/registry sequence it AFTER
  meshlets exist — meshlets are Phase 6 (geometry). The decision moves there.
- **EVSM:** the charter's own text places it "later as the scalable
  alternative" to PCSS — registry note rides Task 17's close.
- **Volumetrics tier (c)** (local fog volumes / height fog): in scope if the
  Stage 4 schedule permits (it rides the same froxel grid as tier (b)); else
  registry-deferred at the Stage 4 checkpoint by this plan's own text.
- **Priorities 9–11** (sheen, iridescence+dispersion, diffuse transmission):
  in-plan stretch (Task 34); any not landed move to the registry with
  sources noted. **GI (priority 12) stays deferred** (existing registry line).
- **Hardware upscaling wrappers (DLSS/FSR/XeSS), motion blur:** layer-9
  registry cluster items outside the charter's priority order; TAA (Task 33)
  lands the shared velocity/history infrastructure they consume later.

---

## STAGE 0 — Foundations & Prerequisites

### Task 1 (PRIMARY GATE): Phase 5 design spec + ticket completeness research & hardening

Per the Phase 4 Task-9 "primary gate" pattern (user-mandated there; standing
here): before ANY other Phase 5 implementation dispatches, (a) the coordinator
authors the Phase 5 design spec from the charter (decision D-series: HDR
working format, MSAA ruling input, SH-vs-irradiance-cubemap, point-shadow
cubemap-vs-dual-paraboloid, depth-prepass policy, AgX-vs-ACES default,
volumetrics priority slot, module layout, `rx_techniques` yes/no), and (b)
research agents produce, per ticket in this plan, a completeness matrix —
[required feature] × [first-tier precedent: Filament / glTF Sample Viewer /
Falcor / Unreal / Godot / the relevant Khronos extension spec in full] ×
[consume-now / preserve-later / log-don't-drop / genuinely-N/A] × [does the
named port source actually contain it, cited at the pinned version]. The
coordinator rewrites each ticket with binding acceptance criteria from the
matrices; newly surfaced gaps go to the feature-gap register with phase fits.
Every claim cited; port-source capability verified against the actual
Filament/sample-viewer code, never assumed.

**Files:** `docs/superpowers/specs/2026-08-2x-phase5-techniques-design.md`
(coordinator-authored); `.superpowers/sdd/2026-08-20-phase5-techniques/gate/`
(matrices + rulings file); board tickets cut from this plan.
**Acceptance sketch:**
- Every Phase 5 ticket carries exhaustive, citation-grounded acceptance
  criteria (matrix + binding rulings, committed) before Task 2 dispatches.
- The spec records every decision this plan defers to it (list above), each
  with precedent citations and a Deck-floor viability note.
- Filament/sample-viewer/LTC port licenses recorded; the exact Filament
  commit to port from is pinned in the rulings.
- Charter conformance check: every charter bullet maps to a task or a
  recorded deferral — no silent drops.
**Steps:** research dispatch (parallel, read-only, disjoint matrices) →
coordinator adjudication + spec authoring → ticket rewrites + board cut →
gate closure recorded in ledger.

### Task 2: Compute pipeline capability (registry pull-forward)

The repo contains ZERO `vkCreateComputePipelines`/`vkCmdDispatch` (verified
2026-08-18); MaterialSystem is vertex+fragment-only by construction and
`getPipeline()` rejects attachment-free PassSignatures. This task delivers the
committed compute capability pulled forward from the geometry phase: compute
PSO creation + dispatch API, Slang compute entry-point compilation +
reflection through the existing `rx_shader` path, and lifting the
attachment-free-signature rejection so the render graph's already-delivered
compute-class passes become executable. This is thin RHI surface over
volk/VMA/the existing pipeline cache — no ready-made library applies beyond
what is already adopted (explicit from-scratch call, recorded per CLAUDE.md).

**Files:** `src/rx_rhi_vk` (compute PSO create/cache path),
`src/rx_material/material_system.cpp` + headers (attachment-free signature
acceptance; compute-capable pipeline resolution — spec decides whether via
MaterialSystem or a parallel `ComputePipeline` path), `src/rx_shader`
(compute-stage reflection already flows through Slang — verify + test),
`src/rx_graph` (compute pass execution wiring if any gap remains), tests
(+ `shaders/tests/*.slang` compute fixtures).
**Acceptance sketch:**
- A Slang compute shader compiles + reflects through the existing path; its
  PSO is created with a reflection-derived layout and cached in the existing
  VkPipelineCache.
- GPU test: graph compute pass dispatches a storage-buffer AND a
  storage-image write; a downstream pass reads both; readback asserts exact
  VALUES (not just execution); barriers derived by the graph, zero
  validation errors with sync validation, lavapipe + real driver.
- Attachment-free PassSignature accepted end-to-end; the old rejection path's
  test updated, not deleted.
- Bindless set 0 accessible from compute (the RT-compatibility rationale).
**Steps:** failing GPU tests → implement → both presets + real-driver run →
commit.

### Task 3: HDR scene-color infrastructure + MSAA policy decision (FG6)

Formalize the scene-color convention the whole phase renders through: opaque
lighting targets a named HDR working image (format per spec — B10G11R11 vs
RGBA16F ruling), tonemap consumes it, and the graph treats "scene color" as
the documented seam later tasks (mip chain, SSR, transmission, bloom, TAA)
attach to. Decide and implement the FG6 MSAA policy in the same task —
resolve-attachment semantics in the graph, or a recorded no-MSAA/TAA-first
ruling — BEFORE aliasing/history semantics ossify further (the FG6 rationale).

**Files:** `src/rx_graph` (resolve-attachment semantics if ruled in; scene-
color conventions doc), sample forward/tonemap pass declarations
(`samples/08_gltf_viewer`, `samples/09_scene`), `shaders/multipass`/
`shaders/material` tonemap entry, spec FG6 ruling section, tests.
**Acceptance sketch:**
- HDR intermediate proven by VALUE: a >1.0 radiance input survives to the
  tonemap input (readback assertion), and the chosen format's precision
  characteristics are documented with a test pinning the format.
- FG6 ruling recorded with resolve semantics implemented+tested, or the
  rejection recorded with rationale and the graph's assumption documented.
- Existing sample pixel gates regenerated with provenance + discrimination
  floors intact; zero validation errors both drivers.
**Steps:** ruling from Task 1 spec → failing tests → implement → regen gates
with provenance → both presets + real driver → commit.

### Task 4: Camera exposure + physical-units API

Land Filament's camera-owned exposure model (port the formulas from Filament
source): aperture/shutter/ISO → EV100 → exposure, plus direct `setExposure`
overrides, replacing Phase 4's manual tonemap-side `--exposure` (D22's
recorded successor; registry: "camera exposure API, techniques phase, with
FG1 IBL/physical light units"). Establish the pre-exposure convention Stage 1
IBL and Stage 2 physical light units are authored against (Filament
pre-exposes lights to keep half-float ranges safe — the spec rules whether we
adopt pre-exposure or full-float; the ruling binds Stages 1–2).

**Files:** `src/rx_scene/include/rx_scene/camera.h` + impl (exposure state +
EV100 helpers), tonemap shader/entry (exposure application point),
samples' `--exposure` migration, tests.
**Acceptance sketch:**
- EV100/exposure math matches Filament reference values (unit tests against
  ported formulas, cited to the Filament source location).
- Neutral-value regression guard: default exposure reproduces Phase 4 output
  byte-identically on existing gates.
- Exposure applied exactly once, pre-tonemap, at a documented pipeline point;
  `--exposure` flags migrate with behavior preserved.
**Steps:** device-free math tests → implement → gate regression run → both
presets + real driver → commit.

### Task 5: Material-path API-gap audit CLOSURE (sample-driven)

The owner's standing concern, folded in where the material path gets reworked
anyway. Sweep samples 07–09 for hand-rolled engine facilities and close the
inherited backlog: `createMaterialParamArena` duplicated across 08/09 →
engine-owned demand-sized arena API; the per-FIF draw-data buffer pattern
(exit-review fix I1, hand-built in both exit samples) → engine helper;
`samples/09_scene/{mouse_capture.h,fly_camera.h,grid_layout.h}` → promote
into `rx_platform`/`rx_scene` (or record an explicit per-item ruling why
sample-local stands); sample-recorder worker-side per-frame vector
allocations (`splitByBlockAndGroup`/`resolveDrawGroups` — Phase 4 exit-review
registry item (b)) → zero-alloc recorder helpers in `rx_scene`. Output
includes the audit table itself so the promote/rule disposition of every
hand-roll is recorded, closing the "sample-driven API-gap audit" owner
decision.

**Files:** `src/rx_material` (param-arena API), `src/rx_scene` (recorder
helpers, fly-camera/grid-transform facilities per ruling), `src/rx_platform`
(mouse-capture facility per ruling), `samples/07..09` (hand-rolls deleted,
promoted APIs consumed), audit table committed to the SDD workspace, tests.
**Acceptance sketch:**
- Audit table enumerates every sample-side hand-roll with promote/rule
  disposition; zero undispositioned rows.
- Promoted APIs consumed by the samples; the sample-local copies are GONE
  (grep-enforced), behavior byte-identical on existing pixel gates.
- Zero-alloc discipline extended to the sample recording path
  (capacity-snapshot test per the Task-23 methodology).
**Steps:** audit sweep → per-item failing tests → promote/implement → gates
byte-identical → both presets + real driver → commit(s).

### Task 6: Cubemap/array KTX2 loading + HDR image input (FG1 rider)

The TextureCache-side work FG1's skybox/IBL consumer needs (registry:
"cubemap/array KTX2 loading, techniques phase, riding FG1" — Phase 4 ships
the log path only): KTX2 cubemap/array/full-mip-chain load + upload through
the existing chunked-staging path (verify the #32 chunking against 6-face mip
chains), plus equirectangular HDR input for Stage 1's environment pipeline
(Radiance `.hdr` via the existing stb path — library-first; float formats
through TextureCache with a new environment role). libktx stays at the
v4.4.2 pin (v5/UASTC-HDR remains a registry watch item).

**Files:** `src/rx_asset/texture_cache.{h,cpp}` (+role table),
`src/rx_rhi_vk` uploader only if a cube/array gap surfaces (additive),
committed tiny cubemap/HDR fixtures (documented `toktx` commands), tests.
**Acceptance sketch:**
- Committed KTX2 cubemap loads; GPU test samples all 6 faces + a lower mip
  and asserts per-face VALUES (decoded-value discipline).
- Equirect HDR loads as float; a >1.0 texel survives to a sampled readback.
- Non-cube/array behavior byte-identical; role-appropriate fallbacks; loads
  respect the byte-source abstraction (no filesystem in the load path —
  grep-enforced, Phase 4 invariant).
**Steps:** fixtures → failing tests → implement → both presets + real driver
→ commit.

---

## STAGE 1 — Core PBR + IBL (charter priorities 1–2)

### Task 7: Filament BRDF module port (Slang)

Port the core BRDF from Filament's CURRENT `shaders/` (pinned commit from
Task 1's rulings; Apache-2.0 recorded): D_GGX, height-correlated
Smith visibility, Schlick Fresnel, and — the charter's explicit bar above
"basic GGX" — **energy compensation for single-scattering** (DFG-based
multi-scattering, matters on rough metals), plus the diffuse lobe
(Lambert/Burley per spec ruling). Organized as the charter's composable Slang
module architecture (`shaders/material/brdf.slang` and friends) with the
lobe structure diffuse + specular(GGX/Smith+Fresnel) prepared for
clearcoat(GGX) growth; GLSL→Slang translation follows Falcor's expression
patterns where idioms differ.

**Files:** `shaders/material/brdf.slang` (+ module siblings per the spec's
layout ruling), `shaders/material/material.slang` integration seams, tests
(compute-based numerical harness — a Task 2 consumer).
**Acceptance sketch:**
- Port-parity tests: BRDF term values match reference values computed from
  the pinned Filament shader formulas at a table of (NoV, NoL, roughness,
  F0) points (compute-shader evaluation, exact-tolerance asserts).
- White-furnace energy test: with energy compensation ON, integrated
  response at F0=1 ≈ 1 across roughness; without it, the deficit is
  measurable (discrimination — proves compensation is live).
- Modules compile standalone through the existing Slang path; existing
  material gates unaffected.
**Steps:** compute harness + failing parity tests → port → both presets +
real driver → commit.

### Task 8: StandardPBR rework onto the module architecture

Rebuild `standard_pbr.slang` on Task 7's modules and grow the flagship
material toward the full glTF-extension parameter set (baseColor, metallic,
roughness, ior, specular, emissive + emissive strength now; clearcoat/
anisotropy/sheen/transmission/thickness/attenuation/dispersion/iridescence/
diffuseTransmission as declared-but-gated slots that later tasks light up),
with feature permutation via the existing specialization-bit system / Slang
generics (D28 axis + Phase-3 D8 variant machinery) so materials only pay for
the features they use. KHR_materials_ior, KHR_materials_specular, and
KHR_materials_emissive_strength consume here (cheap scalar extensions already
parsed/preserved by the Phase 4 importer — no importer rework, per charter).

**Files:** `shaders/material/{standard_pbr.slang,forward_entry.slang,
material.slang}`, `src/rx_material` (specialization axes growth),
`src/rx_asset` material parameter resolution (ior/specular/emissive-strength
consume-now), tests.
**Acceptance sketch:**
- Variant discrimination: a material using no optional feature produces
  SPIR-V free of that feature's code path (spec-constant/generic dead-code
  proof), and pays no measured cost vs the Phase 4 baseline.
- ior/specular/emissive_strength consumed with value-asserted GPU tests
  (e.g., emissive_strength 4.0 → 4× radiance probe pre-tonemap).
- Existing 08/09 gates regenerated with provenance + discrimination floors;
  energy conservation preserved (Task 7 harness re-run on the composed
  material).
**Steps:** failing variant/value tests → rework → regen gates with provenance
→ both presets + real driver → commit.

### Task 9: Environment pipeline — equirect→cubemap, SH irradiance, prefiltered specular, BRDF LUT (compute)

The compute-based IBL bake chain, first production consumer of Task 2:
equirect→cubemap conversion; diffuse irradiance (SH9 or irradiance cubemap —
spec ruling; Filament `cmgen` is the port source either way); prefiltered
specular cubemap with GGX importance-sampled roughness-per-mip; DFG
BRDF-integration LUT. Runs at load time on the GPU through render-graph
compute passes; results cached per environment. Offline baking + derived-data
cache belong to Phase 7's layer-10 tooling (registry pointer recorded here —
runtime generation stands for Phase 5, matching the committed inventory).

**Files:** `shaders/ibl/*.slang` (ported kernels), `src/rx_scene` or
`src/rx_asset` environment-build orchestration (spec rules the owner),
tests (+ tiny committed HDR fixtures).
**Acceptance sketch:**
- SH/irradiance VALUES asserted against analytic ground truth (uniform
  white environment → known coefficients/irradiance; directional impulse →
  known lobe).
- Prefiltered chain: mip-0 ≈ source; highest-roughness mip ≈ irradiance
  (value probes); DFG LUT spot values match published Karis/Filament table
  points.
- Full chain executes as graph compute passes, zero validation errors both
  drivers; bake timings measured and published.
**Steps:** failing value tests per kernel → port kernels → both presets +
real driver → publish timings → commit.

### Task 10: IBL runtime integration + skybox (FG1 closure)

Wire environments into rendering: a Scene-level environment binding (skybox
pass sampling the cubemap; IBL diffuse + specular terms feeding every lobe of
the lit path per the charter — "each lobe fed by both direct lighting and
IBL"), replacing the Phase 4 interim flat ambient term (FG1's second half),
exposure-aware via Task 4. Environment intensity in physical units.

**Files:** `shaders/material/{forward_entry.slang + ibl consumption}`,
`shaders/ibl/skybox.slang`, `src/rx_scene` (environment API),
`samples/08_gltf_viewer` (`--env <path.hdr>`), tests.
**Acceptance sketch:**
- Discrimination against the old ambient: the regenerated references must
  FAIL against the Phase 4 flat-ambient renderer (gate-flip evidence — the
  reference-vs-ground-truth rule applied to our own upgrade).
- Mirror-metal sphere under a known environment reproduces the environment
  (matched-pose value probes); rough-metal energy sane via the furnace test
  on the full path.
- Skybox pass gated (reference + provenance); sponza/workshop sustained
  real-driver runs clean.
**Steps:** failing tests → implement → regen + discrimination proofs → both
presets + real driver → commit.

### Task 11: glTF PBR conformance harness vs Khronos Sample Viewer

Stand up the conformance discipline the charter names: fetch Khronos
glTF-Sample-Assets conformance models (MetalRoughSpheres, EnvironmentTest,
EmissiveStrengthTest, TextureTransformTest, … — per-model licenses recorded
in fetch manifest), generate ground-truth reference renders from the Khronos
glTF Sample Viewer under matched camera/environment (generation procedure
scripted/documented + committed — the exact mechanism is a Task 1 gate
question), and gate our renderer against them with tolerance comparisons on
both drivers. Failures are findings to fix, never tolerance widenings.

**Files:** `tools/fetch_assets.sh` growth (sample-asset models + checksums),
`tools/` reference-generation procedure, `tests/` conformance gate suite,
committed references + provenance.
**Acceptance sketch:**
- ≥6 conformance models gated at Stage 1 close (core PBR + emissive +
  texture-transform set); the suite grows in Stages 2–4 as features land.
- MetalRoughSpheres within ruled tolerance on real driver; per-model
  discrimination proof (perturb roughness constant → gate fails).
- Ground-truth provenance (viewer version, camera, env, settings) committed
  next to every reference.
**Steps:** fetch + reference generation → gate harness → wire models →
discrimination proofs → both presets + real driver → commit.

### Task 12: Stage 1 exit — viewer upgrade + checkpoint numbers

`08_gltf_viewer` becomes the Stage 1 demonstrator using engine facilities
only: environment switching (`--env`), Task 4 exposure controls, HUD
environment/exposure readout; packaging/CI updated; stage checkpoint per the
Phase 4 pattern (suite green both presets + real driver, sample packaged +
standalone-verified, numbers in ledger: bake timings, frame times
helmet/sponza/workshop, driver-labeled).

**Files:** `samples/08_gltf_viewer`, `tools/package_samples.sh`, CI, README/
MANUAL_VERIFICATION rows, ledger.
**Acceptance sketch:**
- Viewer consumes only engine APIs (audit row per the Task 5 rule).
- Headless gates green incl. new env path; packaged zip standalone-verified.
- Stage numbers published (desktop, driver-labeled; Deck rows tracked).
**Steps:** implement → gates → package → numbers → commit.

---

## STAGE 2 — Lights + Shadows (charter priorities 3–4)

### Task 13: Physical light units + punctual lights (KHR_lights_punctual consumption)

Grow `rx_scene` lights to the charter's set with physical units per
Filament's model: directional in lux (exists), point/spot in lumens/candela
with inverse-square attenuation + radius windowing, spot inner/outer cone
semantics; consume KHR_lights_punctual at import (parsed + preserved since
Phase 4 — turns on here, per charter). Pre-exposure/range policy per Task 4's
ruling.

**Files:** `src/rx_scene` (light descs/SoA managers), `src/rx_asset`
(punctual-light consumption into ImportedScene), `shaders/material` direct-
lighting units, tests.
**Acceptance sketch:**
- Unit math matches Filament reference formulas (candela/lumen conversions,
  attenuation window — cited unit tests).
- Authored glTF punctual lights arrive in the Scene with value-asserted
  intensities/cones (decoded-value discipline).
- Single-light analytic falloff probe: rendered intensity at distance d
  matches inverse-square expectation within tolerance.
**Steps:** device-free unit tests → import consumption test → GPU falloff
probe → implement → both presets + real driver → commit.

### Task 14: Froxel grid + clustered light assignment (compute; Filament port)

The clustered Forward+ core: camera-frustum froxel grid + per-froxel light
lists built in compute, ported GLSL→Slang from Filament's published
froxelizer. The grid is explicitly designed as SHARED infrastructure — the
charter commits volumetrics (Task 30) to riding the SAME grid, so the grid's
layout/bindings are authored for two consumers from day one (a Task 1 spec
decision records the shared shape).

**Files:** `shaders/cluster/*.slang` (ported), `src/rx_scene` froxel/cluster
orchestration (graph compute passes), tests.
**Acceptance sketch:**
- Device-free froxel math: index↔slice round-trips, depth-slice
  distribution matches the ported reference's formula.
- GPU test: synthetic light sets → readback of per-froxel lists asserts
  EXACT membership for hand-computed cases (corner lights, spanning lights,
  behind-camera culls).
- Capacity+1 behavior loud and defined (max lights per froxel / total —
  content-scale rule); counters exact and CI-gateable.
**Steps:** failing math tests → GPU membership tests → port → both presets +
real driver → commit.

### Task 15: Clustered shading integration + frame-pipeline adoption

The lit path consumes cluster lists for point/spot (directional stays
direct), targeting hundreds-to-thousands of local lights; the scene path
adopts the charter frame-pipeline spine this stage needs (depth prepass
policy per the Task 1 ruling; shadows → cluster assignment → opaque lighting
order).

**Files:** `shaders/material/forward_entry.slang` (+lighting module),
sample scene-path pass graphs, `src/rx_scene`, tests.
**Acceptance sketch:**
- Clustered-vs-unclustered equivalence: an N-light scene renders within
  tolerance of a brute-force all-lights reference path (discrimination:
  clustering changes cost, never the image).
- Scaling numbers published: 100 / 1k / 5k synthetic lights, desktop
  driver-labeled (the "suddenly scales" claim measured, not asserted).
- Zero validation errors incl. sync validation on the new pass chain, both
  drivers.
**Steps:** equivalence harness first → integrate → measure/publish → both
presets + real driver → commit.

### Task 16: Cascaded shadow maps (sun)

Extend the Phase 4 shadow bridge (`src/rx_shadow`, D21/D29 seams: dual depth
conventions, texel snapping, caster culling via `buildShadow`) to real CSM:
3–4 cascades, stable per-cascade fit + snapping, cascade selection with
seam blending, per-cascade caster culling, resolution/format policy tiers
desktop/Deck (registry: the cascades-phase spec inherits Phase 4's
parameterized 1024/D32_SFLOAT default — tiers become explicit spec'd policy
here).

**Files:** `src/rx_shadow` (cascade fit/selection), `shaders/shadow` +
`shaders/material` cascade sampling, `src/rx_scene` shadow list growth,
tests.
**Acceptance sketch:**
- Cascade-boundary continuity probe: shadow test values across a boundary
  differ within tolerance (no visible seam), with a discrimination variant
  (blending off → probe fails).
- Two-position shimmer test extends to all cascades (snapping holds under
  camera translation).
- Per-cascade caster counters exact; sponza sustained real-driver run with
  cascades live, zero validation errors.
**Steps:** failing probes → implement → both presets + real driver → commit.

### Task 17: PCSS filtering (first-quality filter)

The charter's named first-quality filter: PCSS (blocker search → penumbra
estimation → variable PCF disc), ported from Filament's shadow-filtering
implementations, layered over the existing hardware-PCF path which remains
the quality-ladder fallback (Deck tier per the Task 16 policy). EVSM stays
deferred by the charter's own text (registry note at close).

**Files:** `shaders/shadow`/`shaders/material` shadow filter modules,
`src/rx_shadow` filter selection plumbing, tests.
**Acceptance sketch:**
- Penumbra-width monotonicity probe: measured penumbra widens with
  occluder-receiver distance (the visible-varying-penumbra property, value-
  measured at ≥3 distances).
- PCF fallback path byte-stable vs pre-task gates; filter selectable per
  quality tier.
- Cost measured + published for both filters at both policy tiers.
**Steps:** failing probes → port → measure → both presets + real driver →
commit.

### Task 18: Spot shadow atlas + point shadows

Per the charter ladder: spot lights render into a shadow ATLAS (allocator
over one atlas texture; slot policy per spec), point lights via cubemap or
dual-paraboloid atlas (Task 1 spec ruling decides — charter allows either;
the ruling cites Deck-floor cost). Shadowed spot/point integrate with the
clustered lists (per-light shadow indices).

**Files:** `src/rx_shadow` (atlas allocator, per-light-type paths),
`shaders/shadow` + sampling modules, `src/rx_scene`, tests.
**Acceptance sketch:**
- N spots share one atlas with correct per-light lookups (readback probes
  per light; two lights swapped → probes discriminate).
- Point-light shadows continuous across face/hemisphere seams (seam probe
  at a boundary direction).
- Atlas exhaustion past declared capacity → loud, defined degrade (never
  corrupt); counters exact.
**Steps:** failing probes → implement → capacity tests → both presets + real
driver → commit.

### Task 19: Screen-space contact shadows

The charter ladder's short-range term: screen-space ray-marched contact
shadows against the depth buffer (Filament's contact-shadows implementation
is the port source), closing the bias-induced contact gap CSM/PCSS leave.

**Files:** `shaders/shadow/contact.slang` (ported), lit-path integration,
tests.
**Acceptance sketch:**
- Contact-gap discrimination: a caster-receiver contact point shadowed with
  contact shadows ON, unshadowed (peter-panned) with them OFF — value probe
  pair.
- Cost measured; default on/off per spec ruling; toggle exposed to samples.
**Steps:** failing probe pair → port → measure → both presets + real driver
→ commit.

### Task 20: Stage 2 exit — sample 10_lights + checkpoint numbers

New sample `10_lights`: a night-scene demonstrator (sponza with authored
punctual lights + synthetic `--lights N` stress) exercising physical units,
clustered Forward+, CSM+PCSS sun, spot atlas, point shadows, contact
shadows; HUD shows light/froxel/shadow counters + quality-tier toggles;
headless counter gates + tolerance pixels; CI perf gate on the light-scaling
numbers; packaging; stage checkpoint numbers published (desktop,
driver-labeled; Deck rows tracked in MANUAL_VERIFICATION).

**Files:** `samples/10_lights` (+ shaders as needed via engine paths),
`tools/package_samples.sh`, CI, README/MANUAL_VERIFICATION, ledger.
**Acceptance sketch:**
- Sample consumes engine facilities only (Task 5 rule; audit row).
- Exact counter gates (lights assigned/culled, froxel occupancy, shadow
  casters) + pixel gate with discrimination floor.
- `--lights` scaling table published; CI perf gate wired on it.
**Steps:** TDD gate → implement → numbers → packaging/CI → commit(s).

---

## STAGE 3 — Advanced Surfaces (charter priorities 5–8)

### Task 21: Clearcoat + anisotropy

Port Filament's clearcoat (second GGX specular lobe with energy accounting —
**from the shader code, never the docs**: the June-2026 clearcoat doc
discrepancy is resolved correctly only there) and anisotropy (tangent-space
stretched GGX); consume KHR_materials_clearcoat + KHR_materials_anisotropy
(parsed since Phase 4). Both feed from direct light AND IBL per the charter's
lobe rule; both are specialization-gated (Task 8 slots light up).

**Files:** `shaders/material/{clear_coat.slang,anisotropy.slang}` + standard-
pbr integration, `src/rx_asset` parameter consumption, tests.
**Acceptance sketch:**
- Clearcoat port-parity vs the pinned Filament shader (value table incl.
  the energy-compensation interaction the docs got wrong — cited).
- Anisotropic highlight orientation probe: rotating the tangent rotates the
  measured highlight (discriminates sign/convention errors).
- Sample-viewer conformance models for both extensions gated via the
  Task 11 harness; variant-gating cost proof (unused → free).
**Steps:** failing parity/orientation tests → port → conformance gates →
both presets + real driver → commit.

### Task 22: Scene-color HDR mip chain

The opaque scene color renders into an HDR mip chain (Task 3's scene-color
seam grows mips; filter ported from Filament's mipmap/blur passes) —
transmission roughness selects the mip (sharp→blurred→frosted, Filament's
refractive-scatter model) and Stage 4's bloom reuses the same chain. Graph
integration respects history/transient rules (persistent-vs-transient class
per the Phase 4 sequencing constraint).

**Files:** `src/rx_graph` consumers/conventions as needed, `shaders/post/
mip_chain.slang` (ported), scene-path pass wiring, tests.
**Acceptance sketch:**
- Mip VALUES asserted: known test pattern → expected filtered values at
  mips 1..N (not just "chain exists").
- Chain built at the charter's pipeline point (after opaque+volumetrics,
  before transmission); zero validation errors both drivers.
- Cost measured at 1080p/1440p and published.
**Steps:** failing value tests → port/implement → measure → both presets +
real driver → commit.

### Task 23: Thin-surface transmission (real glass)

The charter's headline: **glass is REAL transmission, never alpha blending.**
Thin-surface mode (windows, spectacles): transmissive BTDF keeping the
Fresnel surface reflection, IOR/transmission/roughness/tint
(KHR_materials_transmission + ior), refraction sampling the scene color —
screen-space refraction first, environment/probe fallback on miss. Transmission
draws render in their charter pipeline slot (after the scene-color chain).

**Files:** `shaders/material/transmission.slang` + entry integration,
`src/rx_scene` transmission partition wiring, `src/rx_asset` consumption,
tests.
**Acceptance sketch:**
- Dual-lobe probe: a glass surface shows BOTH the transmitted (refracted)
  background and the Fresnel specular highlight (two value probes on one
  frame — discriminates against alpha-blend impostors).
- Refraction geometry probe: a known background feature appears at the
  IOR-predicted offset; miss regions provably fall back to the environment.
- Sample-viewer TransmissionTest models gated via Task 11.
**Steps:** failing probes → implement → conformance gates → both presets +
real driver → commit.

### Task 24: Thick-volume transmission + frosted glass

Thick-volume mode (bottles, liquids): thickness map + Beer–Lambert
absorption `T = exp(-σ·d)` via attenuationColor/attenuationDistance
(KHR_materials_volume, per the Khronos thickness-approximation design for
raster); frosted glass via transmission roughness → Task 22 mip selection.

**Files:** `shaders/material/transmission.slang` growth, `src/rx_asset`
volume-extension consumption, tests.
**Acceptance sketch:**
- Beer–Lambert VALUE test: doubling thickness squares the measured
  transmittance (analytic ground truth, per-channel via attenuationColor).
- Frosted monotonicity probe: increasing transmission roughness strictly
  increases measured blur (mip-selection discrimination).
- Sample-viewer DragonAttenuation / AttenuationTest gated via Task 11.
**Steps:** failing value tests → implement → conformance gates → both
presets + real driver → commit.

### Task 25: Transparency ordering — blendOrder tier + D27 partition revisit

The registry's translucency riders land where transparency is reworked:
per-primitive `blendOrder` populates the sort-key bits Phase 4 reserved
(documented layout + decode round-trip per the bgfx discipline), and the
Phase 4 exit-review item (a) closes — D27 pre-resolution currently re-fires
per blend-partition run; make resolve-once-per-distinct-key hold across
partitions (cost, not correctness — measured).

**Files:** `src/rx_scene/draw_list.{h,cpp}` (+ sort-key doc), `src/rx_asset`
blendOrder consumption, tests.
**Acceptance sketch:**
- blendOrder overrides depth within its documented tier (order test vs
  glTF/Filament semantics); determinism across `--threads` preserved
  (byte-identical lists).
- D27 resolve-count counter: interleaved-partition scene resolves each
  distinct key exactly once (test fails on pre-task code — discrimination).
- Sort-key layout doc + decode() round-trip updated for the new bits.
**Steps:** failing order/counter tests → implement → both presets + real
driver → commit.

### Task 26: SSR + probe fallback

Screen-space reflections (march strategy per spec ruling; Filament's SSR is
the port source) with the charter's fallback chain: SSR hit → scene color;
miss/off-screen → environment/probe (Task 10). Roughness-aware filtering via
the Task 22 chain; history-resource consumption (Phase 4 Task 1 machinery)
where the ported design reflects the previous frame; TAA interplay noted for
Task 33.

**Files:** `shaders/post/ssr.slang` (ported), scene-path wiring, tests.
**Acceptance sketch:**
- Mirror-plane alignment probe: a known feature's reflection appears at the
  analytically predicted pixel (value-asserted), on both drivers.
- Edge/miss discrimination: off-screen-reflection regions provably show the
  probe fallback, not garbage/stretch.
- Roughness-aware blur monotonicity; cost measured + published.
**Steps:** failing probes → port → measure → both presets + real driver →
commit.

### Task 27: LTC area lights

The charter's "suddenly looks AAA" feature: rect area lights (panels,
screens, softboxes) via Linearly Transformed Cosines — LTC LUTs + evaluation
from the original authors' reference code (permissive; license recorded).
Scene gains rect-light proxies; area lights participate in the froxel lists.

**Files:** `shaders/material/ltc.slang` + LUT assets, `src/rx_scene`
(RectLightDesc + clustered integration), tests.
**Acceptance sketch:**
- LUT fidelity: sampled LUT values match the reference tables at cited
  indices.
- Convergence probe: at low roughness the LTC result converges to the
  mirror reflection of the rect (analytic pose); at high roughness to the
  cosine-weighted solid-angle expectation (tolerance-band probes).
- One-sided/two-sided semantics tested; per-light cost measured.
**Steps:** failing LUT/convergence tests → port → both presets + real driver
→ commit.

### Task 28: Stage 3 exit — sample 11_surfaces + checkpoint numbers

New sample `11_surfaces`: a materials showcase — glass storefront vignette
(thin + thick + frosted), clearcoat/anisotropy spheres, LTC panel-lit set —
demonstrating priorities 5–8 together; HUD material/feature toggles;
headless gates; packaging/CI; stage numbers published.

**Files:** `samples/11_surfaces`, `tools/package_samples.sh`, CI, README/
MANUAL_VERIFICATION, ledger.
**Acceptance sketch:**
- Engine-facilities-only (Task 5 rule); pixel gate + discrimination floor;
  counter gates on transmission/SSR paths.
- Real-driver sustained run zero validation errors; numbers published
  (driver-labeled).
**Steps:** TDD gate → implement → numbers → packaging/CI → commit(s).

---

## STAGE 4 — Volumetrics + Post + Exit (charter volumetrics tiers a+b; FG8; stretch 9–11)

### Task 29: God rays — volumetrics tier (a)

The charter ladder's entry tier: screen-space radial god rays as a cheap
post pass (expressible against the Phase 3 graph today, per the charter).
Sun-position-driven radial march on the composited scene, applied before
tonemap.

**Files:** `shaders/volumetric/godrays.slang`, scene-path wiring, tests.
**Acceptance sketch:**
- Occluder-shaft discrimination probe: shafts appear only in
  sun-visible-through-occluder configurations (paired value probes).
- Degenerates defined: sun behind camera / off-screen → identity (byte-
  stable gate).
- Cost bounded + measured (post-pass budget per spec).
**Steps:** failing probes → implement → both presets + real driver → commit.

### Task 30: Froxel volume fog — volumetrics tier (b)

The "physical god rays" tier: froxel-marched participating media
(Frostbite/id-style) riding the SAME camera-frustum froxel grid as Task 14 —
per-froxel scattering/extinction accumulation fed by the clustered light
lists (shadowed sun + local lights), temporal reprojection for stability
(history resources), full-screen apply at the charter's pipeline point
(before transparency, so glass sees fog). Tier (c) — local fog volumes /
height fog on the same grid — lands here if schedule permits, else is
registry-deferred at the stage checkpoint BY THIS PLAN'S TEXT.

**Files:** `shaders/volumetric/{froxel_fog,apply}.slang`, `src/rx_scene`
volumetric orchestration, tests.
**Acceptance sketch:**
- Shadowed in-scattering discrimination: a light shaft appears exactly where
  the shadow map says the sun reaches the medium (probe pair vs an
  unshadowed control).
- Physical units: extinction/scattering in 1/m, analytic single-slab
  transmittance probe matches `exp(-σd)`.
- Temporal stability: two-frame variance bound under a static camera;
  reprojection off → bound fails (discrimination).
- Deck-tier cost target from the spec measured on desktop + carried to the
  Task 36 Deck rows.
**Steps:** failing probes → implement → measure → both presets + real driver
→ commit.

### Task 31: Bloom

Physically-based bloom (threshold-free downsample/upsample chain per the
Filament/COD-style reference; reuses or mirrors the Task 22 chain per spec
ruling) feeding the tonemapper.

**Files:** `shaders/post/bloom.slang`, post-stack wiring, tests.
**Acceptance sketch:**
- Energy bound: bloom never adds net energy beyond its documented weight
  (integrated-value probe).
- Firefly behavior measured (single hot pixel → bounded, stable spread);
  HDR input value test through the full chain.
- Cost measured at 1080p/1440p.
**Steps:** failing value tests → port/implement → both presets + real driver
→ commit.

### Task 32: Tone mapping (AgX/ACES) + FG8 HDR display output

The tonemap ladder the charter ties to FG8: AgX and ACES-class tonemappers
ported from reference implementations (Filament's ColorGrading/AgX path is
the port source; default per spec ruling), replacing the Phase 4 utility
tonemap; FG8's HDR display output — swapchain colorspace ladder (scRGB /
HDR10 where the surface supports it) as an OPTIONAL capability with the SDR
path as the engineered fallback per the optionality principle.

**Files:** `shaders/post/tonemap_*.slang`, `src/rx_rhi_vk` swapchain
colorspace ladder, sample flags, tests + MANUAL_VERIFICATION rows.
**Acceptance sketch:**
- Tonemapper curve VALUES match the reference implementation at a sampled
  input table (per-tonemapper unit tests, cited source).
- SDR path: all existing gates regenerated once with provenance; byte-stable
  thereafter.
- HDR swapchain: capability-queried, logged fallback, zero validation
  errors when active; HDR-display correctness is a genuine human-hardware
  MANUAL_VERIFICATION row (recorded honestly).
**Steps:** failing curve tests → port → colorspace ladder → regen with
provenance → both presets + real driver → commit.

### Task 33: TAA

Temporal anti-aliasing riding the seams Phase 4 pre-paid: the Camera's inert
jitter offset (`rx_scene/camera.h`), render-graph history resources, and
prev-frame transforms in the transform pools → per-pixel velocity buffer
(the motion-vector infrastructure the registry's post-processing cluster
builds ONCE — TAA is its first consumer; upscalers/motion blur consume it
later). Halton jitter sequence, history reprojection, neighborhood clamping
per the ported reference (Filament TAA).

**Files:** velocity-buffer pass (`shaders/post/velocity.slang` + scene-path
wiring), `shaders/post/taa.slang` (ported), `src/rx_scene` jitter
activation, tests.
**Acceptance sketch:**
- Convergence measured: static-scene edge-aliasing energy reduced vs the
  no-TAA reference by a spec'd factor (value metric, not eyeballs).
- Ghosting bounded: moving-object test with clamping ON vs OFF
  discriminates (clamp provably live).
- Jitter-off path byte-identical to pre-task gates; velocity VALUES asserted
  for a known-motion object (analytic pixel offset).
- Interplay verified: SSR + volumetric reprojection stable under jitter
  (their gates re-run with TAA on).
**Steps:** velocity probe tests → jitter+history wiring → port TAA → both
presets + real driver → commit.

### Task 34: Stretch tier — sheen (9), iridescence + dispersion (10), diffuse transmission (11)

EXPLICITLY stretch / registry-deferrable BY THIS PLAN'S OWN TEXT: after
Tasks 29–33 land, implement in charter priority order as the schedule
allows — sheen/cloth (Filament cloth model + KHR_materials_sheen),
iridescence (thin-film, KHR_materials_iridescence) + dispersion
(KHR_materials_dispersion, rides the Task 23/24 transmission path), diffuse
transmission (leaves/wax, KHR_materials_diffuse_transmission). Each is a
specialization-gated Task 8 slot lighting up with its Filament/sample-viewer
port source; any not landed at the Stage 4 checkpoint move to the registry
with sources noted — a recorded ruling, not a silent drop. GI (priority 12)
is NOT in this task under any schedule outcome.

**Files:** `shaders/material/{sheen,iridescence,transmission}.slang` growth,
`src/rx_asset` consumption per landed extension, tests.
**Acceptance sketch (per landed feature):**
- Port-parity vs the named source + sample-viewer conformance model gated
  via Task 11 (SheenChair / IridescenceLamp / DispersionTest /
  DiffuseTransmissionPlant class models).
- Variant discrimination: unused → zero cost (Task 8 proof re-run).
- Checkpoint ruling committed for any deferred remainder.
**Steps:** per feature: failing parity/conformance tests → port → both
presets + real driver → commit.

### Task 35: Bistro conversion + hero-scene curation

The committed one-time curated FBX/USD→glTF conversion of Amazon Lumberyard
Bistro (NVIDIA ORCA, CC-BY 4.0 — license + provenance recorded; no official
glTF exists). Conversion fidelity IS the task, per the charter and the
Phase 4 own-content-blindness lesson: alpha modes (masked foliage), normal-map
orientation, emissive signage (strength units), glass materials mapped to the
real transmission extensions (never alpha blending), punctual light
placement for the night rig; scripted/documented pipeline (Blender-based
where scriptable); asset distributed via `tools/fetch_assets.sh` (checksums;
too large to commit).

**Files:** `tools/fetch_assets.sh` + conversion scripts/docs,
`assets/test/ASSET-NOTES.md` provenance, material-disposition table in the
SDD workspace, tests (import gate).
**Acceptance sketch:**
- Imports clean under the Phase 4 gate rules: zero unknown-REQUIRED
  extensions, every material disposition (Bistro material → engine feature)
  tabled and committed.
- Visual ground truth: matched-pose comparisons against ORCA reference
  renders (never certified by import success — the sponza-texture lesson);
  alpha-mask foliage + normal orientation explicitly probed.
- Exterior + interior sustained real-driver runs, zero validation errors.
**Steps:** conversion pipeline → disposition table → import gate →
ground-truth comparison → both presets + real driver → commit.

### Task 36: Sample 12_bistro + phase benchmarks + release v0.5.0-phase5 (EXIT)

The exit task. New sample `12_bistro`: the hero showcase running the full
charter frame pipeline over Bistro — day rig (sun CSM+PCSS, volumetrics,
glass storefronts with real transmission, SSR) and night rig (emissive
signage, hundreds of punctual lights clustered, LTC panels, god rays) — with
a deterministic `--bench` camera path emitting per-frame CSV. Then the
phase-exit ladder per the Phase 4 pattern: whole-phase exit review (top-tier
model, cross-stage seams: cluster↔volumetric grid sharing, scene-color
chain↔transmission↔bloom ordering, TAA↔SSR↔volumetric history interplay,
threading-contract adherence in all compute paths), one fix wave (all
findings in-round), CI green, packaging, release.

**Files:** `samples/12_bistro`, `tools/package_samples.sh`, CI (perf
regression gates on the benchmark), README/roadmap/MANUAL_VERIFICATION,
registry layer-9 row tick, ledger; tag v0.5.0-phase5.
**Acceptance sketch:**
- Sample consumes engine facilities only; headless gate (counters + pixels
  + discrimination floor); packaged zips standalone-verified both presets.
- **Published benchmark numbers, desktop AND Steam Deck** (CLAUDE.md exit
  criterion): `--bench` CSV rows for both rigs, driver/hardware-labeled;
  the Deck run is an owner-executed scripted procedure (one command) whose
  published rows BLOCK the release — not an unchecked box.
- CI perf regression gates wired on the benchmark numbers alongside the
  correctness gates (a regression blocks phase exit like a failing test).
- Exit review EXIT-READY verdict + fix-wave closure recorded; registry
  layer-9 row annotated with delivered-vs-deferred precision; release
  v0.5.0-phase5 live with both packages + numbers.
**Steps:** TDD gate → sample + bench mode → desktop numbers → exit review →
fix wave → CI green → Deck numbers → tag + release → morning report.

---

## Execution notes (coordinator)

- **Sequencing:** Task 1 (gate) completes before ANY other task dispatches.
  Stage 0: T2→T3→T4 sequential (shared pipeline/tonemap seams); T5, T6
  parallelizable in worktrees after T2 (disjoint). Stage 1: T7→T8→T9→T10
  sequential (each consumes the last); T11 parallelizable after T10; T12
  closes the stage. Stage 2: T13→T14→T15 sequential; T16→T17 sequential;
  T18, T19 after T16; T20 closes. Stage 3: T21 independent; T22→T23→T24
  sequential; T25 after T23; T26 after T22; T27 independent; T28 closes.
  Stage 4: T29 independent; T30 after Stage 2's grid; T31→T32 after T22;
  T33 after T32; T34 strictly after T29–T33; T35 is asset-only and may
  START any time after Task 1 (long-pole conversion work, worktree/
  no-code); T36 last.
- **Models:** implementers/reviewers Sonnet by default; Haiku only for
  fully-specified mechanical micro-tasks; exit review + any audit-class
  work top-tier per the Phase 4 precedent; coordinator does not implement.
- **Stage checkpoints:** each stage ends with suite green both presets +
  real-driver sustained runs, stage sample packaged and run standalone,
  numbers recorded in the ledger (driver-labeled), board cards moved, then
  the next stage dispatches.
- **Phase exit criteria (binding, CLAUDE.md):** all stages checkpointed;
  whole-phase exit review EXIT-READY; CI green with perf regression gates
  active on the exit samples; published desktop AND Steam Deck benchmark
  numbers in the release; tag v0.5.0-phase5 + release with both packages;
  board fully mirrored Done.
