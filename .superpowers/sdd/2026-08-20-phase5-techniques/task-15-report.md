# Task 15 report — clustered shading integration + frame-pipeline adoption (issue #51)

**Branch:** `task/t15-clustered-shading` (base `0907540`).
**Gate binding:** `.superpowers/sdd/2026-08-20-phase5-techniques/gate/matrix-p5t15-clustered-shading.md`,
`.superpowers/sdd/2026-08-20-phase5-techniques/gate/rulings-2026-08-20.md` (RC5).

## Scope delivered

1. Shading-side clustered Point/Spot lighting (`shaders/material/cluster_lighting.slang`) consuming T14's
   compute-produced froxel light lists, additive alongside the existing single-slot punctual/directional
   term — a scene with only a directional light stays byte-identical (D17-gated).
2. `RxDrawData`/`DrawDataGpu` extended on both the Slang and C++ sides with the froxel grid's own per-view
   constants and three bindless buffer indices, matching the codebase's own existing shadow-bridge
   precedent (`static_assert`-enforced size discipline on both sides).
3. My own multi-frame-in-flight redesign of `rx::cluster::ClusterPipelines`' per-frame consumption (T14's
   own explicitly-left scope boundary) — a real bug caught and fixed before any test ran.
4. `samples/09_scene`'s frame-pipeline adoption: depth prepass → T14's clustered light assignment → opaque
   forward lighting, through the render graph's own named-resource/barrier-derivation machinery, no
   hand-rolled barriers, no private depth copy (RC5).
