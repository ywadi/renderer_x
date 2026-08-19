# Issue #30 fix report — rx_asset_gltf_gpu_tests determinism

Scope: `src/rx_asset/tests/async_import_test.cpp` only (the one file in
`rx_asset_gltf_gpu_tests` that contains wall-clock-derived loops — confirmed
by grepping all four member files of the binary; `import_gltf_gpu_test.cpp`,
`damaged_helmet_test.cpp`, and `import_gltf_basisu_test.cpp` contain no
`while` loops at all). No production code changes ship in this fix — two
production one-liners were touched and restored strictly as temporary
revert-test probes (criterion 4), confirmed byte-identical to HEAD
afterward (`git diff --stat` empty for both files post-restore).

## Root cause

Six loops in `async_import_test.cpp` put a doctest `REQUIRE`/`CHECK` call
**inside** a `while` loop whose iteration count is itself a function of live
wall-clock/thread-scheduling timing (polling `pumpMain()` until an async
import reaches some state, or spinning until a flag flips). Because doctest
counts every `REQUIRE`/`CHECK` **evaluation**, not just failures, the
reported assertion total for an otherwise-always-green run varied by
hundreds of thousands to millions between runs — exactly the "passes green
today, different assertion count each run" symptom in the ticket.

Baseline measurement, captured before any change, 4 unconstrained runs of
the unmodified binary (all 57/57 test cases passed every time):

```
[doctest] assertions: 8203599 | 8203599 passed | 0 failed |
[doctest] assertions: 8153987 | 8153987 passed | 0 failed |
[doctest] assertions: 8399006 | 8399006 passed | 0 failed |
[doctest] assertions: 8324786 | 8324786 passed | 0 failed |
```

Four different totals, same 57/57 pass — the defect reproduced directly,
matching the ticket's own description.

## Criterion 1 — deterministic assertion quantities

All six offending loops were restructured the same way: the loop body now
only accumulates state (a `bool` flag that latches permanently on the first
violating observation, or a running `max`/`min`), and the
`REQUIRE`/`CHECK` moves to **exactly once, after the loop**, evaluating the
aggregated result. This makes the assertion **count** for each claim fixed
regardless of how many wall-clock-timed iterations the loop happened to run,
while the assertion **outcome** is unchanged: a defect that would have
tripped the old per-iteration assertion on any single iteration still trips
the new aggregated one (a flag latches the first time it's violated; a `max`
still captures the worst outlier no matter how many fast iterations
surround it).

Six sites fixed, by TEST_CASE (line numbers are post-fix):

1. **L406 "buffer/image byte-source reads happen off BOTH threads"**
   (io-thread-participation wait, ~L433): per-spin `REQUIRE(now < deadline)`
   → loop condition absorbs the deadline; single `REQUIRE(ioThreadIdKnown...)`
   after.
2. **L470 "progress is monotonic"** (~L510-536): per-tick `CHECK(rank >=
   lastRank)` + `CHECK(itemsCompleted >= lastItemsCompleted)` +
   `REQUIRE(now < deadline)` → `rankRegressed`/`itemsRegressed` flags,
   3 fixed assertions after the loop (`REQUIRE(sawTerminal)`,
   `CHECK_FALSE(rankRegressed)`, `CHECK_FALSE(itemsRegressed)`).
