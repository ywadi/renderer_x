# `rx_asset_gltf_gpu_tests` load-flake investigation

Scope: `src/rx_asset/tests/async_import_test.cpp` only (the WALL-CLOCK GATE
TEST_CASE). No production code ships changed -- one production one-liner
(`import_gltf.cpp`'s `marshalGltfImportPrepareStep()`) was touched strictly
as a temporary revert-discrimination probe and restored byte-identical
(confirmed via `git diff --stat`, empty, before the final commit).

## 1. Symptom and history read first

Tonight's packaging run (`scratchpad/release-phase4/notes.md` section 7)
recorded `rx_asset_gltf_gpu_tests` failing twice during full-serial `ctest`
runs (one native, one `xvfb-run`) while a packaging agent was building the
`windows-cross-zig` preset concurrently -- passing cleanly in isolation and
on a clean full-serial re-run. Per the briefed history:

- Issue #30 (closed, `45936cf`/`adaa4b3`/`07a2474`/`b931188`) fixed
  assertion-count nondeterminism (six wall-clock polling loops aggregated
  to a single post-loop assertion each) and a real product race in
  `computeGltfImport()`'s parse-failure stage stores. Neither is what
  tripped tonight -- both are structurally incapable of reproducing a
  clean single-assertion `REQUIRE` failure with a stable, small-percentage
  margin (see below).
- `issue-30-report.md`/`issue-30-review.md` explicitly disclose, as an
  accepted, out-of-scope, unfixed property: the WALL-CLOCK GATE's
  self-calibrated stall-detector (`kCiStallDetector = max(2ms, 4x live
  calibration probe)`, RC6) "remains a live-timing pass/fail property" that
  "can still FAIL under genuinely heavy external GPU/CPU contention" --
  the round-1 reviewer reproduced this 2/40 times under real contention
  (a leftover competing GPU-rendering process), always as a tight-margin
  `REQUIRE(maxPumpDuration < kCiStallDetector)` miss (e.g. `8876us <
  8828us`), never a crash, hang, or assertion-count change.

## 2. Reproduction (Method step 1)

Built a load harness matching tonight's own trigger shape plus the
reviewer's own historical trigger shape, run simultaneously:

- **CPU**: a loop of `ninja -C build/windows-cross-zig -t clean` +
  `cmake --build --preset windows-cross-zig -j 8`, repeated continuously --
  literally "a packaging agent building the other preset concurrently."
- **GPU**: `xvfb-run -a ./build/linux-native/samples/09_scene/sample_09_scene
  --present --stress`, looped -- the same mechanism (a competing real-GPU
  renderer process) the round-1 reviewer's own 40-run reproduction used.

Both run from the real path (`/media/ywadi/second/renderer_x`, never the
`/home/ywadi/d2/renderer_x` symlink alias, per the dep-cache-key quirk).

Ran repeated `xvfb-run -a ctest --test-dir build/linux-native
--output-on-failure -j1` passes under this load. **Run 15 failed**:

```
/media/ywadi/second/renderer_x/src/rx_asset/tests/async_import_test.cpp:988: FATAL ERROR: REQUIRE( maxPumpDuration < kCiStallDetector ) is NOT correct!
  values: REQUIRE( 9689µs <  9160µs )
[doctest] assertions: 1240 | 1239 passed | 1 failed |
[doctest] Status: FAILURE!
```

The calibration probe that run read `2290us` -- MESSAGE line: `wall-clock
gate: calibration probe (2048x2048 synthetic registerDecoded()) took 2290
us`, well BELOW this file's own recorded quiet/constrained baseline
(5634-6202us, see the multiplier-derivation comment). `maxPumpDuration`
(9689us) and D25's zero-wait-calls counters stayed clean (no blocking-wait
regression). This is byte-for-byte the same failure SHAPE as issue #30's
own disclosed limitation and the round-1 reviewer's reproduction: a
single-assertion, tight-margin `REQUIRE` miss on the stall-detector tier,
with the calibration probe itself reading anomalously low.

**Verdict: tonight's symptom is exactly the documented, disclosed RC6
live-timing-gate limitation, not a new defect.** 14 loaded full-serial runs
preceded this reproduction (RUN 1-14, all clean) -- consistent with the
reviewer's own historically-observed low trip rate (~2/40) under real
contention.

## 3. Root cause

