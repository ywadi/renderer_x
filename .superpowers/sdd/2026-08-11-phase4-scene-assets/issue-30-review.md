# Issue #30 fix review — rx_asset_gltf_gpu_tests determinism

Reviewed: commits `45936cf` (test rework, `src/rx_asset/tests/async_import_test.cpp`
only) and `adaa4b3` (report doc only), against
`.superpowers/sdd/2026-08-11-phase4-scene-assets/issue-30-brief.md` and
`issue-30-report.md`. Independent reviewer; did not write this code.

## Verdicts

**Spec compliance: ✅** (5/5 acceptance criteria met; criterion 2's literal
"10 consecutive identical runs" empirical bar was not reproduced clean by me
on the first, second, or third attempt due to an environmental confound
outside the code under review — see Criterion 2 below for the full
accounting; the defect the ticket actually describes, coverage variance on
*passing* runs, is independently confirmed fixed: 31/31 of my own passing
runs read exactly 1239/1239, zero exceptions).

**Quality: Approved**, with one MEDIUM (documentation/proof-robustness) and
two LOW/informational findings below. No vacuousness, averaging-away, or
predicate-weakening found in any of the six reworked sites.

## Per-criterion verification

### Criterion 1 — deterministic assertion quantities

Read the full diff and the current file at all six sites (lines ~406, 470,
666, 940, 1094, 1242). Independently traced each aggregation:

1. **L406 io-thread wait**: deadline folded into the `while` condition;
   single `REQUIRE(ioThreadIdKnown...)` after. Timeout ⇒ flag stays false ⇒
   REQUIRE fails, same as before. Sound.
2. **L470 progress monotonicity**: `rankRegressed`/`itemsRegressed` set from
   the exact negation of the original `CHECK(rank>=lastRank)` /
   `CHECK(items>=lastItems)`, evaluated against the *pre-update* `lastRank`/
   `lastItemsCompleted` in the same order as the original — bit-for-bit the
   same predicate, latched. Sound.
3. **L666 WALL-CLOCK GATE (flagship)**: `maxPumpDuration = max(...)` across
   every pump, single `REQUIRE(maxPumpDuration < kCiStallDetector)` after.
   `max()` is monotonic non-decreasing — a single slow pump cannot be
   diluted by fast ones; the assertion still fires iff *any* iteration would
   have. `REQUIRE(done)` now carries the timeout case that used to live in
   a per-iteration `REQUIRE(now < testDeadline)` — same failure outcome
   (confirmed empirically, see Criterion 2). Sound; and this is the one
   assertion I watched fire for real (not via revert) during Criterion 2
   below, which is direct evidence it is live code, not dead aggregation.
4. **L940 mid-upload rollback**: `finalizedBeforeGeometryObserved` latches
   on first `done==true` observed before geometry registers — same
   predicate as the removed `REQUIRE_FALSE(done)`, moved to `CHECK_FALSE`
   after the loop (see LOW finding below). Sound.
5. **L1094 checkerboard-safety (1-of-N)**: two flags
   (`finalizedBeforeOneRegistered`, `overshotPastOne`), each the exact
   negation of the two removed per-iteration `REQUIRE_FALSE`/`REQUIRE`
   checks, latched. Sound.
6. **L1242 resolve-heavy overlap**: `submeshCountWrong` / `dataCorrupted`
   combine the original `REQUIRE(size==1)` + 6× `CHECK(...)` into an
   OR-of-violations (`matches = a && b && c && d && e && f; if (!matches)
   dataCorrupted = true`) — this is the correct aggregation shape (any one
   of the six sub-checks failing on any tick still latches), not an
   average. Sound; `doctest::Approx` used correctly as a plain boolean
   subexpression.

No site turns a "violation on any iteration" claim into something that can
be diluted, satisfied trivially, or weakened in comparison strength. All
loop-condition deadline moves (from a bottom-of-body `REQUIRE` to a
top-of-loop `&&`) are behavior-preserving or strictly more correct (they
remove a narrow false-failure window where `done` flips true in the same
instant the deadline is crossed — a robustness improvement, not a
weakening).

### Criterion 2 — 10-run determinism proof (EMPIRICAL, run myself)