3. **L666 "WALL-CLOCK GATE"** (~L855-888), THE flagship load-bearing test
   per the file's own header comment: per-pump `REQUIRE(pumpDuration <
   kCiStallDetector)` + `REQUIRE(now < testDeadline)` → `maxPumpDuration`
   tracked across the whole run, single `REQUIRE(maxPumpDuration <
   kCiStallDetector)` after the loop. This loop was the dominant
   contributor to the multi-million-assertion baseline above.
4. **L940 "cancelImport() after >=1 real GPU resource... rolls BOTH
   back"** (mid-upload rollback, ~L995-1013): per-spin
   `REQUIRE_FALSE(done)` → `finalizedBeforeGeometryObserved` flag,
   `CHECK_FALSE(...)` after.
5. **L1094 "cancelImport() after EXACTLY 1 of N texture slots"**
   (checkerboard-safety, ~L1140-1163): per-spin `REQUIRE_FALSE(done)` +
   `REQUIRE(live <= +1)` → `finalizedBeforeOneRegistered` /
   `overshotPastOne` flags, two `CHECK_FALSE(...)` after.
6. **L1242 "a resolve-heavy loop against an ALREADY-COMPLETED import"**
   (concurrent overlap test, ~L1300-1335): per-tick `REQUIRE(submeshes.size()
   == 1)` + 6x `CHECK(...)` range/bounds comparisons + `REQUIRE(now <
   deadline)` → `submeshCountWrong` / `dataCorrupted` flags,
   `REQUIRE_FALSE(submeshCountWrong)` + `CHECK_FALSE(dataCorrupted)` after.

No case required a missing product seam (a controllable fake clock, etc.) —
every claim's discriminating signal was already available as a real,
already-computed value inside the loop (a duration, a boolean flag, a
comparison result); the only change needed was moving *when* it gets
asserted, not what gets measured. **No BLOCKED cases.**

## Criterion 2 — 10-run determinism proof

Post-fix, 10 consecutive serial runs, `taskset -c 0,1` (throttled to 2
cores), no other GPU/CPU-heavy process running concurrently:

```
RUN 1  (exit 0): test cases: 57 | 57 passed | 0 failed | 0 skipped | assertions: 1239 | 1239 passed | 0 failed | | Status: SUCCESS!
RUN 2  (exit 0): test cases: 57 | 57 passed | 0 failed | 0 skipped | assertions: 1239 | 1239 passed | 0 failed | | Status: SUCCESS!
RUN 3  (exit 0): test cases: 57 | 57 passed | 0 failed | 0 skipped | assertions: 1239 | 1239 passed | 0 failed | | Status: SUCCESS!
RUN 4  (exit 0): test cases: 57 | 57 passed | 0 failed | 0 skipped | assertions: 1239 | 1239 passed | 0 failed | | Status: SUCCESS!
RUN 5  (exit 0): test cases: 57 | 57 passed | 0 failed | 0 skipped | assertions: 1239 | 1239 passed | 0 failed | | Status: SUCCESS!
RUN 6  (exit 0): test cases: 57 | 57 passed | 0 failed | 0 skipped | assertions: 1239 | 1239 passed | 0 failed | | Status: SUCCESS!
RUN 7  (exit 0): test cases: 57 | 57 passed | 0 failed | 0 skipped | assertions: 1239 | 1239 passed | 0 failed | | Status: SUCCESS!
RUN 8  (exit 0): test cases: 57 | 57 passed | 0 failed | 0 skipped | assertions: 1239 | 1239 passed | 0 failed | | Status: SUCCESS!
RUN 9  (exit 0): test cases: 57 | 57 passed | 0 failed | 0 skipped | assertions: 1239 | 1239 passed | 0 failed | | Status: SUCCESS!
RUN 10 (exit 0): test cases: 57 | 57 passed | 0 failed | 0 skipped | assertions: 1239 | 1239 passed | 0 failed | | Status: SUCCESS!
```

All 10 runs: **identical** 1239/1239 assertions, 57/57 test cases, 0
failed. Down from a baseline that varied by up to ~245,000 assertions run
to run (8153987-8399006), now bit-for-bit identical across every run.

Additionally: 3/3 unconstrained sanity runs (no taskset) also produced
1239/1239 every time, and the same binary rebuilt for windows-cross-zig and
run under Wine (both a single `--test-case="*WALL-CLOCK*"` invocation and
the full suite via ctest) also produced exactly 1239/1239 — the fixed count
holds across both platforms this repo builds for, not just one throttled
environment.

**[Round 2 amendment — scoping this claim honestly, per independent review]**
The framing above ("1239/1239 assertions... now bit-for-bit identical
across every run") is accurate for every run in the batch actually captured
and for what issue #30 itself targets, but reads as fully load-independent,
which overstates it. Precisely: **the assertion COUNT is deterministic on
every PASSING run** — that is the defect #30 describes ("passes green
today, different assertion count each run") and it is fixed, unconditionally,
by construction (the six sites no longer have a wall-clock-derived number of
assertion evaluations, full stop, regardless of host load). It is a
SEPARATE property that **the WALL-CLOCK GATE's stall-detector gate can still
FAIL** (not silently pass with a different count — actually red) under
genuinely heavy external GPU/CPU contention: it is a live-timing pass/fail
check by design (RC6, self-calibrated, `max(2ms, 4x live probe)`), and this
fix's own stated scope never touches that calibration formula. The
independent reviewer's 40-run reproduction under real contention (a
leftover competing GPU-rendering process) saw exactly this: 31/31 *passing*
runs read bit-for-bit 1239/1239 (zero exceptions — the count-determinism
claim held perfectly), while 3/40 runs failed outright on tight
wall-clock-gate margins or a separate pre-existing race (see the Round 2
section below) — contention that produces a red run, not a silently
different green count, which is a materially different (and much less
severe) failure mode than the one issue #30 was filed against. See the
Round 2 section below for the reviewer's full reproduction and this
addendum's disposition.

**Process note for the reviewer:** an earlier attempt at this same 10-run
proof, run while a separate Wine-based ctest invocation of this same
GPU-backed binary was running concurrently on the same machine (my own
methodology error, not a product or test defect), produced 2 spurious
failures (runs 6-7, both `1231/1230 passed/1 failed`, identical signature
both times — consistent with resource contention, not a real race: a lone,
immediate rerun once the concurrent Wine job had finished passed cleanly at
1239/1239). That contaminated attempt was discarded; the 10-run sequence
above was captured with nothing else running on the machine.

## Criterion 3 — no weakening of load-bearing claims

Every rework in this fix is a pure **aggregation**, not a relaxation: the
per-iteration `REQUIRE`/`CHECK` predicate is unchanged (same comparison,
same operands), only *when* it is evaluated moves (from every iteration to
once, over the aggregated worst-case/flag). A regression that would have
tripped the old form on iteration *k* still trips the new form, because:

- A boolean flag (`rankRegressed`, `itemsRegressed`,
  `finalizedBeforeGeometryObserved`, `finalizedBeforeOneRegistered`,
  `overshotPastOne`, `submeshCountWrong`, `dataCorrupted`) latches
  permanently the first time it is set — a violation on iteration 3 of 50000
  is exactly as visible in the aggregated flag as it was in the original
  per-iteration assertion.
- `maxPumpDuration` (the wall-clock stall-detector claim) takes the running
  maximum — a single slow outlier cannot be diluted by however many fast
  iterations run alongside it; `max()` is monotonically non-decreasing and
  ignores nothing but the best-case iterations, which were never the
  discriminating signal anyway.

The stall-detector self-calibration itself (max(2ms, 4x live probe),
RC6) is untouched — this fix only changes how many times its result gets
asserted, never what it measures or how the threshold is derived.

## Criterion 4 — revert-style discrimination evidence

Two most load-bearing claims in this file (per its own header comment,
which names the WALL-CLOCK GATE as "THE load-bearing test" and the D25
zero-wait-calls counters as the direct, non-timing-based proof of the same
no-blocking-on-the-async-path invariant), both inside the same reworked
TEST_CASE (WALL-CLOCK GATE, L666), in-tree revert-and-restore (scratch
worktree's `toolchain/` symlink recipe was not needed — no `toolchain/`
dependency touched by either revert):

### Discriminator 1 — wall-clock stall detector (`REQUIRE(maxPumpDuration < kCiStallDetector)`, L888)

Revert: `src/rx_asset/import_gltf.cpp`, `marshalGltfImportPrepareStep()` —
removed the early `return true;` that yields after registering ONE texture
slot per call (the RC6 time-slicing fix), reproducing the pre-RC6
"register every texture slot inside one call" shape.

FAIL (reverted), `--test-case="*WALL-CLOCK*"`:
```
/home/ywadi/d2/renderer_x/src/rx_asset/tests/async_import_test.cpp:888: FATAL ERROR: REQUIRE( maxPumpDuration < kCiStallDetector ) is NOT correct!
  values: REQUIRE( 18296µs <  7672µs )
