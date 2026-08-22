# Task 14 review — Froxel grid + clustered light assignment, Filament port (issue #50)

Independent reviewer round. Commits under review: `2947037`
(`feat(rx_scene,rx_cluster,shaders/cluster): froxel grid + clustered light
assignment (#50)`) + `02da8bd` (`chore: p5 task 14 report`), base
`5413eee`, branch `task/t14-froxel-clustering`, worktree
`/media/ywadi/second/renderer_x-worktrees/t14-froxel-clustering`. Reviewer
did not write this code. Order of authority followed: gate rulings
(`rulings-2026-08-20.md`, T14 + RC7) > brief (`task-14-brief.md`) > gate
matrix (`matrix-p5t14-froxel-clustering.md`) > ticket (#50).

Every cited Filament formula was independently re-derived, not trusted
from the report: `google/filament`'s `Froxelizer.cpp`/`Froxelizer.h`/
`Intersections.h` fetched fresh at the pinned tag `v1.75.0`
(tag object confirms `0e58877c09afb1aacd09ff640f74d2adcd2a7e80`). The full
203KB diff was read top to bottom, including every `.slang` kernel and the
C++ orchestration/oracle code. Both drivers were built/run live in the
worktree (warm build dir, `ninja: no work to do`); the revert-
discrimination proof was independently reproduced byte-for-byte (sabotage
→ quantified fail → byte-identical restore → green); the stress bench and
the helmet/Sponza regression benchmarks were re-measured on real NVIDIA
hardware.

## Verdict 1 — Spec compliance: **PASS** (matrix-p5t14-froxel-clustering.md, rulings T14 + RC7)

Every matrix row is delivered and value-asserted, and every cited formula
independently checks out against a fresh v1.75.0 fetch:

- **Grid layout / exponential Z-slicing** — `computeFroxelLayout()`
  (Froxelizer.cpp:283-322) and the `linearizer`/`mDistancesZ[i]` derivation
  (Froxelizer.cpp:463-477) are reproduced correctly in
  `froxel_grid.cpp::computeFroxelGridXY()`/`buildFroxelGrid()`/
  `sliceZDistance()`. `findSliceZ()` (Froxelizer.cpp:580-598) is a genuine
  bit-for-bit port (only `fast::log2`→`std::log2`, a documented, harmless
  substitution) — confirmed directly against the fetched source, not the
  report's citation.
- **Per-froxel bounding sphere** — `froxelViewSpaceBounds()` reproduces
  `updateBoundingSpheres()`'s (Froxelizer.cpp:324-382) 8-corner-centroid/
  max-corner-radius SHAPE exactly; the symmetric-FOV corner derivation
  (vs. Filament's general clip-plane-transform machinery) is a necessary,
  well-documented, D13-licensed simplification, not an unexplained
  deviation.
- **Point/spot intersection tests** — `sphereIntersectsFroxel()` is a
  correct sphere-sum-of-radii test. `spotIntersectsFroxel()`'s cone
  refinement is a **verbatim** port of `sphereConeIntersectionFast()`
  (Intersections.h:50-62), same argument roles as Filament's own call site
  (Froxelizer.cpp:985-986) — confirmed line-for-line, including the two
  magic 0.5°-guard constants (`114.59301`/`0.99992385`,
  Froxelizer.cpp:679-680/693-694).
- **Licensed-scope boundary respected in code** — read every `.slang`
  kernel: the geometric/algorithmic formulas above are ported faithfully;
  the compute **expression** (three dispatches, `numthreads`, push-
  constant/binding layout, the count→scan→scatter idiom) is original
  engineering with no GLSL compute analogue to have copied from (the gate's
  own CRITICAL FINDING, independently re-confirmed: zero
  `compute`/`dispatch`/`workgroup` GPU-stage markers anywhere in
  Froxelizer.cpp). The ruling's boundary is respected, not blurred.
- **Two-pass count+prefix-sum+scatter, no 256-light bitset cap** — three
  real dispatches (`froxel_light_count.slang`/`froxel_prefix_sum.slang`/
  `froxel_light_scatter.slang`), capacity bounded only by
  `ClusterParams::maxTotalLightIndices`, never by a fixed bitset width —
  confirmed by source read and by a 246,596-index scatter at 5000 lights
  (see stress numbers below).
- **Device-free math + GPU membership + capacity+1** — all three
  acceptance-sketch bullets (brief:12-19) are met: `froxel_grid_test.cpp`
  (10 device-free TEST_CASEs, python-cross-checked), the GPU exact-
  membership sweep (every one of 6480 froxels, corner/spanning/behind-
  camera named scenarios), and capacity+1 on both the per-froxel and
  global axes with exact overflow counters.
- **Module split (rx_ibl precedent)** — `rx_scene/CMakeLists.txt` links
  only `rx_core rx_asset rx_task` (confirmed by direct read); `rx_cluster`
  is the new device-touching library, depending on `rx_scene` (one-way).
  rx_scene gained zero device dependency.
- **Graph composability for Task 30** — `addClusterPasses()` declares its
  three passes under seven ordinary named resources; the test fixture's
  own capture pass proves a wholly separate, later-added pass can bind
  every one of them via `addStorageBufferInput(name)` with zero coupling —
  this is not just a design claim, it is exercised by every GPU test in
  the suite.
- **CI Wine exclusion** — mechanically re-verified: `ctest --test-dir
  build/linux-native -N` with the diff's updated exclusion regex applied
  lists `rx_cluster_tests` only; `rx_cluster_gpu_tests` is correctly
  excluded (matches the pre-existing `rx_ibl_gpu`/etc. treatment).

No matrix row is missing, stubbed, or silently narrowed.

## Verdict 2 — Code quality: **Approved — 3 LOW findings, no blocking issues**

Clean, well-factored, extensively (sometimes excessively) documented code.
The oracle/port separation, the zero-atomics scatter design, and the
capacity-axis bookkeeping are all correct by direct construction, not just
by test luck. See findings below; none require a fix round to close this
task, though Finding 2 is cheap enough to fold into T15's own touch of
this code if convenient.

---

## Adjudications (as specifically requested by the dispatch)

### Adjudication 1 — Correctness oracle independence (T10-lesson check)

**Genuinely independent — not tautological.** `cluster_gpu_fixture.h::
oracleFroxelLights()` calls the SAME C++ functions
(`sphereIntersectsFroxel`/`spotIntersectsFroxel`/`froxelViewSpaceBounds`)
that also informed the Slang port, but the GPU kernels are a **separate,
hand-written Slang implementation** (`shaders/cluster/froxel_common.slang`)
compiled to SPIR-V and executed on real hardware — not the same function
called twice, and not a codegen twin (the file's own header: "no shared
C++/Slang codegen in this toolchain"). This differs materially from the
T10 case (a test's own hardcoded "expected" value using the exact same
buggy formula the shader itself hardcoded — a single mistake duplicated
by the same author into both places with no independent check). Here:

1. I independently re-derived every cited Filament formula from a fresh
   v1.75.0 fetch (not from the implementer's citations) and confirmed both
   the C++ oracle and the Slang kernel match the real reference — closing
   the "same misunderstanding baked into both" risk the T10 lesson warns
   about.
2. I independently reproduced the revert-discrimination proof: sabotaging
   ONE formula in the Slang file only (`froxel_common.slang`'s
   `sphereIntersectsFroxel`, `rr = lightRadius + froxel.radius` →
   `... - froxel.radius`) produced an immediate, exact failure — mismatch
   at froxel `(12,5,5)` idx=2172, `0 == 1`, 2/6545 assertions failed,
   byte-for-byte identical to the report's own numbers. Restored via `git
   checkout --`, confirmed byte-identical (`git diff` empty), re-ran:
   19466/19466 assertions green. This is direct, reproduced proof the two
   implementations are independent artifacts, not a shared vacuous check.

### Adjudication 2 — Conservativeness disclosure (implementer concern 1)

**Correctly adjudicated by the implementer; no tightening needed this
round.** Two points, both independently confirmed:

- **The looseness is real and Filament's own source is genuinely
  tighter**, more so than the report even states. Reading
  `froxelizePointAndSpotLight()` (Froxelizer.cpp:854-999) directly:
  Filament does **not** run a sphere-vs-froxel-bounding-sphere test per
  froxel at all for point lights — it iteratively clips the light's sphere
  through the Z, then Y, then X froxel planes (`spherePlaneIntersection()`
  cascade) to find an exact candidate range, and only the SPOT refinement
  uses the precomputed bounding sphere (via `sphereConeIntersectionFast`).
  The gate matrix's own row-3 citation ("sphere-vs-froxel-bounding-sphere
  for point lights... is the algorithmic reference") is itself an
  imprecise simplification of what Filament actually does — the
  implementer's own "Honest design note" is *more* accurate about this
  than the matrix it cites, and correctly identifies the tighter
  plane-slab alternative exists in the same pinned source.
- **The over-inclusion is a mathematical guarantee, not merely an
  empirical observation**: a froxel's convex volume is, by construction of
  `froxelViewSpaceBounds()` (radius = max corner distance from centroid),
  entirely contained in its own bounding sphere — so "light sphere
  misses froxel's bounding sphere" provably implies "light sphere misses
  the froxel's true geometry" (contrapositive: no false negatives is
  guaranteed independent of any test run). The spot-cone refinement
  inherits the same property from Filament's own `sphereConeIntersectionFast`
  doc comment ("returns a false-**positive** in a small area... never a
  false negative"). The GPU exact-membership sweep is still valuable — it
  proves the GPU/CPU/reference formulas agree — but the conservative
  property itself does not depend on it.

**Perf-implication ruling**: the looseness is bounded (empirically: 8/6480
froxels for a corner light, <10% of the grid) and disclosed honestly in
both the source comments and the report's own "Concerns" section. At 5000
lights (5x T15's own top named scale) the full chain measures ~2.2-2.6ms
on real NVIDIA hardware (see stress numbers below) — comfortably inside
even a 60fps frame budget with headroom for T15's own per-pixel light
loop, whose extra iterations from over-inclusion are a few froxel-corner
lights, not a systemic multiplier. Tightening now (Filament's own
plane-slab reduction) would be premature optimization against an
acceptance criterion that does not ask for it, spending scope this ticket
does not need to spend. **Ruling: accept as documented; keep the follow-up
noted for T15/T30 to revisit only if real frame-integration numbers show
it mattering.**

### Adjudication 3 — Descriptor-pool-reset-per-call scoping (implementer concern 2)

**Legitimate phase boundary, not a no-stub violation.** Grepped
`samples/`, `src/rx_frame_loop`, and every `CMakeLists.txt` for
`ClusterPipelines`/`addClusterPasses`/`rx_cluster` — **zero production
call sites exist today**; the only callers are the GPU test suite and
`tools/rx_cluster_bench`, both of which call `CommandContext::runOnce()`
(confirmed via source read: `vkQueueWaitIdle` inside `runOnce()`) between
every `addClusterPasses()` call, so no descriptor set from a prior call is
ever still in flight when `vkResetDescriptorPool()` runs. The class's own
header comment discloses the frames-in-flight boundary explicitly and
names it as T15's own integration responsibility, not a silently-assumed
fact. Since no production path calls this per-frame yet, there is no
live violation of the "no stub without a phase-fit reason" rule — the
reason is real and the phase boundary is exactly where the plan places
T15's own frame integration.

### Adjudication 4 — Serial prefix-sum at 6480 froxels

Plausible, not independently instrument-verified per-pass. A single
thread executing a handful of ALU ops (min/subtract/compare/store) per
froxel over 6480 iterations is consistent with a microseconds-scale cost
on any GPU this engine targets — the claim is not contradicted by the
aggregate full-chain numbers (which are dominated by the O(froxelCount ×
lightCount) count/scatter passes at the tested scales). I did not extract
a per-pass GPU timestamp breakdown (Tracy GPU context is present but a
full profiling capture was out of this round's time budget) — this
specific sub-claim is accepted on the strength of its design (a serial
scan over a few thousand scalar iterations) rather than independently
timed.

---

## Findings

### Finding 1 (LOW) — `computeFroxelGridXY()` diverges from Filament's exact integer-truncation order

`froxel_grid.cpp::computeFroxelGridXY()` computes `froxelCountX`/`Y` via
`std::sqrt(static_cast<double>(froxelPlaneCount) * width / height)` — the
multiply/divide happens in **double** precision. Filament's own
`computeFroxelLayout()` (Froxelizer.cpp:299-300) computes the identical
expression in **integer** arithmetic first (`froxelPlaneCount * width /
height`, all `size_t`, truncating before the implicit `double` conversion
for `sqrt`), then floors the square root. For every configuration this
round's tests exercise (512×512, 1920×1080, 64×64 at their respective
budgets) the two evaluation orders happen to produce identical results
(confirmed by hand for each), so nothing is currently wrong — but this is
a latent divergence from a strict port, not covered by any test, that
could floor to a different integer at the `sqrt` boundary for some
untested viewport/budget combination. The matrix's own acceptance
criterion does not demand bit-for-bit here (only `findSliceZ()` is named
bit-for-bit), so this is not a compliance failure — worth a one-line fix
(cast the intermediate product+division to match Filament's integer
order, or note the deviation explicitly) whenever this file is next
touched.

### Finding 2 (LOW) — no test asserts overflow counters exactly AT the capacity boundary (not just capacity+1)

`test_cluster_capacity_determinism_gpu.cpp` tests capacity+1 on both axes
(the standing content-scale rule) but no scenario is engineered to land
`trueCount == maxLightsPerFroxel` **exactly**, asserting `writeCount ==
trueCount` and `perFroxelOverflow == 0` at that boundary specifically (the
existing zero-overflow assertions are incidental — either generously
oversized caps or unrelated empty froxels). The underlying arithmetic
(`min(trueCount, cap)`, `froxel_prefix_sum.slang`) makes an off-by-one at
this boundary unlikely (bugs of this shape usually come from a hand-rolled
`>`/`>=` comparison, which this code does not have), so this is a coverage
gap rather than a suspected defect — but per the task's own capacity-test
discipline, the exact boundary should have its own case alongside
capacity+1, not be inferred from the clamp's own simplicity.

### Finding 3 (LOW / informational) — stress-bench headline numbers do not tightly reproduce; helmet/Sponza numbers do

Re-ran `tools/rx_cluster_bench` three times on the same real NVIDIA RTX
2080 (driver 580.82.07, solo GPU, niced, offscreen):

| Lights | Report | Run 1 | Run 2 | Run 3 | Avg drift vs report |
|---|---|---|---|---|---|
| 100 | 0.405 ms | 0.476 ms | 0.432 ms | 0.466 ms | **+13.1%** |
| 1,000 | 0.726 ms | 0.902 ms | 0.862 ms | 0.905 ms | **+22.5%** |
| 5,000 | 2.157 ms | 2.600 ms | 2.302 ms | 2.600 ms | **+15.9%** |

All three runs exceed the 10% drift threshold, consistently (not a one-off
outlier). Two mitigating facts: (1) `totalAssignedIndices` reproduced
**exactly** (5391/42136/246596) on every run — the correctness/determinism
claims are unaffected, this is a wall-clock-only discrepancy; (2) the
qualitative exit conclusion ("well within frame budget even at 5x
target scale") still holds under the higher, reproduced numbers (2.3-2.6ms
is still a small fraction of a 16.6ms 60fps budget). Most likely
explanation is ordinary desktop GPU clock/boost-state variance between the
implementer's measurement session and this one (same hardware, same
driver, same methodology — confirmed `vkQueueWaitIdle`-blocking, full-chain
timing). By contrast, the helmet/Sponza `--bench-frames 200` regression
numbers reproduced tightly (helmet: report 0.212ms vs. my 0.210ms, −0.9%;
Sponza: report 0.477ms vs. my 4.378ms... see below) — flagged as
informational rather than blocking since the exit criterion these numbers
serve (no regression, room under budget) is unaffected either way.

---

## Stress methodology + numbers (re-run, real NVIDIA RTX 2080, driver 580.82.07)

**Full-chain stress (`tools/rx_cluster_bench`)** — confirmed the measured
window is genuinely the full GPU-inclusive chain: `CommandContext::
runOnce()` submits then calls `vkQueueWaitIdle()` (confirmed by direct
source read of `src/rx_rhi_vk/src/command.cpp`), so the `std::chrono`
window wrapping it captures dispatch + execution + completion, not just
CPU-side recording. See Finding 3 above for the reproduced numbers
(correctness exact, timing +13-23% high vs. the report).

**Regression check (`sample_08_gltf_viewer --bench-frames 200
--validate`)**, same driver:

| Scene | Stage-1 baseline (4d52d8f) | Report (this round) | My re-measurement | Δ vs. report |
|---|---|---|---|---|
| DamagedHelmet | 0.219 ms | 0.212 ms | **0.210 ms** | −0.9% |
| Sponza | 4.547 ms | 4.477 ms | **4.378 ms** | −2.2% |

Both comfortably within noise of both the original Stage-1 baseline and
the report's own figures — **no regression confirmed**, and this
methodology (unlike the synthetic stress bench above) reproduces tightly.
Zero unfiltered Vulkan validation errors on either run (all messages
matched this repo's own documented false-positive categories).

---

## Empirical verification (driver-labeled, all independently re-run this round)

**Lavapipe** (llvmpipe/Mesa, `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`,
serial, niced, offscreen via `xvfb-run`): **44/44 ctest tests green**
(127.97s). Zero unfiltered Vulkan validation errors (`grep -i validation`
minus the documented false-positive categories → empty).

**Real NVIDIA** (GeForce RTX 2080, driver 580.82.07 — confirmed via
`nvidia-smi` and `vulkaninfo --summary` vendorID `0x10de`), solo GPU
(confirmed no other compute apps via `nvidia-smi --query-compute-apps`),
niced, offscreen:
- At `-j2`: 43/44 green, one flake — `rx_asset_gltf_gpu_tests`.
- At `-j1` (serial, matching the "empirical minimum" instruction):
  **44/44 green** (245.80s), zero unfiltered validation errors.
- I additionally isolated `rx_asset_gltf_gpu_tests` standalone (no
  concurrent GPU load at all) and it **still flaked once** (61/62 test
  cases, 1341/1342 assertions) on a repeat standalone run, then passed
  cleanly (62/62, 1350/1350) on the next. This is a slightly broader
  characterization than the report's own "cross-test-GPU-contention flake
  category" — the flake reproduces even without contention from other
  ctest jobs. Confirmed out of scope for T14: `git diff --stat
  5413eee..2947037 -- src/rx_asset/` is empty, and the last commit
  touching `import_gltf_gpu_test.cpp` predates this task (T13). Pre-
  existing, unrelated to this diff, correctly not claimed as fixed by this
  report — flagged here only to correct the record on the flake's actual
  scope, not as a T14 finding.

**`rx_cluster_gpu_tests`** specifically, real NVIDIA: 4/4 test cases,
38982/38982 assertions, matching the report's own numbers exactly,
including the named-scenario counts I independently reproduced
(`cornerLightFroxelCount=8`, `spanningLightFroxelCount=256`).

**Conformance harness + D17 gates**: all 8 `rx_conformance_*` tests
(including `MetalRoughSpheres_discrimination_proof`) green on both
drivers, in both full-suite runs above.

**CI Wine exclusion (mechanical)**: `ctest --test-dir build/linux-native
-N -E '...|rx_cluster_gpu|...'` (the diff's own updated regex) lists only
`rx_cluster_tests`, confirming `rx_cluster_gpu_tests` is excluded exactly
as the report claims.

**Revert-discrimination**: independently reproduced, see Adjudication 1
above — quantified fail (2/6545 assertions, froxel (12,5,5) idx=2172,
byte-for-byte match to the report), byte-identical restore (`git diff`
empty after `git checkout --`), green re-run (19466/19466), `git status`
clean throughout except the pre-existing, unrelated untracked scaffolding
(`.deps-cache`, `assets/fetched` symlink, `toolchain`).

## Hygiene

- 2 commits on `task/t14-froxel-clustering` (base `5413eee`): `2947037`
  (feat) + `02da8bd` (chore: report). Both authored/committed by `Yousef
  Wadi <ywadi85@gmail.com>` — confirmed via `git log --format='%an <%ae>
  ... %cn <%ce>'`. No AI attribution anywhere in either commit message.
- `02da8bd` touches only `.superpowers/sdd/2026-08-20-phase5-techniques/
  task-14-report.md` (411 insertions, confirmed via `git show --stat`) —
  SDD-only, as required.
- Not pushed: `git ls-remote --heads origin | grep -i t14` returns
  nothing.
- Main checkout untouched: `main` remains at `b8fa449`, its only
  modification is the pre-existing `progress.md` diff noted in the
  session's own git status at the start (left alone, not touched by this
  review).
- Worktree left clean: no residual sabotage, no stray edits — `git status
  --porcelain -uall` shows only the pre-existing untracked build
  scaffolding.

## Not independently verified this round

- Windows-cross-zig cross-compile + Wine run (the report's own
  `rx_scene_tests.exe`/`rx_cluster_tests.exe` 104/104 and 4/4 claims) —
  the CI exclusion regex was mechanically confirmed (see above), which
  was the specific item in scope; the full Wine build/run cycle itself
  was not re-executed this round (time-boxed; not flagged as a gap since
  the CI-mechanics check was the requested verification).
- Per-pass GPU timestamp breakdown of the prefix-sum kernel's own
  microseconds claim (Adjudication 4) — accepted on design grounds, not
  independently instrumented.
- Steam Deck numbers — honest-manual per RC8, as in every other Phase 5
  task; not claimed as measured by the report, nothing to verify.

---

## Re-review (fix round 1)

Scoped re-review of `c632d8a` (`fix(rx_scene,rx_cluster): review fix round
1 -- integer-truncation port fidelity, at-capacity boundary tests,
re-measured stress numbers (#50)`), same branch/worktree, on top of
`02da8bd`. Scope: closure of this review's own three LOW findings only.
Read the branch's `task-14-report.md` "Fix round 1" section first, then
independently re-verified in the worktree (`cd -P`, niced, offscreen,
solo GPU confirmed via `nvidia-smi --query-compute-apps` before every GPU
run).

### Finding 1 (integer-truncation port fidelity) — **CLOSED**

`froxel_grid.cpp::computeFroxelGridXY()` now computes `productX =
uint64_t(froxelPlaneCount) * width; froxelCountX =
uint32_t(sqrt(double(productX / height)))` — the division truncates in
64-bit **integer** arithmetic before `sqrt()` ever runs, matching
Filament's own `size_t(std::sqrt(froxelPlaneCount * width / height))`
(Froxelizer.cpp:299-300, all-`size_t` operands) exactly in both arithmetic
type and order — confirmed by direct re-read against the same v1.75.0
source I fetched last round.

The path-equivalence proof is sound: I independently re-derived `floor(
sqrt(x)) == floor(sqrt(floor(x)))` for real `x >= 0` from first principles
(let `n = floor(x)`, `m = floor(sqrt(x))`; `m² ≤ x` and `m²` integer `⟹ m²
≤ n`; `n ≤ x < (m+1)²` and both integers `⟹ n < (m+1)²`; so `m² ≤ n <
(m+1)²`, i.e. `floor(sqrt(n)) = m`) — matches the report's own proof, no
gap found.

Ran my own brute-force sweep (Python, independent of the implementer's
script) across **9 `froxelPlaneCount` values × ~1.86M width/height
combos** (`[16,4000]`, step 7/11, `froxelPlaneCount ∈
{16,64,128,256,512,1024,2048,4096,8192}`) comparing the old double-path
formula against the new integer-path formula: **zero divergences**,
corroborating (with a wider sweep than the implementer's own single-
`froxelPlaneCount=512` check) that no case in this codebase's practical
range would ever have diverged. I also hand-traced the new test's first
combo (`1366,769,8192,16` → `froxelPlaneCount=512`): old-path raw
`sqrt(512.0*1366/769)=909.48…` → floor 30; new-path raw
`sqrt(699392÷769=909)` → floor 30 — identical, confirming by hand that
this specific combo does **not** actually diverge either.

So: **no test combo in this fix, or any combo in this range, actually
diverges under the two paths** — the task's first verification option
("verify at least one actually WOULD have diverged") cannot be satisfied
because nothing does, for the same reason the exhaustive sweep found
nothing. The **second option applies and is satisfied**: the test's own
new header comment explicitly, honestly says so ("these two cases exist
to LOCK today's correct behavior against a future regression, not because
a divergence was ever actually observed") — the combos (odd/non-multiple-
of-8 resolutions, one at a non-default budget/slice count) are the
plausible shape a truncation-order bug would need, and the fix itself
removes the only theoretical residual risk (IEEE-754 rounding in the
double path) regardless. Re-ran `rx_scene_tests`: 105/105 (26683
assertions) on lavapipe — both new TEST_CASEs pass, zero regressions in
the pre-existing 103.

### Finding 2 (at-capacity boundary tests) — **CLOSED**

Two new GPU TEST_CASEs confirmed present (`--list-test-cases`) and green
on **both drivers**:

- **Per-froxel axis, AT capacity**: `trueCounts[targetIdx] == 8 ==
  writeCounts[targetIdx]` (full assignment, nothing truncated),
  `perFroxelOverflow[targetIdx] == 0` — re-ran with `-s` on lavapipe AND
  real NVIDIA (RTX 2080, driver 580.82.07), assertion values identical on
  both.
- **Global axis, AT capacity**: `maxTotalLightIndices` set to the exact
  discovered real need (240); every hit froxel gets full assignment,
  `globalOverflow == 0` everywhere, `totalUsed == realNeed` (`240 ==
  240`) — re-ran on both drivers, identical.
- The pre-existing capacity+1 cases (past-boundary, both axes) still pass
  unchanged alongside the new at-boundary ones.

Both capacity axes are covered at both boundaries now (at-capacity +
capacity+1, per-froxel + global — four distinct boundary scenarios total).
Full `rx_cluster_gpu_tests` suite: **6/6 test cases, 58462/58462
assertions on lavapipe; 6/6, 58462/58462 on real NVIDIA** — identical
counts on both drivers (matches the report's own numbers exactly), zero
unfiltered Vulkan validation errors on either.

### Finding 3 (stress-bench reproducibility) — **CLOSED**

The new N=5-separate-process methodology directly addresses the finding's
own root cause (cross-process GPU clock/boost-state variance a single
run's intra-run min/max cannot characterize). Checked the published
NVIDIA table against my own three independent re-runs from last round:

| Lights | My range (3 runs, last round) | Published median (N=5) | Published range (N=5) | My range ⊆ published range? |
|---|---|---|---|---|
| 100 | 0.432 – 0.476 ms | 0.449 ms | 0.328 – 0.546 ms | **yes** |
| 1,000 | 0.862 – 0.905 ms | 0.865 ms | 0.675 – 1.187 ms | **yes** |
| 5,000 | 2.302 – 2.600 ms | 2.448 ms | 2.078 – 3.860 ms | **yes** |

Every published median sits close to the center of my own observed range
at every tier, and my full observed range is a strict subset of the newly
published range at every tier — the new numbers honestly bracket what I
measured, not a cherry-picked or narrowed re-framing.

Spot-ran the 5000-light tier myself once more this round (real NVIDIA RTX
2080, solo GPU, niced, offscreen): **avg=2.5506 ms, min=2.5252 ms,
max=2.6081 ms** — inside the published 2.078–3.860ms range, close to the
published 2.448ms median, `totalAssignedIndices=246596` exact match to
every prior run on any driver. Confirms the published range is honest and
still reproducible today, not a one-time-lucky sample.

### Overall verdict: **ALL ADDRESSED**

All three LOW findings from the original review are closed: the port
fidelity fix is verified correct and matches the cited source exactly
(with the "no combo actually diverges" nuance disclosed honestly rather
than fabricating a divergence); both capacity axes now have real
at-boundary test coverage on both drivers; the stress numbers are now a
5-sample median+range that demonstrably, honestly brackets my own
independent measurements, and I independently reproduced a sample inside
the published range.

### Hygiene (`c632d8a`)

- Author/committer: `Yousef Wadi <ywadi85@gmail.com>` (confirmed via `git
  log --format`). No AI attribution anywhere in the commit message.
- Explicit pathspecs: exactly 4 files changed
  (`task-14-report.md`, `test_cluster_capacity_determinism_gpu.cpp`,
  `froxel_grid.cpp`, `froxel_grid_test.cpp`) — no broad `git add -A`
  footprint.
- Not pushed: `git ls-remote --heads origin | grep -i t14` returns
  nothing.
- Worktree clean: `git status --porcelain -uall` shows only the
  pre-existing untracked build scaffolding (`.deps-cache`,
  `assets/fetched` symlink, `toolchain`).
- Main checkout clean: only the pre-existing, unrelated `progress.md`
  modification noted at the start of the original review session — left
  untouched.
- No temporary edits were made to source in this round (verification-only:
  builds were already warm/up to date, `ninja: no work to do`; no
  sabotage/revert cycle was needed this round since no new port-fidelity
  claim required a discrimination re-proof).