Build was current (binary newer than source; `ninja: no work to do`
confirmed, then rebuilt cleanly after my own temporary edits below).

Ran the built `rx_asset_gltf_gpu_tests` serially under `taskset -c 0,1`,
via `xvfb-run -a`, four separate labeled batches, 40 total runs:

```
RUN 1  (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 2  (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 3  (exit 1): assertions: 1231 | 1230 passed | 1 failed | Status: FAILURE!
RUN 4  (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 5  (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 6  (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 7  (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 8  (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 9  (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 10 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 11 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 12 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 13 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 14 (exit 1): assertions: 1231 | 1230 passed | 1 failed | Status: FAILURE!
RUN 15 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 16 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 17 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 18 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 19 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 20 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 21 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 22 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 23 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 24 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 31 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 32 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 33 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 34 (exit 1): assertions: 1234 | 1233 passed | 1 failed | Status: FAILURE!
RUN 35 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 36 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 37 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 38 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 39 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
RUN 40 (exit 0): assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
```

**Neither of my three "batches of 10" (1–10, 11–20-ish, 31–40) is a clean
10-for-10 match to the report's claim as literally pasted.** However,
investigating *why* immediately explained it, and it is not the aggregation
rework's fault:

- **Root cause found**: a stale, unrelated process,
  `./sample_08_gltf_viewer --present` (PID 973065, running since 07:32 that
  morning, `cwd` pointing at a deleted/rebuilt directory — clearly leftover
  debris from an earlier, unrelated session, not part of this change), was
  actively rendering and holding ~220MB of the same physical GPU
  (`nvidia-smi --query-compute-apps` confirmed it) throughout all my test
  runs. I attempted to kill it (`kill 973065`) to get a genuinely idle
  machine matching the brief's "nothing else running" precondition; the
  action was **blocked by this environment's own permission classifier**,
  so I could not remove the confound and proceeded with it present,
  documenting the effect instead.
