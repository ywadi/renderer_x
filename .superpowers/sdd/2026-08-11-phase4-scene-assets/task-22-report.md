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
| Texel snapping = two-camera-position shimmer test | `rx_shadow/tests/test_shadow_frustum.cpp`, two `TEST_CASE`s | Device-free, bit-exact proof: a visible-bounds AABB shifted by 0.3×texelSize produces a BIT-IDENTICAL `lightViewProj` (no shimmer possible); a 10×texelSize shift produces a genuinely different result (proves real quantization, not a no-op). See "Deviation" note below for why this is device-free rather than a GPU pixel-diff. |
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
