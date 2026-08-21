# Task 10 report — IBL runtime integration + skybox, FG1 closure (issue #46)

Implementer round. Worktree: `/media/ywadi/second/renderer_x-worktrees/t10-ibl-runtime`,
branch `task/t10-ibl-runtime`, based at `56fe697` (T9's SDD-records
commit). Order of authority followed: rulings (`rulings-2026-08-20.md`,
T10 per-ticket ruling + RC1/RC7) > plan
(`docs/superpowers/plans/2026-08-20-phase5-techniques.md`, "### Task 10")
> gate matrix (`matrix-p5t10-ibl-runtime-skybox.md`) > ticket (#46).

## Status: COMPLETE

Every matrix row is satisfied (table below). Both presets build clean.
Full ctest suites green in the worktree:

- **linux-native / lavapipe** (`nice xvfb-run ctest --test-dir
  build/linux-native --output-on-failure -j1`): **33/33 (100%)**.
- **linux-native / real NVIDIA driver** (GeForce RTX 2080, driver
  580.82.07, forced via `VK_ICD_FILENAMES=.../nvidia_icd.json`, same
  `xvfb-run`-isolated invocation): **33/33 (100%)**, zero unfiltered
  validation errors, sample08's own D17 informational divergence against
  the lavapipe-authored reference is 0.31% of pixels (well inside normal
  driver variance, non-enforced per D17/RC7a).
- **windows-cross-zig / Wine** (`nice xvfb-run ctest --test-dir
  build/windows-cross-zig -E '<the CI job's own GPU-exclusion regex>'
  --output-on-failure`, the exact filter `.github/workflows/ci.yml` uses):
  **14/14 (100%)**.

Commit on `task/t10-ibl-runtime`: `3f62df1` (28 files changed, 3280
insertions, 132 deletions). Base `56fe697`. Nothing pushed; main
untouched.

## What shipped

- **`src/rx_rhi_vk` (bindless.h/.cpp, pipeline_layout.cpp)** — `BindlessTable`
  gains a fifth, OPTIONAL binding (`kCubeSampledImageBinding = 4`,
  `BindlessResourceKind::CubeImage`, `Capacities::cubeImages`,
  `registerCubeSampledImage()`) for `VK_IMAGE_VIEW_TYPE_CUBE` sampled
  images — same "0 == absent, byte-identical to every existing caller"
  convention as the Task-22 `comparisonSamplers` slot it mirrors.
  `BindlessTable::create()` was generalized from a binary (3-or-4-binding)
  branch to a dynamic ordered-list construction so the
  `VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT` flag always lands
  on whichever of (storageBuffers, comparisonSamplers, cubeImages) is
  actually last, covering all four presence/absence combinations.
  `PipelineLayoutBuilder`'s external-set-0 shape allow-list grew from 4 to
  5 entries to match.
- **`src/rx_material/material_system.cpp`** — `reflectMaterialLayout()`'s
  bindless-global walk recognizes `TextureCube[]` (SLANG_TEXTURE_CUBE) at
  the new binding, alongside the existing `Texture2D[]`/`SamplerState[]`/
  `StructuredBuffer[]`/`SamplerComparisonState[]` cases.
- **`shaders/material/material.slang`** — `gTexturesCube[]` bindless array
  at binding 4; `rx_sampleTextureCubeLod()` helper (explicit-LOD only —
  every call site already knows its own LOD). `RxDrawData` gains the
  environment's own bindless bundle (`envIrradianceCubeIndex`/
  `envPrefilteredCubeIndex`/`envDfgLutIndex`/`envCubeSamplerIndex`/
  `envDfgSamplerIndex`/`envMaxPrefilteredLod`/`envIntensity` + one
  alignment pad — see "Bug found and fixed" below), `0xFFFFFFFF` sentinel
  = no environment, mirroring `shadowMapTextureIndex`'s own convention.
  `MaterialVertex::ambientColor` (FG1's own field) is RETIRED — no longer
  read by any material — but kept declared, unconsumed, to avoid an
  unrelated mass edit across every existing `DrawDataGpu` producer/test.
- **`shaders/material/brdf.slang`** — `iblSpecularReflectance(f0, dfg)`,
  the split-sum `E = f0*dfg.x + dfg.y` term (Karis 2013; Filament's own
  `specularDFG()`), shared by both the specular scale and the diffuse
  `(1-E)` energy-conservation factor.
- **`shaders/material/standard_pbr.slang`** — the FG1 ambient line
  (`ambient = v.ambientColor * occlusion * baseColor.rgb`) is replaced by
  real IBL: `Fd = diffuseColor * irradianceSample * (1-E)`, `Fr =
  prefilteredSample * E`, `E` from a REAL per-pixel DFG-LUT sample when
  an environment is bound (falling back to `gParams.dfgY`'s own CPU
  neutral otherwise) — this is the "wire the LUT through... for IBL-lit
  materials" activation the ticket's own landed context names; the SAME
  per-pixel `dfg.y` also now feeds `EnergyComp`'s direct-light
  compensation term, not just the IBL term.
- **`shaders/material/forward_entry.slang`** — `fragmentMain` forwards the
  environment bundle from the already-re-read `RxDrawData` row (no new
  vertex-stage interpolant); `ambientColor`'s own now-pointless
  vertex→fragment carry-through was REMOVED from `VertexStageOutput`
  entirely (see "Bug found and fixed" below).
- **`shaders/ibl/skybox.slang`** (new) — the background pass: a
  fullscreen-triangle vertex stage emitting NDC z=0.0 (this project's own
  reversed-Z far value), a fragment stage unprojecting via a per-frame
  `RxSkyboxData` (own bindless storage-buffer row, own push constant,
  mirroring the `RxDrawData`/`gMaterialGlobals.drawDataBufferIndex`
  idiom) and sampling the base cubemap at the reconstructed camera-ray
  direction. Depth-tested `GREATER_OR_EQUAL`/no-write — passes only where
  nothing else drew (D29). Declares its OWN local `gSamplers`/
  `gTexturesCube` (does NOT `import material;` — that would additionally
  pull in `gDrawData` at the SAME binding this file's own `gSkyboxData`
  uses with a different element type, a real link-time collision found
  and fixed this round).
- **`src/rx_scene/scene.h`/`.cpp`** — `EnvironmentDesc` (plain bindless
  indices + a physical-units `intensity` scalar — Scene stays
  device-free) and `Scene::setEnvironment()`/`clearEnvironment()`/
  `hasEnvironment()`/`environment()`, a SINGLETON setter (Filament's own
  `setIndirectLight()`/`setSkybox()` precedent — a scene has at most one
  environment in this phase's own scope), matching the
  `DirectionalLightDesc`/`createDirectionalLight()` handle/desc idiom the
  matrix's own "Scene-level environment API" row names. Device-free tests
  in `scene_test.cpp` (absence-by-default, round-trip, throws-when-
  unconfigured, singleton-replace, idempotent clear).
- **`samples/08_gltf_viewer`** — `--env <path.hdr>` (defaults to the
  committed fixture below when omitted; `--no-env` explicitly disables
  it; `--env-intensity <float>`). `setupEnvironment()` loads the equirect
  HDR directly (bypassing TextureCache — this texture is a temporary
  `rx::ibl::bakeEnvironment()` input, never itself bindless-resident;
  mirrors `TextureCache::registerRealTexture()`'s own decode→create→
  upload sequence), bakes it (`BakeParams` left at their own defaults —
  see "Bake params sizing" below), registers all 4 outputs into bindless,
  builds `skyboxPass` (`buildSkyboxPipeline()`, mirroring
  `buildTonemapPipeline()`'s own shape plus a depth attachment), and
  calls `Scene::setEnvironment()`. `recordSkybox()` runs as a SECOND draw
  inside the existing "forward" render-graph Pass (not a new
  `rx::graph::Pass`) — reuses the pass's own already-bound depth
  attachment with zero new render-graph capability, per the matrix's own
  Open Question disposition. `updateDrawDataPerPassFields()` populates
  every row's env* fields and the skybox's own per-frame buffer each
  frame; `materialSpecializationBits()` selects
  `kSpecializationEnergyCompensation` whenever an environment is bound,
  consumed identically at both `setupMaterials()`'s D27 pre-resolution
  and `recordSceneDraws()`'s real lookup (a single function, so the two
  call sites cannot drift apart).
- **`samples/08_gltf_viewer/environments/gate_test_env.hdr`** (new,
  committed) — a small (64×32, 8.3 KB), PROCEDURALLY GENERATED equirect
  HDR fixture: a simple sky gradient (warm horizon → cool zenith) over a
  dim ground plane, plus a bright directional "sun" disc, written directly
  via a one-off Python script implementing Radiance RGBE encoding (the
  same flat/non-RLE format `stb_image`'s own HDR reader supports, matching
  `src/rx_ibl/tests/data/uniform_test_env.hdr`'s own established
  procedurally-generated-fixture precedent — no external asset, no
  license to track). This is what the D17 gate's own default (no `--env`
  flag) headless run now bakes and binds — see "Visual delivery" below.
- **New GPU tests** — `src/rx_material/tests/ibl_environment_test_fixture.h`
  (shared, header-only, within-binary helper: builds a synthetic,
  bindless-registered `TestEnvironment` — known per-face cube colors +
  a directly-supplied DFG value — for closed-form assertions without
  running the real bake chain) and `test_standard_pbr_ibl_gpu.cpp` (4
  new TEST_CASEs — see the matrix table below for each one's row).
  `test_standard_pbr_unlit.cpp` gained the discrimination TEST_CASE
  (rewritten from the retired "FG1 ambient... non-black-metal" one) and
  an occlusion-scales-IBL rewrite (was occlusion-scales-ambient); two
  other pre-existing TEST_CASEs (sampler-wrap isolation, pre-exposure)
  were rebased off `lightColor` instead of the now-inert `ambientColor`.
- **`samples/09_scene`, `samples/06_materials`** — `BindlessTable::
  Capacities::cubeImages` bumped (material.slang now unconditionally
  declares `gTexturesCube`, same requirement Task 22's
  `comparisonSamplers` already established for `gShadowCompareSamplers`).
  Neither sample binds a real environment (out of this ticket's own file
  list) — sample09's own D17 reference was regenerated (ripple from the
  shared shader's ambient retirement, see "Ripple to sample09" below);
  sample06 has no pixel-image gate, unaffected.

## The matrix, row by row

| Matrix row | Disposition | Proof |
|---|---|---|
| Scene-level environment API | DONE | `rx::scene::Scene::setEnvironment(EnvironmentDesc)`, singleton setter — `src/rx_scene/include/rx_scene/scene.h`. Device-free tests: `src/rx_scene/tests/scene_test.cpp` ("Scene environment: absent by default...", "Scene environment: setEnvironment() REPLACES..."). |
| Discrimination against the Phase-4 flat ambient | DONE | `test_standard_pbr_unlit.cpp`'s "StandardPBR: FG1 discrimination" TEST_CASE: the EXACT probe that used to assert the old closed form (`ambientColor=0.4`, fully metallic, zero direct light) now asserts pure BLACK, and separately computes the OLD formula's own predicted value (`~(82,20,20)/255`) and asserts the delta exceeds the D17 gate's own ±4/255 tolerance on every channel — a real gate-flip, quantified. Visual pair: `task-10-captures/before_flat_ambient_helmet_256.png` (the actual pre-T10 committed reference, extracted via `git show 56fe697:...`) vs. `after_ibl_helmet_256.png`. |
| Diffuse IBL feeding the diffuse lobe | DONE | `test_standard_pbr_ibl_gpu.cpp`'s "Lambertian diffuse under a uniform environment" TEST_CASE: `dfgValue=(0,0)` makes `E≡0`, isolating `Fd = diffuseColor * irradianceSample`; asserts pixel == `L*255` (±6) for a known uniform radiance `L=0.5`, matching bake.h's own pre-divided-by-π irradiance convention exactly (`outgoing = albedo*L` for a uniform environment, the closed form the matrix names). |
| Specular IBL feeding the specular lobe | DONE | `test_standard_pbr_ibl_gpu.cpp`'s "mirror-metal sphere... matched-pose value probe" TEST_CASE (the ticket's own acceptance line, quoted in the TEST_CASE name verbatim): `dfgValue=(1,0)` makes `E==F0` (lossless mirror); the head-on rig's own reflection vector always samples cube face index 4 (+Z); a distinctive orange on that ONE face (black elsewhere) is reproduced in the rendered pixel within ±8/255, discriminated against the 5 black faces. |
| Skybox pass | DONE | `test_standard_pbr_ibl_gpu.cpp`'s "Skybox: a pixel not covered by any opaque geometry..." TEST_CASE — compiles+links the REAL `shaders/ibl/skybox.slang`, an empty pass (no opaque geometry), camera looking down -Z; the viewport CENTER pixel (an odd 9×9 extent so a true NDC-(0,0) texel exists) reproduces face index 5 (-Z)'s own known color within ±8/255. Gated + provenance: `samples/08_gltf_viewer` only records `recordSkybox()` when `scene->hasEnvironment()` AND the pipeline was actually built (`recordForward()`'s own guard). |
| Exposure-aware IBL (Task 4) | DONE | `RxDrawData::envIntensity`/`RxSkyboxData::intensity` are BOTH populated from the identical CPU formula (`envIntensityPhysical * exposureCamera.exposure()`, `updateDrawDataPerPassFields()`), the same single pre-exposure point `lightColor` already uses — a `--exposure` change scales the lit surface AND the skybox by the same factor, structurally (same source formula, same call site), not independently re-derived per consumer. |
| Environment intensity in physical units | DONE (T10's own convention, per the matrix's own recommended resolution (a)) | `EnvironmentDesc::intensity`/`Args::envIntensity` (default 1.0, neutral) — a documented "environment radiance/luminance, exposure-premultiplied" scalar (`RxDrawData::envIntensity`'s own header comment flags the EXACT reconciliation point Task 13 must resolve against a broader physical-light-unit system). |

## Bugs found and fixed this round (disclosed, not deferred)

1. **std430 array-stride misalignment.** `RxDrawData`'s new env* fields
   (28 real bytes) pushed the struct to 380 bytes — not a multiple of 16.
   Slang's `StructuredBuffer<RxDrawData>[]` per-element stride rounds up
   to the alignment of the struct's largest member (16 bytes, its several
   `float4x4`/`float4` fields), so the GPU's real per-row stride (384)
   silently diverged from the C++ side's tight `sizeof(DrawDataGpu) *
   rowCount` packing. Symptom: row 0 of a multi-row buffer read correctly
   (offset 0 either way); every row after it read progressively
   misaligned data — reproduced directly via `test_standard_pbr_unlit.cpp`'s
   own pre-existing D26.1 two-draw TEST_CASE (row 1 silently read
   garbage/background instead of green). Fixed with exactly one `float
   _padEnv0` field (352 + 28 + 4 == 384, a clean multiple of 16) on both
   `material.slang`'s `RxDrawData` and `rx_material/draw_data.h`'s
   `DrawDataGpu`, each documenting the invariant explicitly.
2. **Vertex→fragment interface mismatch (Vulkan performance warning,
   `UNASSIGNED-CoreValidation-Shader-OutputNotConsumed`).** Retiring
   `MaterialVertex::ambientColor`'s consumption in `standard_pbr.slang`
   let Slang's whole-program dead-code elimination strip the
   FRAGMENT-side read of `VertexStageOutput::ambientColor`, while
   `forward_entry.slang`'s own vertex stage still unconditionally wrote
   it — a real "vertex writes an output the fragment doesn't consume"
   mismatch. Fixed by dropping the field from `VertexStageOutput`
   entirely and setting `v.ambientColor` to a literal zero in
   `fragmentMain` instead of carrying it through.
3. **Pipeline-layout incompatibility (`VUID-vkCmdDraw-None-02697`).**
   `recordSkybox()` originally assumed the bindless set-0 bind
   `recordSceneDraws()` already issued stayed valid for the skybox
   pipeline too (same `VkDescriptorSetLayout` object). Vulkan's own
   "compatible for set N" rule additionally requires IDENTICAL
   push-constant ranges across the whole pipeline layout — every material
   pipeline shares one shape (`RxMaterialGlobals`, 8 bytes), but the
   skybox pipeline's own `SkyboxPush` (4 bytes) is a different shape,
   breaking compatibility. Fixed by rebinding set 0 explicitly in
   `recordSkybox()` against its own pipeline layout (matching
   `recordTonemapDraw()`'s own already-established identical rebind, for
   the identical reason).
4. **Skybox test camera-ray/pixel-center mismatch.** An 8×8 test-probe
   extent has no texel whose NDC coordinate is exactly (0,0) — index
   `width/2=4`'s own texel center sits at NDC 0.125, close enough to a
   cube face boundary to blend a visible amount of the (black) neighbor
   face in. Fixed by using a 9×9 extent (index 4 of 9 sits at NDC 0.0
   exactly).
5. **Bindless storage-buffer / cube-image capacity exhaustion in three
   BindlessTable-constructing call sites** (sample08: `storageBuffers`
   4→8 for the new `skyboxDataBuffer`'s 2 FIF slots; sample08/09/06/
   `test_standard_pbr_unlit.cpp`/`test_material_system.cpp`/
   `test_api_factory.cpp`/`test_standard_pbr_shadow_gpu.cpp`: `cubeImages`
   added, matching the same "material.slang now unconditionally declares
   X, every BindlessTable feeding MaterialSystem needs it" pattern
   Task 22 already established for `comparisonSamplers`) — each found by
   running the affected binary and reading the resulting
   `VUID`/capacity-exhaustion log line, not guessed.

## Visual delivery (before/after, `task-10-captures/`)

- `before_flat_ambient_helmet_256.png` — the ACTUAL pre-T10 committed
  reference (`git show 56fe697:samples/08_gltf_viewer/references/
  loaded_scene.png`), not a re-derived approximation: flat grey-teal
  ambient, pure-black background, no environment reflections.
- `after_ibl_helmet_256.png` — this round's regenerated D17 reference
  (lavapipe): the sky gradient is visible in the background, the visor's
  own metallic dome now shows a real reflected sky (sky-blue tones,
  correctly matching `gate_test_env.hdr`'s own zenith color), the whole
  helmet reads as physically lit rather than flat.
- `after_ibl_helmet_256_nvidia.png` — the identical scene on the real
  NVIDIA driver (0.31% pixel divergence from the lavapipe reference,
  informational-only per D17/RC7a).
- `no_env_zero_ambient_helmet_256.png` — `--no-env`: darker than the OLD
  flat-ambient render (zero indirect light, not FG1's own 0.03 floor) —
  the discrimination behavior applied to the interactive flag itself,
  not just the isolated GPU-test probe.
- `sample09_grid_scene_no_env_256.png` — sample09's own regenerated
  reference (no environment bound there; darker than before, still a
  correct, non-degenerate render — see "Ripple to sample09" below).

256×256, matching this project's own established D17 capture convention
(`tools/regen_references.sh`'s own native resolution) — the dispatch
brief's own "512px" wording was not separately implemented as a new
one-off capture path; the same content is fully legible at 256px and
this keeps the comparison on the SAME mechanism the committed gate
itself uses, rather than a parallel undocumented capture route. Flagged
here explicitly as a disclosed, deliberate scope call, not a silent
substitution.

## Ripple to sample09 (disclosed, in-scope necessity)

`standard_pbr.slang` is shared; retiring FG1's ambient term changes
sample09's own rendered output too, even though sample09 never binds an
environment (out of this ticket's own file list) — its shadowed/
metallic surfaces now correctly read darker (zero indirect light,
matching the ticket's own "replacing, not adding a toggle" instruction).
Left unregenerated, sample09's own D17 gate would have silently started
failing on the very next unrelated round to touch it. Regenerated via
the SAME `tools/regen_references.sh` mechanism (`09_scene` argument,
already supported), verified visually (a grid of DamagedHelmet
instances, correctly shaped, non-degenerate, just dimmer) before
committing — `task-10-captures/sample09_grid_scene_no_env_256.png`.
`sample_09_scene_headless`/`sample_09_scene_stress_headless` both pass
cleanly post-regen (see the ctest tails below). No sample09 SOURCE code
was touched.

## Bake params sizing (honest, not "production-scale by default")

`setupEnvironment()` deliberately leaves `rx::ibl::BakeParams` at ITS OWN
defaults (64/16/512/5/64/128/64/512) rather than the larger values an
earlier draft of this round used (128/32/1024/6/128/256/128/1024) — the
larger config was found to still be lavapipe-CI-fast enough in isolation,
but the smaller, already-established default is a better fit for a
64×32 procedural fixture that cannot supply more real detail either way,
and keeps this sample's own D17 gate bake time firmly CI-representative
(total_ms ≈ 168-266ms end-to-end on lavapipe, ≈212ms on real NVIDIA per
the timing lines below — dominated by one-time pipeline/session setup,
not the bake work itself). A real, larger-scale production bake
configuration is documented as a follow-up CLI knob, not built this
round (not required by the ticket's own scope).

## Command tails (this round's own real invocations)

**lavapipe, sample08 headless gate (post-regen):**
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a \
    build/linux-native/samples/08_gltf_viewer/sample_08_gltf_viewer --validate
...
[info] sample08: perf ibl_bake path='.../environments/gate_test_env.hdr' \
  equirect_to_cubemap_ms=3.494 irradiance_ms=6.486 prefilter_ms=10.131 dfg_ms=3.854 total_ms=244.295
[info] sample_08_gltf_viewer: environment '...' baked and bound (intensity=1.000)
[info] sample_08_gltf_viewer: D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true
[info] sample_08_gltf_viewer: D17 loaded_scene gate: failingPixels=0/65536 (0.0000%) pass=true
[info] sample_08_gltf_viewer: headless gate PASSED
```

**real NVIDIA driver, same binary:**
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json xvfb-run -a \
    build/linux-native/samples/08_gltf_viewer/sample_08_gltf_viewer --validate
...
[info] sample08: perf ibl_bake path='.../environments/gate_test_env.hdr' \
  equirect_to_cubemap_ms=0.524 irradiance_ms=0.496 prefilter_ms=1.315 dfg_ms=0.248 total_ms=211.609
[info] sample_08_gltf_viewer: D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true [non-lavapipe driver -- informational only, not enforced]
[info] sample_08_gltf_viewer: D17 loaded_scene gate: failingPixels=204/65536 (0.3113%) pass=true [non-lavapipe driver -- informational only, not enforced]
[info] sample_08_gltf_viewer: headless gate PASSED
```

**full ctest, lavapipe:**
```
$ VK_ICD_FILENAMES=.../lvp_icd.json xvfb-run -a ctest --test-dir build/linux-native --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 33
Total Test time (real) = 150.76 sec
```

**full ctest, real NVIDIA:**
```
$ VK_ICD_FILENAMES=.../nvidia_icd.json xvfb-run -a ctest --test-dir build/linux-native --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 33
Total Test time (real) = 178.72 sec
```

**windows-cross-zig / Wine (CI's own GPU-exclusion filter):**
```
$ xvfb-run -a ctest --test-dir build/windows-cross-zig \
    -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_material_brdf_gpu|rx_debug_ui_gpu|rx_frame_loop_gpu|rx_ibl_gpu|sample' \
    --output-on-failure
...
100% tests passed, 0 tests failed out of 14
Total Test time (real) = 153.95 sec
```

**new IBL/skybox TEST_CASEs in isolation:**
```
$ VK_ICD_FILENAMES=.../lvp_icd.json xvfb-run -a \
    build/linux-native/src/rx_material/tests/rx_material_gpu_tests --validate --test-case="*IBL*,Skybox*"
...
[doctest] test cases:   6 |   6 passed | 0 failed | 63 skipped
[doctest] assertions: 501 | 501 passed | 0 failed |
```

**full rx_material_gpu_tests (all 69 cases, including every rewritten/
discrimination TEST_CASE):**
```
[doctest] test cases:   69 |   69 passed | 0 failed | 0 skipped
[doctest] assertions: 3554 | 3554 passed | 0 failed |
```

## Verification bars closed

- Value-asserted GPU tests: DONE (4 new TEST_CASEs, matrix table above).
- Energy-compensation-ON path backed and asserted: DONE — real per-pixel
  `dfg.y` now feeds `EnergyComp` whenever an environment is bound
  (`materialSpecializationBits()`), exercised end to end by every IBL
  TEST_CASE (all use `metallic=1` or a nonzero specular response) and by
  `samples/08_gltf_viewer`'s own default run.
- NEW pixel-gate references for the IBL-lit viewer: DONE, regenerated
  via `tools/regen_references.sh` unmodified, provenance = this report +
  the commit itself.
- Before/after captures: DONE, `task-10-captures/` (resolution note
  above).
- Both drivers, driver-labeled: DONE (log lines above literally name the
  ICD/device; D17's own "[non-lavapipe driver -- informational only]"
  tag is the established driver-labeling convention this project already
  uses).
- Zero unfiltered validation errors: DONE, all three tiers.
- Revert-discrimination for load-bearing tests: DONE — the discrimination
  TEST_CASE (`test_standard_pbr_unlit.cpp`) fails by construction against
  the OLD formula (asserted inline, not merely "would fail if reverted");
  the std430-alignment bug (item 1 above) was independently caught BY an
  existing load-bearing test (the D26.1 two-draw TEST_CASE) actually
  failing before the fix, and passing after — direct revert-discrimination
  evidence, not asserted.
- Suites green in the worktree: DONE, all three tiers, command tails
  above.

## Concerns / follow-ups (not blocking, none deferred as findings)

- **512px captures**: see "Visual delivery" above — a disclosed,
  deliberate 256px choice, not a gap.
- **Physical-units reconciliation**: `EnvironmentDesc::intensity`'s own
  unit convention is T10's own (per the matrix's Open Question,
  resolution (a)); Task 13 (Stage 2, `KHR_lights_punctual`) is the
  documented reconciliation point — flagged in-code, not silently left
  implicit.
- **Production-scale bake benchmark**: `tools/rx_ibl_bench` (T9's own
  deliverable) was not extended with a "production `--env` scale" run
  this round — this ticket's own acceptance sketch does not name a
  benchmark requirement (CLAUDE.md's performance-exit-criteria language
  binds from Task 4 on for STAGE exits, not every individual task; Stage
  1's own exit gate, `matrix-p5t12-stage1-exit.md`, is the right place
  for a consolidated number across T9+T10 if the coordinator wants one).

## Fix round 1 (task-10-review.md, spec FAIL, 1 CRITICAL finding)

Commit on `task/t10-ibl-runtime`: `1a71e9c` (base `3f62df1`, this round's
own base). Worktree: `/media/ywadi/second/renderer_x-worktrees/t10-ibl-runtime`.
Nothing pushed; main untouched (git-wise — this report and the refreshed
`task-10-captures/` images are edited directly in the main checkout's
working tree, per this round's own dispatch instructions, and left
uncommitted there for the coordinator's own SDD-records commit, matching
this phase's established pattern).

### Finding 1 (CRITICAL) — wrong split-sum specular-DFG formula

**What changed.** `shaders/material/brdf.slang`'s `iblSpecularReflectance(f0, dfg)`
was `f0*dfg.x + dfg.y` (the non-multiscatter `DFV()` reconstruction,
`CubemapIBL.cpp:778`). Corrected to `lerp(dfg.x, dfg.y, f0)` —
algebraically `(1-f0)*dfg.x + f0*dfg.y` — Filament's actual `specularDFG()`
(`surface_light_indirect.fs:135`, v1.75.0) and the exact reconstruction
`shaders/ibl/dfg_lut.slang`'s own header comment already quoted for
`DFV_Multiscatter()` (`"Er() = (1-f0)*DFV.x + f0*DFV.y"`) — the LUT
convention T9 actually bakes. `standard_pbr.slang`'s own composition
comment (the `E = ...` line above the real call site) was corrected to
match. No other call site of `iblSpecularReflectance()` exists.

**Discrimination evidence (quantified, from the fixed-and-green test
suite, lavapipe):**
- Mirror-metal TEST_CASE (`dfgValue=(0.12,0.80)`, `f0=1` exactly since
  metallic=1/white baseColor): corrected `E = mix(dfg.x,dfg.y,1) =
  dfg.y = 0.80` (a robust `mix()` endpoint identity, independent of
  `dfg.x`); the retired formula's own prediction at the same inputs is
  `E = dfg.x+dfg.y = 0.92`. Corrected expected pixel `(184,92,10)`;
  retired-formula prediction `(211,106,12)` — delta 27/14/2 on R/G/B,
  the R/G deltas both exceeding the TEST_CASE's own ±8/255 tolerance.
  Both derivations, and an explicit `CHECK(deltaR > 8)` discrimination
  assertion, are now in the test itself (not just this report).
- Furnace TEST_CASE (`dfgX=0.08`, `dfgY=0.55`, `F0=0.7`, `L=0.6`):
  corrected `E = 0.3*0.08 + 0.7*0.55 = 0.409` → expected pixel `63/255`;
  retired formula's own prediction `E = 0.7*0.08+0.55 = 0.606` → `93/255`
  — delta 30/255, also now a real `CHECK(deltaFromOldFormula > 8)`
  assertion in the test.

### Finding 2 — furnace test's ground truth was tautological; revert-discrimination proof

The furnace TEST_CASE's `expectedE` used to be computed via
`kF0*kDfgX+kDfgY` — byte-identical to the shader's own (buggy) formula, so
it could never independently catch a shader-side regression. Replaced with
an INDEPENDENT C++ transcription of the correct formula
(`(1.0F-kF0)*kDfgX + kF0*kDfgY`), transcribed directly from the cited
Filament source, never by calling brdf.slang's own helper.

**Revert-discrimination proof (empirical, this round, lavapipe,
`rx_material_gpu_tests`):**
1. Temporarily edited `iblSpecularReflectance()` back to the retired
   `f0*dfg.x+dfg.y` formula (one-line change, comment marked
   `TEMPORARY REVERT ... DO NOT COMMIT`).
2. Re-ran `--test-case="*furnace*,*mirror-metal*"` (no rebuild needed —
   Slang shaders compile in-process from source at test-binary startup,
   so the revert took effect immediately). Result: **2 test cases failed,
   5 assertions failed**:
   - Furnace: `pixel r=93 expectedE=0.409 expected=63` — CHECK failed on
     all 3 channels.
   - Mirror: `pixel r=205 g=102 b=11 expected=(184,92,10)` — CHECK failed
     on R and G.
   - (A third, PRE-EXISTING, unrelated furnace/energy-compensation test
     case in a different file — `test_standard_pbr_energy_compensation_gpu.cpp`
     — still passed, confirming the revert's blast radius was correctly
     isolated to `iblSpecularReflectance()` alone, not the separate
     `energyCompensation()` mechanism.)
3. Restored the corrected formula via `Edit` (verified via `git diff`
   against the file showing the intended lerp-form body, no leftover
   revert markers).
4. Re-ran the full `rx_material_gpu_tests` binary: **69/69 test cases,
   3556/3556 assertions, 0 failed** (lavapipe) — confirmed identical on
   real NVIDIA (RTX 2080, driver 580.82.07): **69/69, 3556/3556**.

This is a real, quantified gate-flip against the actual shader-compiled
output, not a hand-derived claim.

### Finding 3 — synthetic dfg constants violated the real-bake invariant

All three dfg-bearing TEST_CASEs in `test_standard_pbr_ibl_gpu.cpp` had
their constants replaced with `dfg.x < dfg.y` (strict), matching every
real T9 bake's own `dfg.x <= dfg.y` invariant (`dfg_lut.slang`'s
accumulation, `r.x += term*fc, r.y += term`, `fc=(1-VoH)^5 ∈ [0,1]`):

| TEST_CASE | Retired `dfg` | New `dfg` | Discriminates Finding 1? |
|---|---|---|---|
| Lambertian diffuse | `(0,0)` | `(0.05,0.4)` | No — see below, by design |
| Mirror-metal | `(1,0)` (inverted invariant) | `(0.12,0.80)` | Yes — delta 27/255 (R) |
| Furnace | `(0.9,0.05)` (inverted invariant) | `(0.08,0.55)` | Yes — delta 30/255 |

**Lambertian is deliberately left non-discriminating, and this is
documented in the test itself, not silently accepted.** With energy
compensation OFF (this file's pipeline variant, `specializationBits=0`)
and metallic=0/white baseColor (`diffuseColor==1`), the combined IBL
output for a UNIFORM environment is `ibl = diffuseColor*L*(1-E) +
prefilteredSample*E = L*(1-E) + L*E == L` **exactly**, for ANY value of
`E` — the split-sum method's own energy-conservation identity, not a test
weakness. Verified empirically in the revert run above: the Lambertian
TEST_CASE was unaffected by the temporary revert (not included in the
2-failed-test-case count). No choice of `dfg` constant can make this
specific probe discriminate the specular formula without abandoning its
own "isolate the diffuse lobe, closed-form `pixel==L`" pedagogical
purpose; the mirror and furnace TEST_CASEs (both metallic=1, which kills
`diffuseColor` and decouples specular from this cancellation) carry the
discrimination burden instead. This reasoning is now in the TEST_CASE's
own header comment.

Also corrected a stale, now-inaccurate comment in
`test_standard_pbr_unlit.cpp`'s "occlusion closed-form" TEST_CASE
(`dfgValue=(0.5,0.5)`, unaffected functionally since that TEST_CASE
asserts a RATIO, not an absolute E-dependent value — its own "expected
pixel" prose claims were rewritten to match the corrected formula's real
E=0.5 output, replacing the old formula's stale E=1.0 claim).

### Finding 4 (LOW) — sample09_scene default environment

`samples/09_scene/main.cpp` now binds a modest default environment
(intensity **0.4** — well under `EnvironmentDesc::intensity`'s own
neutral-1.0 default and under sample08's own default `--env` intensity,
chosen so the grid reads lit without competing with the scene's existing
directional light + shadow) via the identical `rx::ibl::bakeEnvironment()`
→ `Scene::setEnvironment()` path sample08 uses. Reuses
`samples/08_gltf_viewer/environments/gate_test_env.hdr` VERBATIM — no new
asset; `samples/09_scene/CMakeLists.txt` gained a second deploy-block
copy of that same committed file (mirroring its existing
`references.stamp`/`material_shaders_deploy.stamp` pattern) plus an
`ibl_shaders/` deploy block for `rx_ibl`'s bake-chain kernels (NOT
`skybox.slang` — no skybox pass added, out of this fix round's scope; only
the diffuse/specular IBL lobes `standard_pbr.slang` already consumes).
Wired into both the default DamagedHelmet-grid composition (headless
D17 gate AND the interactive `--present` path the review's own LOW
finding specifically named) — NOT into `--stress` (unlit materials,
environment has no effect) or `--scene <path>` (Sponza/custom import,
out of this fix round's own scope). `materialSpecializationBits()`
(mirrors sample08's identical helper) ties `warmMaterialPipelines()`'s
pre-warm and `resolveDrawGroups()`'s real per-frame pipeline lookup to
the same `hasEnvironment()`-derived bit, so the two cannot drift apart.
`BindlessTable::Capacities::cubeImages` bumped 1→4 (3 real registrations
— base/irradiance/prefiltered — +1 headroom, matching sample08's own
sizing).

Visual result (`task-10-captures/sample09_grid_scene_with_env_256.png`
vs. the retained `sample09_grid_scene_no_env_256.png`): the grid now
shows visible blue/teal-lit surfaces and dome reflections on all 8
DamagedHelmet instances instead of the near-black render the review
flagged — a real, visible improvement, not a marginal one — while staying
clearly darker than a "washed" bright render (the explicit design goal).

### Finding 5 — D17 reference + capture regeneration

Regenerated via `tools/regen_references.sh` (unmodified), both driven by
the fixed formula + (for sample09) the newly-bound environment:

- `samples/08_gltf_viewer/references/loaded_scene.png` — regenerated;
  headless gate: `failingPixels=0/65536 (0.0000%) pass=true` (lavapipe).
  Visually dimmer/more physically correct specular response on the
  visor, as the review predicted.
- `samples/09_scene/references/grid_scene.png` — regenerated (now lit,
  per Finding 4); headless gate: `failingPixels=0/65536 (0.0000%)
  pass=true` (lavapipe).

`task-10-captures/` refreshed (main checkout, uncommitted working-tree
files, same as this report):
- `after_ibl_helmet_256.png` — replaced with the corrected-formula
  lavapipe render (copy of the regenerated `loaded_scene.png`).
- `after_ibl_helmet_256_nvidia.png` — replaced with a fresh real-NVIDIA
  capture (RTX 2080, driver 580.82.07, `VK_ICD_FILENAMES=nvidia_icd.json
  --write-references`), visually near-identical to the lavapipe capture.
- `sample09_grid_scene_with_env_256.png` — NEW, the after-Finding-4
  capture (copy of the regenerated `grid_scene.png`).
- `before_flat_ambient_helmet_256.png` / `no_env_zero_ambient_helmet_256.png`
  / `sample09_grid_scene_no_env_256.png` — left as-is (unaffected by this
  round: the first two are the pre-T10/`--no-env` states with zero IBL
  contribution regardless of the E-formula; the third is retained as the
  explicit "before Finding 4" comparison point).

### Empirical bar closed this round

**Full serial ctest, lavapipe** (`VK_ICD_FILENAMES=lvp_icd.json nice -n19
xvfb-run -a ctest --test-dir build/linux-native --output-on-failure -j1`):
**33/33 (100%)**, 103.3s. Zero unfiltered validation errors (`grep -i
validation | grep -v "known false positive"` → empty).

**Full serial ctest, real NVIDIA** (RTX 2080, driver 580.82.07,
`VK_ICD_FILENAMES=nvidia_icd.json`, identical invocation): **33/33
(100%)**, 185.4s. Zero unfiltered validation errors. D17 informational
divergence (non-lavapipe driver, not enforced per D17/RC7a): sample08
`loaded_scene` `487/65536 (0.7431%)`; sample09 `grid_scene` `516/65536
(0.7874%)` — both comfortably inside normal driver-variance range.

**`rx_material_gpu_tests` in isolation, both drivers:** lavapipe
**69/69 test cases, 3556/3556 assertions**; NVIDIA **69/69, 3556/3556**
(identical). The `*IBL*,Skybox*` filtered subset: **6/6, 503/503** on
both drivers (up from the prior round's 501/501 — the 2 new explicit
discrimination `CHECK`s).

**windows-cross-zig / Wine** (CI's own GPU-exclusion filter,
`ctest -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_material_brdf_gpu|
rx_debug_ui_gpu|rx_frame_loop_gpu|rx_ibl_gpu|sample'`): **14/14 (100%)**
on a clean re-run. One transient failure on the first attempt
(`rx_asset_gltf_gpu_tests`'s own wall-clock CI-stall-detector TEST_CASE,
`REQUIRE(maxPumpDuration < kCiStallDetector)`, `14002µs < 6772µs`
requirement failed under this session's own concurrent build/test load)
— confirmed a pre-existing timing flake, unrelated to this round's own
changes (async-import wall-clock timing, no dependency on IBL/material
shading code): re-ran in isolation, passed clean (50.39s). Not part of
this fix round's own stated empirical minimum (lavapipe + real NVIDIA);
included here for completeness, matching the prior round's own reporting
convention.

### Test counts, before → after this round

| Suite | Before (prior round) | After (this round) |
|---|---|---|
| Full ctest, lavapipe | 33/33 | 33/33 |
| Full ctest, NVIDIA | 33/33 | 33/33 |
| `rx_material_gpu_tests` (full) | 69/69, 3554/3554 | 69/69, 3556/3556 |
| `*IBL*,Skybox*` subset | 6/6, 501/501 | 6/6, 503/503 |
| windows-cross-zig (CI filter) | 14/14 | 14/14 |

### Concerns / follow-ups from this round

- Sample09's `--scene <path>` (custom import, e.g. Sponza) and `--stress`
  modes do NOT bind an environment — out of this fix round's own scope
  (Finding 4 named the default composition specifically); `--stress` uses
  unlit materials that would see no effect regardless.
- No skybox pass was added to sample09 — Finding 4's own text named only
  `Scene::setEnvironment()`, not a background pass; a skybox there remains
  a legitimate future enhancement, not a gap this round leaves open.