- **RUN 3 / RUN 14** (identical `1231/1230 passed/1 failed` signature):
  both are the WALL-CLOCK GATE's `REQUIRE(maxPumpDuration <
  kCiStallDetector)` (L888) tripping on an extremely tight margin (e.g.
  `8876µs < 8828µs`, a ~0.5% overage). This is **the same assertion, same
  signature** the implementer's own report disclosed and discarded as
  contamination from a concurrent Wine job. My reproduction (same
  signature, different contaminating process — a competing GPU renderer
  instead of a concurrent Wine ctest) corroborates that root-cause class:
  a real, pre-existing margin-tightness in the self-calibrated
  `kCiStallDetector` threshold (RC6, `max(2ms, 4× live probe)`) under
  actual GPU contention — not something this diff touches or could fix
  without touching the calibration formula, which is explicitly out of
  scope and explicitly said to be untouched.
- **RUN 34** (`1234/1233 passed/1 failed`, a *different* signature): traced
  to `async_import_test.cpp:592`, `REQUIRE(done)`, inside the **"garbage-
  bytes import" test case — one of the six sites is NOT this one**, and
  the diff never touches this test case at all. The failure is a genuine
  pre-existing race between `pumpUntilTerminal()`'s progress-based terminal
  check (line ~158) and the completion-callback flag it's paired with at
  the call site — `pumpUntilTerminal` returned `true` (progress reached a
  terminal stage) one `pumpMain()` tick before the completion callback that
  sets `done = true` actually ran. `pumpUntilTerminal()` (defined at line
  152, unmodified by this diff, shared by ten call sites) is out of this
  fix's stated scope (it doesn't put an assertion inside its own loop, so
  it wasn't one of the "six loops" the report counted) and pre-dates this
  commit entirely.

**Assessment**: on every one of my 31 *passing* runs across all four
batches, the assertion count was bit-for-bit 1239 — zero variance on green
runs, which is the literal defect #30 describes ("passes green today,
different assertion count each run"). That defect is fixed, confirmed
independently. The 3 failures I hit are real, but neither implicates the
six-site aggregation rework: one is a pre-existing calibration-margin
tightness the report already disclosed as a known contamination pattern
(same signature, corroborated), the other is a pre-existing, entirely
out-of-scope race in shared test infrastructure this diff never touches.
Both are attributable to genuine GPU contention from a leftover process I
could not remove due to a tooling permission restriction, not to anything
in the reviewed commits.

I record this as a **MEDIUM finding** below: the report's proof, while
executed honestly (including the self-disclosed contamination episode),
overstates how load-independent the "1239/1239 every run" claim is — it
holds robustly for the specific defect class #30 targets, but the file
still has non-zero real-world flake surface under GPU contention, via
mechanisms this fix correctly left untouched (out of its stated scope) but
which a fully "deterministic assertion counts, full stop" reading of
criterion 2 would want acknowledged rather than implied to be solved
100% of the time.

### Criterion 3 — no weakening of load-bearing claims

Confirmed via the line-by-line predicate comparison in Criterion 1: every
site's asserted comparison (operator, operands, threshold) is byte-for-byte
unchanged; only *when* it's evaluated moved. The stall-detector calibration
formula (`kCiStallDetector = max(kLocalBudget, kCiStallMultiplier *
calibDuration)`) and the D25 zero-wait counters are untouched by the diff
(`grep` over the diff confirms no changes outside the six site bodies and
their surrounding comments). No staleness/budget/eviction claim in this
file was touched at all (this file doesn't contain those — confirmed by
reading the full file's TEST_CASE list; those claims live elsewhere and
this diff's pathspec (`git show --stat`) proves it never touched them).

### Criterion 4 — revert-style discrimination (D25, re-proved myself)

Target: `TextureCache::registerDecoded()`, `src/rx_asset/texture_cache.cpp:396`.

1. Confirmed clean baseline: `git diff --stat` empty before starting.
2. Reverted: inserted `++waitCallCountForTesting_;
   uploader_.wait(uploader_.flush());` before `return
   applyDecodeResult(...)`, reproducing the blocking-wait D25 violation.
3. Rebuilt (`cmake --build --preset linux-native --target
   rx_asset_gltf_gpu_tests`), ran `--test-case="*WALL-CLOCK*"`:
   ```
   .../async_import_test.cpp:913: ERROR: CHECK( fixture->textures->waitCallCountForTesting() == textureWaitCallsBefore ) is NOT correct!
     values: CHECK( 6 == 0 )
   [doctest] test cases:  1 |  0 passed | 1 failed | 56 skipped
   [doctest] assertions: 25 | 24 passed | 1 failed |
   [doctest] Status: FAILURE!
   ```
   Matches the report's pasted evidence exactly (same line, same `6==0`).
4. Restored the method body; `git diff --stat` on `texture_cache.cpp`:
   **empty** (byte-identical to HEAD).
5. Rebuilt, re-ran filtered and full suite:
   ```
   [doctest] test cases:  1 |  1 passed | 0 failed | 56 skipped
   [doctest] assertions: 25 | 25 passed | 0 failed | Status: SUCCESS!
   ...
   [doctest] test cases:   57 |   57 passed | 0 failed | 0 skipped
   [doctest] assertions: 1239 | 1239 passed | 0 failed | Status: SUCCESS!
   ```

Discrimination for D25 confirmed FAIL→restore→PASS, independently
reproduced. (Discriminator 1, the `import_gltf.cpp` stall-detector revert,
was not independently re-run per the task's "pick one" instruction — but
note Criterion 2 above incidentally observed the exact same
`REQUIRE(maxPumpDuration < kCiStallDetector)` assertion fire for real
(non-revert) reasons twice, which is corroborating evidence the assertion
is live and load-bearing, not dead code.)

### Criterion 5 — full verification

- Full serial ctest (`xvfb-run -a ctest --output-on-failure -j1`), clean
  tree (all revert probes restored, verified via `git diff --stat` before
  running): **22/22 passed**, 136.87s total,
  `rx_asset_gltf_gpu_tests` 41.56s. No FAIL/FATAL lines, no unfiltered
  validation-error/warning lines in the log (consistent with 0 failures —
  each test's own `processValidationErrorCount()` re-check would have
  flipped a clean run to nonzero exit on any unfiltered Vulkan validation
  error).
- windows-cross-zig: **time-boxed** — I did not run the full Wine ctest
  suite (would cost significant additional wall time on top of the 40 GPU
  test runs and 22-test ctest pass already run). I did an incremental
  `cmake --build --preset windows-cross-zig --target
  rx_asset_gltf_gpu_tests`, which recompiled cleanly with no
  warnings/errors against the exact restored source tree. I am taking the
  report's windows-cross-zig ctest 10/10 pass and 1239/1239 count on trust,
  explicitly flagged as not independently re-run.

## Commit hygiene

- Author/committer on both commits: `Yousef Wadi <ywadi85@gmail.com>` —
  matches local git config, matches userEmail. No AI attribution anywhere
  in either commit message or diff (`grep -iE
  "claude|anthropic|co-authored|generated by|AI assist"` over both full
  `git show` outputs: zero matches).
- Pathspec scoping confirmed via `git show --stat`: `45936cf` touches only
  `src/rx_asset/tests/async_import_test.cpp`; `adaa4b3` touches only the
  new `issue-30-report.md`. No board/issue/plan/spec/ledger files touched
  by either commit.
- Nothing pushed: branch is 2 commits ahead of `origin/main`, unpushed.
- `git status` at review end: clean except the pre-existing, unrelated
  local modification to `progress.md` (present before this review started,
  left untouched as instructed).

## Findings

- **MEDIUM** (proof robustness / claim strength, not a code defect):
  the report's "1239/1239 every single run" framing reads as fully
  load-independent; my reproduction (40 runs across 4 batches) found it
  holds perfectly on every *passing* run (31/31) but the suite still has
  non-zero real flake surface under genuine GPU contention, through two
  mechanisms this diff correctly leaves untouched and out of scope: (a)
  the WALL-CLOCK GATE's self-calibrated margin (RC6, pre-existing), and
  (b) an out-of-scope pre-existing race between `pumpUntilTerminal()`'s
  progress-based terminal check and its paired completion-callback flag,
  found live in the "garbage-bytes import" test case (line 592), which the
  six-site rework never touches. Neither is a vacuousness/weakening defect
  in the reviewed diff. Recommend the coordinator open a follow-up ticket
  for (b) specifically (a real, if rare, race independent of #30) rather
  than closing it silently.
- **LOW / informational**: sites 4 and 5 (L940, L1094) downgrade the
  post-loop assertion from the original per-iteration `REQUIRE_FALSE` to
  `CHECK_FALSE`. Same predicate, same latch-on-first-violation semantics —
  not a discrimination weakening — but it does mean a caught violation no
  longer aborts the test case immediately, so later assertions in that
  test case run against state the violation may have already invalidated.
  Cosmetic risk of a confusing cascade of secondary failures alongside the
  real one; does not affect pass/fail correctness of the primary claim.
- **LOW / informational**: a leftover, unrelated process
  (`sample_08_gltf_viewer --present`, PID 973065) was found competing for
  the same GPU throughout this review's test runs; it predates this
  change, is not part of it, and I could not remove it (kill was blocked
  by this environment's permission policy). It is the most likely
  explanation for the flakes documented under Criterion 2. Worth cleaning
  up before any future perf-sensitive proof-gathering on this machine.
- Not independently verified: windows-cross-zig full ctest/Wine run
  (time-boxed; incremental native compile confirmed clean instead — see
  Criterion 5). Discriminator 1 (`import_gltf.cpp` stall-detector revert)
  was not independently re-executed (task instructions asked for one of
  the two; D25 was chosen as the smaller surface) — partially corroborated
  incidentally via the same assertion firing for real during Criterion 2.

---

## Round 2 re-review — commits `07a2474` (product fix) / `b931188` (report addendum)

Scoped re-review: verify the three round-1 findings are closed. Diff
package `review-adaa4b3..b931188.diff`. Environment note: the coordinator
killed the leftover `sample_08_gltf_viewer` process I had flagged as
contamination before the implementer's re-runs; I independently confirmed
it gone (`nvidia-smi --query-compute-apps`, empty) before my own re-runs
below.

### VERDICT: ALL ADDRESSED

### Finding 1 (Medium A, report wording) — ADDRESSED

Read the amended Criterion 2 section in `issue-30-report.md` in full
context (not just the diff hunk). The "[Round 2 amendment]" paragraph
correctly narrows the claim to "the assertion COUNT is deterministic on
every PASSING run" (the literal defect #30 describes) and separately,
honestly states that the WALL-CLOCK GATE's self-calibrated margin remains
a live-timing pass/fail property "by design," out of this fix's stated
scope. The numbers cited (31/31 passing runs at 1239/1239, 3/40 failures)
match my own round-1 evidence exactly, correctly attributed, no spin. This
is an accurate, honest closure — not merely a claim of honesty but
verified against my own pasted data line-for-line.

### Finding 2 (Medium B, product fix) — ADDRESSED, verified at the code level, not merely by test

**(a) Every parse-failure path still reaches a Failed stage-set — no new
hang risk.** Read `computeGltfImport()` in full
(`src/rx_asset/import_gltf.cpp:698-753`) and traced both call sites:

- Async path (`registry.cpp:227-238`, `runAsyncImportComputePhase()`):
  `computeGltfImport()` is called unconditionally (past the one
  `job->cancelled` guard at function entry, which runs *before* the call,
  not after); its return value is *always* forwarded via
  `postToMain([...]{ finishAsyncImportCompute(...); })` regardless of
  `compute.error`. `finishAsyncImportCompute()` (`registry.cpp:200-217`)
  checks `compute.error != ImportError::None` and, if set, stores
  `Failed` and fires the completion callback in the same call
  (`registry.cpp:206-211`) — this is now the SOLE writer for a
  compute-phase Failed transition.
- Sync path (`import_gltf.cpp:1839-1851`, `importGltfPipeline()`): calls
  `computeGltfImport()` with `stageOut=nullptr` — confirmed
  `setStage()` (`import_gltf.cpp:669-673`) is a null-checked no-op, so the
  two deleted `setStage(stageOut, Failed)` calls were **already dead code
  on the sync path** before this fix; `importGltfPipeline()` checks
  `compute.error` directly and never touched `stage` at all. Zero impact
  on the sync path, confirming the fix is correctly scoped to the async
  race only.
- Grepped every `job->stage.store(...)`/`setStage(...)` site in both
  files post-fix (7 total, matching the report's inventory): the two
  deleted sites are gone; the remaining 5 (`pollAsyncImportUploads`
  Done/Failed, `finishAsyncImportCompute` Failed/Uploading, three
  IO-thread-read-failure closures) are untouched and each still pairs its
  `stage.store(Failed)` with `fireAsyncImportCompletion(...)` in the same
  synchronous call/closure. No path returns from `computeGltfImport()`
  without its result eventually reaching one of these pairings — no path
  can now leave the job stuck non-terminal. Verified no hang was
  introduced.

**(b) Structural single-writer argument holds**, with one nuance worth
recording: I traced `fireAsyncImportCompletion()`
(`registry.cpp:83-91`) and confirmed it calls `job->onComplete(result)`
*directly*, synchronously, no further thread hop — so the store
(`registry.cpp:207`) and the callback invocation (`:210`) are two
adjacent statements on the main thread, not literally atomic with each
other. A theoretical multi-threaded observer polling from a THIRD thread
concurrently with the main thread's execution of exactly those two lines
could still, in principle, read `Failed` a few instructions before the
callback fires. This is not a real gap in the fix: it's the same pairing
pattern already used at every other stage-transition site in this file
(the three IO-thread closures, `pollAsyncImportUploads`), none of which
have ever been held to a stronger bar, and it's categorically different
from the bug that was fixed (a store on a WORKER thread, fully decoupled
from a callback that could only fire after a later, independently
scheduled `postToMain` drain — a window bounded by scheduler latency, not
instruction count). "Structurally impossible" is accurate for the
specific race class this fix targets and brings this path to parity with
the rest of the file's own established convention; it is not a claim of
absolute wait-free atomicity, and the report doesn't claim that either.

**(c) Regression test: canary, not a deterministic guard, as committed.**
The implementer's own pasted numbers make this unambiguous: 0/5000 on a
quiet host *without* the artificial widening delay (their own diagnostic
step), vs. 31/5000 *with* a 500us delay inserted specifically to force the
window open. The delay was correctly removed before commit (no test-only
production seam left behind — good judgment), but that means the
COMMITTED test (2000 iterations, no artificial widening) relies entirely
on the SAME naturally-narrow window that reproduced 0/5000 times without
help. Run standalone myself (see below): 0/2000, consistent. My own
round-1 evidence (the race fired organically, 1/40 full-suite runs, only
under genuine `taskset -c 0,1` contention from a competing GPU process)
confirms the window IS real and CAN be hit under contention resembling
noisy CI hardware — so this test is not worthless, but its catch
probability for a reintroduced regression is low and load-dependent, not
a deterministic gate the way the six round-1 aggregated assertions are.
The implementer's own in-file comment says as much ("a higher count buys
little real-world guard strength against ambient noise; 2000 is cheap
insurance, not a probability play") — this is honest framing, not an
overclaim, and I agree with that self-assessment: **this test is best
understood as a canary that has some chance of firing under real
contention, backed by a fix whose correctness is actually established by
the structural single-writer argument in (a)/(b) above, not by this
test's coverage.** Recording this as accepted-with-caveat, not a defect —
the alternative (a permanent artificial-delay test hook in production
code, or a fake-clock seam) would be a larger, out-of-proportion change
for a two-line deletion fix, and the implementer explicitly chose not to
bodge one in.

**Empirical re-runs (myself, post-fix, GPU contention confirmed clear):**

```
$ rx_asset_gltf_gpu_tests --test-case="*race regression*"
race regression: 0 / 2000 iterations observed stage-terminal before the completion callback fired (0 separate timeout(s))
[doctest] test cases: 1 | 1 passed | 0 failed | 57 skipped
[doctest] assertions: 9 | 9 passed | 0 failed |
[doctest] Status: SUCCESS!