`kCiStallDetector` is derived from exactly ONE `TextureCache::registerDecoded()`
timing sample, taken once, immediately **before** the ~300+-iteration
`pumpMain()` measurement loop starts. External contention (a concurrent
build's per-file compile/link cadence, or a competing GPU-present loop) is
genuinely bursty on a timescale comparable to or shorter than the gap
between "calibration probe fires" and "the loop's own worst pump happens."
A single point-in-time sample can land in a transient lull -- exactly what
happened in RUN 15 (calibration read `2290us`, well below this file's own
recorded quiet baseline, moments before the loop hit a real `9689us`
contention spike) -- undercalibrating the ceiling relative to what the
loop itself goes on to encounter.

This is a genuine, previously-disclosed-but-unaddressed weakness in the
*measurement* side of the self-calibration, not in the underlying product
performance property being asserted (RC6's time-slicing fix, D25's
poll-never-wait discipline) and not a repeat of issue #30's
assertion-count defect.

## 4. Fix

**Bracket the calibration**: take a second sample immediately **after**
the loop, and derive the ceiling from the larger of the two
(`kCiStallDetector = max(kLocalBudget, kCiStallMultiplier * max(before,
after))`). This directly implements the "probe under the same conditions
as the measurement" option named in this investigation's brief:

- The multiplier (4x) and floor (2ms) are **untouched** -- no blind
  threshold bump.
- `max()` of two nonnegative samples is `>=` the before-only value alone,
  so this can only **raise** the derived ceiling relative to the formula
  it replaces -- strictly more permissive under contention, never less
  sensitive to a real blocking-wait regression. The blocking-defect
  signature this gate exists to catch (~39-41ms, per the file's own
  22-run constrained measurement) sits roughly 4x further out than
  anything this widening could plausibly reach.
- **Rejected alternative**: a repeated back-to-back burst of calibration
  samples (tried first, as an experiment, not committed) was found to
  introduce its own confound -- rapid-fire `registerDecoded()` calls with
  no intervening drain measurably slow down across the burst from
  upload-path effects unrelated to external contention (observed directly:
  sample 0 of a 10-sample burst was consistently the smallest, every run,
  regardless of load). Bracketing with two independent, well-separated
  (by the loop's own real duration) samples avoids this confound.
- **Rejected**: a quiet-host skip. This is a CI-tier gate; CI runners are
  themselves routinely contended, so skipping it whenever load is
  detected would neuter exactly the runs it exists to protect. Not
  applicable here.
- **Not applicable**: "strengthen product-side determinism." This gate
  asserts a genuine live-timing performance property (stall detection) by
  design -- it is not a hidden determinism bug the way issue #30's
  assertion-count defect was.

### 4.1 Self-caught regression during this fix (disclosed)

Adding the AFTER-loop probe naked (no drain) introduced a **new**
resource-lifetime hazard, caught by this investigation's own load-testing
before it was committed: the AFTER probe's texture upload has no
subsequent `pumpMain()` loop to drain it naturally (unlike the BEFORE
probe, which the following 300+-iteration loop drains as a side effect).
Left undrained, under heavy contention the TEST_CASE could end and its
fixture (`TextureCache`/`Uploader`/`Device`) tear down while that upload
was still in flight on the GPU -- reproduced directly:

```
[error] [vulkan validation] Validation Error: [ UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkImage ] ...
  bound VkImage ... was destroyed.
```
followed by the whole binary stalling past ctest's 300s per-test timeout
(`9 - rx_asset_gltf_gpu_tests (Timeout)`, `Total Test time (real) = 404.92
sec`). This is the exact same validation-error class
`TextureCache::registerRealTexture()`'s own header comment documents and
already guards against on ITS failure paths (`flush()`+`wait()` before an
upload-recorded resource is destroyed). Fix: an explicit drain immediately
after the AFTER probe, using the SAME public poll API the async import
pipeline itself uses (`flushPendingUploads()`/`isUploadComplete()`, D25's
"poll, never a blocking wait" contract) -- not a reintroduced blocking
`uploader_.wait()` call, and confirmed (by reading both methods) to never
touch `waitCallCountForTesting()`, so it cannot skew the D25 zero-wait-calls
assertions later in the same TEST_CASE. Bounded by a generous 10s deadline
(two orders of magnitude above the ~2-25ms scale of the operation it is
draining) specifically so this teardown-safety check does not itself
become a new tight-margin flake source.

This self-caught defect never shipped in a commit -- caught during this
investigation's own iterative load-testing, fixed before finalizing.

## 5. Revert-discrimination evidence

Reproduced the SAME revert issue-30-report.md used (removing the RC6
time-slicing early-return in `marshalGltfImportPrepareStep()`, so all 5
DamagedHelmet texture slots register inside one call instead of one per
`pumpMain()`):

```
wall-clock gate: calibration probe AFTER the loop took 4747 us on this run (BEFORE sample was 2321 us) -- CI-tier threshold derived as 4x the larger of the two (4747 us)
/media/ywadi/second/renderer_x/src/rx_asset/tests/async_import_test.cpp:1072: FATAL ERROR: REQUIRE( maxPumpDuration < kCiStallDetector ) is NOT correct!
  values: REQUIRE( 22187µs <  18988µs )
[doctest] Status: FAILURE!
```

Restored: `git diff --stat src/rx_asset/import_gltf.cpp` -- empty
(byte-identical to HEAD), rebuilt, confirmed clean before continuing.

The gate still fails clearly on the exact defect class it exists to
catch, with the bracketing fix in place.

## 6. 10-run proof, loaded and quiet

**10 consecutive full-serial `ctest` runs, WITH the load harness (CPU
rebuild loop + GPU present-loop) running throughout**, post-fix:

```
RUN 18  rc=0  dur=149s  100% tests passed, 0 tests failed out of 29  |  rx_asset_gltf_gpu_tests Passed 45.04 sec
RUN 19  rc=0  dur=151s  100% tests passed, 0 tests failed out of 29  |  rx_asset_gltf_gpu_tests Passed 45.95 sec
RUN 20  rc=0  dur=148s  100% tests passed, 0 tests failed out of 29  |  rx_asset_gltf_gpu_tests Passed 45.71 sec
RUN 21  rc=0  dur=153s  100% tests passed, 0 tests failed out of 29  |  rx_asset_gltf_gpu_tests Passed 44.73 sec
RUN 22  rc=0  dur=151s  100% tests passed, 0 tests failed out of 29  |  rx_asset_gltf_gpu_tests Passed 44.07 sec
RUN 23  rc=0  dur=150s  100% tests passed, 0 tests failed out of 29  |  rx_asset_gltf_gpu_tests Passed 46.50 sec
RUN 24  rc=0  dur=154s  100% tests passed, 0 tests failed out of 29  |  rx_asset_gltf_gpu_tests Passed 44.27 sec
RUN 25  rc=0  dur=150s  100% tests passed, 0 tests failed out of 29  |  rx_asset_gltf_gpu_tests Passed 44.46 sec
RUN 26  rc=0  dur=152s  100% tests passed, 0 tests failed out of 29  |  rx_asset_gltf_gpu_tests Passed 47.67 sec
RUN 27  rc=0  dur=151s  100% tests passed, 0 tests failed out of 29  |  rx_asset_gltf_gpu_tests Passed 44.94 sec
```

All 10: 29/29 passed, normal timing (no timeouts, no hangs -- confirming
the drain fix from section 4.1 as well). (RUN 16-17, immediately prior,
demonstrated the pre-drain-fix hang described in 4.1 and are the direct
motivation for that fix; not part of this clean 10-run block.)

**Quiet-host run** (load harness stopped, verified via `top -bn1`: 96.2%
idle immediately before):

```
100% tests passed, 0 tests failed out of 29
Total Test time (real) = 139.00 sec
```

**Assertion-count stability** (issue #30's own discipline, unaffected by
this fix -- the drain loop's own `while` condition asserts nothing per
iteration, only the fixed-count `REQUIRE` after it):

```
[doctest] test cases:   58 |   58 passed | 0 failed | 0 skipped
[doctest] assertions: 1250 | 1250 passed | 0 failed |
```
Identical across 3 separate quiet runs (was 1248 pre-fix; +2 from the two
new fixed-count `REQUIRE`s this fix adds -- the AFTER probe's handle
validity and its drain completion).

**Cross-platform**: `windows-cross-zig` rebuilt clean; `rx_asset_gltf_gpu_tests`
under Wine (`xvfb-run -a ctest --test-dir build/windows-cross-zig -R
rx_asset_gltf_gpu_tests`): `100% tests passed`, `47.63 sec`.

Zero unfiltered validation errors in any run reported above (all report
`Status: SUCCESS!`; the process-lifetime validation-error re-check never
flipped a clean run to nonzero exit).

## 7. Commits

- `src/rx_asset/tests/async_import_test.cpp` only: bracketed
  before/after self-calibration (section 4) + the drain fix for the
  self-caught teardown hazard (section 4.1), and the comment updates
  documenting both.
- This report, as a separate commit.

No production code diff ships (the `import_gltf.cpp` revert-probe was
restored byte-identical before any commit).

## 8. Disposition

Tonight's flake is the previously-disclosed RC6 live-timing gate
limitation (issue-30-review's "MEDIUM finding," never fixed there because
it was explicitly out of that fix's scope), now closed: the calibration
that feeds the gate's threshold is bracketed around the actual measurement
window instead of sampled once before it, which directly targets the
demonstrated failure mode (a lucky-quiet single sample) without touching
the multiplier, the floor, or the underlying product performance property
being asserted. A second, self-inflicted defect (a resource-teardown race
introduced by the naive first version of this fix) was caught by this
investigation's own load-testing before it shipped, root-caused, and
fixed using the same test-only polling idiom the rest of the file already
uses.
