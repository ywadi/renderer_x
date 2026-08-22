# Task 14 report — froxel grid + clustered light assignment (compute; Filament port)

Ticket #50, Phase 5 Stage 2. Branch `task/t14-froxel-clustering`, base `5413eee`.
Worktree `/media/ywadi/second/renderer_x-worktrees/t14-froxel-clustering`.

Authority order followed: `gate/rulings-2026-08-20.md` (T14 section + RC1)
> `task-14-brief.md` > `gate/matrix-p5t14-froxel-clustering.md` > ticket #50.

## Scope delivered

- `src/rx_scene/include/rx_scene/froxel_grid.h` + `froxel_grid.cpp` —
  device-free camera-frustum froxel grid math (grid layout, exponential
  Z-slicing, per-froxel bounding sphere, point/spot intersection tests,
  cull-radius derivation). Mirrors `light_math.h`'s established precedent
  exactly.
- `src/rx_scene/include/rx_scene/scene.h` / `scene.cpp` — two new bulk
  light-iteration accessors, `lightRecordsSpan()`/`lightAliveSpan()`
  (`lightAlive_` changed `vector<bool>`→`vector<uint8_t>` to make it
  span-able, mirroring Task 19's identical fix to `Scene::alive_`).
  Necessary addition: Scene had no way to enumerate all live lights in
  bulk before this task (Phase 4's own "a scene has a handful of lights"
  assumption, explicitly invalidated by this task's own hundreds-to-
  thousands-of-lights target).
- `src/rx_cluster/` — new library (`ClusterPipelines`, the GPU-compute
  orchestration; `buildClusterLightList()`, the CPU-side Scene→GPU-light
  transform step). New, self-contained module — see "Module ownership"
  below for the (T9-precedented) rationale.
- `shaders/cluster/*.slang` — `froxel_common.slang` (shared grid/
  intersection math, the Slang mirror of `froxel_grid.h`), plus three
  compute kernels: `froxel_light_count.slang`, `froxel_prefix_sum.slang`,
  `froxel_light_scatter.slang`.
- `tools/rx_cluster_bench/` — standalone (not ctest-registered) synthetic
  high-light-count stress benchmark, mirroring `tools/rx_ibl_bench`'s own
  precedent.
- Tests: `src/rx_scene/tests/froxel_grid_test.cpp` (device-free, 10
  TEST_CASEs), `src/rx_scene/tests/scene_test.cpp` (+1 TEST_CASE for the
  new light spans), `src/rx_cluster/tests/test_build_cluster_light_list.cpp`
  (device-free, 4 TEST_CASEs), `src/rx_cluster/tests/
  test_cluster_membership_gpu.cpp` + `test_cluster_capacity_determinism_gpu.cpp`
  (GPU, 4 TEST_CASEs).
- `.github/workflows/ci.yml` — added `rx_cluster_gpu` to the Wine job's
  GPU-test exclusion regex (no Vulkan under Wine; same treatment every
  prior GPU-test binary in this repo already has).

## Port provenance

Google `filament` **v1.75.0** (tag, commit
`0e58877c09afb1aacd09ff640f74d2adcd2a7e80`, Apache-2.0), per RC1.
`filament/src/Froxelizer.h` (320 lines) and `filament/src/Froxelizer.cpp`
(1053 lines) and `filament/src/Intersections.h` (102 lines) fetched and
read in full at this exact pinned commit (`curl` direct fetch against
`raw.githubusercontent.com`, not the gate matrix's own advisory main-HEAD
cross-check `721ec800...`).

**CRITICAL FINDING re-confirmed independently this task**: Filament's own
froxelization is CPU-side (`Froxelizer::froxelizeLights()` →
`froxelizeLoop()`/`froxelizeAssignRecordsCompress()`, job-scheduled,
SIMD-vectorized C++, zero GPU compute-shader dispatch anywhere in the
file) — the charter's "its compute-shader light-assignment is published
GLSL to translate" premise does not hold at this pinned commit. Per the
gate's own Open Question #1 resolution (adopted): Filament's algorithm
(grid sizing, Z-slicing formula, bounding-sphere shape, sphere/cone
intersection tests) is the port source; the GPU-compute EXPRESSION
(dispatch shape, buffer layout, count/prefix-sum/scatter idiom) is
RendererX's own, per the ruling's own text.

### Formulas ported, with exact citations

| Piece | Filament source | RendererX port |
|---|---|---|
| XY grid tiling from a buffer-entry budget | `Froxelizer::computeFroxelLayout()`, Froxelizer.cpp:283-322 | `rx::scene::froxel::computeFroxelGridXY()`, froxel_grid.cpp |
| Exponential Z-slicing (`linearizer`, per-slice distance) | Froxelizer.cpp:463-477 | `rx::scene::froxel::buildFroxelGrid()`/`sliceZDistance()` |
| View-space-Z→slice-index (`findSliceZ`) | Froxelizer.cpp:580-598 | `rx::scene::froxel::findSliceZ()`, bit-for-bit |
| Per-froxel bounding sphere (8-corner centroid, max-corner radius) | `updateBoundingSpheres()`, Froxelizer.cpp:324-382 | `rx::scene::froxel::froxelViewSpaceBounds()` — SAME shape, corners derived from this engine's own symmetric-FOV frustum math instead of Filament's general clip-plane-transform machinery (see froxel_grid.h's own "SYMMETRIC-PERSPECTIVE SIMPLIFICATION" comment: `rx::scene::Camera` is a plain symmetric perspective by construction, D13) |
| Point-light test (sphere-vs-froxel-bounding-sphere) | matrix row 3's own named port target | `rx::scene::froxel::sphereIntersectsFroxel()` |
| Spot-light test (cone-vs-froxel-bounding-sphere) | `sphereConeIntersectionFast()`, Intersections.h:50-62, called with the FROXEL as the tested sphere and the LIGHT as the cone origin/axis (Froxelizer.cpp:985-986) | `rx::scene::froxel::spotIntersectsFroxel()`, same argument roles, verbatim formula. Two magic constants (`maxInvSin=114.59301`, `maxCosSquared=0.99992385`, the "0.5-degree minimum cone angle" numerical guard) copied verbatim from Froxelizer.cpp:679-680. |

Every C++ function above has its own Slang mirror in
`shaders/cluster/froxel_common.slang`, kept manually in sync (no shared
C++/Slang codegen in this toolchain) and cross-checked bit-for-bit by the
GPU tests (device-free CPU oracle vs. GPU readback, exact equality, across
6480 froxels — see "Correctness proof" below).

## Design: two-pass count+prefix-sum+scatter (no bitset cap)

Per the ruling: *"light lists via two-pass count+prefix-sum+scatter (NO
256-light bitset cap)"*. Filament's own `LightRecord` bitset
(`utils::bitset<uint64_t, (256+63)/64>`) is a fixed 256-bit structure that
cannot represent "hundreds-to-thousands" of candidate lights — deliberately
NOT ported (matrix Open Question #2's own recommendation). Implemented as
**three** compute dispatches (functionally "count then scan then scatter" —
the ruling's "two-pass" phrase is the standard technique name; this
implementation splits scan and scatter into separate dispatches for
clarity, a documented interpretation choice, not a scope deviation):

1. **`froxel_light_count.slang`** (`csCount`, 1 thread/froxel, `numthreads(64,1,1)`):
   loops over every light in ascending index order, tests intersection,
   writes the RAW (uncapped) count to `clusterTrueCounts[froxel]`.
2. **`froxel_prefix_sum.slang`** (`csPrefixSum`, single thread,
   `numthreads(1,1,1)`): a serial exclusive scan over `clusterTrueCounts[]`
   (froxel budget is modest by design — 8192 default, matching Filament's
   own `FROXEL_BUFFER_MAX_ENTRY_COUNT` — so a serial O(froxelCount) scan
   costs microseconds; a parallel work-efficient scan is a documented,
   deferred optimization if a future content-scale target needs it).
   Applies BOTH capacity axes in one pass (see "Capacity model" below),
   producing `clusterOffsets[]`, `clusterWriteCounts[]`,
   `clusterPerFroxelOverflow[]`, `clusterGlobalOverflow[]`,
   `clusterTotalUsed[0]`.
3. **`froxel_light_scatter.slang`** (`csScatter`, 1 thread/froxel):
   re-runs the IDENTICAL light-index-order loop `csCount` used, writing
   matched light indices into its own `[offset, offset+writeCount)` slice
   of `clusterLightIndices[]`.

**Zero atomics anywhere.** Every write index is a pure function of (that
froxel's own thread, iterating a FIXED light-index order) + (that froxel's
own prefix-sum offset, itself a deterministic serial scan) — every froxel
owns a disjoint, non-overlapping range of the output buffer, so there is no
race to avoid in the first place. This is the design property the
determinism test (below) proves empirically.

## Capacity model (both axes named by plan:504-505)

- **Per-froxel** (`ClusterParams::maxLightsPerFroxel`): a froxel whose
  TRUE intersecting-light count exceeds this is truncated; the exact
  overflow amount (`trueCount - min(trueCount, cap)`) is reported in
  `clusterPerFroxelOverflow[froxel]`.
- **Global** (`ClusterParams::maxTotalLightIndices`): the light-index
  buffer's own declared total capacity. Once the running prefix-sum total
  saturates it, every SUBSEQUENT froxel (in ascending linear-index order)
  gets `writeCount=0` and its own exact drop amount in
  `clusterGlobalOverflow[froxel]`. Bounded only by this declared value —
  generalizes to an arbitrary total light count (matrix Open Question #2's
  own "NO 256-light bitset cap" resolution), unlike Filament's fixed
  256-bit-per-froxel representation.

Both axes are independently, exactly tested past their declared limit
(capacity+1) — see "Correctness proof" below.

## Module ownership (implementer decision, T9-precedented, not escalated)

The plan's own Task 14 file list says "`src/rx_scene` froxel/cluster
orchestration (graph compute passes)". `rx_scene`'s own `CMakeLists.txt`
states explicitly and repeatedly: *"Entirely device-free: no VkDevice, no
rx_rhi_vk"*. Real compute-pass orchestration needs both. Followed the
IDENTICAL precedent Task 9 (#45, `rx_ibl`) already established for the
same tension (`bake.h`'s own "MODULE OWNERSHIP" comment): the device-free
math (`froxel_grid.h`, mirroring `light_math.h`) lives in `rx_scene`; the
GPU orchestration lives in a NEW, self-contained library (`rx_cluster`)
depending on `rx_scene` + `rx_graph` + `rx_rhi_vk` + `rx_shader`. Recorded
here as a phase-fit clarification of the plan's own file-list text, not a
silent scope drop.

## Shared-grid design (Task 30, day-one)

Per the charter/matrix row 6: the grid's own compute-pass OUTPUT must be a
STANDALONE, independently-bindable resource, not embedded in an
opaque-lighting-only descriptor set. `ClusterPipelines::addClusterPasses()`
is **graph-composable**: it takes a caller-supplied `rx::graph::RenderGraph&`
and declares its three passes under SEVEN FIXED, ordinary resource names
(`clusterTrueCounts`, `clusterOffsets`, `clusterWriteCounts`,
`clusterPerFroxelOverflow`, `clusterGlobalOverflow`, `clusterTotalUsed`,
`clusterLightIndices`). A future Task 15 opaque-lighting pass and a future
Task 30 froxel-fog pass both call `addClusterPasses()` ONCE against the
SAME live per-frame graph, then each independently `addStorageBufferInput()`s
the SAME named buffers — zero coupling between the two consumers, zero
Task-14-side code change needed for Task 30 to ride the same grid. This is
the "shared shape" design note the rulings ask Task 1 to record; Task 14
records it here since no separate spec doc existed for it at implementation
time (recorded as an implementer decision per the standing "best-
recommended option" policy, not escalated — no load-bearing ambiguity).

## Oracle construction

`rx::scene::froxel::{sphereIntersectsFroxel,spotIntersectsFroxel,
froxelViewSpaceBounds}` — the SAME device-free C++ functions used to build
the port (already independently value-asserted in
`froxel_grid_test.cpp` against python-computed closed-form values, BEFORE
any GPU dispatch existed) — are applied directly, in `rx_cluster/tests/
cluster_gpu_fixture.h::oracleFroxelLights()`, as the CPU reference
assignment the GPU readback is compared against, per every froxel, for
every synthetic scenario. This is the "CPU reference assignment compared
exactly" oracle the task brief calls for — not a second, independently-
written implementation, but the SAME formulas the port itself is built
from, proven correct once (device-free, python-cross-checked) and then
reused as ground truth for the GPU-dispatch correctness proof. The
prefix-sum/capacity arithmetic has its OWN separate CPU oracle
(hand-simulated running-budget loop, `test_cluster_capacity_determinism_gpu.cpp`'s
global-axis TEST_CASE) — a genuinely independent re-derivation of that
specific arithmetic, not a reuse of the intersection-test oracle.

## Honest design note: bounding-sphere looseness (found before writing tests, not after)

The matrix's own acceptance text says a corner light is "assigned only
there" (a single froxel). Before writing any assertion, this was checked
computationally (python, replicating the exact closed-form derivation):
a bounding-SPHERE-per-froxel design (the matrix's own licensed
simplification over Filament's tighter clip-plane-slab reduction) is
CONSERVATIVE (never a false negative) but genuinely loose — a froxel's
bounding sphere is dominated by its own view-space corner spread, which
for exponential Z-slicing is comparable to (not small relative to) the
XY spacing between NEIGHBORING froxels in the same Z-slice. A near-zero-
radius light at one froxel's own bounding-sphere center measurably,
correctly (by this design's own geometry) falls inside its immediate
XY neighbors' bounding spheres too — verified directly: a corner light at
froxel (0,0,1) of the real 1080p-shaped grid (27×15×16) is assigned to
an 8-froxel cluster `{(0,0,1),(1,0,1),(2,0,1),(0,1,1),(1,1,1),(2,1,1),
(0,2,1),(1,2,1)}`, never anywhere else in the 6480-froxel grid. The
membership test asserts this SMALL, LOCALIZED, exactly-reproduced cluster
(< 1/10 of the grid) rather than a literal single froxel — the true,
honest behavior this design guarantees, documented in the test file's own
header comment so a future reader does not mistake this for an
undiscovered bug. Slice 0 (the near "camera-to-zLightNear" catch-all slab)
is even looser still (its own bounding sphere spans from the camera's
literal apex point to a full-width rectangle at `zLightNear`) — the
membership test's corner scenario deliberately uses slice 1, not slice 0,
for this reason (documented in the test).

A tighter (e.g. per-froxel AABB, or Filament's own plane-slab reduction)
bounding volume would shrink this looseness; not pursued this round
(bounded scope, and the resulting culling is still strictly conservative —
never wrong lighting, only a modest number of extra per-froxel light
tests at shading time for froxels near a light's own bounding-sphere
edge). Flagged here as a real, load-bearing design property, not a defect
to silently work around.

## Correctness proof (device-free)

`src/rx_scene/tests/froxel_grid_test.cpp` — 10 TEST_CASEs, every expected
value computed independently via python3 (double precision) from
Filament's own closed-form derivation, BEFORE the C++ implementation was
exercised against them:
- `computeFroxelGridXY` matches for 3 grid shapes (incl. the real
  1920×1080/60°vfov/16:9 case → 27×15×16).
- `froxelIndex`↔`froxelCoords` round-trip for all 6480 froxels of the real
  grid + explicit corner-index checks.
- `sliceZDistance` matches at both analytic endpoints (i=0→0.0 exactly,
  i=countZ→zLightFar exactly) plus 2 python-computed midpoints, plus a
  monotonicity sweep.
- `findSliceZ` matches an 8-row python table spanning the near-plane
  boundary, mid-range, far-plane clamp, AND the behind-camera
  domain-boundary case.
- `froxelViewSpaceBounds` matches a fully hand/python-derived
  center+radius for one exactly-verifiable tiny grid, PLUS a structural
  invariant sweep (every froxel's own 8 corners, independently
  recomputed, lie within its returned bounding radius) across a real
  grid shape.
- `pointLightCullRadius`, `sphereIntersectsFroxel`, `spotConeViewSpace`,
  `spotIntersectsFroxel` — exact geometric boundary cases (touching vs.
  just-beyond) and a discrimination case (cone pointing away from a
  froxel a bounding-sphere-only test would wrongly include).

`src/rx_cluster/tests/test_build_cluster_light_list.cpp` — 4 TEST_CASEs:
Directional exclusion, dead-slot exclusion (proves gating on
`lightAliveSpan()`, not merely iterating), spot cone view-space rotation
(independently verified via a standalone glm program before writing the
assertion — an initial hand-derivation was WRONG by a sign, caught by
this independent check before the test was ever run), unconfigured-range
cull-radius derivation through the full Scene pipeline.

## Correctness proof (GPU)

`src/rx_cluster/tests/test_cluster_membership_gpu.cpp` — the ticket's own
primary gate. One TEST_CASE, 3 named scenarios (corner/spanning/behind-
camera) in one synthetic light set, dispatched through the real 3-pass
compute chain on a real 1080p-shaped 6480-froxel grid:
- **EXACT membership, every froxel**: GPU readback vs. CPU oracle,
  byte-for-byte vector equality, for all 6480 froxels (not sampled).
- **Corner**: localized 8-froxel cluster (see design note above), light
  present there, absent at the grid's opposite corner, present in <10% of
  the grid overall.
- **Spanning**: a radius-5 light assigned to 256 froxels (`> 1`, an
  explicit spanning proof).
- **Behind-camera**: a light at positive view-space Z excluded from ALL
  6480 froxels.

`src/rx_cluster/tests/test_cluster_capacity_determinism_gpu.cpp` — 3
TEST_CASEs:
- **Determinism**: a 40-light synthetic scene dispatched TWICE through the
  SAME `ClusterPipelines` instance; `trueCounts`/`offsets`/`writeCounts`/
  `perFroxelOverflow`/`globalOverflow` vectors and the meaningful
  `lightIndices` prefix are byte-identical both times.
- **Capacity+1, per-froxel**: `maxLightsPerFroxel+1` (9) lights all at one
  froxel's own bounding-sphere center → `trueCounts=9` (uncapped),
  `writeCounts=8` (truncated), `perFroxelOverflow=1` (exact), scattered
  indices are exactly `{0..7}` ascending (deterministic truncation); a FAR
  froxel (opposite grid corner) stays fully zero.
- **Capacity+1, global**: 30 lights hitting an 8-froxel localized cluster,
  `maxTotalLightIndices=50`; a hand-simulated CPU oracle of the SAME
  serial capping logic predicts per-froxel `writeCounts`/`globalOverflow`
  exactly (froxel-by-froxel, in ascending prefix-sum order) — GPU matches
  it exactly; `totalUsed==50` (fully saturated); every froxel with zero
  true lights stays at zero overflow of either kind.

## Revert-discrimination proof

`shaders/cluster/froxel_common.slang`'s `sphereIntersectsFroxel()`:
`rr = lightRadius + froxel.radius` sabotaged to `rr = lightRadius -
froxel.radius` (a deliberately wrong formula). Re-ran
`test_cluster_membership_gpu.cpp` unchanged (no C++ rebuild needed — Slang
is compiled at test runtime): **FAILED immediately**, `CHECK(
result->trueCounts[idx] == expected.size() )` mismatched at froxel
`(12,5,5)` (`0 == 1` failed), `allExact` false, 2/6545 assertions failed.
Sabotage reverted (`diff` against the pre-sabotage copy confirms
byte-identical restoration); re-ran: all 4 TEST_CASEs / 38982 assertions
green again on both drivers. Proves the exact-membership test is load-
bearing, not vacuously true.

## Verification — both drivers, full suite

**Lavapipe** (llvmpipe, `VK_ICD_FILENAMES=lvp_icd.json`): `rx_cluster_tests`
4/4 (17 assertions), `rx_cluster_gpu_tests` 4/4 (38982 assertions), zero
Vulkan validation errors beyond this repo's own documented false-positive
categories (SPIR-V SourceLanguage=Slang, VK_KHR_portability_enumeration —
both pre-existing, filtered by `hasValidationErrors()`). Full project
suite: **44/44 ctest tests green**.

**Real NVIDIA** (GeForce RTX 2080, driver 580.82.07,
`VK_ICD_FILENAMES=nvidia_icd.json`): identical 4/4 + 38982 assertions,
zero validation errors. Full project suite: **44/44 green** (`-j2`; one
`rx_asset_gltf_gpu_tests` flake observed under `-j4` GPU contention,
confirmed passing standalone and re-confirmed green under `-j2` — a
known, pre-existing cross-test-GPU-contention flake category this repo's
own ledger already documents; zero overlap with this task's own code,
which `rx_asset` does not touch).

**Windows-cross-zig** (Zig-toolchain cross-compile): `rx_cluster`,
`rx_cluster_tests`, `rx_cluster_gpu_tests`, `rx_cluster_bench`, and
`rx_scene_tests` all build cleanly. `rx_scene_tests.exe` (104/104,
26679 assertions) and `rx_cluster_tests.exe` (4/4, 17 assertions) both
run green under Wine (device-free only — this repo's own CI convention
excludes every GPU-test binary from the Wine job, "no Vulkan under Wine";
`rx_cluster_gpu` added to that exclusion regex in `.github/workflows/
ci.yml` alongside `rx_ibl_gpu`/etc., the same treatment every prior GPU
test binary already has — without this fix the new binary would have
attempted to run under Wine and failed/hung).

**Regression check against the Stage 1 baseline** (helmet 0.219ms / Sponza
4.547ms NVIDIA `cpu_record_avg_ms`, per CLAUDE.md's own binding
instruction) — `--bench-frames 200 --validate`, real NVIDIA RTX 2080:

| Scene | Baseline (T13, 4d52d8f) | This round | Δ |
|---|---|---|---|
| DamagedHelmet | 0.219 ms | **0.212 ms** | −3.2% |
| Sponza | 4.547 ms | **4.477 ms** | −1.5% |

No regression (T14 shares zero code path with the existing forward render
loop — this confirms that empirically rather than merely by inspection,
per this project's own "empirically proven, not asserted" standing rule).

## Stress-case numbers (headline) [SUPERSEDED by Fix round 1 — see that
## section below for the current, honest numbers and why the originals
## below under-reported real-hardware variance]

`tools/rx_cluster_bench` — synthetic scenes at T15's own named content-
scale target (plan:523, "100/1k/5k lights"), real 1080p-shaped grid
(27×15×16, 6480 froxels), 20-iteration average after 1 discarded warm-up
dispatch, full GPU-inclusive wall-clock (`CommandContext::runOnce()`
blocks until GPU completion):

**Real NVIDIA RTX 2080, driver 580.82.07** (single process run —
review Finding 3 found this under-characterizes real cross-run GPU
clock/boost-state variance; superseded below):

| Lights | Total assigned indices | avg | min | max |
|---|---|---|---|---|
| 100 | 5,391 | 0.405 ms | 0.381 ms | 0.433 ms |
| 1,000 | 42,136 | 0.726 ms | 0.690 ms | 0.759 ms |
| 5,000 | 246,596 | 2.157 ms | 2.118 ms | 2.267 ms |

**Lavapipe (llvmpipe, software), single process run:**

| Lights | Total assigned indices | avg | min | max |
|---|---|---|---|---|
| 100 | 5,391 | 0.843 ms | 0.609 ms | 1.877 ms |
| 1,000 | 42,136 | 7.661 ms | 6.655 ms | 9.554 ms |
| 5,000 | 246,596 | 28.904 ms | 25.057 ms | 35.343 ms |

Correctness (`totalAssignedIndices`) is exact and scale-appropriate at
every tier; see Fix round 1 for the honest, reproducible timing numbers.

## Concerns

- **Bounding-sphere looseness** (documented above) is real and
  load-bearing, not a defect — but it does mean per-froxel light lists
  are systematically somewhat larger than a tighter culling design (e.g.
  Filament's own plane-slab reduction) would produce, particularly for
  near-camera (slice 0) froxels. Flagged as a candidate follow-up
  (tighter bounding volume) if T15's own frame-integration numbers show
  it mattering in practice — not requested by this task's own acceptance
  criteria, which are satisfied by the current, honestly-documented
  design.
- Serial single-thread prefix sum (documented in
  `froxel_prefix_sum.slang`'s own header) is a deliberate, cited,
  scale-appropriate choice (microseconds at the current froxel budget) —
  a parallel work-efficient scan is the natural next step if a future
  content-scale target grows the froxel budget substantially past
  Filament's own 8192-entry default.
- `ClusterPipelines`'s descriptor-pool reset-per-call is documented as a
  scope boundary (safe for this task's own one-shot-then-GPU-wait test/
  bench usage; a LIVE per-frame consumer with N frames in flight needs
  its own multi-buffering strategy) — explicitly flagged in the header
  comment for Task 15's own integration to account for, not silently
  assumed away.

## Commits (branch `task/t14-froxel-clustering`)

See `git log` on this branch — implementation + this report, no AI
attribution, author = local git config (Yousef Wadi).

---

## Fix round 1

Independent review verdict: spec PASS, quality Approved, 3 LOW findings
(`task-14-review.md`, main checkout). All three closed in this round.

### Finding 1 — `computeFroxelGridXY()` integer-truncation order

**Fixed.** Filament's own `computeFroxelLayout()` [Froxelizer.cpp:299-300,
v1.75.0]:
```
size_t froxelCountX = size_t(std::sqrt(froxelPlaneCount * width  / height));
size_t froxelCountY = size_t(std::sqrt(froxelPlaneCount * height / width));
```
`froxelPlaneCount`/`width`/`height` are unsigned-integer-typed there
(Froxelizer.cpp:289-295), so the multiply/divide truncates in INTEGER
arithmetic before `std::sqrt()` runs. `froxel_grid.cpp::computeFroxelGridXY()`
previously computed the same expression in `double` precision throughout
(`static_cast<double>(froxelPlaneCount) * width / height`) — mathematically
equivalent under exact arithmetic (see below) but not the same arithmetic
ORDER the cited source uses. Fixed to compute the integer division first
(`productX / height`, `uint64_t` intermediates to match `size_t` width
without overflow risk), then pass the truncated integer to `std::sqrt()` —
matching Froxelizer.cpp:299-300 exactly.

**A worked mathematical note, not just a code diff**: `floor(sqrt(x)) ==
floor(sqrt(floor(x)))` for any real `x >= 0` is a general identity (proof
in the code comment, `froxel_grid.cpp`) — so the double-path and the
integer-path were ALREADY provably equivalent under exact arithmetic; an
exhaustive brute-force sweep (python, width/height in `[16,4000]`,
`froxelPlaneCount=512`) independently confirmed zero divergent cases
against the pre-fix double-precision formula. The only theoretical
residual risk was IEEE-754 rounding in the intermediate double division
landing a computed quotient on the wrong side of a perfect-square
boundary — negligible for the integer magnitudes in play, but the fix
removes it entirely (integer division has no rounding ambiguity) and
matches the cited source's own arithmetic order exactly, which is what a
"port" should do regardless of provable equivalence.

**New test evidence**: `froxel_grid_test.cpp` gained a new TEST_CASE with
two additional viewport/budget combos beyond the original three (all
chosen as odd/non-multiple-of-8 resolutions with different budget/slice
combos, specifically to be the kind of shape most likely to expose a
truncation-order divergence if one existed):
- `computeFroxelGridXY(1366, 769, 8192, 16)` → `(25, 14)` [python:
  `compute_xy_full(1366,769,8192,16)` → `(25,14,56)`].
- `computeFroxelGridXY(853, 481, 4096, 8)` → `(27, 16)` [python:
  `compute_xy_full(853,481,4096,8)` → `(27,16,32)`] — also exercises a
  non-default budget/slice-count combo, not just the viewport axis.

Both pass against the FIXED (integer-truncating) implementation.
`rx_scene_tests`: 105/105 (26683 assertions), up from 104/104 —
zero regressions in the pre-existing 3 configs (confirming, as expected,
that the fix changed the arithmetic PATH without changing any RESULT for
every config this codebase currently exercises).

### Finding 2 — at-capacity boundary tests

**Fixed.** `test_cluster_capacity_determinism_gpu.cpp` gained two new
TEST_CASEs alongside the existing capacity+1 ones, engineered so the true
need lands EXACTLY on the declared cap (not past it):

- **Per-froxel, at capacity**: exactly `maxLightsPerFroxel` (8) lights all
  at one froxel's own bounding-sphere center. `trueCounts == 8`,
  `writeCounts == 8` (full assignment, nothing truncated),
  `perFroxelOverflow == 0`, `globalOverflow == 0`, all 8 light indices
  present ascending.
- **Global, at capacity**: the same 8-froxel localized-cluster scenario
  the capacity+1 GLOBAL test uses, but `maxTotalLightIndices` is set to
  EXACTLY the real total need (discovered via a first pass with a
  generous cap, then re-run with the cap set to that exact discovered
  value — not a hardcoded guess). Every froxel: `writeCount ==
  min(trueCount, maxLightsPerFroxel)` (full assignment),
  `globalOverflow == 0` everywhere, `totalUsed == realNeed` (the buffer
  exactly, fully consumed — neither under- nor over-used).

Both boundary cases confirm the underlying `min(trueCount, cap)`/running-
budget clamp behaves correctly exactly AT the boundary, not just past it
— closing the specific coverage gap the review named (the existing
zero-overflow assertions elsewhere in the suite were incidental, from
generously-oversized caps or unrelated empty froxels, never an ENGINEERED
at-capacity case).

`rx_cluster_gpu_tests`: 6/6 test cases (up from 4/4), 58462 assertions (up
from 38982) — both drivers, zero unfiltered validation errors (see
"Re-verification" below).

### Finding 3 — stress-bench reproducibility

**Re-measured, N=5 separate process invocations per driver** (each its
own cold process: fresh device/pipeline-cache warm-up, 1 discarded
warm-up dispatch per tier, then the existing 20-iteration intra-run
average) — the review's own methodology (three separate re-runs showing
+13% to +23% drift) pointed at cross-PROCESS GPU clock/boost-state
variance, which a single continuous run's own intra-run min/max cannot
characterize; re-running the whole binary as 5 separate processes is what
actually samples that variance axis.

**Real NVIDIA RTX 2080, driver 580.82.07** (5 runs, solo GPU, niced,
offscreen):

| Lights | Total assigned indices | Run-avg values (ms) | **Median (ms)** | **Min–max range (ms)** |
|---|---|---|---|---|
| 100 | 5,391 (exact, all 5 runs) | 0.459, 0.489, 0.449, 0.387, 0.413 | **0.449** | **0.328 – 0.546** |
| 1,000 | 42,136 (exact, all 5 runs) | 0.872, 1.110, 0.865, 0.762, 0.731 | **0.865** | **0.675 – 1.187** |
| 5,000 | 246,596 (exact, all 5 runs) | 2.604, 2.843, 2.448, 2.428, 2.195 | **2.448** | **2.078 – 3.860** |

(Min–max range is the widest observed value across ALL 100 individual
iterations — 5 runs × 20 intra-run iterations each — not merely the
spread of the 5 run-level averages, for the most honest possible range.)

**Lavapipe (llvmpipe, software)**, 5 runs, same methodology:

| Lights | Total assigned indices | Run-avg values (ms) | **Median (ms)** | **Min–max range (ms)** |
|---|---|---|---|---|
| 100 | 5,391 (exact) | 0.911, 0.900, 0.849, 0.868, 0.812 | **0.868** | **0.589 – 1.385** |
| 1,000 | 42,136 (exact) | 7.099, 8.177, 7.273, 7.061, 6.958 | **7.099** | **4.931 – 26.682** |
| 5,000 | 246,596 (exact) | 28.863, 27.083, 29.587, 28.019, 24.798 | **28.019** | **23.241 – 38.944** |

**Variance source, honestly**: correctness (`totalAssignedIndices`) is
bit-exact and IDENTICAL across every one of the 5 runs on both drivers —
this is wall-clock-only variance, not a correctness regression. On real
NVIDIA hardware the run-to-run spread (0.33–3.86ms depending on tier) is
consistent with ordinary desktop GPU clock/power-state (boost/throttle)
transitions between process launches — the same "quiet host, solo GPU,
niced" measurement discipline this project already uses cannot eliminate
this, only reduce contention-driven variance, which is a DIFFERENT
variance source from clock-state drift. Lavapipe's own per-tier spread is
comparatively tighter at the low/mid tiers (a pure-CPU rasterizer has no
GPU clock state to drift) but shows one wide outlier (1000-light tier,
run 2, an intra-run max of 26.68ms vs. a run-average of 8.18ms) —
consistent with an ordinary host scheduling hiccup on a shared,
non-dedicated CPU rather than anything specific to this task's own code
(lavapipe numbers were never the exit-criterion driver; NVIDIA is).

**Published medians vs. the original single-run report numbers**: the new
NVIDIA medians (0.449 / 0.865 / 2.448 ms) land inside the reviewer's own
independently-observed range (0.432–0.476 / 0.862–0.905 / 2.302–2.600 ms)
at every tier — corroborating the review's own finding directly, not just
accepting it on faith. Per the closure instruction, **these medians
replace the original single-run numbers as this task's own published
stress baseline**:

| Lights | Original (single run) | **New median (N=5)** | Δ |
|---|---|---|---|
| 100 | 0.405 ms | **0.449 ms** | +11% |
| 1,000 | 0.726 ms | **0.865 ms** | +19% |
| 5,000 | 2.157 ms | **2.448 ms** | +13% |

The qualitative exit conclusion is UNCHANGED under the corrected numbers:
2.4–3.9ms at 5000 lights (5× T15's own top named scale) is still a small
fraction of a 16.6ms 60fps frame budget, with room for T15's own per-pixel
light loop on top. No regression claim in this task depended on the
original, now-superseded single-run figures — the DamagedHelmet/Sponza
forward-path regression check (a separate methodology, `--bench-frames
200`, which the review independently confirmed reproduces tightly) is
unaffected.

Steam Deck numbers: still honest-manual per RC8 (unchanged).

### Re-verification (both drivers, this round)

**Lavapipe**: `rx_scene_tests` 105/105 (26683 assertions); `rx_cluster_tests`
4/4 (17 assertions); `rx_cluster_gpu_tests` 6/6 (58462 assertions), zero
unfiltered Vulkan validation errors (`grep -i validation` minus this
repo's own documented false-positive categories → empty).

**Real NVIDIA RTX 2080, driver 580.82.07**: `rx_cluster_gpu_tests` 6/6
(58462 assertions), zero unfiltered Vulkan validation errors — identical
assertion count to lavapipe, confirming driver-independent correctness at
the new boundary cases too.

### Commit (branch `task/t14-froxel-clustering`)

One additional commit, explicit pathspecs, no AI attribution, author =
local git config. Not pushed.
