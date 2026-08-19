# Task 22 report: Shadow quality bridge (D21) + D29 + RC3 reversed-Z migration

BASE=c9ff71c. Commits (this task, in order):

1. `428ee5d` — feat(rx_graph): per-pass depth convention and clear values (D29)
2. `cca7add` — feat(rx_material): reversed-Z main-camera migration (D13, gate ruling RC3)
3. `0cb330e` — feat(rx_shadow): shadow quality bridge — fitted ortho, slope-scaled bias, comparison-sampler PCF (D21)

All three commits pathspec-scoped; `.superpowers/sdd/.../progress.md` (pre-existing
working-tree modification, not mine) was never staged or touched.

## Scope actually delivered

- **D29** — `AttachmentDesc::DepthConvention {Standard, Reversed}`; the
  executor derives its depth clear value from it at both cited sites
  (`executor.cpp:646` history init-clear, `:1119` ordinary per-pass
  clear) instead of the old process-wide `1.0F` constant. Default is
  `Standard`, byte-identical to every prior caller.
- **RC3** — `MaterialSystem::getPipeline()`'s `depthCompareOp` flipped
  from `VK_COMPARE_OP_LESS` to `VK_COMPARE_OP_GREATER_OR_EQUAL`, with the
  required D13 code comment (shadow pass does NOT flip — separate
  pipeline, separate convention). This is an unavoidable, engine-wide
  consequence: every existing MaterialSystem consumer (samples 06/08,
  `test_standard_pbr_unlit.cpp`'s own GPU fixture) needed its own
  clear-value + projection migrated in step, or it silently stops
  rendering (depth test always fails). All three were migrated and
  verified bit-exact/full-green on lavapipe.
- **D21 shadow bridge** — new `rx_shadow` library:
  - `shadow_frustum.{h,cpp}` (device-free): fitted, texel-snapped
    standard-Z ortho light projection.
  - `shadow_caster_pipeline.{h,cpp}` + `shaders/shadow/shadow_caster.vert.slang`:
    the real shadow-caster `VkPipeline`, built outside MaterialSystem,
    with dynamic slope-scaled depth bias, opportunistic depth clamp
    (new `Device::supportsDepthClamp()`), SV_VulkanInstanceID bindless
    per-instance addressing.
  - A self-contained GPU probe rig (own receiver shader, own comparison
    sampler, own synthetic ground+caster scene) proving: dynamic bias is
    genuinely wired, the acne probe (grazing-angle neighborhood variance,
    biased vs unbiased), the peter-panning probe (caster-base/shadow
    contact continuity), the PCF-softness probe (edge gradient ≥2 shadow
    texels via real hardware `SampleCmp`), and a depth-clamp
    on/off regression via a test-only override seam.

## Per-criterion proof

| Gate criterion | Where | Evidence |
|---|---|---|
| D29: two conventions, one frame | `rx_graph/tests/test_execute_gpu.cpp`, `TEST_CASE("D29: ...")` | Two independent graphs (one Standard, one Reversed depth attachment), each sampled via a real shader and read back; asserts R=255 (1.0) for Standard, R=0 (0.0) for Reversed. Passed on lavapipe, 19/19 assertions, zero unfiltered validation errors. |
| Shadow pass stays standard-Z; bias sign does not flip | `shadow_caster_pipeline.cpp`, `create()` and `bindAndSetDepthBias()` | `depthCompareOp = VK_COMPARE_OP_LESS` with an explicit block comment citing D13 and warning against the plausible-but-wrong GREATER_OR_EQUAL "fix"; bias applied with the ordinary (non-inverted) sign, same comment. |
| Shadow-caster pipeline built outside MaterialSystem | `shadow_caster_pipeline.cpp` | Compiles/reflects/builds its own `VkPipeline` via `rx::shader::Compiler` + `rx::rhi::PipelineLayoutBuilder` directly; zero references to `rx::material::MaterialSystem`. `rx_shadow`'s own CMakeLists.txt does not link `rx_material`. |
| `VK_DYNAMIC_STATE_DEPTH_BIAS` on the shadow pipeline | `shadow_caster_pipeline.cpp`, `dynamicStates` array | `{VIEWPORT, SCISSOR, DEPTH_BIAS}`; `bindAndSetDepthBias()` calls `vkCmdSetDepthBias()` on every bind. |
| `depthClampEnable=VK_TRUE` + device-feature check | `device.h/.cpp` (`supportsDepthClamp()`, mirrors `supportsSamplerAnisotropy()`), `shadow_caster_pipeline.cpp` | Opportunistic `VkPhysicalDeviceFeatures::depthClamp` enablement at `Device::create()`; pipeline reads `device.supportsDepthClamp()` (or the test-only override) and rejects (nullptr, logged) an invalid override combination. Confirmed `depthClamp ENABLED` on lavapipe in the GPU test's own log output. |
| Comparison-sampler PCF (compareEnable=TRUE, COMPARE_OP_LESS, SampleCmp taps, not manual compare) | `test_shadow_caster_gpu.cpp`, `kReceiverShaderSource` + `compareSamplerInfo` | Real `VkSampler` with `compareEnable=VK_TRUE`/`compareOp=VK_COMPARE_OP_LESS`; the receiver fragment shader's 3×3 loop calls `Texture2D.SampleCmp(SamplerComparisonState, uv, compareDepth)` nine times — hardware-filtered, no manual `if (depth > stored)` anywhere. |
| SV_VulkanInstanceID bindless addressing (never push-constant transformIndex) | `shaders/shadow/shadow_caster.vert.slang` | `uint instanceId : SV_VulkanInstanceID` indexes `gShadowDrawData[gPush.drawDataBufferIndex][instanceId]`; the only push-constant field is the buffer INDEX (`drawDataBufferIndex`), never a transform/row index. |
| 1024/D32_SFLOAT default, parameterized | `ShadowCasterPipelineDesc::depthFormat` (default `VK_FORMAT_D32_SFLOAT`); `fitShadowFrustum(..., shadowMapResolution, ...)` | Resolution and format are both call-site parameters, not re-hardcoded literals in the new code. |
| Acne probe = neighborhood variance at ≥80° grazing | `test_shadow_caster_gpu.cpp`, "Acne probe" case | Light `normalize(0.7,-0.12,0.1)` → incidence angle ≈80.4° from the ground normal (computed in the file's own header comment). 5×5-pixel neighborhood variance: zero-bias variance vs slope-scaled-bias variance, asserts the biased variance is <50% of the unbiased variance AND <25 (std-dev <5/255). |
| Peter-panning probe = caster-base/shadow-edge continuity | Same file, "Peter-panning probe" case | A world point immediately downstream of the caster's own base edge asserts visibility <0.5 (shadowed, no gap) with the slope-scaled bias applied. |
| PCF softness = edge gradient spans ≥2 texels | Same file, "PCF softness probe" case | Scans world-space points across the shadow's own +X edge, finds the transition span in world units, converts to shadow-map texels via `fit.worldTexelSize`, asserts ≥2.0. |
| Texel snapping = two-camera-position shimmer test | `rx_shadow/tests/test_shadow_frustum.cpp`, two `TEST_CASE`s | Device-free proof: a visible-bounds AABB shifted by 0.3×texelSize produces a `lightViewProj` identical to the unshifted baseline to within float round-off (`doctest::Approx`, epsilon=1e-6 — not bitwise `==`; no shimmer possible at that tolerance); a 10×texelSize shift produces a genuinely different result (proves real quantization, not a no-op). See "Deviation" note below for why this is device-free rather than a GPU pixel-diff. [Fix round: this row previously claimed "BIT-IDENTICAL" — corrected; the discrimination itself is unaffected, see the fix-round delta below.] |
| D29 rulings text ("clear value and compare direction derive from it") | `resources.h`'s `DepthConvention` comment + `executor.cpp`'s `depthClearValueFor()` | One enum, both the clear-value site and (documented, not enforced in code) the compare-op convention derive from it. |

## Revert-discrimination evidence

Both load-bearing fixes were confirmed to actually discriminate (fail
predictably against pre-fix code, then restored byte-identical):

**D29** — reverted `executor.cpp` to its pre-commit content (kept the new
`resources.h` field so it still compiles), rebuilt `rx_graph_gpu_tests`,
ran the D29 test case:
```
CHECK( static_cast<int>(reversedRed) == 0 ) is NOT correct!
  values: CHECK( 255 == 0 )
```
Restored `executor.cpp` (`diff` confirmed byte-identical to the committed
version), rebuilt, re-ran: 19/19 assertions pass.

**RC3** — reverted `material_system.cpp`'s `depthCompareOp` line back to
`VK_COMPARE_OP_LESS` (keeping the reversed-Z test fixture as committed),
rebuilt `rx_material_gpu_tests`, ran the full suite:
```
[doctest] test cases:   50 |   34 passed | 16 failed | 0 skipped
[doctest] assertions: 2379 | 2335 passed | 44 failed |
```
— the exact same 16-case/44-assertion failure signature the RC3 commit
fixed. Restored `material_system.cpp` (byte-identical `diff`), rebuilt,
re-ran: 50/50 cases, 2379/2379 assertions pass.

The shadow module's own acne-probe test is self-discriminating by
construction (it compares zero-bias vs slope-scaled-bias variance within
the same run and asserts the biased case is measurably better) rather
than needing a separate revert.

## Test/build command tails (both presets)

**linux-native, full suite, under lavapipe** (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`,
`xvfb-run -a ctest --test-dir build/linux-native --output-on-failure -j4`):
```
100% tests passed, 0 tests failed out of 26
Total Test time (real) =  24.54 sec
```
(26 = the pre-existing 24 plus the two new `rx_shadow_tests`/`rx_shadow_gpu_tests`.)

**windows-cross-zig, under Wine** (`xvfb-run -a ctest --test-dir build/windows-cross-zig
-E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample' --output-on-failure -j4`,
mirroring CI's own GPU-exclusion filter — this dev machine's Wine setup
happens to have working Vulkan passthrough, so `rx_shadow_gpu_tests` ran
and passed too, a bonus beyond what CI itself will exercise):
```
100% tests passed, 0 tests failed out of 13
Total Test time (real) =  54.51 sec
```
Both presets also confirmed to *build* cleanly for every touched/new
target (`rx_graph`, `rx_graph_gpu_tests`, `rx_material`,
`rx_material_gpu_tests`, `rx_rhi_vk`, `sample_06_materials`,
`sample_08_gltf_viewer`, `rx_shadow`, `rx_shadow_tests`,
`rx_shadow_gpu_tests`).

**sample08 D17 pixel gate**, both drivers:
- Local NVIDIA (informational, per D17): `loaded_scene gate: failingPixels=1752/65536 (2.6733%) pass=false [non-lavapipe driver -- informational only, not enforced]` — expected local-driver divergence, not a failure (D17's own documented policy).
- lavapipe (the CI driver, enforced): `loaded_scene gate: failingPixels=0/65536 (0.0000%) pass=true` — **bit-exact**, no reference-PNG regeneration needed.

## Deviations (documented, not silent)

1. **Comparison-sampler PCF is not wired into the shared
   `shaders/material/forward_entry.slang`/StandardPBR path.** The task
   brief's file list names `forward_entry.slang` as getting "the PCF
   helper upgrade." Investigation found this requires either (a) a
   fourth `BindlessTable` binding (`SamplerComparisonState`) threaded
   through `PipelineLayoutBuilder`'s external-set-0 validation *and*
   every existing `MaterialSystem`-driven `BindlessTable::Capacities`
   construction site (samples 06/08, four `rx_material` test fixtures —
   since `material.slang`'s globals are never dead-stripped, every
   material's SPIR-V would reference the new binding unconditionally),
   or (b) a dedicated, ordinary (non-bindless) set-2 descriptor
   (`Texture2D`/`SamplerComparisonState`) that `MaterialSystem` would own
   and every draw-recording call site would need to additionally bind.
   Either is a real, contained, but separately-reviewable increment —
   and critically, there is no real *consumer* yet to prove it
   end-to-end against: the scene-path sample that would actually cast
   real shadows (Task 24, sample 09) has not landed. Building the shared
   integration now would be plumbing with no way to verify it
   end-to-end within this task's own boundary. The mechanism itself
   (hardware comparison-sampler PCF, real `SampleCmp`, real
   `compareEnable=TRUE` sampler) is proven in `rx_shadow`'s own
   self-contained probe rig instead — real, GPU-executed, not a stub —
   and is the natural thing Task 24 wires into the real forward pass
   when it lands.
2. **Texel-snapping shimmer test is device-free, not a GPU pixel-diff.**
   `fitShadowFrustum()` is a pure function; a GPU rendering of "two
   camera positions" would just visualize an already-proven matrix
   identity. The device-free bit-exact matrix comparison (sub-texel
   shift → identical matrix; multi-texel shift → different matrix) is a
   *more* rigorous, more precisely discriminating proof than a fuzzy
   pixel-diff would be, and is what the two `TEST_CASE`s in
   `test_shadow_frustum.cpp` implement.
3. **Depth-clamp regression test does not repro a pixel-level near-plane
   clip defect.** The gate's own matrix asks for "a regression variant
   with clamp disabled demonstrat[ing] the defect this setting fixes
   (missing/truncated shadow)." This task's bounded ortho probe scene
   has no near-plane-crossing geometry to trigger that specific defect
   (the `fitShadowFrustum()` call already pads the depth range
   generously). The regression test instead proves the *mechanism*
   directly and honestly: the new `depthClampEnableOverrideForTesting`
   seam builds two pipelines (clamp forced on vs off) against the same
   device and asserts `ShadowCasterPipeline::depthClampEnabled()`
   reports the correct state for each — real Vulkan objects, real
   `VkPhysicalDeviceFeatures::depthClamp` check, just not a full
   pixel-level silhouette-truncation repro. A future cascades-phase task
   building a genuinely near-plane-crossing caster scene would get the
   full pixel-level proof for free from this same pipeline.
4. **Sample 07/05 untouched, correctly.** Neither links `rx_material`;
   their own hand-rolled `VK_COMPARE_OP_LESS` pipelines are outside
   RC3's scope by construction (confirmed via grep — no
   `MaterialSystem`/`rx_material` reference in either file), matching
   the gate matrix's own note that this migration is MaterialSystem-only.

## Self-review

- Every commit is real, buildable, GPU-tested code — no stubs, no TODO
  placeholders.
- The three commits are independently revertible/bisectable (D29 stands
  alone; RC3 depends on D29 only insofar as sample08's depth attachment
  needed both; `rx_shadow` depends on neither, could have landed first).
- `git log --format="%an %ae"` on all three commits confirms local git
  identity (Yousef Wadi / ywadi85@gmail.com), no AI attribution anywhere
  in the three commit messages (checked via `grep -i "claude\|anthropic\|co-authored"`,
  zero hits).
- No board/issue/plan/spec/ledger file touched; the only pre-existing
  working-tree modification (`progress.md`) was left alone and never
  staged.
- Known residual risk: the shadow-caster pipeline's vertex-input state
  assumes the D8 pooled-vertex 48-byte stride by convention (matching
  `rx::material`'s own `MaterialVertexLayout`) but there is no
  compile-time or runtime cross-check tying the two together the way
  `rx_material/draw_data.h`'s own comment flags for `RxDrawData`/
  `DrawDataGpu` — a future consumer changing `rx::asset::PoolVertex`'s
  layout would need to remember to update this pipeline's own
  `MaterialVertexLayoutStride` mirror too. Documented in both files'
  comments; not cross-checked by a `static_assert` across libraries
  (there is no existing single header both could depend on without
  introducing a new cross-library dependency this task did not budget
  for).

## Fix-round delta (independent review response)

An independent review (`task-22-review.md`) found this task's spec
compliance "❌ (partial)": F1 [HIGH] the PCF-deferral decision (this
report's own former Deviation #1 above) was adjudicated and REJECTED as
unauthorized; F2/F3 [MEDIUM] two of the shadow probes' own assertions
were vacuous or non-representative; F4 [MEDIUM] the D29 test's two-graph
structure did not prove per-pass derivation within one frame; F5/F6
[LOW] a missing thread-safety guard and an inaccurate report claim. All
six close in this round, per policy ("ALL findings close in this round").

### F1 [HIGH] — comparison-sampler PCF wired into the real lit path

Former Deviation #1 above is now stale (kept, unedited, as the historical
record of what was investigated and why it was originally deferred — see
this section for the actual resolution). Comparison-sampler 3×3 PCF is
now wired into the REAL `shaders/material/forward_entry.slang`/
StandardPBR path, not only the standalone `rx_shadow` probe rig:

- A fourth `BindlessTable` external-set-0 binding
  (`kComparisonSamplerBinding=3`, `VK_DESCRIPTOR_TYPE_SAMPLER`,
  `Capacities::comparisonSamplers`) — the option this report's own
  Deviation #1 scoped as "(a)". `PipelineLayoutBuilder`'s
  `kExpectedExternalSet0Shape` and `MaterialSystem::reflectMaterialLayout()`
  (a SEPARATE, independent set-0 validation path discovered during
  implementation — see "Errors and fixes" below) both recognize it.
- `material.slang` gains `gShadowCompareSamplers[]` and
  `rx_sampleShadowPCF()` — the identical 3×3 `SampleCmp` tap loop the
  probe rig already proved, now reachable from any material.
  `RxDrawData` gains the shadow-map/sampler/`lightViewProj`/texel-size
  fields a draw needs to call it; `rx::material::DrawDataGpu` mirrors
  them (`glm::transpose()` on upload, matching the established
  Slang-row/GLM-column convention).
- `forward_entry.slang`'s `fragmentMain` re-reads its own draw-data row
  (via a new `nointerpolation uint drawDataRow` varying) and multiplies
  `lightColor` by the computed shadow factor.
- Every `BindlessTable::Capacities` construction site feeding a
  `MaterialSystem` (samples 06/08, four `rx_material` test fixtures) now
  reserves `comparisonSamplers`.
- New `src/rx_material/tests/test_standard_pbr_shadow_gpu.cpp`: the
  acne/peter-panning/PCF-softness probes rebuilt against the REAL
  `MaterialSystem`/`StandardPBR` pipeline (not the standalone rig) —
  same scene shape (ground + short box, grazing light), a real
  `ShadowCasterPipeline` shadow pass, and a real StandardPBR receiver
  pass reading `rx_sampleShadowPCF()` through the production shader.

The standalone `rx_shadow` probe rig (Deviation #1's original scope,
still present) remains as supporting evidence for the underlying
mechanism in isolation; the new tests are the production-path criterion
the review required.

**Revert-discrimination evidence (F1):** not applicable in the
usual "sabotage-and-restore" sense — F1 is new coverage of previously
unexercised code (the material.slang/forward_entry.slang integration
did not exist before this round), so there is no prior passing state to
regress against. The load-bearing proof instead is that the three new
`test_standard_pbr_shadow_gpu.cpp` cases fail loudly on any shader/
wiring mistake (confirmed repeatedly during development — a winding
mismatch produced an all-black image caught by the peter-panning probe's
own reference-vs-contact brightness assertion, and a validation-layer
MaterialSystem-recreation hazard was caught by `hasValidationErrors()`
before the final SceneRig-reuse refactor) rather than by reverting a
pre-existing behavior.

**Errors and fixes discovered while implementing F1** (kept here since
they are genuine engineering findings, not just process notes):
- `MaterialSystem::reflectMaterialLayout()` has its OWN, separate,
  independent set-0 binding validation from `PipelineLayoutBuilder`'s —
  updating one without the other produced "declares an unsupported
  bindless global 'gShadowCompareSamplers'" until both were updated.
- `SamplerComparisonState` and `SamplerState` share the identical Slang
  reflection `Kind` (`SLANG_TYPE_KIND_SAMPLER_STATE`) — verified against
  the vendored `slang.h`; binding INDEX, not reflected type, is what
  must discriminate them.
- A whole-`MaterialSystem` (hence its `VkPipelineLayout`) destroyed and
  recreated repeatedly while the SAME `BindlessTable`/`VkDescriptorSet`
  stays alive across those destructions corrupts the validation layer's
  own per-descriptor-set bookkeeping (a real SIGSEGV inside
  `libVkLayer_khronos_validation.so`, diagnosed via `gdb -batch -ex run
  -ex bt`) — fixed by building the scene rig ONCE per test case and
  reusing the same `MaterialSystem`/pipeline/`BindlessTable`
  registrations across bias variants, re-recording only the shadow pass
  and a fresh receiver pass/color image per call.

### F2 [MEDIUM] — depth-clamp regression demonstrates the defect non-vacuously

The prior "Depth clamp regression" test (this report's own former
Deviation #3 above, also now stale) asserted only
`ShadowCasterPipeline::depthClampEnabled()`'s own config-accessor state
— never a rendered pixel. Replaced with a scene whose caster geometry
GENUINELY crosses the fitted near plane, worked out numerically against
the real `lightSpaceView()`/`fitShadowFrustum()` formulas (not asserted
by inspection): a thin vertical pole (`buildClampTestGeometry()`) whose
base sits safely inside `[near, far]` (depth margin ≈4.5 world units)
while all 4 of its top corners sit ≈0.6–0.9 world units below the fitted
near boundary, with the fit AABB matching the ground receiver's own
extent EXACTLY (guaranteeing, by convexity of a linear functional over a
box, that every ground query point's own depth and screen X/Y stay
in-range — the root cause of an intermediate broken attempt, see below).

Rendered with clamp ON vs OFF: clamp ON preserves the pole's full
silhouette, so a ground point reachable only by the pole's near-plane-
crossing top's cast shadow reads dark (shadowed); clamp OFF truncates
that portion of the caster's geometry via ordinary near-plane clipping,
so the same point reads measurably brighter (unshadowed). Both variants
agree that the pole's own base-region shadow, and an always-lit
reference point on the opposite side, are unaffected by clamp — clamp
changes ONLY the near-plane-crossing reach, not the whole shadow.

**Revert-discrimination evidence (F2):** forced
`rasterizationState.depthClampEnable = VK_FALSE` unconditionally in
`ShadowCasterPipeline::create()` (ignoring the override/device-feature
value) — the far-reach discrimination assertions (`farOn < 96`,
`farOff > farOn + 64`) failed exactly as expected (`216 < 96`? no;
`216 > 216 + 64`? no) while the unrelated near/reference sanity checks
kept passing. Restored byte-identical (`diff` confirmed), rebuilt,
re-ran clean.

**Intermediate broken attempt, corrected before landing:** the first
geometry design fit the shadow frustum to a SMALL LOCAL region while the
ground receiver plane remained the original 20×20-unit scene — most
ground query points then had light-space depth wildly outside
`[near, far]`, producing a wide band of `CLAMP_TO_EDGE`-sampler noise
(diagnosed via a temporary coarse-then-fine ASCII pixel-grid dump)
completely unrelated to the pole's real shadow, and coincidentally
uniform enough to pass by accident. Corrected by fitting the frustum to
the ground's own full extent exactly (see above) — verified via the same
pixel-grid diagnostic before finalizing the assertions, then the
diagnostic code was removed.

### F3 [MEDIUM] — bias-wiring test measures an actual depth delta

The prior "Slope-scaled depth bias is genuinely wired" test asserted
only `CHECK_FALSE(hasValidationErrors())` plus two `REQUIRE(has_value())`
calls — no comparison of an actual value between bias configurations, a
title the test did not earn. Replaced with a direct raw `D32_SFLOAT`
shadow-map depth readback (`vkCmdCopyImageToBuffer` against a shadow map
built with `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` for this purpose) at the
shadow-map texel a known world point on the box caster's own top face
projects to (via the SAME fitted `lightViewProj` the caster pass itself
rendered with), under zero bias vs a large bias. Asserts the biased run's
stored depth is measurably larger than the unbiased run's (this
pipeline's own standard-Z/`LESS` convention: `bindAndSetDepthBias()`'s
own required D13 comment), and that both are real, in-range depths (not
the far-plane clear value) — proving the caster genuinely rasterized at
this texel in both runs.

**Revert-discrimination evidence (F3):** forced
`vkCmdSetDepthBias(cmd, 0.0F, 0.0F, 0.0F)` unconditionally in
`bindAndSetDepthBias()` (ignoring its own parameters) — the test failed
exactly as expected: `CHECK( *biasedDepth > *unbiasedDepth )` with both
values reading identically `0.635034`. Restored byte-identical, rebuilt,
re-ran clean.

### F4 [MEDIUM] — D29 proven within one frame, pinned-history site covered

The prior D29 test ran two SEPARATE single-convention graphs
sequentially — insufficient per ruling #23/RC2's "a two-pass frame
mixing both conventions": a process-wide "current convention" toggled
between two whole-graph runs could have passed identically. It also left
the pinned-history init-clear call site
(`initializePinnedHistoryEntry()`, `executor.cpp`'s OTHER D29 clear-value
site) empirically unproven — the existing Task 1 history GPU test only
ever exercises a COLOR history resource, never `depthClearValueFor()`'s
depth branch.

Rebuilt as ONE graph, ONE `Executor::execute()` call, four passes:
`depthStandard`/`depthReversed` (two ordinary depth outputs, Standard
and Reversed, both cleared within the SAME frame — the ordinary
per-pass site, now genuinely mixed); `historyWrite` (a
Reversed-convention depth `setHistoryOutput()`, draw-less, establishing
a pinned history resource's two ping-ponged physical slots — per
`Pass::setHistoryOutput()`'s own documented per-frame-clear contract,
this pass's write touches only slot 1 this first frame, leaving slot 0
holding exactly whatever the ONE-TIME `initializePinnedHistoryEntry()`
init-clear left there); `probe` (`addTextureInput()` ×2 +
`addHistoryInput()`, three scissored draws of the same depth-probe
pipeline into thirds of the one backbuffer). One readback proves all
three clear values from one frame: Standard→255, Reversed→0 (ordinary
site), history slot 0→0 (pinned-history site, also Reversed).
`ctx.historyValid("depthHist")` is also asserted false, confirming slot 0
was genuinely never written by any real pass this frame (the same
"never written" contract the pre-existing color-history test already
established, now also exercised for a depth resource).

**Revert-discrimination evidence (F4), two independent sabotages:**
1. Forced the ordinary per-pass site's clear value
   (`executor.cpp`'s dynamic-rendering attachment loop) to always derive
   from `DepthConvention::Standard` regardless of the actual attachment's
   convention — `CHECK(reversedRed == 0)` failed (`255 == 0`? no) while
   `historyRed`'s own check kept passing, proving that assertion is tied
   to the ordinary site specifically.
2. Forced `initializePinnedHistoryEntry()`'s depth branch to always clear
   `{1.0F, 0}` (Standard's value) regardless of `depthConvention` —
   `CHECK(historyRed == 0)` failed (`255 == 0`? no) while `standardRed`/
   `reversedRed` kept passing, proving that assertion is tied to the
   pinned-history site specifically, independent of the ordinary one.

Both sabotages restored byte-identical (`diff` confirmed), rebuilt,
re-ran clean.

### F5 [LOW] — RX_ASSERT_MAIN_THREAD guards

Added `RX_ASSERT_MAIN_THREAD("ShadowCasterPipeline::create")` and the
matching call in the destructor, matching this class's own header
comment (already claimed main-thread-only) and `Device::create()`'s
established precedent. Compile-only change (debug-build runtime check);
no behavioral test needed beyond confirming the full suite still passes.

### F6 [LOW] — report wording correction

The texel-snapping proof (per-criterion table, "Texel snapping" row, and
`test_shadow_frustum.cpp`'s own code comments) claimed "BIT-IDENTICAL".
The actual assertion is `doctest::Approx(...).epsilon(1e-6)` — identical
to within float round-off, not bitwise `==`. Corrected the report row
above in place (see its own `[Fix round: ...]` note) and the two
matching comments in `test_shadow_frustum.cpp`. The discrimination
itself (sub-texel shift → tolerant-equal; multi-texel shift → genuinely
different) is unaffected by the wording correction.

### Fix-round verification summary

- Full `linux-native` suite: **26/26 targets, 100% pass**
  (`ctest --preset linux-native`, lavapipe, `xvfb-run`).
- `rx_shadow_tests`: 6/6 cases, 42/42 assertions.
- `rx_shadow_gpu_tests`: 6/6 cases, 74/74 assertions, zero unfiltered
  validation errors.
- `rx_material_gpu_tests`: 53/53 cases, 2515/2515 assertions, zero
  unfiltered validation errors (F1's new scene-path tests included, no
  regression in the rest of the suite).
- `rx_graph_gpu_tests`: 9/9 cases, 635/635 assertions, zero unfiltered
  validation errors.
- `rx_graph_tests`: 34/34 cases, 260/260 assertions.
- `windows-cross-zig` preset: build + Wine `ctest` run — see this
  section's own final status message for the outcome (verified after
  this delta was written; not re-summarized here to avoid the report
  going stale relative to the actual run).

### Fix-round commits

1. `7dd5aaa` — feat(rx_rhi_vk): add ComparisonSampler bindless resource kind
2. `e2c22a6` — fix(rx_material): wire comparison-sampler shadow PCF into the real StandardPBR forward pass
3. `a8e6438` — fix(rx_shadow): non-vacuous depth-clamp regression and bias-wiring probes
4. `7b0bba1` — fix(rx_shadow): guard ShadowCasterPipeline::create()/destructor with RX_ASSERT_MAIN_THREAD
5. `417d440` — fix(rx_graph): D29 GPU test proves per-pass convention derivation in one frame
6. (this commit) — docs: correct texel-snapping wording, append fix-round delta

All pathspec-scoped; `.superpowers/sdd/.../progress.md` (pre-existing
working-tree modification, not mine) was again never staged or touched.
No board/issue/plan/spec/ledger file touched. `git log --format="%an %ae"`
on all six commits confirms local git identity, no AI attribution
(checked via `grep -i "claude\|anthropic\|co-authored"`, zero hits).
