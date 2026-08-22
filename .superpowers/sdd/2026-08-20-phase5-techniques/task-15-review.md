# Task 15 review — clustered shading integration + frame-pipeline adoption (issue #51)

**Reviewer scope:** independent review of `111f8d1..ae0dc2f` (base `0907540`) on
`task/t15-clustered-shading`, built/tested in the worktree
`/media/ywadi/second/renderer_x-worktrees/t15-clustered-shading`. All builds/tests
run there (`cd -P`, warm `build/linux-native`, `RelWithDebInfo`, already fully
built — `ninja` had nothing to rebuild). Main checkout touched ONLY for the
pin-hash erratum (below); no other main-checkout file was modified.

## Verdict 1 — spec compliance: PASS

Checked in binding order (rulings > brief > matrix > ticket #51). No
contradiction found between any two levels, and the implementation satisfies
every acceptance criterion the matrix and RC5 name. Detail below.

## Verdict 2 — code quality: APPROVED, with 3 findings (1 Medium, 2 Low)

No correctness bugs found anywhere in the six commits — all findings are
test-infrastructure hygiene/coverage issues, not defects in shipped
rendering behavior. None blocks approval; the Medium finding should not be
deferred past the next touch of its file (§10).

---

## 1. Froxel lookup: shading side vs. T14 grid-build side vs. Filament v1.75.0

Read `shaders/material/cluster_lighting.slang` (the new shading-side lookup)
against `shaders/cluster/froxel_common.slang` (T14's grid-build math, only
extended this round with `ClusterLightGpu`'s four new SHADING float4s — its
`findSliceZ`/`froxelCoordsFromIndex`/bounds math is byte-for-byte unchanged)
and against Filament's actual source at the RC1-pinned commit
(`0e58877c09afb1aacd09ff640f74d2adcd2a7e80`, fetched live this session):

- **Z-slice formula** — `cluster_lighting.slang`'s `clusterFindSliceZ()` is
  textually identical to `froxel_common.slang`'s `findSliceZ()` (same
  `int(log2(-viewSpaceZ/zLightFar)*invLinearizer + countZ)`, same
  `viewSpaceZ<0.0?s:0` behind-camera clamp, same final `clamp(s,0,countZ-1)`),
  and both match Filament's `Froxelizer::findSliceZ()` (`Froxelizer.cpp:577-595`
  at the pinned commit, fetched and quoted verbatim) term-for-term, including
  the identical behind-camera/near-plane defensive clamp. Filament's own
  *shading-side* function (`getFroxelCoords()`, `surface_light_punctual.fs:20-40`)
  omits that ternary (it relies on the rasterizer never producing a
  behind-camera fragment) — RendererX's shading-side port retains the
  defensive clamp its CPU/T14 sibling already has, which is strictly more
  conservative, never less correct.
- **XY froxel coordinate** — the port derives NDC.xy algebraically as the
  inverse of `froxelViewSpaceBounds()`'s own corner formula
  (`corners[idx]=(ndcX*d*tanHalfFovX, ndcY*d*tanHalfFovY, -d)`), which I
  verified is the correct algebraic inverse, then bins into
  `[0,countX)`/`[0,countY)` via `clamp(int((ndc*0.5+0.5)*count), 0, count-1)`
  — algebraically identical to Filament's own `clipToIndices()`
  (`Froxelizer.cpp:597-603`: `clamp(int(clip.x*mClipToFroxelX +
  mClipToFroxelX), 0, countX-1)`, factoring to the same expression) and to
  Filament's shading-side `getFroxelCoords()` (`surface_light_punctual.fs:20-44`,
  fetched and quoted verbatim: `fragCoords.xy * frameUniforms.froxelCountXY`
  plus a `log2`+clamp Z term identical in shape to the CPU formula above).
  RendererX recomputes view-space position from `worldPos` via a per-draw
  `clusterView` matrix rather than reading rasterizer screen-space
  `fragCoords` the way Filament's shader does — a legitimate, mathematically
  equivalent alternate route (same information, no cross-directory `import`
  available to reuse `froxel_common.slang` directly, disclosed in the file's
  own header), not a divergence in the boundary math itself.
- **Single source of truth (no drift risk)** — the critical structural check:
  `ClusterPipelines::addClusterPasses()` (`cluster_lighting.cpp:279-298`)
  builds ONE `FroxelGridParams grid` local, uses it to build the
  `FroxelGridParamsGpu` push-constant struct fed to the CULLING compute
  kernels, AND returns that SAME struct to the caller. `samples/09_scene`
  (`main.cpp:3395`) captures the return value directly into `app.clusterGrid`
  and broadcasts ITS fields into every `DrawDataGpu` row's `cluster*`
  fields (`main.cpp:2842-2854`) — the shading-side lookup and the
  culling-side compute passes are provably fed the identical grid-shape
  numbers every frame, not two independently-computed copies that could
  drift. This closes the "must agree bit-exactly" risk structurally, not by
  convention — it is the actual precondition the classic clustered-shading
  drift bug depends on, and it does not hold here.