[doctest] test cases:  1 |  0 passed | 1 failed | 56 skipped
[doctest] assertions: 17 | 16 passed | 1 failed |
[doctest] Status: FAILURE!
```

Restored (`git diff --stat` on `import_gltf.cpp` empty), rebuilt, PASS:
```
wall-clock gate: max single pumpMain() call = 4938 us across 7819235 in-flight frame(s); ...
self-calibrated CI-tier ceiling this run was 9160 us
[doctest] test cases:  1 |  1 passed | 0 failed | 56 skipped
[doctest] assertions: 25 | 25 passed | 0 failed |
[doctest] Status: SUCCESS!
```

### Discriminator 2 — D25 zero-wait-calls-on-async-path (`CHECK(textureWaitCallsBefore ...)`, L913)

Revert: `src/rx_asset/texture_cache.cpp`, `TextureCache::registerDecoded()`
— added `++waitCallCountForTesting_; uploader_.wait(uploader_.flush());`,
reintroducing a blocking wait on the async-only path (a direct D25
violation).

FAIL (reverted), `--test-case="*WALL-CLOCK*"`:
```
/home/ywadi/d2/renderer_x/src/rx_asset/tests/async_import_test.cpp:913: ERROR: CHECK( fixture->textures->waitCallCountForTesting() == textureWaitCallsBefore ) is NOT correct!
  values: CHECK( 6 == 0 )