5. Two permanent GPU value-assertion tests (`src/rx_material/tests/test_cluster_shading_gpu.cpp`): exact
   per-froxel membership (a light outside a probe's froxel contributes EXACTLY zero) and
   clustered-vs-brute-force equivalence (the matrix's own required permanent reference harness).
6. A `--stress-lights`/`--bench-frames` scaling-numbers benchmark in `samples/09_scene`, published at
   100/1k/5k synthetic lights, real NVIDIA hardware, N=5 median+range.
7. Stage 1 baseline regression re-check (DamagedHelmet/Sponza, `samples/08_gltf_viewer`) — no regression.

## Port provenance (Filament v1.75.0, google/filament)

- **Clustered evaluation loop shape**: `shaders/src/surface_light_punctual.fs:117-225`
  (`getLight()`/`evaluatePunctualLights()`), pinned commit `721ec800093de984cbee155e459298b6b2dbb855`
  (per matrix-p5t15's own "Sources consulted" section, fetched/read in full at gate time) — fetch froxel
  via the fragment's own froxel index, iterate `[offset, offset+count)`, early-out on `NdotL<=0` before any
  D/V/Fresnel evaluation (shadow lookups, the most expensive step, are gated last — this engine has no
  per-clustered-light shadow test yet, T18's own future scope, but the early-out shape is ported as-is
  since it is the SAME performance-motivated ordering independent of whether a shadow test follows).
- **Froxel-index derivation** (`clusterFindSliceZ()`/`clusterFroxelCoords()`): duplicated, bit-for-bit,
  from `shaders/cluster/froxel_common.slang`'s own identical copy — itself T14's own port of
  `Froxelizer.cpp:580-598`, pinned commit `0e58877c09afb1aacd09ff640f74d2adcd2a7e80` (T14's own citation;
  two separate fetch operations of the same v1.75.0 tag recorded two different commit hashes across the
  two gate matrices — an upstream documentation discrepancy neither task's own scope resolves, noted here
  rather than silently reconciled). `cluster_lighting.slang` cannot `import` across
  `shaders/material/`/`shaders/cluster/` (rx_material's own Slang session has no cross-directory search
  path, verified directly against `material_system.cpp`'s `createMaterialSession()` before writing the
  file), so this is a THIRD manually-synced copy of the same math, following the "kept in sync manually,
  cross-checked bit-for-bit by tests" convention `froxel_common.slang`'s own top comment already
  establishes for its C++/Slang pair — `test_cluster_shading_gpu.cpp`'s membership test exercises this
  copy's real output against the same scenario T14's own oracle already proved correct.
- **Energy-compensation composition**: `rx_evaluateClusteredLights()` returns RAW (pre-energy-compensation)
  diffuse/specular sums; `standard_pbr.slang` applies `EnergyComp::apply()` once to the accumulated total
  rather than per-light, since energy compensation is a pure multiplicative scalar of `(F0, dfgY)`
  independent of a light's own scale — `sum(apply(x_i)) == apply(sum(x_i))` exactly, avoiding a circular
  import of `standard_pbr.slang`'s own link-time-specialized `EnergyComp` type into `cluster_lighting.slang`.

## Per-draw data extension (`RxDrawData`/`DrawDataGpu`)

Twelve cluster scalar fields plus a `clusterView` `mat4` appended to the struct tail on both sides
(`src/rx_material/include/rx_material/draw_data.h`, `shaders/material/material.slang`) — grid dimensions,
Z-slicing constants, and three bindless indices (`clusterOffsetsBufferIndex`/`clusterWriteCountsBufferIndex`/
`clusterLightIndicesBufferIndex`) plus the per-frame `clusterLightsBufferIndex`. `sizeof(DrawDataGpu) == 544`
bytes (`static_assert`, draw_data.h:254), up from 432 at T13's own close. `ClusterLightGpu` (`rx_cluster/
cluster_lighting.h`) grew four SHADING-side `float4`s (112 bytes total, `static_assert`, cluster_lighting.h:149)
alongside T14's own three CULLING-side `float4`s, populated by `buildClusterLightList()` using the same
`rx::scene::lightmath::spotAngleScaleOffset()` T13 already established.

## Bindless plumbing (two new element types)

`shaders/material/cluster_lighting.slang` needs two new bindless-array element types reachable from a
fragment shader's compiled program: a generic `StructuredBuffer<uint>[]` (T14's own three named uint[]
render-graph outputs — offsets/write-counts/light-indices — all serve through ONE binding, selected
per-buffer by index, matching `gDrawData`/`gTextures`'s own "one binding per element TYPE" idiom) and a
`StructuredBuffer<ClusterLightGpu>[]`. New in `src/rx_rhi_vk/include/rx_rhi_vk/bindless.h`:
`BindlessResourceKind::GenericStorageBuffer`/`ClusterLightBuffer`, `Capacities::genericStorageBuffers`/
`clusterLightBuffers`, bindings 5/6 (`kGenericStorageBufferBinding`/`kClusterLightBufferBinding`),
`registerGenericStorageBuffer()`/`registerClusterLightBuffer()`. `MaterialSystem::reflectMaterialLayout()`'s
Case-2 bindless-global whitelist and `PipelineLayoutBuilder`'s `kExpectedExternalSet0Shape` (5→7 entries)
both needed extending — two real validation errors reproduced and fixed while wiring this up (see Bugs
below).

## Multi-frame-in-flight design (my own scope, T14's explicit boundary)

T14's own report flagged `ClusterPipelines`' single-shot descriptor-pool-reset-per-call design as
insufficient for a live multi-frame-in-flight consumer. My design: `ClusterPipelines::create()` now
allocates BOTH descriptor sets (offsets/write-counts/light-indices producer set, light-buffer consumer set)
for EVERY frame-in-flight slot ONCE at creation, never resetting/reallocating afterward.
`ClusterFrameInputs` gained a `frameSlot` field read FRESH on every `addClusterPasses()`-registered pass
lambda invocation (the same reference-capture mechanism already used for `lightsBuffer`/`lightCount`), via
a new `resolveFrameSlot()` helper that clamps an out-of-range slot and logs rather than reading OOB. Each
frame's execute() call only rewrites the resolved slot's descriptors (`vkUpdateDescriptorSets`) with that
frame's own light-buffer handle — no allocation churn per frame, matching this project's own
"pooled/persistent, not retrofit-later" performance posture.

**Design bug caught before it shipped**: my FIRST draft made `frameSlot` a parameter to the one-time
`addClusterPasses()` declare-time call — meaning the slot was selected ONCE at graph-declare time, never
varying per actual frame, completely defeating the purpose of per-frame descriptor selection. Caught via
code review before any test ran (not via a failing test), fixed by moving `frameSlot` into
`ClusterFrameInputs` so it is read fresh inside every pass lambda at EXECUTE time, exactly mirroring how
`lightsBuffer`/`lightCount` already worked.

## Depth prepass (RC5: T15 owns it)

`samples/09_scene::declareGraph()` adds a `"depth_prepass"` pass FIRST in the chain
(`shaders/depth/depth_prepass.vert.slang`, position-only, no fragment stage, `VK_CULL_MODE_BACK_BIT`,
reversed-Z `GREATER_OR_EQUAL` — matches this codebase's own D13 main-camera convention), writing a NAMED
graph resource `"sceneDepth"`. The forward pass reuses `"sceneDepth"` as its OWN depth-stencil output
(write-after-write LOAD, the SAME attachment-reuse mechanism the codebase's own `attachmentEverWritten`
tracking already provides for color) rather than a fresh `"depth"` — no private depth copy, satisfying
RC5's "delivering a sampleable scene-depth resource through the graph... must not ship private depth
copies" mandate structurally: any future pass (T19/T26) can declare `.addTextureInput("sceneDepth")` on the
SAME named resource and the render graph's own compile-time usage-flag union (`resources.h:97`: "a
texture-input read always implies `VK_IMAGE_USAGE_SAMPLED_BIT`") picks up sampled usage transparently, no
change needed on my end. This exact write-then-sample sequence is ALREADY covered generically by
`rx_graph`'s own GPU test suite (`src/rx_graph/tests/test_execute_gpu.cpp:2016-2038`: a pass
`setDepthStencilOutput()`s a resource, a later pass `addTextureInput()`s and REAL-samples it, asserted by
value) — I did not duplicate that proof at the application layer; my own D17 gate (byte-identical rendered
output) is the application-layer confirmation that THIS specific depth-prepass-then-forward-pass reuse is
wired correctly.

Excludes `Mask`-alpha draws from the depth prepass (`fixedFunctionState(handle).alphaMode != Opaque`,
clipping block ranges to `[0, opaqueCommandCount)`) — a deliberate departure from the shadow-caster's own
"full silhouette" precedent, justified because the main camera's depth directly determines final visible
pixels (a MASK draw's un-clipped silhouette in the depth buffer would incorrectly occlude geometry behind
it), whereas a shadow map's imprecision at MASK edges is a smaller, already-accepted limitation.

## Bugs found and fixed (this round)

- **Compile-order**: `DepthPrepassPushConstants` used before declaration — moved the struct definitions
  earlier in `main.cpp`.
- **Missing shader deploy**: samples 06/08/09 failed with "cannot open file 'cluster_lighting.slang'" —
  the new file wasn't in the CMake shader-deploy lists; added to all three.
- **Two real Vulkan validation errors**, reproduced directly: `reflectMaterialLayout()`'s whitelist and
  `PipelineLayoutBuilder::kExpectedExternalSet0Shape` both needed extending for bindings 5/6 — fixed by
  adding the two new recognized shapes to each.
- **Multi-FIF design bug**: see above — caught via review, not a failing test.
- **Push-constant conflict** in the test probe shader: a second `[[vk::push_constant]]` block alongside
  `material.slang`'s own `gMaterialGlobals` produced `VUID-VkPipelineLayoutCreateInfo-pPushConstantRanges-00292`
  — fixed by moving `ProbeParams` to an ordinary bound uniform buffer (set 1, binding 0).
- **Hardcoded cull radius defeated the exclusion test**: an arbitrary 1000-unit cull radius made the
  "excluded" light's culling sphere trivially reach every froxel regardless of real position — the excluded
  light leaked through at EXACTLY its own closed-form value (`10/(70²·π)`), proving genuine, not rounding,
  leakage. Fixed by deriving cull radius from the SAME production
  `rx::scene::froxel::pointLightCullRadius()` the real light-list builder uses.
- **Bindless capacity exhaustion across repeated `runProbe()` calls** within one `TEST_CASE` (positive run,
  sabotaged run, restored run) — the three generic-storage-buffer registrations were never released between
  calls, exhausting a 4-slot fixture capacity by the second call. Fixed by hoisting the handles out of the
  lambda and releasing all four (including the light-buffer handle) at the end of `runProbe()`.
- **Safety incident**: the capacity-exhaustion `REQUIRE()` failure aborted the TEST_CASE mid-body during
  the SECOND `runProbe()` call (the sabotaged run), before the planned restore step executed — leaving the
  PRODUCTION `cluster_lighting.slang` sabotaged on disk. Caught via a file-change notification, verified
  with `grep`, fixed immediately via `Edit`, confirmed clean via `git diff --stat`. Every subsequent test
  run in this round was followed by an explicit `git status --short` check before moving on.
- **Stale pipeline cache producing an implausible benchmark reading**: a first `sample_08_gltf_viewer
  --bench-frames 200` run gave `cpu_record_avg_ms=0.016` (13× faster than the known baseline) — traced to a
  stale/corrupted `scene_pipeline.cache` from a prior build; deleting the cache before each fresh benchmark
  invocation is now this round's own standing methodology (applied to every `*.cache` file before every
  timed run in this report).
- **Pipeline staleness across a sabotage edit**: Slang compiles at pipeline-BUILD time, not per-dispatch —
  the sabotaged run initially returned the unchanged, correct value until the compiled `VkPipeline` was
  explicitly rebuilt from the edited source after both the sabotage AND the restore.

## Correctness proofs

### Exact per-froxel membership (shading side)

`TEST_CASE("Clustered shading: exact per-froxel membership...")` — a probe fragment sees an "in-froxel"
Point light (closed-form `10/π` Lambertian match) and an "excluded" light entirely behind the camera (T14's
own proven exclusion case). The excluded light's channel is asserted `== 0.0F` EXACTLY, not merely small.
**Revert-discrimination proof**: sabotages `cluster_lighting.slang`'s own `if (NdotL <= 0.0)` early-out
(inverts the comparison, so every light including the in-froxel one gets skipped), rebuilds the pipeline,
re-runs — the in-froxel light's own contribution collapses to exactly `0.0F` — then restores the source,
rebuilds again, re-confirms the original passing value. 100/100 assertions pass on both drivers.

### Clustered-vs-brute-force equivalence (matrix's own required permanent harness)

`rx_evaluateBruteForceClusteredLights()` (new, `cluster_lighting.slang`) loops every light in
`gClusterLights` directly with NO froxel indirection, sharing the EXACT SAME per-light attenuation/BRDF
code (`clusterAccumulateSingleLight()`) as the clustered path — the only possible source of disagreement
between the two is WHICH lights get visited, never how a visited light's contribution is computed. A
sibling compute entry point (`csProbeBruteForce`, `test_cluster_shading_probe.slang`) exercises it.
`TEST_CASE("...matches the PERMANENT brute-force reference path...")`: 6 lights (Point ×4, one finite-range
Point, one Spot) against 5 probes at increasing depth, each seeing a DIFFERENT-sized real subset (0, 1, 3,
5, then all 6 — membership determined purely by each light's world-Z relative to the probe's fixed normal,
not hand-picked) — clustered and brute-force diffuse/specular are asserted equal within a
float-accumulation-order-only tolerance (0.01, generous against the ≤6-term sums involved); the
zero-membership probe is additionally pinned to EXACT zero in both paths (not merely mutual agreement), and
every non-empty probe asserts a non-vacuous (`>0.05`) combined contribution. 258/258 assertions pass on
both drivers, zero unfiltered Vulkan validation errors.

### Directional-stays-direct regression

Unchanged from pre-Task-15 behavior by construction (`clusterEnabled == 0` default; the additive clustered
term literally does not execute when unset) and confirmed live: `sample_09_scene_headless`'s own D17 gate
(default grid mode, directional-light-only) still compares byte-identical against the SAME committed
reference PNG this round, with no reference regeneration needed.

## Scaling numbers (100 / 1,000 / 5,000 synthetic lights)

**Mechanism**: `samples/09_scene --stress --stress-draws 2048 --stress-lights <N> --bench-frames 60`
(`addStressLights()` places `N` synthetic Point lights over the existing `--stress` cube field's own
footprint, wrapping/jittering past the field's own cell count so per-froxel occupancy grows with light
count — a deliberate dense-occupancy stress regime). `--bench-frames` mirrors
`samples/08_gltf_viewer`'s own `cpu_record_avg/min/p95/max_ms` methodology verbatim (timed around ONLY
`executor->execute()`). N=5 separate process runs per light count, median + range published (T14's own
established cross-process GPU-clock-variance methodology) — headless, `256×256`, real NVIDIA RTX 2080
(driver 580.82.07), `nice -n 10`.

| Lights | `cpu_record_avg_ms` median (N=5) | Range |
|---|---|---|
| 100 | **0.084** | 0.070 – 0.100 |
| 1,000 | **0.076** | 0.060 – 0.097 |
| 5,000 | **0.082** | 0.062 – 0.092 |

**Reading**: cost is flat within run-to-run noise across a 50× light-count increase — direct empirical
confirmation that per-pixel cost scales with lights-in-froxel, not lights-in-scene, at this scale (this
codebase's own binding performance rule). At 256×256/2048 draws the full-frame cost here is dominated by
fixed per-frame overhead (CPU submission, T14's compute-dispatch floor) rather than by shading-loop
iteration count — an honest characteristic of this measurement's own scale, not a claim that a much larger
viewport/heavier per-froxel occupancy would show the identical flatness; T20's own scope owns the CI
regression gate built on these numbers.

**Same runs, `--validate` (zero-validation-error confirmation, not the headline perf number — matches T14's
own precedent of a separate `--bench-frames ... --validate` pass for the regression check)**:

| Lights | `cpu_record_avg_ms` median (N=5) | Range | Unfiltered validation errors |
|---|---|---|---|
| 100 | 0.883 | 0.871 – 0.926 | 0 (15/15 runs) |
| 1,000 | 0.884 | 0.868 – 0.952 | 0 (15/15 runs) |
| 5,000 | 0.909 | 0.884 – 0.924 | 0 (15/15 runs) |

All 15 runs across both tables: `rc=0`, zero `[error]` log lines, zero unfiltered Vulkan validation lines
(sync validation active). Steam Deck: N/A this round — honest-manual per RC8, unchanged (Deck hardware not
yet in this task's own verification loop).

## Stage 1 baseline regression check (`samples/08_gltf_viewer`, real NVIDIA, `--bench-frames 200 --validate`)

| Scene | T13 baseline (`cpu_record_avg_ms`) | This round | Δ |
|---|---|---|---|
| DamagedHelmet | 0.219 | **0.210** | −4.1% |
| Sponza | 4.547 | **4.379** | −3.7% |

No regression (both scenes measure slightly faster, within ordinary run-to-run GPU/driver variance) —
expected, since `sample_08_gltf_viewer` never sets `clusterEnabled` on any row (its own per-frame code was
not touched this round beyond the two-line `BindlessTable::Capacities` addition), so the new clustered
branch in `standard_pbr.slang` is imported but never taken there; this check exists specifically to confirm
that the unconditional `import cluster_lighting;`/branch addition itself carries no measurable shader
compile/register-pressure cost for the untouched path. Zero unfiltered validation errors, both scenes.

## Verification — both drivers, full suite

**Lavapipe** (llvmpipe): full `ctest` suite 44/44 passed (`~36-40s`). `rx_material_gpu_tests` standalone:
77/77 test cases, 4,450/4,450 assertions. `rx_cluster_gpu_tests`: 6/6, 58,462/58,462 assertions.

**Real NVIDIA RTX 2080, driver 580.82.07**: full `ctest` suite 44/44 passed (`~85-87s`). `rx_material_gpu_tests`:
77/77, 4,450/4,450 — IDENTICAL assertion count to lavapipe (driver-independent correctness). `rx_cluster_gpu_tests`:
6/6, 58,462/58,462 — identical to lavapipe. Zero unfiltered Vulkan validation errors across every run in
this report (every `[vulkan validation]` line observed carries this codebase's own pre-documented "known
false positive" tag — SPIR-V-SourceLanguage=Slang predates-layer and separate-sampler-misclassification
categories, both already-established false-positive classes, not new ones introduced by this round).

## Commits (branch `task/t15-clustered-shading`)

- `111f8d1` — clustered Point/Spot shading plumbing (`RxDrawData` extension, bindless plumbing,
  `ClusterLightGpu` shading fields).
- `3311f56` — frame-pipeline integration: depth prepass + clustered shading (`samples/09_scene`, the
  multi-FIF `ClusterPipelines` redesign).
- `7d752a2` — value-asserted exact per-froxel membership from the shading side.
- `09a7f3d` — `--stress-lights`/`--bench-frames` scaling-numbers benchmark.
- `262937e` — permanent clustered-vs-brute-force equivalence harness.

## Concerns

- The scaling-numbers table (256×256, 2,048 draws) shows a genuinely flat cost curve dominated by
  fixed per-frame overhead rather than shading-loop iteration cost at this specific scale — a faithful
  measurement, but the ceiling this proves is narrower than "5,000 lights costs nothing at any resolution."
  T20's own regression-gate scope should consider whether a higher-resolution/higher-occupancy stress point
  is also worth gating on, not just this round's own headless-methodology numbers.
- The Filament pinned-commit discrepancy noted under Port provenance (`721ec80...` vs `0e58877c...` for the
  same nominal v1.75.0 tag, across two gate matrices written on the same day) is a pre-existing
  documentation artifact from before this task, not something this round introduced or was in scope to
  resolve — flagged for whoever next touches either matrix.
- No Steam Deck numbers this round (RC8 honest-manual convention, hardware not yet in this task's own loop).

## Fix round 1

Independent review verdict: spec PASS, quality Approved, 1 Medium + 2 Low finding — full review at
`.superpowers/sdd/2026-08-20-phase5-techniques/task-15-review.md`. The froxel-lookup design was fully
upheld (shared-`FroxelGridParams`-object construction verified bit-exact against T14, and term-for-term
against Filament's actual pinned-commit source). The pin-hash question raised in this report's own
Concerns section was independently settled by the reviewer: the true `v1.75.0` tag commit is
`0e58877c09afb1aacd09ff640f74d2adcd2a7e80` (confirmed via `git ls-remote`); `matrix-p5t07`/`matrix-p5t13`
were corrected (doc-only, main checkout) — this task's own load-bearing citation (`froxel_common.slang`'s
top comment) was already using the correct hash, so no code/citation change was needed here.

All three findings closed in-round, no deferred fixes:

### Finding 1 (Medium) — sabotage/restore crash-safety

`test_cluster_shading_gpu.cpp:563-626`'s revert-discrimination proof wrote the sabotaged
`cluster_lighting.slang` to disk and restored it only after `runProbe()` — a ~330-line function with over a
dozen of its own `REQUIRE()` calls — with no protection against any of those failing mid-sabotage and
unwinding past the restore. This is not hypothetical: this report's own "Bugs found and fixed" section
already discloses this exact incident firing once (a bindless-capacity-exhaustion `REQUIRE()` failure left
the production shader corrupted on disk until caught and fixed by hand).

**Fix**: added `SourceFileRestoreGuard`, a small RAII type constructed with the path and the ORIGINAL file
bytes (captured before any mutation) immediately before the sabotage-write. Its destructor unconditionally
rewrites the original bytes to disk unless `dismiss()` was already called — `dismiss()` is called only
after the sequence's own explicit restore-write AND its `REQUIRE()`-verified byte-identical readback both
succeed, so the normal path performs the restore exactly once (the guard's own write, not a second one from
the destructor) and ANY exit from the risk window (a `REQUIRE()` failure, an exception, an early return)
now restores the file regardless of where in that window it happens. The destructor is deliberately
best-effort (never throws) since it may run during unwinding from a fatal doctest assertion, where a second
throw would call `std::terminate()`.

**Verification (the review's own explicit ask — "verify by deliberately failing a probe assertion once and
confirming the file is byte-identical afterward (md5)")**: temporarily inserted
`REQUIRE((false && "TEMPORARY..."))` immediately after the sabotage-write (inside the guard's own risk
window, before the pipeline rebuild). Recorded `md5sum shaders/material/cluster_lighting.slang` before
running: `23b567f606b6bbc60f64db3cc3746379`. Ran the TEST_CASE — it failed at the deliberate `REQUIRE`
exactly as expected (doctest `FATAL ERROR`, 1 case failed). Re-ran `md5sum` immediately after: **same hash,
`23b567f606b6bbc60f64db3cc3746379`** — the file was restored to byte-identical content despite the fatal
failure occurring inside the risk window. Removed the deliberate failure, rebuilt, and re-ran: normal
100/100 assertions pass on both lavapipe and real NVIDIA, `git diff --stat` on the shader shows nothing.

### Finding 2 (Low) — dedicated froxel Z-slice boundary test

Added `TEST_CASE("...fragment/light pair placed exactly AT a computed froxel Z-slice boundary...")`.
Construction: `rx::scene::froxel::buildFroxelGrid()` called directly with the SAME arguments
`ClusterPipelines::addClusterPasses()` passes it internally (`cluster_lighting.cpp:284-286`) — an
independent call to the same pure/deterministic function, not a duplicated grid. Boundary index
`i = grid.countZ/2` (interior, avoiding the near/far degenerate edges); `boundaryDist =
sliceZDistance(i, grid)`. A light sits `delta=1.0` unit closer to the camera than the boundary with a
DELIBERATELY TIGHT `cullRadius=0.5` (unlike this file's other tests' deliberately generous radii — here the
opposite is deliberate, to keep the light's own real build-side slice assignment unambiguously confined to
slice `i-1` alone; a `REQUIRE()` on the neighboring slice's own width, not an assumption, backs this). A
probe sits EXACTLY at the boundary distance. `rx::scene::froxel::findSliceZ(-boundaryDist, grid)` — "the
build-side's own membership decision," queried rather than hand-assumed since `exp2`/`log2` need not
round-trip to an exact integer — determines which slice the boundary itself resolves to; the assertion
branches on that real verdict (closed-form nonzero if it resolves to the light's own slice, exactly zero
otherwise), so either branch is a falsifiable prediction the real GPU shading path could in principle
contradict, not a tautology.

**Result**: on this grid (`countZ=16`, `zLightNear=5`, `zLightFar=100`), `boundaryIndex=8`,
`boundaryDist≈20.236`, `lowerBound≈16.572` (slice width ≈3.66, comfortably clearing the `delta+cullRadius=
1.5` geometric precondition). `findSliceZ()` resolves the boundary to slice 8 (the HIGHER slice, not the
light's own slice 7) — the GPU probe's real diffuse contribution is exactly `0.0`, matching the CPU oracle's
prediction exactly. No CPU/GPU divergence found at this boundary. 38/38 assertions pass on both drivers,
zero unfiltered validation errors.

### Finding 3 (Low) — stale comment / unused elevated capacity

`test_standard_pbr_punctual_gpu.cpp`'s fixture comment claimed "this file's own cluster-shading TEST_CASEs
(below) register REAL buffers" into `genericStorageBuffers`/`clusterLightBuffers` — false; this file
contains only Directional/Point/Spot punctual tests, no cluster-shading scenario, and never registers a
real buffer into either slot. Corrected the comment to state the real reason (pipeline LAYOUT well-formedness
only, matching the `comparisonSamplers`/`cubeImages` precedent's own "never registers a real X" shape
immediately above it) and reduced the capacities from `8`/`4` to the minimum nonzero value `1`/`1` that
satisfies layout validity.

### Re-verification (both drivers, this round)

`rx_material_gpu_tests` (full binary, both fix files rebuilt): **78/78 test cases, 4,488/4,488 assertions —
identical on lavapipe and real NVIDIA RTX 2080 (580.82.07)**. Full `ctest` suite: **44/44 passed on both
drivers** (lavapipe ~38s, NVIDIA ~88s). Zero unfiltered Vulkan validation errors across every run this
round (`--validate` re-runs of the touched binary and the full suite, both drivers).

### Commit

- `a37eca6` — review fix round 1: crash-safe sabotage restore, froxel boundary test, stale comment.