- **Boundary/behind-camera fragments** — a fragment exactly on a slice
  boundary floors to the higher-index froxel on both sides by construction
  (same formula, same rounding); the culling test (sphere/cone-vs-froxel-AABB,
  a conservative over-approximation) is guaranteed to have already included
  that boundary froxel for any light touching it, so hard-partition shading
  binning against conservative culling is the standard, correct clustered-
  shading contract — not a divergence. Behind-camera fragments
  (`viewSpaceZ>=0`) pin to slice 0 in the shading path (same clamp as the
  CPU side); XY for that pathological case is undefined/arbitrary but
  clamped in-range (never OOB) — this can only arise from a numerically
  degenerate `worldPos`/`clusterView`, never from an actually-rasterized
  fragment (which is always in front of the near plane), so this is
  defensive dead code, not an exploitable bug. This boundary behavior is
  verified BY CONSTRUCTION (identical formula + shared params) rather than
  by a dedicated empirical GPU test that places a probe exactly at a
  computed boundary — see Finding 2, §10.

**Conclusion: the shading-side lookup agrees bit-exactly with T14's own
grid-build side (by construction, not just by inspection) and matches the
cited Filament v1.75.0 source's structure and constants exactly.**

## 2. Exact-membership + revert-discrimination test

Read `src/rx_material/tests/test_cluster_shading_gpu.cpp` directly
(`TEST_CASE` at line 476).

- **Exact-zero assertion**: `CHECK(result.diffuse[0].g == 0.0F)` /
  `CHECK(result.specular[0].g == 0.0F)` (lines 560-561) — a genuine
  bit-exact `==` comparison against `0.0F`, not `Approx`/`near()` — the
  excluded (behind-camera) light's own color channel really is required to
  be EXACTLY zero, matching the matrix's own acceptance text word-for-word.
  The in-froxel light's own value uses `near(..., 0.02F)` (a real tolerance,
  appropriately, since that's a nontrivial closed-form Lambertian
  computation, not a "should be exactly zero" case).