[doctest] test cases:  1 |  0 passed | 1 failed | 56 skipped
[doctest] assertions: 25 | 24 passed | 1 failed |
[doctest] Status: FAILURE!
```

Restored (`git diff --stat` on `texture_cache.cpp` empty), rebuilt, full
suite PASS: `[doctest] test cases: 57 | 57 passed | 0 failed | 0 skipped` /
`[doctest] assertions: 1239 | 1239 passed | 0 failed |` / `Status:
SUCCESS!`.

Both reverts are minimal, single-purpose, and were restored to
byte-identical HEAD content (`git diff --stat` empty for both files)
before any further verification or the final ctest/commit steps below.

## Criterion 5 — full verification, zero unfiltered validation errors

- `rx_asset_gltf_gpu_tests` standalone (post-fix, post-revert-restore):
  `57 | 57 passed | 0 failed | 0 skipped`, `1239 | 1239 passed | 0 failed`,
  `Status: SUCCESS!`.
- Serial linux ctest (`xvfb-run -a ctest --output-on-failure -j1`):
  **22/22 passed**, 137.34s total (`rx_asset_gltf_gpu_tests` 41.48s).
- windows-cross-zig build: clean rebuild (`ninja` reports the touched
  translation units + dependents relinked, no warnings-as-errors/failures).
- windows-cross-zig ctest under Wine, matching CI's own exclusion filter
  (`-E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'`): **10/10 passed**,
  111.75s total; `rx_asset_gltf_gpu_tests` itself: `1239 | 1239 passed | 0
  failed`, `57 | 57 passed` — the SAME fixed count as linux-native.
- Zero unfiltered validation errors: every clean run above reports
  `Status: SUCCESS!` with 0 failed assertions; `doctest_main_gltf_gpu.cpp`'s
  own process-lifetime `processValidationErrorCount()` re-check (which
  would flip a clean doctest run to a nonzero exit code if any unfiltered
  Vulkan validation error fired anywhere, including during fixture
  teardown) never triggered. The only validation-layer lines observed in
  any run are the two pre-existing, explicitly-tagged known-false-positives
  (`VUID-VkInstanceCreateInfo-flags-zerobitmask` /
  `VK_KHR_portability_enumeration` warning) that this project's own
  `debugCallback()` filter list already documents and excludes.

## Deviations from the brief

- No BLOCKED cases — all six sites were fixable without a new product
  seam.