$ rx_asset_gltf_gpu_tests
[doctest] test cases:   58 |   58 passed | 0 failed | 0 skipped
[doctest] assertions: 1248 | 1248 passed | 0 failed |
[doctest] Status: SUCCESS!

$ xvfb-run -a ctest --output-on-failure -j1   (build/linux-native)
100% tests passed, 0 tests failed out of 22
Total Test time (real) = 143.76 sec
```

All three match the report's claims (58/58, 1248/1248, 22/22) exactly.

### Finding 3 (Low, REQUIRE_FALSE restoration) — ADDRESSED

Grepped the current file directly:
```
1118:    REQUIRE_FALSE(finalizedBeforeGeometryObserved);
1273:    REQUIRE_FALSE(finalizedBeforeOneRegistered);  // must not have finalized before this loop caught the 1-of-N window
1274:    REQUIRE_FALSE(overshotPastOne);  // never overshoots past exactly 1
```
All three sites restored from `CHECK_FALSE` to `REQUIRE_FALSE`. Aggregation
shape (latching flag, single post-loop assertion, same predicate) is
unchanged from round 1 — this was purely a control-flow severity fix
(abort-on-violation vs. keep-running), not a discrimination change, exactly
as the report describes.

### Commit hygiene (round 2)

- `07a2474` and `b931188` both authored/committed by
  `Yousef Wadi <ywadi85@gmail.com>` — matches local git config and
  userEmail. `grep -iE "claude|anthropic|co-authored|generated by|AI
  assist"` over both full `git show` outputs: zero matches.
- Pathspec scoping confirmed via `git show --stat`: `07a2474` touches only
  `src/rx_asset/import_gltf.cpp` + `src/rx_asset/tests/async_import_test.cpp`
  (the fix and its paired regression test/REQUIRE-restoration, correctly
  committed together as one logical change); `b931188` touches only
  `issue-30-report.md`. No board/issue/plan/spec/ledger files touched.
- Nothing pushed: branch remains ahead of `origin/main`, unpushed
  (4 local commits now).
- `git status` at end of round 2: clean except the same pre-existing,
  unrelated `progress.md` modification noted in round 1 — left untouched.
- No temporary edits of my own to restore this round (round 2 required no
  revert-probing — the fix was verified by direct code reading plus
  running the tests as committed).