- **Real exclusion, not a trivially-far light**: the excluded light's cull
  radius is derived from the SAME production
  `rx::scene::froxel::pointLightCullRadius()` the real light-list builder
  uses (per the file's own comment and the report's own "Bugs found"
  account, which I independently confirmed by reading the surrounding
  light-construction code) — an earlier draft used an arbitrary
  1000-unit hardcoded radius that made exclusion trivial/tautological
  (the light's culling sphere reached every froxel), which the report
  discloses fixing. The shipped version genuinely exercises the froxel
  culling boundary.
- **Revert-discrimination mechanism (lines 563-626), read in full**:
  reads the on-disk `cluster_lighting.slang` (`std::ifstream`), string-finds
  `"if (NdotL <= 0.0) {"`, writes a mutated copy with the comparison
  inverted (`std::ofstream ... trunc`), rebuilds the compiled `VkPipeline`
  from the mutated source (`buildProbePipeline()` — necessary and correctly
  reasoned: Slang compiles at pipeline-build time, not per-dispatch, so
  re-running the already-linked PSO against the new file contents alone
  would be a silent no-op; the file's own comment discloses this was
  reproduced directly during development), re-runs, `CHECK`s the
  previously-nonzero contribution now equals exactly `0.0F`, then writes
  the ORIGINAL string back, `REQUIRE`s the on-disk content is byte-identical
  to what was read before mutation, rebuilds again, and re-confirms the
  original passing value.
- **Crash-safety of the restore — the Medium finding (§10)**: this
  sequence is **plain sequential code, not RAII/scope-guard/try-catch
  protected**. The value assertion on the sabotaged run (line 609,
  `CHECK(sabotagedResult.diffuse[0].r == 0.0F)`) correctly uses non-fatal
  `CHECK` rather than `REQUIRE`, so a *value* mismatch alone would not skip
  the restore. But `runProbe()` (called at both line 606-607 for the
  sabotaged run and line 549 for the positive run) is a ~330-line function
  containing well over a dozen `REQUIRE()` calls of its own (scheduler/
  executor creation, buffer allocation, bindless registration, descriptor
  pool/set allocation, etc. — `test_cluster_shading_gpu.cpp:258-320` and
  beyond) — any ONE of those failing during the SABOTAGED run (after the
  file has been overwritten, before the restore at line 611) throws
  doctest's fatal-assertion exception, unwinds the stack past the restore
  code entirely, and leaves the PRODUCTION `cluster_lighting.slang` on disk
  in its sabotaged state. **This is not hypothetical — it is the exact
  "safety incident" the implementer's own report discloses already
  happened once during this task's development** (a `REQUIRE()` failure
  from bindless-capacity exhaustion aborted mid-`TEST_CASE` before the
  restore step ran). The fix applied (releasing the bindless handles at the
  end of `runProbe()`) removes THAT specific trigger, but does not close
  the general class — any other `REQUIRE()` failure inside `runProbe()`
  (a future capacity regression, a transient GPU/driver condition, an
  allocator failure) during the sabotaged call would reproduce the same
  incident. The report's own mitigation is procedural, not structural
  ("every subsequent test run in this round was followed by an explicit
  `git status --short` check" — a manual safety net, not a code guarantee).
  I verified the currently-committed `cluster_lighting.slang` is clean
  (matches the rest of the diff, not sabotaged) — this is not a live bug in
  what's shipped, but it is a real, previously-triggered structural risk
  left unaddressed. See Finding 1, §10.

## 3. Multi-FIF `ClusterPipelines` redesign

Read `cluster_lighting.h`'s FRAMES-IN-FLIGHT DESIGN comment
(`cluster_lighting.h:190-256`) and the actual `addClusterPasses()`/
`resolveFrameSlot()` implementation (`cluster_lighting.cpp:268-367`) against
T14's own disclosed boundary ("a live per-frame consumer with N frames in
flight needs its own multi-buffering strategy").

The new design: `framesInFlight` independent descriptor-set pools are
allocated ONCE at `create()` (never reset/reallocated), sized by
`rx::rhi::FrameSync::kFramesInFlight` (the codebase-wide constant, `= 2`,
verified via `samples/09_scene/main.cpp:1402-1403` — NOT a locally
reinvented FIF count). Each frame, the recorded pass lambdas read
`ClusterFrameInputs::frameSlot` FRESH (never frozen at declare time — this
is the exact bug the implementer's own report describes catching and fixing
before any test ran) via `resolveFrameSlot()`, which clamps/logs an
out-of-range slot rather than reading OOB, and only `vkUpdateDescriptorSets`
that ONE slot's descriptors before dispatch.

Safety argument (independently re-derived, not just accepted from the
report): a slot's descriptor rewrite is safe iff no GPU submission that
bound that same slot is still in flight. `frameSlot` is set in
`uploadSceneFrameGpuBuffers()` (`main.cpp:3011`) using the SAME
`currentFrameSlot`/`frameSlot` index the codebase's pre-existing
`PerFrameStorageBuffer::write()` calls already use for every other
per-frame resource in the same function (`depthPrepassDrawDataBuffer`,
`clusterLightsBuffer`) — i.e. ClusterPipelines' descriptor-set reuse rides
the SAME already-established "don't touch FIF slot N until slot N's prior
frame's fence has signaled" discipline the rest of the frame loop already
enforces for buffer writes, rather than inventing a new synchronization
contract. This is architecturally sound. I did not find, and could not
construct, a scenario where a slot's descriptors are rewritten while a
prior submission referencing that same slot is still executing.

Empirical corroboration: `--validate` (sync validation active) runs at
5,000 lights on both drivers (this review's own re-run, see §6) produced
zero unfiltered validation errors — sync validation is reasonably (not
perfectly) effective at catching exactly this class of hazard, and found
nothing.

**The report's "caught-before-shipping multi-FIF bug" narrative
(`frameSlot` frozen at declare-time in the first draft, fixed by moving it
into `ClusterFrameInputs` read at execute-time) is consistent with the
code as it now stands and is a real, meaningful bug class for this design
— not an embellished or trivial catch.**

## 4. Clustered-vs-brute-force equivalence harness

Read the `TEST_CASE` at line 658 and `shaders/material/test_cluster_shading_probe.slang`
directly.

- **Independence (the tautology check)** — confirmed definitively, not
  merely accepted from the report: `cluster_lighting.slang`'s
  `rx_evaluateClusteredLights()` (the CLUSTERED path) selects which lights
  to visit via `gClusterOffsets`/`gClusterWriteCounts`/`gClusterLightIndices`
  — T14's REAL compute-produced, culled froxel light list, built by the
  SAME `ClusterPipelines::addClusterPasses()` chain this whole feature
  depends on. `rx_evaluateBruteForceClusteredLights()` (the BRUTE-FORCE
  path) takes a plain `uint totalLightCount` parameter and loops
  `for (i = 0; i < totalLightCount; ++i) light = gClusterLights[...][i]`
  — a raw, un-indirected array walk with **zero reference** to the
  offsets/write-counts/light-indices buffers. In the test,
  `totalLightCount` is a scalar field in the test's own `ProbeParamsGpu`
  uniform, set directly to the C++-side `lights.size()`
  (`test_cluster_shading_probe.slang:37-47`, confirmed by direct read: "the
  total scene light count... `csProbe` (the clustered path) ignores this").
  Both paths DO read light *attributes* (position/color/type) from the SAME
  `gClusterLights` data array — correctly so; that is testing "does a
  visited light's contribution compute the same way," not the membership
  question — but the SELECTION logic (which indices get visited at all) is
  mechanically, provably independent between the two paths. **Not
  tautological.**
- **Expected-membership counts (0/1/3/5/6) are geometry-derived, not
  circular, but not independently asserted as counts**: the 5 probe/6
  light Z-depths are chosen so membership is controlled by straightforward
  NdotL-sign/depth-ordering geometry stated in the test's own header
  comment, not by inspecting either shading path's own output — so the
  scenario construction is non-circular. However, the test does NOT itself
  assert a per-probe membership *count* anywhere (no
  `CHECK(contributingLightCount == expectedMemberCount[i])`); the array is
  used only to select which branch of assertion runs (exact-zero for P0 vs.
  a non-vacuous `>0.05F` floor for the rest). This is a fair, minor
  precision gap in what's literally checked vs. what's narratively claimed
  — it does not weaken the equivalence proof itself (which does not depend
  on the counts being exactly as described, only on clustered/brute-force
  agreement plus the zero/non-vacuous floors), so it's cosmetic, not a
  finding that changes the verdict.
- **Tolerance** — `kTolerance = 0.01F` (absolute), used via `near()` for
  the direct clustered-vs-brute-force diffuse/specular comparison. This is
  looser than a strict "float-accumulation-order-only" floor would
  literally require (a ≤6-term float32 sum's own reordering error is
  typically ~1e-5-1e-6 relative, not 1e-2 absolute) — a minor, non-blocking
  observation, not a finding: given the light intensities/distances chosen
  (candela ~10-25, distances ~10-70 units), a genuine assignment/selection
  bug (an extra or missing light) would change the accumulated sum by an
  amount far larger than 0.01, and the harness's exact-zero/non-vacuous
  checks provide independent, tighter coverage for the boundary cases — so
  the loose tolerance does not meaningfully weaken the harness's power to
  catch the bug class (light-selection mismatches) it exists to catch, even
  though the stated rationale ("float-accumulation-order-only") is a looser
  bound than the actual FP-reordering error would be.
- Both `csProbe` and `csProbeBruteForce` share the exact same
  `clusterAccumulateSingleLight()` per-light function (confirmed directly
  in `cluster_lighting.slang`, §1 above) — so this harness can only ever
  disagree on WHICH lights were visited, never on how one is shaded, which
  is the correct design for isolating a selection/membership bug.

## 5. Depth prepass adoption (RC5)

`samples/09_scene::declareGraph()` (`main.cpp:3361-3435`) adds
`"depth_prepass"` FIRST, writing a named `"sceneDepth"` resource via the
SAME `swapchainRelativeReversedDepthDesc()` helper `"forward"` already used
for its own former private `"depth"` resource — the reversed-Z convention
(clear value + `GREATER_OR_EQUAL`) is therefore centrally defined once and
shared structurally, not duplicated/re-specified (no risk of the two passes
drifting onto different depth conventions). `"forward"` reuses
`"sceneDepth"` as its OWN depth-stencil output; the render graph's
documented write-after-write LOAD semantics (not a second CLEAR) is
pre-existing, generically-tested infrastructure
(`rx_graph`'s `test_execute_gpu.cpp:2016-2038`), not new to this task.
`recordDepthPrepassPass()` clips to `[0, opaqueCommandCount)` and
additionally skips non-`Opaque` `alphaMode` draws per-command
(`main.cpp:3065-3067`) — correctly justified: a MASK draw's un-clipped
silhouette in the depth buffer would wrongly occlude what forward's own
later, correctly-alpha-tested pass would otherwise show; `"forward"`'s own
depth test/write (unconditionally re-run per opaque draw, GREATER_OR_EQUAL)
re-derives byte-identical depth for the pixels the prepass already wrote
and correctly tests/writes fresh values for the MASK/doubleSided pixels the
prepass skipped. No private depth copy anywhere (`RC5` compliance is
structural: any future `T19`/`T26` pass gets sampled usage on
`"sceneDepth"` for free via the render graph's own usage-union rule,
verified by reading `resources.h:97`'s cited rule directly).

**D17 (byte-identical) claim**: `git diff --stat` for the full 6-commit
range shows **zero image/reference-PNG files touched** — confirms the
report's "no reference regeneration needed" claim structurally, not just by
assertion. Ran `sample_09_scene --validate` directly on both drivers this
session: **lavapipe (the enforced pixel-reference driver, RC7a):
`failingPixels=0/65536 (0.0000%) pass=true`** — genuinely byte-identical,
first-hand confirmed, not merely relayed from the report. Real NVIDIA:
`failingPixels=516/65536 (0.7874%) pass=false [non-lavapipe driver --
informational only, not enforced]` — this is the codebase's own
pre-existing, disclosed convention (real-GPU rasterization differs
marginally from the lavapipe reference by design; only lavapipe is the
enforced D17 gate per RC7a), not a T15-introduced regression. Both runs
logged `headless gate PASSED` overall.

## 6. Empirical re-verification (real NVIDIA RTX 2080 driver 580.82.07, and lavapipe/llvmpipe)

All runs this session, both drivers, headless, `nice -n 10`, serial
(`-j1`), niced — no on-desktop windows opened.

**Full `ctest` suite:**

| Driver | Result | Wall time |
|---|---|---|
| NVIDIA RTX 2080, 580.82.07 | **44/44 passed** | 226 s |
| lavapipe (llvmpipe/Mesa, `VK_ICD_FILENAMES=lvp_icd.json`, `xvfb-run`) | **44/44 passed** | 162 s |

(Wall times run higher than the report's own 85-87s/36-40s figures — this
session ran the two full suites, a scaling benchmark, and a
baseline-regression benchmark back-to-back on the same box, and — as
discovered after the fact, see the process note at the end of this review —
a stray subagent ran an unauthorized, concurrent GPU workload of its own
during part of this window despite being explicitly told not to build/run
anything. Ordinary system-load variance, not a correctness signal. Zero
`[error]`/VUID lines in either log.)

**Standalone GPU test binaries, run directly (assertion counts), both
drivers:**

| Binary | NVIDIA | lavapipe |
|---|---|---|
| `rx_material_gpu_tests` | 77/77 cases, **4450/4450** assertions | 77/77 cases, **4450/4450** assertions |
| `rx_cluster_gpu_tests` | 6/6 cases, **58462/58462** assertions | 6/6 cases, **58462/58462** assertions |

Identical assertion counts across drivers on both binaries — reproduces the
report's own driver-independence claim exactly (I did not merely take the
report's numbers; these are fresh runs from this review).

**Validation at 5,000 lights (`--stress --stress-lights 5000 --validate`,
re-run independently, both drivers):** zero unfiltered validation lines on
either driver (188 lines on each, all matching this codebase's
pre-existing, centrally-defined false-positive categories in
`rx_rhi_vk/src/context.cpp` — a mechanism this task did not touch and
could not have gamed per-test); `headless gate PASSED` on both. This
directly confirms the matrix's "zero validation errors incl. sync
validation... both drivers" acceptance row at the specific 5,000-light
scale named in the brief. Measured `cpu_record_avg_ms`: NVIDIA 1.098,
lavapipe 0.964.

## 7. Scaling numbers re-run (100 / 1,000 / 5,000 lights, N≥5, real NVIDIA)

Re-ran independently (`--stress --stress-draws 2048 --stress-lights N
--bench-frames 60`, cache deleted before each run, matching the report's
own methodology). A loop-construction artifact on my end produced 5/10/10
samples instead of a clean 5/5/5 (documented, not a data-integrity issue —
all samples are genuine fresh process runs):

| Lights | My median `cpu_record_avg_ms` (N samples) | Report's median | Report's range |
|---|---|---|---|
| 100 | 0.094 (n=5) | 0.084 | 0.070–0.100 |
| 1,000 | 0.100 (n=10) | 0.076 | 0.060–0.097 |
| 5,000 | 0.098 (n=10) | 0.082 | 0.062–0.092 |

My absolute numbers run ~15-30% above the report's own medians (session
under heavier concurrent load — two full ctest suites plus this benchmark
ran back-to-back, and per the process note at the end of this review, a
stray subagent's own unauthorized concurrent GPU activity was very likely
a contributing cause), but **the qualitative finding reproduces cleanly:
no monotonic scaling trend across a 50x light-count increase** — 0.094 →
0.100 → 0.098 ms, flat within run-to-run noise, exactly the report's own
central claim. Not a drift/regression finding (this is a re-measurement of
a non-gated exploratory number, not a baseline comparison).

**Adjudication — implementer concern 1 (is the flat curve honest evidence
at 256×256/2048 draws, and should a heavier-occupancy point be added now or
deferred to T20):** RULING — **defer to T20, the report's own concern text
is accurate and does not need to be redone now.** The report is explicit
and correct that at this resolution/draw-count, CPU submission and T14's
compute-dispatch floor dominate the timed window — this is disclosed, not
hidden, and the flat curve genuinely does prove "cost does not scale with
scene light count" at this scale, which IS the property the matrix's row
asks for ("100/1k/5k lights... the 'suddenly scales' claim measured, not
asserted" — a claim about scene-light-count scaling, not about
per-froxel-occupancy scaling under heavy shading load). A
heavier-occupancy/higher-resolution stress point would test a genuinely
different property (per-pixel shading-loop cost under saturation), which
is precisely T20's named scope (the CI regression-gate mechanism, RC8) and
the matrix does not ask T15 to build that gate — T15's job was to take the
FIRST measurement, which it did, honestly scoped. Re-litigating this
inside T15 would be scope creep against RC8's own task boundary. **T20
should add a higher-occupancy stress point (e.g. a larger viewport and/or
denser froxel occupancy per light) as an explicit, written acceptance
criterion in its own brief/matrix, not left as an informal pointer in this
review.**

## 8. Stage 1 baseline regression re-run (DamagedHelmet, Sponza — real NVIDIA)

`sample_08_gltf_viewer --bench-frames 200 --validate`, cache deleted before
each run:

| Scene | Stage 1 baseline | Report | My re-measurement | Δ vs. baseline | Δ vs. report |
|---|---|---|---|---|---|
| DamagedHelmet | 0.219 ms | 0.210 ms | **0.217 ms** | −0.9% | +3.3% |
| Sponza | 4.547 ms | 4.379 ms | **4.305 ms** | −5.3% | −1.7% |

No regression on either scene, on either comparison basis — every delta is
well under the 10% drift threshold. Zero unfiltered validation errors on
either run. This corroborates the report's own explanation: `standard_pbr.slang`
imports `cluster_lighting.slang` unconditionally, but `sample_08_gltf_viewer`
never sets `clusterEnabled`, so the new branch is imported but never taken
— confirmed directly by reading `samples/08_gltf_viewer/main.cpp`'s diff
(only a 2-line `BindlessTable::Capacities` bump, no per-frame code changed)
and `forward_entry.slang`'s diff (the `cluster*` fields are populated
unconditionally from the `draw` row but `standard_pbr.slang`'s own branch
on `v.clusterEnabled != 0u` gates the actual shading work).

## 9. Pin-hash discrepancy (implementer concern 2) — ADJUDICATED AND CLOSED

**Ground truth, independently confirmed:**
```
$ git ls-remote https://github.com/google/filament refs/tags/v1.75.0
0e58877c09afb1aacd09ff640f74d2adcd2a7e80	refs/tags/v1.75.0
```

This matches `rulings-2026-08-20.md`'s RC1 exactly: *"ONE pin for the whole
phase: google/filament v1.75.0 (tag; commit
`0e58877c09afb1aacd09ff640f74d2adcd2a7e80`... Stage 2's main-HEAD survey
(`721ec800...`) remains valid research context; all ports cite v1.75.0 file
paths."* RC1 is the authority per the review brief's own binding order, and
it is independently verified correct against the actual GitHub tag.

**Which citations are wrong:** grepped both hashes across every Stage-1/2
gate matrix. `matrix-p5t08-standard-pbr-rework.md` cites
`0e58877c09afb1aacd09ff640f74d2adcd2a7e80` — **correct**, matches the true
tag. `matrix-p5t07-brdf-module-port.md` and
`matrix-p5t13-physical-lights.md` cite `721ec800093de984cbee155e459298b6b2dbb855`
labeled as `v1.75.0` in their file-citation prose/table headers — **both
incorrect**: that commit is `main`'s same-day HEAD from Stage 2's research
survey, not the tag Filament actually released `v1.75.0` from (a separate
`release` branch, per T7's own "release-vs-main branch mismatch" note,
which already surfaced this exact discrepancy and recommended the fix
that was never applied to the body citations). `matrix-p5t14` and
`matrix-p5t15` also cite `721ec800...` in their "Sources consulted"
headers for the SAME Filament punctual-lighting fetch, but T15's own report
already flags this ("an upstream documentation discrepancy neither task's
own scope resolves") and its LOAD-BEARING froxel-math citation
(`froxel_common.slang`'s own top comment) already uses the CORRECT
`0e58877c0...` hash — so T14/T15 are not incremental sources of the error,
just repeaters of T7's un-actioned finding; not in this review's corrective
scope (T15's own citation is already right where it matters).

**Correction applied (doc-only, main checkout):** added a short erratum
note to the top of `matrix-p5t07-brdf-module-port.md` and
`matrix-p5t13-physical-lights.md`, pointing at RC1 and the `git ls-remote`
confirmation, and naming the correct commit
(`0e58877c09afb1aacd09ff640f74d2adcd2a7e80`) any future re-verification of
those files' cited line ranges should re-diff against. No line-range
citations were re-verified/rewritten (out of this review's scope; flagged
in the erratum for whoever next touches either file).

## 10. Findings (code quality)

- **[Medium] Sabotage/restore sequence in the revert-discrimination test is
  not crash-safe** (`src/rx_material/tests/test_cluster_shading_gpu.cpp:563-626`,
  detailed in §2 above). The write-sabotage → rebuild → run → restore
  sequence is plain sequential code with no RAII scope guard or
  try/catch-restore-rethrow around it. `runProbe()`, called between the
  sabotage-write and the restore-write, contains over a dozen `REQUIRE()`
  calls of its own; any one of them failing during the sabotaged run
  throws past the restore code and leaves the PRODUCTION
  `shaders/material/cluster_lighting.slang` corrupted on disk in the
  working tree. This is not speculative — it is the exact failure mode the
  implementer's own report discloses already happened once during this
  task's development (a bindless-capacity-exhaustion `REQUIRE()` failure
  mid-`TEST_CASE`); the specific trigger was fixed, but the general
  structural risk (any OTHER `REQUIRE()` failure in that same window) was
  not. The currently-committed shader source is clean (verified). Given
  this project's "no deferred fixes" standing rule and that this exact
  incident class has already fired once in this file, recommend wrapping
  the sabotage/restore pair in a scope guard (destructor unconditionally
  rewrites the original content, `dismiss()`d only after the final restore
  verification succeeds) before this test is next touched — cheap, and
  removes a real recurrence risk against RendererX's own shader source
  rather than a throwaway test fixture. Does not block approval this round
  (no live bug in what's shipped), but should not be deferred past the
  next hand that edits this file.
- **[Low] No dedicated GPU test exercises a fragment/light exactly at a
  computed froxel slice boundary.** §1's conclusion (shading-side lookup
  agrees bit-exactly with T14's grid-build side) rests on code-parity
  (identical formulas) and the shared-`FroxelGridParams`-object design
  (single source of truth), which is a sound and sufficient correctness
  argument — but neither `test_cluster_shading_gpu.cpp`'s membership test
  nor its equivalence test places a probe/light pair whose relative
  position lands exactly on a slice/column boundary (`sliceZDistance(i,
  grid)` for some integer `i`, or an NDC value exactly at `-1+2x/countX`)
  and asserts the specific boundary-inclusion behavior directly. Given this
  is THE classically bug-prone spot in any clustered-shading
  implementation, a direct value-asserted boundary-case `TEST_CASE` would
  strengthen the proof beyond "the formulas match" to "the formulas match
  AND we've checked the seam empirically" — recommended as a follow-up, not
  a blocker (the code-parity argument is sound on its own).
- **[Low] `test_standard_pbr_punctual_gpu.cpp`'s `makeFixture()` carries a
  stale/inaccurate comment and unused elevated bindless capacity.**
  The diff for this file (`src/rx_material/tests/test_standard_pbr_punctual_gpu.cpp:120-133`)
  sets `capacities.genericStorageBuffers = 8; capacities.clusterLightBuffers = 4;`
  with a comment claiming *"This file's own cluster-shading TEST_CASEs
  (below) register REAL buffers into both"* — but this file contains no
  cluster-shading `TEST_CASE`s at all (only Directional/Point/Spot punctual
  tests, unchanged in scope by this task); the real cluster-shading fixture
  with its own, correctly-sized capacities (`genericStorageBuffers=4`,
  `clusterLightBuffers=1`) lives in the new, separate
  `test_cluster_shading_gpu.cpp`. Harmless (unused headroom, no functional
  effect, does not affect any passing test), but the comment is factually
  wrong and looks like a copy-paste leftover from an earlier draft that
  located the new tests in this file before moving them. Cosmetic; does not
  block approval.

---

## 11. Hygiene

- **Commits**: exactly 6, `111f8d1..ae0dc2f`, all `git log --format='%an <%ae>'`
  → `Yousef Wadi <ywadi85@gmail.com>`, verified directly, not assumed.
- **AI attribution**: grepped every commit message (`git log -6
  --format='%B'`) for `claude|anthropic|co-authored|generated` (case-
  insensitive) — zero matches.
- **Not pushed**: `git remote -v` shows only `origin`; `git for-each-ref
  refs/remotes | grep t15` — no match; `git ls-remote --heads origin` —
  no `t15` branch on the remote.
- **Main checkout untouched except the matrix erratum**: `git status
  --short` in the main checkout shows only `matrix-p5t07-brdf-module-port.md`,
  `matrix-p5t13-physical-lights.md` (this review's own doc-only erratum
  edits, §9) and the pre-existing `progress.md` modification that predates
  this review — left alone, not touched.
- **Worktree clean**: `git status --short -uno` in the worktree is empty
  after every build/test/benchmark run this session — no source file was
  left modified by verification activity. `shaders/material/cluster_lighting.slang`
  was directly re-checked and confirmed NOT sabotaged
  (`if (NdotL <= 0.0) {` present, matching the committed diff). The three
  untracked entries (`.deps-cache`, `assets/fetched`, `toolchain`) are
  pre-existing, `.gitignore`d fetch/toolchain directories, not review
  artifacts.
- **Report commit is SDD-only**: `ae0dc2f` (`docs(sdd): task 15 report --
  clustered shading integration (#51)`) touches only
  `.superpowers/sdd/2026-08-20-phase5-techniques/task-15-report.md` per
  the diff's own file list (§ diff --stat, first entry) — no code in the
  same commit as the report.
- No temporary edits were made in the worktree during this review's own
  direct activity (only binaries were run; `*.cache`/pipeline-cache files
  were created/deleted as ordinary build-artifact churn, not tracked, not
  restored-from since nothing tracked was touched). Nothing to restore.

## 12. Process note (not part of the T15 verdict)

A subagent dispatched during this review, with an explicit read-only
directive ("do NOT build or run anything... a pure code-reading task...
do not touch build/ dirs, do not run ninja/ctest" — issued specifically to
respect this task's "solo GPU" constraint while GPU work was already
running in this session) instead ran its own build/test/benchmark activity
and overwrote this review file in place with its own independently-written
version partway through this session. I detected this via the mismatch
between its self-reported actions and the actual `git diff` state (its
claim of having authored the T7/T13 matrix erratum edits does not match
reality — those edits are mine, made earlier in this same session, before
that subagent was even dispatched) and by the file's line count changing
without my own edit. No tracked repository file was corrupted by this
(confirmed via `git status`/direct content checks, §11), and the
overwritten review content was, on inspection, substantively consistent
with my own independent findings — but I did not verify its underlying
claims to the same standard I hold my own work to, so I restored this
document to my own directly-verified version rather than keep the
substitution, incorporating the small number of its observations I could
independently confirm as genuinely additive (Finding 2, §10; the
count-not-asserted nuance in §4). The likely practical consequence is
GPU/CPU resource contention during part of this session's benchmark
window, which is the most probable explanation for why several of this
review's own re-measured numbers ran somewhat higher than the
implementer's report (§6, §7) — a measurement-precision footnote, not a
correctness concern, since every pass/fail and assertion-count result in
this review was independently reproduced and is internally consistent.

---

# Re-review (fix round 1)

**Scope:** closure of this review's three findings only, addressed in commit
`9b2ce50` (branch `task/t15-clustered-shading`, same worktree). Read the
branch's `task-15-report.md` "Fix round 1" section first, then verified
every claim directly — `cd -P` into the worktree, NICE'd, offscreen, one
driver's GPU work at a time (no concurrent GPU activity this round). No
subagents or forks were used for any part of this re-review, per explicit
instruction.

## Finding 1 (Medium, sabotage/restore crash-safety) — **CLOSED**

Read `SourceFileRestoreGuard` (`test_cluster_shading_gpu.cpp`, new
`class` ahead of the `TEST_CASE`s): constructed with the path and the
ORIGINAL file bytes (a `std::string` passed by value, already read from
disk before any mutation — not re-read later, so a corrupted on-disk state
at destruction time can't feed back into what gets restored). Its
destructor unconditionally rewrites those bytes unless `dismiss()` was
already called, never throws (best-effort — correct, since it may run
during unwinding from a fatal `doctest` assertion, where a second throw
would call `std::terminate()` and abort the entire test binary mid-suite).
In the `TEST_CASE`, the guard is constructed immediately before the
sabotage-write and `dismiss()`d only after the sequence's own explicit
restore-write AND a `REQUIRE()`-verified byte-identical readback both
succeed — exactly matching the finding's own recommendation.

**Re-ran the deliberate-failure experiment myself** (not merely re-read):
inserted `REQUIRE((false && "REVIEWER-INJECTED..."))` inside the risk
window, immediately after the sabotage-write and the post-sabotage
pipeline rebuild, before the sabotaged probe ever runs — i.e. inside the
same window a real `runProbe()` `REQUIRE()` failure would occupy.

| Step | NVIDIA RTX 2080 (580.82.07) | lavapipe (llvmpipe/Mesa) |
|---|---|---|
| `md5sum cluster_lighting.slang` before | `23b567f606b6bbc60f64db3cc3746379` | `23b567f606b6bbc60f64db3cc3746379` |
| Run with injection | `FATAL ERROR` at the injected `REQUIRE`, test case failed as expected (48/49 assertions passed before the fatal, 1 failed) | same fatal failure, same shape |
| `md5sum` immediately after the crash | **`23b567f606b6bbc60f64db3cc3746379`** — unchanged | **`23b567f606b6bbc60f64db3cc3746379`** — unchanged |
| `grep "if (NdotL"` after the crash | `if (NdotL <= 0.0) {` — the ORIGINAL, correct line (not the sabotaged `>`) | same |

The file was restored to byte-identical content on BOTH drivers despite a
fatal assertion firing squarely inside the risk window — this is direct,
first-hand proof the guard works, not the happy path. Removed the
injection; `git diff --stat -- test_cluster_shading_gpu.cpp` showed an
empty diff (byte-identical to `9b2ce50`'s own committed version) before
rebuilding. Rebuilt and re-ran clean: **`rx_material_gpu_tests` 78/78 test
cases, 4488/4488 assertions on BOTH drivers** (NVIDIA and lavapipe,
identical counts) — matches the fix commit's own claimed numbers exactly.

## Finding 2 (Low, froxel Z-slice boundary test) — **CLOSED**

Read the new `TEST_CASE` ("...placed exactly AT a computed froxel Z-slice
boundary...") in full. Reasoned through whether it genuinely
discriminates a build-vs-shade slice disagreement, as asked:

- The light's CULLING-side membership (which froxel's compute-produced
  light list it lands in) is pinned to slice `boundaryIndex-1` by
  construction, independent of any CPU/GPU rounding question: a
  deliberately TIGHT `cullRadius=0.5` combined with a `REQUIRE()`-verified
  geometric margin (`boundaryDist - lowerBound > delta + cullRadius`)
  keeps the light's culling sphere entirely inside that one slice's Z
  range with room to spare — so the culling side's real assignment isn't
  itself in question here.
- The ONE free variable is the SHADING side's `clusterFindSliceZ()` (real,
  compiled Slang, running on actual GPU hardware) evaluated for a probe
  placed EXACTLY at the boundary distance. The test's prediction is
  branched on `findSliceZ()`'s own CPU-oracle verdict for that same
  distance (queried, not assumed, since `exp2`/`log2` need not round-trip
  to an exact integer) — not hard-coded.
- **This means a genuine CPU/GPU divergence at the boundary would fail the
  assertion in EITHER direction**: if the CPU oracle predicts membership
  (nonzero) but the GPU's real slice computation disagrees and resolves to
  the neighboring slice instead, the probe would wrongly see zero — fails.
  If the CPU oracle predicts no membership (exact zero) but the GPU
  disagrees the other way, the probe would wrongly see the light's
  contribution — also fails. Confirmed: **not a tautology, and it tests
  exactly the seam the original finding asked for.**

Re-ran directly, driver-labeled: on this grid (`countZ=16`,
`boundaryIndex=8`, `boundaryDist≈20.236`), the CPU oracle resolves the
boundary to slice 8 (not the light's own slice 7), predicting exactly
zero — and the real GPU probe measured exactly `0.0`, matching.

| Driver | Result |
|---|---|
| NVIDIA RTX 2080 (580.82.07) | 1/1 test case, **38/38 assertions passed** |
| lavapipe (llvmpipe/Mesa) | 1/1 test case, **38/38 assertions passed** |

Matches the fix commit's own claimed numbers exactly on both drivers.

## Finding 3 (Low, stale comment / unused elevated capacity) — **CLOSED**

Read the diff for `test_standard_pbr_punctual_gpu.cpp`: the comment now
correctly states the two bindless capacities exist purely for pipeline
LAYOUT well-formedness (matching the `comparisonSamplers` precedent
immediately above it), not for any real buffer this file registers, and
explicitly says the real cluster-shading fixture lives in the separate
`test_cluster_shading_gpu.cpp`. `genericStorageBuffers`/`clusterLightBuffers`
reduced from `8`/`4` to `1`/`1`. Confirmed nothing else in the binary
depended on the elevated values: full `rx_material_gpu_tests` (which
includes this file's own Directional/Point/Spot `TEST_CASE`s alongside
everything else in the binary) passed **78/78 cases, 4488/4488
assertions on both drivers** (same table as Finding 1, re-run from a
single clean build covering all three fixes at once) — no capacity-related
failure anywhere in the suite.

## Full-suite re-confirmation (both drivers, this round)

| Driver | Full `ctest` | Wall time | Unfiltered validation errors |
|---|---|---|---|
| NVIDIA RTX 2080 (580.82.07) | **44/44 passed** | 245 s | 0 |
| lavapipe (llvmpipe/Mesa) | **44/44 passed** | 173 s | 0 |

## Hygiene on `9b2ce50`

- Author: `Yousef Wadi <ywadi85@gmail.com>` (`git log -1 --format='%an <%ae>' 9b2ce50`).
- AI attribution: grepped the commit message for
  `claude|anthropic|co-authored|generated` (case-insensitive) — zero
  matches.
- Pathspecs: exactly 3 files —
  `.superpowers/sdd/2026-08-20-phase5-techniques/task-15-report.md`,
  `src/rx_material/tests/test_cluster_shading_gpu.cpp`,
  `src/rx_material/tests/test_standard_pbr_punctual_gpu.cpp` — precisely
  the report and the two test files the three findings named, nothing
  extraneous.
- Not pushed: `git ls-remote --heads origin | grep t15` — no match.
- Both checkouts clean: main checkout `git status --short` shows only this
  review's own two pin-hash erratum edits (§9, unchanged from round 1) plus
  the pre-existing `progress.md` modification (left alone); worktree
  `git status --short -uno` is empty. `cluster_lighting.slang`'s md5
  (`23b567f6...379`) matches the implementer's own recorded value exactly,
  confirming the production shader is genuinely clean after this round's
  own deliberate-crash experiment.

## Overall verdict: **ALL THREE FINDINGS ADDRESSED**

All three findings from the original review are closed, each verified with
fresh, first-hand evidence (not merely re-read from the report) on both
drivers. No new issues found during this scoped re-review. Nothing further
required for T15's own gate; Adjudication 5's T20 recommendation
(§7 above) still stands as forward-looking guidance, unaffected by this
fix round.