- The brief's example revert path ("scratch worktree — symlink
  `toolchain/` into it") was not needed: neither revert touched anything
  under `toolchain/`, so a plain in-tree edit-build-test-restore cycle was
  sufficient and is fully evidenced above (both files confirmed
  byte-identical to HEAD via `git diff --stat` before the final green
  verification run).
- One self-inflicted process hiccup during proof-gathering (documented
  under criterion 2 above) — a stale contaminated attempt, discarded; the
  final 10-run proof was captured cleanly.

## Round 2 — independent review findings, closed in-round

Independent review (`issue-30-review.md`, spec ✅ / Approved, 1 Medium + 2
Low) verified all five criteria and the six-site rework itself line-by-line
("No vacuousness, averaging-away, or predicate-weakening found in any of
the six reworked sites"). Three items closed this round, per project policy
(no deferred fixes; pre-existing defects close when found unless a named
prerequisite blocks — none did here).

### Item 1 (Medium, part A) — report accuracy

Addressed above, in Criterion 2, via the "[Round 2 amendment]" paragraph:
the "1239/1239 every run" claim now explicitly scopes to "deterministic on
every passing run" and separately documents that the WALL-CLOCK GATE's
self-calibrated margin remains a live-timing pass/fail property, not
load-proof, by design and by explicit out-of-scope declaration in this
fix's own Criterion 3. No code change was needed or made for this part (the
finding was about report framing, not behavior).

### Item 2 (Medium, part B) — `pumpUntilTerminal()`/callback race (newly surfaced, pre-existing, fixed)

**Diagnosis.** The reviewer's RUN 34 failure (`async_import_test.cpp:592`
at the time, `REQUIRE(done)` in the "garbage-bytes import" TEST_CASE — one
of the file's ten call sites of the shared `pumpUntilTerminal()` helper,
untouched by the six-site rework) traced to a genuine, pre-existing product
race, not test infrastructure:

- `pumpUntilTerminal()` returns as soon as `registry.importProgress(handle)`
  reports a terminal `stage` (Done/Failed/cancelled). Every TEST_CASE in
  this file (and any real host application following the same documented
  pattern) then assumes the paired completion callback has already fired.
- True for every terminal transition **except** a compute-phase PARSE
  failure: `computeGltfImport()` (`import_gltf.cpp`) used to call
  `setStage(stageOut, ImportStage::Failed)` **directly on the worker
  thread**, in both its `dataBuffer`-wrap and `parser.loadGltf` early-return
  paths — fully decoupled from, and racing ahead of, the completion
  callback, which only fires later when that same worker's own
  `postToMain(finishAsyncImportCompute)` closure is actually drained by a
  `pumpMain()` call on the main thread.
- Every OTHER terminal transition sets `stage` and fires the callback in
  the SAME synchronous call (`pollAsyncImportUploads()`'s Done path,
  `finishAsyncImportCompute()`'s own `compute.error` branch, and all three
  IO-thread-read-failure closures in the path-based `importGltfAsync()`
  overload, `registry.cpp`) — no window exists there. Confirmed by reading
  every `job->stage.store(...)`/`setStage(...)` call site in both files
  (`grep -n` inventory, 7 total sites: 2 racy, 5 already-correctly-paired).
- Verdict: **product defect**, not test infrastructure. `pumpUntilTerminal()`
  itself is correct — it faithfully polls the documented public API; the API
  itself briefly lied about being terminal on this one path.

**Fix.** `src/rx_asset/import_gltf.cpp`: deleted both direct
`setStage(stageOut, ImportStage::Failed)` calls. `finishAsyncImportCompute()`
(`registry.cpp:207`) already authoritatively sets `stage = Failed` in the
exact same synchronous call as firing the completion callback for a
compute-phase error — the deleted stores were pure redundant writes with
zero functional purpose beyond creating the race. This makes the race
**structurally impossible**, not merely less likely: after the fix there is
exactly one write site for a compute-phase `Failed` transition, and it is
permanently paired with the callback by construction.

**Reproduction and regression test (stress harness, per the review's own
suggestion).** Added a new permanent TEST_CASE, `"importGltfAsync: [race
regression] repeated garbage-bytes imports never observe a terminal stage
before the paired completion callback has fired"` (`async_import_test.cpp`),
running the exact garbage-bytes-async-import pattern in a fixed
(non-wall-clock-derived) loop, tracking a single `raceHits` counter, one
aggregated `REQUIRE(raceHits == 0)` after the loop (same discipline as the
six original sites — no per-iteration assertion, no issue-#30-class count
variance of its own):

1. **Pre-fix, unwidened, quiet host** (nothing else running,
   `taskset -c 0,1`, 5000 iterations): **0/5000** — the natural window is
   too narrow to hit reliably without contention on this machine, consistent
   with the reviewer's own 2/40-under-real-contention rate and explaining
   why it was never noticed before.
2. **Pre-fix, artificially widened** (temporary, since-removed
   `std::this_thread::sleep_for(500us)` inserted immediately after the
   direct `setStage(Failed)` store, to force the same window the reviewer
   hit via real scheduling contention): **31/5000**, `taskset -c 0,1`:
   ```
   race regression: 31 / 5000 iterations observed stage-terminal before the completion callback fired (0 separate timeout(s))
   /home/ywadi/d2/renderer_x/src/rx_asset/tests/async_import_test.cpp:676: FATAL ERROR: REQUIRE( raceHits == 0 ) is NOT correct!
     values: REQUIRE( 31 == 0 )
   [doctest] test cases: 1 | 0 passed | 1 failed | 57 skipped
   [doctest] Status: FAILURE!
   ```
   Confirms both the mechanism (real, exploitable) and that the stress
   harness genuinely detects it.
3. **Fix applied, artificial delay removed, same harness, same host**
   (`taskset -c 0,1`, 5000 iterations — later right-sized to 2000 for the
   permanent version, see the test's own comment): **0/5000**:
   ```
   race regression: 0 / 5000 iterations observed stage-terminal before the completion callback fired (0 separate timeout(s))
   [doctest] test cases: 1 | 1 passed | 0 failed | 57 skipped
   [doctest] Status: SUCCESS!
   ```

No BLOCKED — no missing product seam; the fix needed only to delete two
redundant, racy lines.

### Item 3 (Low) — restore `REQUIRE` semantics at sites 4/5

`async_import_test.cpp`: the post-loop aggregated assertions at site 4
(mid-upload rollback, `finalizedBeforeGeometryObserved`) and site 5
(checkerboard-safety, `finalizedBeforeOneRegistered` / `overshotPastOne`)
were `CHECK_FALSE`; restored to `REQUIRE_FALSE`, matching the original
per-iteration `REQUIRE_FALSE`/`REQUIRE` abort-on-violation semantics the
six-site rework had (unintentionally) downgraded. Aggregation shape
(latching flag, single post-loop assertion) is unchanged — this is a
severity-of-failure fix (abort vs. keep running against invalidated state),
not a discrimination change.

### Round 2 re-verification

Full binary now 58 TEST_CASEs (57 original + 1 new race-regression stress
test), 1248 assertions (1239 + 9 new).

**Fresh 10-run determinism proof**, `taskset -c 0,1`, quiet host (the
coordinator killed the reviewer's leftover contending process beforehand;
confirmed via `ps aux`/`uptime` immediately before running), three
foreground batches:

```
RUN 1  (exit 0): test cases: 58 | 58 passed | 0 failed | 0 skipped | assertions: 1248 | 1248 passed | 0 failed | | Status: SUCCESS!
RUN 2  (exit 0): test cases: 58 | 58 passed | 0 failed | 0 skipped | assertions: 1248 | 1248 passed | 0 failed | | Status: SUCCESS!
RUN 3  (exit 0): test cases: 58 | 58 passed | 0 failed | 0 skipped | assertions: 1248 | 1248 passed | 0 failed | | Status: SUCCESS!
RUN 4  (exit 0): test cases: 58 | 58 passed | 0 failed | 0 skipped | assertions: 1248 | 1248 passed | 0 failed | | Status: SUCCESS!
RUN 5  (exit 0): test cases: 58 | 58 passed | 0 failed | 0 skipped | assertions: 1248 | 1248 passed | 0 failed | | Status: SUCCESS!
RUN 6  (exit 0): test cases: 58 | 58 passed | 0 failed | 0 skipped | assertions: 1248 | 1248 passed | 0 failed | | Status: SUCCESS!
RUN 7  (exit 0): test cases: 58 | 58 passed | 0 failed | 0 skipped | assertions: 1248 | 1248 passed | 0 failed | | Status: SUCCESS!
RUN 8  (exit 0): test cases: 58 | 58 passed | 0 failed | 0 skipped | assertions: 1248 | 1248 passed | 0 failed | | Status: SUCCESS!
RUN 9  (exit 0): test cases: 58 | 58 passed | 0 failed | 0 skipped | assertions: 1248 | 1248 passed | 0 failed | | Status: SUCCESS!
RUN 10 (exit 0): test cases: 58 | 58 passed | 0 failed | 0 skipped | assertions: 1248 | 1248 passed | 0 failed | | Status: SUCCESS!
```

All 10 runs identical, 0 failed. This time, unlike the (honestly-scoped, per
item 1 above) round-1 claim, this determinism proof was captured on a
verified-quiet host with no known contention source — the strongest form of
the proof available.

**Full verification, post round-2 changes:**

- Serial linux ctest (`xvfb-run -a ctest --output-on-failure -j1`):
  **22/22 passed**, 138.73s total (`rx_asset_gltf_gpu_tests` 43.96s).
- windows-cross-zig: clean rebuild (`import_gltf.cpp` +
  `async_import_test.cpp` recompiled, static lib + 4 dependent executables
  relinked, no warnings/errors). ctest under Wine, CI's own filter
  (`-E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'`): **10/10 passed**,
  127.98s total; `rx_asset_gltf_gpu_tests` itself:
  `test cases: 58 | 58 passed | 0 failed | 0 skipped` /
  `assertions: 1248 | 1248 passed | 0 failed` — exact count parity with
  linux-native.
- Zero unfiltered validation errors: every run above reports
  `Status: SUCCESS!`; no new validation-layer lines beyond the two
  pre-existing known-false-positives.

### Round 2 diff footprint

- `src/rx_asset/import_gltf.cpp`: 2 lines deleted (`setStage(stageOut,
  ImportStage::Failed)` in `computeGltfImport()`'s two early-return
  compute-phase-failure paths), replaced with explanatory comments. `git
  diff` reviewed directly — no stray whitespace changes, no other lines
  touched.
- `src/rx_asset/tests/async_import_test.cpp`: +1 new permanent TEST_CASE
  (the race-regression stress harness) and the 4 `CHECK_FALSE` →
  `REQUIRE_FALSE` restorations (item 3). No other test case touched.
- No board/issue/plan/spec/ledger files touched
  (`.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md` still
  carries the same pre-existing, not-mine local modification noted in
  round 1 — left as found).

## Commits

Round 1:
- `45936cf` — `test(rx_asset): make rx_asset_gltf_gpu_tests assertion
  counts deterministic (issue #30)`, pathspec-scoped to
  `src/rx_asset/tests/async_import_test.cpp` only.
- `adaa4b3` — `docs: issue #30 fix report...`, pathspec-scoped to the
  report file only.

Round 2:
- `07a2474` — `fix(rx_asset): close pumpUntilTerminal()/completion-callback
  race on async parse failure (issue #30 round 2)`, pathspec-scoped to
  `src/rx_asset/import_gltf.cpp` + `src/rx_asset/tests/async_import_test.cpp`
  (the production fix and its paired regression test/REQUIRE-restoration,
  committed together).
- This report amendment: a separate, following commit, pathspec-scoped to
  `issue-30-report.md` only.

No production code diff ships uncommitted or unexplained — the round-1
revert-test edits to `import_gltf.cpp`/`texture_cache.cpp` were restored
before committing (confirmed via `git diff --stat`); round 2's
`import_gltf.cpp` change is a real, permanent, committed production fix
(the two-line deletion above), independent of and unrelated to round 1's
revert-test probes (different lines, different function branches).
