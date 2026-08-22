# Issue #76: rx_asset GPU-test wall-clock flake — root-cause report

Branch `task/i76-asset-flake`, base `4a14636`. Machine solo (no concurrent
dispatches). Investigator round per repo's Wine-flake precedent:
instrument-first, root-cause before any fix.

## Scope

`rx_asset_gltf_gpu_tests` intermittently fails one specific TEST_CASE in
`src/rx_asset/tests/async_import_test.cpp`: the "WALL-CLOCK GATE" test's
`REQUIRE(maxPumpDuration < kCiStallDetector)` — a self-calibrating
wall-clock stall detector guarding `Registry::importGltfAsync()`'s
time-sliced marshal pipeline (spec D5/D18-RC6/D24/D25). No other test in
the suite has ever failed as part of this flake class across three prior
sighting rounds (T10, T13, T14) or this round's own characterization —
correctness assertions have never failed, only this one timing `REQUIRE`.

Before this round, the detector already used a "self-calibrating" design
(Phase 4 Task 15 / RC6): it measures a synthetic `TextureCache::
registerDecoded()` call immediately before and after the real import loop,
and asserts the loop's worst single `pumpMain()` call stays under 4x the
larger of those two calibration samples (with a 2ms floor). This survived
Phase 4's own recalibration round but still flaked three more times in
Phase 5 (T13 once on NVIDIA, T14 both under `-j2` GPU contention and
standalone-on-repeat) — the standalone-reproduction finding broke the
"contention-only" framing and triggered this dedicated round (issue #76).

## Characterization (pre-fix, wall-clock detector)

All runs: real DamagedHelmet-scale async import (5 JPEG textures + 1
combined geometry upload), `--validate` (Vulkan validation layer active),
offscreen via a private `Xvfb :99` (no on-desktop windows), niced test
process, machine otherwise idle unless a condition below says "load
induced." Driver identity confirmed each batch (`VK_ICD_FILENAMES`
pinned to `lvp_icd.json` or `nvidia_icd.json`; NVIDIA = GeForce RTX 2080,
driver 580.82.07).

| Condition | Driver | N | Fails | Rate |
|---|---|---:|---:|---:|
| Standalone | lavapipe | 90 | 1 | 1.1% |
| Standalone | NVIDIA | 220 | 10 | 4.5% |
| Standalone, synthetic CPU load (6 pinned busy-loops, cores 0-5) | NVIDIA | 20 | 0 | 0% |
| Concurrent with 4 other real GPU-test binaries (`rx_cluster_gpu_tests`, `rx_graph_gpu_tests`, `rx_material_gpu_tests`, `rx_ibl_gpu_tests`) — matches T14's own `-j2`/`-j4` sighting | NVIDIA | 3 | 3 | 100% |
| **Combined pre-fix** | both | **333** | **14** | **4.2%** |

Two things fall directly out of this table:

1. **The flake reproduces standalone, with zero external load, on both
   drivers** — T14's "broader than contention-only" finding is confirmed,
   not an artifact of that review's own environment.
2. **Pure synthetic CPU load does NOT reproduce it** (0/20), but **real
   concurrent GPU-driver activity from other test binaries reproduces it
   at a much higher rate** (3/3 in the small sample gathered before the
   fix — every attempted run failed). This distinction is the first clue
   the mechanism is driver/GPU-queue-level contention, not generic CPU
   scheduling pressure.

Every failure signature was the same assertion at the same line
(`REQUIRE(maxPumpDuration < kCiStallDetector)`), e.g.:

```
REQUIRE( 7997µs <  7636µs )     // NVIDIA standalone, ~5% margin miss
REQUIRE( 15847µs <  7664µs )    // NVIDIA standalone, ~2.1x over
REQUIRE( 25473µs <  25200µs )   // lavapipe standalone, ~1% margin miss
REQUIRE( 17408µs <  9276µs )    // NVIDIA, multi-process GPU contention
```

No correctness assertion ever failed alongside these (mesh/material data
always matched, `waitCallCountForTesting()` counters always stayed at
baseline, pool/texture accounting always checked out) — this is a pure
timing-gate trip, not a data-correctness or liveness failure (the import
always completed; the 20s in-loop deadline and the outer 300s ctest
timeout were never remotely threatened — worst observed wall time on a
single `pumpMain()` call across the whole investigation was 25.5ms).

## Instrumentation

Temporary per-tick forensic instrumentation was added to the WALL-CLOCK
GATE test (env-var gated, `RX_I76_TRACE=1`; fully removed before the final
commit — see "Instrumentation removal" below) capturing, for every
`pumpMain()` call that did real marshal work or exceeded the 2ms local
budget:

- wall-clock duration (`std::chrono::steady_clock`, as before)
- thread CPU time via `getrusage(RUSAGE_THREAD, ...)` (`ru_utime`+`ru_stime`)
- thread CPU time via `clock_gettime(CLOCK_THREAD_CPUTIME_ID, ...)` (added
  after the `getrusage()` samples showed suspicious ~1ms clustering,
  consistent with coarse tick-based accounting that would make small
  calibration samples noisy — the high-resolution source confirmed the
  `getrusage()` readings were directionally correct but coarser)
- voluntary/involuntary context-switch counts, minor/major page faults
  (`getrusage`)
- `GeometryPool::stats()` and `TextureCache::liveTextureCountForTesting()`
  before/after, to attribute each event to a specific marshal step
  (texture N of 5, or the one geometry upload)

### A captured failing run (NVIDIA, standalone)

```
i76trace tick=2801062 stageBefore=3 pumpDurationUs=6034  threadCpuUs=3071 ... voluntaryCtxSwitches=3
i76trace tick=2801063 stageBefore=4 pumpDurationUs=3969  threadCpuUs=1455 ... voluntaryCtxSwitches=3
i76trace tick=2801064 stageBefore=4 pumpDurationUs=8677  threadCpuUs=4839 ... voluntaryCtxSwitches=10  <- the tick that tripped REQUIRE( 8677µs < 7388µs )
i76trace tick=2801065 stageBefore=4 pumpDurationUs=3705  threadCpuUs=1579 ... voluntaryCtxSwitches=2
```

The offending tick spent only **~55% of its wall-clock duration actually
executing on a CPU** (4839µs CPU / 8677µs wall) and recorded 10 voluntary
context switches for a single `TextureCache::registerDecoded()` call that
contains no `wait()`/blocking call of its own (confirmed separately —
`waitCallCountForTesting()` stayed at baseline every single run across
this entire investigation, all 333+ pre-fix runs and all post-fix runs).
A second captured lavapipe failure was even more extreme:

```
i76trace tick=1633959 stageBefore=3 pumpDurationUs=25473 threadCpuUs=3532 ... voluntaryCtxSwitches=2, minorFaults=10
    <- REQUIRE( 25473µs < 25200µs ), only 13.9% CPU utilization during this tick
```

A third, captured under real multi-process GPU contention:

```
i76trace tick=1525819 stageBefore=4 pumpDurationUs=10798 threadCpuUs=2925 ... voluntaryCtxSwitches=33
    <- 33 voluntary context switches inside ONE geometry-upload call
```

Pattern, consistent across every one of the ~15 individually-inspected
failing/near-failing ticks: **wall-clock duration on the offending tick is
dominated by the calling thread being descheduled or blocked inside a
driver call, not by additional work performed.** The specific marshal step
implicated varies run to run (sometimes the first texture registration,
sometimes a later one, once the geometry upload) — there is no single
"slow line of code"; the variance is in *when the OS/driver decides to
make this thread wait*, not in what the thread does.

## Root-cause adjudication

The ticket named three hypotheses:

- **(a) unsound self-calibration** — TRUE, but not for the reason
  previously suspected (a single noisy sample; already patched by the
  Task-15 "bracket before/after" fix). The remaining unsoundness is
  structural: the calibration probes are wall-clock measurements of an
  operation whose wall-clock cost is itself dominated by OS/driver
  scheduling noise unrelated to the amount of work performed, so no
  amount of re-sampling that same wall-clock quantity fixes it.
- **(b) genuine liveness/starvation in the async worker pool** — REFUTED.
  Across 333 pre-fix runs plus 200+ post-fix characterization runs plus
  targeted multi-process-contention and revert-testing runs (roughly 900
  total real async imports run this round), the import *always completed*
  correctly, `framesWhileInFlight >= 300` always held, the 20s in-loop
  deadline and 300s ctest timeout were never remotely approached, and the
  D25 zero-`wait()`-calls counters never moved. There is no engine bug.
- **(c) thresholds too tight for real scheduling** — TRUE, but the fix
  is not a threshold bump: the metric being thresholded was the wrong
  one for the defect class it needed to catch. Wall-clock `pumpMain()`
  duration cannot distinguish "this tick did more real work" from "this
  tick's thread got preempted or blocked inside a driver ioctl" — and it
  is demonstrably the latter, not the former, in every captured failure.
  The shipped fix's own two-part design (see "Fix" below) resolves this
  with a direct, deterministic, non-timing proof of the actual defect
  class, rather than any timing threshold at all.

**Root cause:** the stall detector measured wall-clock time on an
operation (`TextureCache::registerDecoded()` / `GeometryPool::
uploadDeferred()`) whose wall-clock duration is legitimately dominated by
OS-scheduling and GPU-driver-internal (NVIDIA ioctl / llvmpipe worker
synchronization) latency that varies independently of how much real work
the call performs — even completely standalone, with no other load, this
project's own async import pipeline creates enough of its own internal
concurrency (4 decode workers + 1 IO thread + 1 worker-task-lane thread +
main, all real threads, on an 8-core desktop) that this latency is
measurable and occasionally large (up to 25ms observed); it gets
substantially worse under real concurrent GPU-driver activity from other
processes (100% reproduction in the small contention sample), which
explains why T14 saw it flake at `-j2`/`-j4` far more than standalone —
but the mechanism was present, at lower rate, even fully standalone,
which is exactly what broke the "contention-only" framing.

## Fix

The fix went through two rounds of its own, both driven by evidence
uncovered while proving discrimination (revert-testing) — recorded here in
full because both rounds are load-bearing for why the shipped design looks
the way it does.

### Round 1: wall-clock → per-thread CPU time

Switch the CI-tier stall detector's basis from wall-clock time to
**per-thread CPU time** (`clock_gettime(CLOCK_THREAD_CPUTIME_ID, ...)`),
for both the self-calibration probes and the in-loop measurement, keeping
the rest of the design (self-calibration, before/after bracketing, 4x
multiplier, 2ms floor, D18's separate wall-clock trend metric) unchanged.
CPU time is immune to the demonstrated failure mode by construction: it
only advances while the thread is actually executing, so OS descheduling
and driver-internal blocking (voluntary context switches inside an ioctl)
do not count against it. For the 133 forensic runs gathered characterizing
the flake, the ratio of (max in-loop per-tick CPU time) to (max
calibration-probe CPU time, same run) never exceeded 3.242x, comfortably
under the unchanged 4x multiplier — on paper, done.

**Revert-testing round 1 found this alone was insufficient.** Sabotaging
`marshalGltfImportPrepareStep()`'s time-slicing (`import_gltf.cpp`,
removing the early `return true` after one texture registration, so all 5
register in a single tick — the historical BLOCKING-DEFECT/oversized-batch
regression shape) reliably tripped the CPU-time `REQUIRE` on NVIDIA
(8/8), but **passed cleanly on lavapipe (0/8)** — a real discrimination
hole. Investigating why: the calibration-BEFORE sample is measured before
any of the import's own worker-thread decode activity has started, i.e.
in a genuinely cold-start state, while calibration-AFTER runs once
everything is already warm. Across 80 NVIDIA runs the BEFORE sample
averaged 1.57x (max 1.77x) the AFTER sample's cost for the *identical*
call; on lavapipe the gap reached 3.5x in one captured run. Taking the
MAX of the two (the Task-15 bracketing design) meant the inflated,
cold-start BEFORE sample dominated the derived ceiling on lavapipe often
enough to swallow the sabotage signal outright.

**Fix:** one untimed, discarded warm-up `registerDecoded()` call
immediately before the timed BEFORE sample, so both timed samples measure
the same (warm) steady state the bracketing design always intended to
compare. Re-running the sabotage: 8/8 on lavapipe too.

### Round 2: a single shared multiplier cannot separate both drivers

Re-running the full false-positive characterization with the warm-up fix
found a second problem: removing the cold-start inflation also shrank the
derived ceiling, and lavapipe's real (contended, in-loop) per-texture cost
turned out to be genuinely, consistently higher relative to an *isolated*
calibration probe than NVIDIA's — a driver characteristic, not noise
(llvmpipe is a software rasterizer; texture upload processing runs on the
CPU and measurably competes with the import's own decode workers for the
same cores, unlike NVIDIA's hardware-offloaded path). Standalone
lavapipe's own **legitimate** worst-case ratio, re-measured over 279 runs
post-warm-up-fix, reached **4.887x**. NVIDIA's own **sabotage** ratio (the
signal this gate must catch), re-measured over 8 reproductions, was as low
as **4.568x**. These two bands overlap (4.887 > 4.568): no single
multiplier value can sit strictly between "lavapipe's own legitimate
noise" and "NVIDIA's own defect signature" — a purely timing-based gate is
mathematically incapable of both never false-positiving on lavapipe and
always catching the regression on NVIDIA, regardless of what multiplier is
chosen.

**Fix: a direct, timing-independent proof for the actual defect class,**
matching the exact "direct proof, not a timing proxy" posture this same
file already establishes for the reintroduced-`wait()` defect class via
`waitCallCountForTesting()`. The loop now tracks
`TextureCache::liveTextureCountForTesting()` (an O(1) counter read) before
and after every `pumpMain()` tick and records the maximum single-tick
delta; `CHECK(maxTextureRegistrationsInOneTick <= 1)` directly encodes
`marshalGltfImportPrepareStep()`'s own documented contract ("registers AT
MOST ONE still-unregistered texture slot... per call") with zero
sensitivity to scheduling noise, driver quirks, or CPU frequency —
5 textures landing in one tick makes this check fail deterministically,
every time, on both drivers, by construction. This is now **the**
authoritative proof of the historical defect class.

The CPU-time `REQUIRE` stays in place as a real, CI-blocking secondary
safety net (it still usefully catches, e.g., a pathological single-call
performance regression unrelated to batching, and empirically still fires
for *some* fraction of the batching sabotage too — see below), but its
multiplier no longer needs to thread the impossible needle above: raised
to **8x**, derived from the 279-run lavapipe worst case (4.887x), giving a
**8/4.887 = 1.637x margin** — comfortably clear of noise on both drivers
(NVIDIA's own post-warm-up worst-case legitimate ratio was only 2.531x
across 80 runs).

`GeometryPool::stats()` (used only by the removed temporary
instrumentation, never by the shipped count-check) is deliberately NOT
called every tick — it walks every block's VMA virtual-block statistics
and allocates a vector, real per-call cost this loop's own multi-million-
iteration busy-poll cannot absorb; `liveTextureCountForTesting()` is a
plain counter read and safe.

The wall-clock `maxPumpDuration`/`kLocalBudget`/`overLocalBudgetCount`
trend metric (D18, "never CI-blocking", MESSAGE-only) is left completely
unchanged throughout both rounds.

## Post-fix verification (final configuration)

| Condition | Driver | N | Fails |
|---|---|---:|---:|
| Standalone | NVIDIA | 100 | 0 |
| Standalone | lavapipe | 100 | 0 |
| Concurrent with 4 other real GPU-test binaries (identical harness to the pre-fix 3/3) | NVIDIA | 12 | 0 |
| **Combined post-fix (final configuration)** | both | **212** | **0** |

Statistical significance: against the combined pre-fix empirical rate of
4.2% (14/333), the probability of observing zero failures across 212
independent post-fix trials by chance alone, if the true rate were
unchanged, is `0.958^212 ≈ 0.011%` (≈ 1 in 9,000). The multi-process
GPU-contention condition — the one that failed 3/3 (100%) pre-fix — passed
12/12 post-fix on an identical harness, a direct apples-to-apples
confirmation independent of the aggregate statistic.

Full suite, both drivers, serial (`ctest -j1`), after the fix and full
revert of the discrimination sabotage: **44/44 green** on lavapipe
(143.4s) and **44/44 green** on NVIDIA (214.5s). Every fixture in this
suite builds its `rx::rhi::Context` with `enableValidation=true` and
`CHECK_FALSE`s `hasValidationErrors()` itself — a full green run is a
direct zero-unfiltered-validation-error proof, not merely an absence of
grep hits.

## Discrimination proof (revert-testing)

Sabotage: `src/rx_asset/import_gltf.cpp`, `marshalGltfImportPrepareStep()`
— comment out the early `return true;` after one texture registration
(the RC6 time-slicing yield), so the `while` loop instead drains every
remaining texture slot in the SAME call, reintroducing the historical
"all 5 textures register in one tick" regression shape. Applied as a
scratch, uncommitted edit; reverted byte-identically after each round
(`git diff` empty both times, confirmed).

**Final configuration, both drivers, N=10 each: 10/10 (100%) fail on
NVIDIA, 10/10 (100%) fail on lavapipe** — 20/20 total, zero misses. Every
failure includes `CHECK( maxTextureRegistrationsInOneTick <= 1 )` failing
(the deterministic, always-firing proof); a fraction also independently
trip the secondary CPU-time `REQUIRE` (e.g. one NVIDIA run:
`REQUIRE( 15205µs < 14152µs )`, ~7.4% over its own self-calibrated
ceiling) — a bonus signal, not a requirement, since the count check alone
is a logical guarantee (5 registrations in one tick is always > 1,
independent of any timing measurement or machine condition).

Earlier, intermediate rounds (kept here for the record, not part of the
final claim): with the CPU-time switch alone (before the warm-up fix),
NVIDIA sabotage was 8/8 caught but lavapipe was 0/8 (the discrimination
hole that motivated the warm-up fix); with the warm-up fix alone (before
the count-check), both drivers caught 8/8 via the CPU-time `REQUIRE` at
the (then still 4x) multiplier — but the *same* run's own
false-positive characterization (n=279 lavapipe) subsequently proved that
4x multiplier itself was unsafe against legitimate noise, which is what
motivated round 2's redesign.

## Instrumentation removal

All temporary forensic instrumentation (the `RX_I76_TRACE` env-var gate,
`getrusage()` sampling, voluntary/involuntary context-switch and
page-fault counters, per-step pool/texture state snapshots printed via
`MESSAGE`) was removed before the final commit — confirmed via
`grep -n "RX_I76_TRACE\|i76Trace\|i76CpuUs\|i76ThreadCpuUsHires"
src/rx_asset/tests/async_import_test.cpp` returning empty. The single
remaining `i76` string in the shipped file is a citation of this report's
own filename in a comment, not leftover instrumentation.

Two pieces earn **permanent** status, because they are not diagnostic
instrumentation — they are the fix itself:

- The `CLOCK_THREAD_CPUTIME_ID`-based `threadCpuTimeUs()` helper (the new
  basis for the secondary CI-tier stall detector).
- The `liveTextureCountForTesting()`-based per-tick texture-registration
  count tracking and its `CHECK(maxTextureRegistrationsInOneTick <= 1)`
  (the new primary, deterministic defect-class proof).

## Concerns / follow-ups

- The historical wall-clock "MULTIPLIER DERIVATION" comment block is kept
  in the test file, marked SUPERSEDED, for provenance (it explains why
  self-calibration/bracketing exist at all, which the new sections build
  on rather than repeat).
- This round did not touch `rx_rhi_vk` or the async import production
  code path at all (confirmed via `git diff` on the final commit) — the
  fix is entirely test-side (a detector-soundness fix), matching the
  root-cause finding that there is no engine defect.
- The two-round discovery process here is itself evidence for why
  "instrument first, sound fix, then prove discrimination" is the right
  order: a plausible, individually-well-reasoned first fix (CPU time)
  looked complete until revert-testing exposed a real gap (the warm-up
  confound), and the SECOND fix's own re-characterization then exposed a
  second, more fundamental gap (the cross-driver multiplier conflict) that
  neither of the first two rounds' own evidence alone would have surfaced.

## Fix round 1 (teardown-hang closure)

Independent review of `30c4b56` (charter PASS, quality Approved) found one
out-of-scope, pre-existing gap while proving discrimination for hypothesis
(b) (genuine liveness/starvation) — see `task-i76-review.md`, adjudication
(b), Finding 1. Reproduced directly, closed in this same round per the
repo's no-deferred-fixes policy.

### The gap

The reviewer injected a real, permanent hang: an infinite loop at the top
of `computeGltfImport()` (the async import compute phase, dispatched via
`Scheduler::runOnWorkerThread()`). `REQUIRE(done)` in the WALL-CLOCK GATE
test — unchanged by `30c4b56`, pre-existing since the [Issue #30] redesign
— correctly detects the hang within its own bounded ~20s in-loop deadline.
But the test **process** then does not exit on its own:
`Scheduler::~Scheduler()`'s call to enkiTS's `WaitforAllAndShutdown()` has
no built-in timeout and blocks forever joining the permanently-stuck
worker thread. Only an external kill (ctest's own outer 300s per-test
`TIMEOUT`, by this project's convention) ever ends such a process, with
zero diagnostic identifying what actually happened. The old wall-clock-
only detector design had the identical `REQUIRE(done)` and identical
`Scheduler` teardown, so it hung identically on a true worker deadlock —
not a regression introduced by `30c4b56`, but a real, pre-existing gap in
the engine's own thread-lifetime safety.

### Design

`Scheduler::~Scheduler()` now races enkiTS's `WaitforAllAndShutdown()`
call against a configurable deadline (`Scheduler::create()`'s new
`shutdownJoinDeadline` parameter, default `kDefaultShutdownJoinDeadline` =
30s) using a dedicated watchdog thread:

- The watchdog thread does **not** call any enkiTS API itself — it only
  races a plain `condition_variable::wait_for()` deadline against a
  `shutdownComplete` flag the original destructor-calling thread sets
  once its own (unmoved, unchanged) call to `WaitforAllAndShutdown()`
  returns. enkiTS keys its own internal bookkeeping (`gtl_threadNum`, a
  `thread_local`) to whichever OS thread first registered as "thread 0"
  at `Scheduler::create()` time (verified directly against the vendored
  enkiTS source) — calling `WaitforAllAndShutdown()` from a *different*,
  unregistered thread would silently alias that same thread-local slot,
  so the actual enkiTS call deliberately stays on the original thread.
- **Happy path**: one thread spawn, one `condition_variable` wait
  satisfied almost immediately once the (unchanged) `WaitforAllAndShutdown()`
  call returns, one join. No new blocking wait is added to that call
  itself.
- **On deadline expiry**: each of the two individually-nameable dedicated
  threads (the IO thread; the worker-task-lane thread `runOnWorkerThread()`
  targets) carries an atomic `executingClosure_` flag, set/cleared around
  the one call that can run arbitrary caller code. The watchdog samples
  both, builds a diagnostic naming whichever was mid-closure (or, if
  neither individually-nameable thread was stuck, honestly reports "likely
  one of the ordinary parallelFor() worker threads — not individually
  nameable here" rather than overclaiming precision), logs it via
  `RX_LOG_ERROR` + an explicit `spdlog` flush (SIGABRT gives no flush
  guarantee), then calls `std::abort()`.
- **Why abort, not detach**: a genuinely stuck thread still holds live
  references into the `Scheduler`'s own state and whatever engine/GPU
  resources its closure was touching. Letting `~Scheduler()` return
  anyway, with that thread still running detached, is not safe — the
  caller is then free to tear down everything downstream (`Device`,
  `Allocator`, GPU memory), and a detached thread that later wakes (or was
  merely slow, not truly stuck) and resumes touching any of that is a
  silent use-after-free at an unpredictable later time, with no
  diagnostic in a release build. A deterministic, loudly-diagnosed crash
  now is strictly safer — the identical "silent corruption is worse than
  a loud crash" posture `rx::core::debug::assertMainThreadImpl()`'s own
  default violation hook already applies to a main-thread guard violation
  (`debug_checks.cpp`), applied here to a different guard.

### Test evidence

An in-process doctest `TEST_CASE` cannot exercise this path: it ends in a
real `std::abort()`, which would take the entire `rx_task_tests` binary
(every other `TEST_CASE` in it) down with it. Closed with a subprocess
harness instead — `rx_task_shutdown_hang_probe` (Linux-only; guarded via
the identical `if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")` precedent
`src/rx_shader/CMakeLists.txt` already establishes; confirmed the
`windows-cross-zig` preset both configures cleanly with the new guard and
builds `rx_task_tests.exe` with the guarded block correctly compiling to
nothing) reproduces the reviewer's own hang-injection technique exactly —
a `runOnWorkerThread()` closure that never returns — with a short,
explicit deadline passed on its command line. A new `scheduler_test.cpp`
`TEST_CASE` spawns it via `fork()`/`execv()`, captures its combined
stdout+stderr through a pipe, and waits via a `waitpid(WNOHANG)` poll
loop bounded by a generous test-side-only safety net (deadline + 15s),
asserting the child terminates via `SIGABRT` (not a normal exit, not the
safety net's own `SIGKILL`) with the expected diagnostic text present.

**Direct probe runs** (`rx_task_shutdown_hang_probe <deadline_ms>`, this
machine):

| Configured deadline | Observed wall time | Exit |
|---|---|---|
| 500ms | 1.035s | SIGABRT (exit 134), diagnostic present |
| 1500ms | 2.019s | SIGABRT (exit 134), diagnostic present |
| 3000ms | 3.525s | SIGABRT (exit 134), diagnostic present |

(Each observed wall time ≈ the configured deadline + the probe's own
200ms head-start sleep + small process overhead — confirms the deadline
is genuinely load-bearing, not a fixed/hardcoded delay.) Every run's
captured output contained `STUCK: worker-task lane thread` and
`shutdown-join deadline`, and never `STUCK: IO thread` (no IO work was
ever submitted in this reproduction).

**Subprocess `TEST_CASE` reliability**: 10/10 clean runs
(`rx_task_tests -tc="*teardown-hang*"`). `rx_task_tests` is device-free
(no `VkDevice`, no Vulkan at all — `src/rx_task/tests/CMakeLists.txt`'s
own header comment) — "driver" is not a meaningful axis for this specific
binary; it ran identically both times it executed as part of the full
44-test `ctest` suite below (once per driver-selected `ctest` invocation,
since `VK_ICD_FILENAMES` has no effect on a binary that never opens a
Vulkan device), plus standalone: **21/21 test cases, 35074/35074
assertions, 0 failed** (up from the pre-existing 20/20, 35066/35066 — the
one new `TEST_CASE` accounts for the delta).

**Happy-path cost**: a normal (non-hung) `rx_task_tests -tc="Scheduler::workerCount*"`
run (creates/destroys several real `Scheduler`s) completed in 0.360s
wall — no perceptible overhead from the new watchdog thread.

**Full suite, both drivers, serial (`ctest -j1`), after this fix**:
**44/44 green** on lavapipe (138.0s) and **44/44 green** on NVIDIA
(210.9s). Zero unfiltered validation errors on the NVIDIA run's own
`LastTest.log` (`grep -c "vulkan validation" ... == 1749`; every single
hit tagged `known false positive`; `grep -v "known false positive"`
→ 0 matches) — the same driver-labeled, zero-unfiltered-validation-error
bar this report's own earlier sections hold to.

### Scope

Touches only `src/rx_task/` (`scheduler.h`, `scheduler.cpp`,
`tests/CMakeLists.txt`, `tests/scheduler_test.cpp`, new
`tests/shutdown_hang_probe.cpp`) — no change to `rx_asset` or the async
import production path itself; the gap this closes was always in
`rx_task::Scheduler`'s own teardown, not in anything the wall-clock-gate
fix (`30c4b56`) touched. `Scheduler::create()`'s new `shutdownJoinDeadline`
parameter is defaulted (`kDefaultShutdownJoinDeadline`, 30s) at every one
of this project's ~119 existing call sites — none required changes.

Commit: `62d7d89` on `task/i76-asset-flake` (base `30c4b56`), not pushed.

## Wine regression fix

`30c4b56`+`62d7d89` merged to `main` (`6dbdd2e`). The `linux-native` CI job
went green, but `windows-cross-zig` failed: CI run `32573745354`, test
`8/15` — `rx_asset_gltf_gpu_tests ..........***Failed   36.76 sec`. This
had been green on the immediately prior push (T14, run `32564785861`).
Closed in a fresh worktree (`i76-wine-fix`, branch `task/i76-wine-fix`,
base `6dbdd2e`) per the coordinator's own charter, main left untouched
beyond this report.

### Reproduction

`rx_asset_gltf_gpu_tests` genuinely runs under Wine — it is **not** in
the Wine CI job's own GPU-exclusion regex
(`rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_material_brdf_gpu|rx_debug_ui_gpu|rx_frame_loop_gpu|rx_ibl_gpu|rx_cluster_gpu|rx_conformance|sample`),
because Wine's `winevulkan` passthrough hands it a real, working
`VkDevice` (backed by the CI runner's own `lavapipe` on the host side).
Reproduced locally with the CI-identical command (`windows-cross-zig`
preset built fresh in the new worktree; a private `Xvfb`-backed Wine
session warmed up with `toolchain_check.exe` first, matching CI's own
warm-up step):

```
ctest --preset windows-cross-zig -E '<the CI regex above>' -R rx_asset_gltf_gpu --output-on-failure
```

**Reproduced on the first try, byte-for-byte identical to the CI log**:

```
FATAL ERROR: REQUIRE( maxPumpCpuDuration < kCiStallDetector ) is NOT correct!
  values: REQUIRE( 10000µs <  2000µs )
[doctest] test cases:   62 |   61 passed | 1 failed | 0 skipped
```

The exact same single `TEST_CASE` (WALL-CLOCK GATE), the exact same
values (`10000µs`/`2000µs`), on both the CI run and this local
reproduction — confirmed via `gh run view 32573745354 --log-failed`
directly against the archived CI log, not inferred.

**The "InvalidFileData" flood was a red herring, not the regression.**
`grep -c InvalidFileData` on the CI log returns 2004 hits, but every one
of them precedes the WALL-CLOCK GATE's own `TEST CASE:` header and
belongs to a *different*, pre-existing, unrelated `TEST_CASE`:
`importGltfAsync: [race regression] repeated garbage-bytes imports...`,
which deliberately imports 2000 iterations of garbage bytes and asserts
zero races (`race regression: 0 / 2000 iterations observed
stage-terminal before the completion callback fired` — passed cleanly).
Each intentional garbage-byte import logs one `RX_LOG_ERROR` line for its
own expected parse failure, accounting for the 2004 hits exactly. This
test predates issue #76 entirely (Issue #30 round 2) and was unaffected
by `30c4b56`/`62d7d89`; it produces the identical log flood on every
green Wine run too. The doctest summary itself is unambiguous: **62 |
61 passed | 1 failed** — exactly one failing assertion, the WALL-CLOCK
GATE's own `REQUIRE`.

### Root cause

`30c4b56` switched the WALL-CLOCK GATE's stall-detector basis from
wall-clock time to `clock_gettime(CLOCK_THREAD_CPUTIME_ID, ...)`,
reasoning (correctly, on Linux) that it measures only time a thread
spends actually executing, immune to OS-scheduling noise. That reasoning
implicitly assumed the clock has FINE resolution on every platform this
binary runs on. Measured directly, on this exact toolchain
(`clock_getres()`, both a standalone probe and in-binary):

| Platform | `clock_getres(CLOCK_THREAD_CPUTIME_ID)` |
|---|---|
| Linux (native) | **1ns** |
| Windows (cross-compiled via zig/mingw-w64, run under Wine) | **15,625,000ns (15.625ms)** |

15.625ms is the classic Windows system clock tick (1/64 second).
mingw-w64's `clock_gettime()` shim for this clock ID is backed by
`GetThreadTimes()`, which the Windows kernel (and Wine's own faithful
emulation of it) updates only once per tick — confirmed directly with a
busy-loop probe cross-compiled and run under Wine: five ~5ms-cost
"small" operations measured deltas of exactly `10000000`/`20000000` ns
(10ms/20ms, whole-tick multiples), never anything finer. **Not fixable
from user space**: `timeBeginPeriod(1)` (the standard Windows API for
requesting finer scheduler-tick granularity) was tried against this exact
toolchain under Wine and changed neither `clock_getres()`'s reported
value nor the measured per-call granularity at all — per-thread CPU-time
accounting on Windows is a mechanism entirely separate from the periodic
timer interrupt that API controls.

Every real operation this gate measures costs single-digit milliseconds
(see the report's own earlier MULTIPLIER DERIVATION sections) — an order
of magnitude *below* a 15.625ms tick. On this platform, every sample this
gate takes therefore reads as either exactly `0` (no tick boundary
crossed during the call) or a spurious whole multiple of one tick (a
boundary happened to be crossed) — precisely the `0 us CPU` calibration
readings and the `10000µs` spike both the CI log and the local
reproduction show, byte-for-byte. This is not corruption, not a byte-source
bug, and not a liveness issue — it is the coarse clock producing exactly
the numbers its own resolution predicts, compared as if they meant
something finer.

### Fix

Measure `clock_getres(CLOCK_THREAD_CPUTIME_ID, ...)` once, at runtime
(not an `#ifdef _WIN32`/platform-macro guess — the actual, measured
condition), and skip the secondary CPU-time `REQUIRE` — **loudly**, via a
`MESSAGE` citing the measured resolution and stating explicitly that this
is a deliberate, justified accommodation — only when the resolution
exceeds a 1ms cutover. That cutover sits seven orders of magnitude above
Linux's own measured value (1ns) and more than one order of magnitude
below Windows/Wine's own measured value (15,625,000ns): picked with
deliberate margin on both sides of the two values that were actually
measured, not tuned to a borderline case, so it cannot misfire on either
platform this binary runs on today.

**No coverage is lost on any platform.** The PRIMARY defect-class proof
this round's own earlier fix already established —
`CHECK(maxTextureRegistrationsInOneTick <= 1)`, fully timing-independent
— is completely untouched by this change and runs, asserted, identically
on every platform. The CPU-time `REQUIRE` was already explicitly
documented (this report's own "Fix round 2" section) as a *secondary*
safety net, not the sabotage-discrimination proof — this fix only
disables that secondary net on the one platform where it is
mathematically incapable of measuring anything meaningful, and only after
measuring that fact directly rather than assuming it.

### Test evidence

**Wine reproduction, before/after** (CI-identical command, private Xvfb,
Wine session warmed up first):

| | Before | After |
|---|---|---|
| `rx_asset_gltf_gpu_tests` under Wine | `REQUIRE(10000µs < 2000µs)` FAILED — 61/62 test cases | **62/62 test cases, 0 failed** |
| Full Wine-filtered `ctest` set (15 tests, CI's own regex) | 1/15 failed | **15/15 (100%) green** (93.0s) |

Repeated 5x standalone under Wine after the fix: **5/5 clean**. The loud
skip diagnostic is present and reads exactly as designed:

```
MESSAGE: wall-clock gate: CLOCK_THREAD_CPUTIME_ID resolution on this platform is too coarse to use as a
CI-blocking secondary safety net (measured via clock_getres(), see cpuTimeResolutionUsable's own comment
above for the full derivation) -- SKIPPING the REQUIRE(maxPumpCpuDuration < kCiStallDetector) check on
THIS run only; the PRIMARY defect-class proof (maxTextureRegistrationsInOneTick <= 1, above) is unaffected
and still asserted.
```

**Discrimination re-proof under Wine**: re-applied the exact same
sabotage this round's own earlier discrimination proof used
(`marshalGltfImportPrepareStep()`'s early `return true` commented out,
`import_gltf.cpp`) and rebuilt for `windows-cross-zig`. The primary count
check still fires under Wine even with the secondary CPU-time check
skipped: `CHECK( maxTextureRegistrationsInOneTick <= 1 )` → `CHECK( 5 <=
1 )` failed, test case FAILED — confirming the platform-independent proof
this round's earlier fix established genuinely holds on every platform,
not just the two Linux drivers. Sabotage reverted byte-identically
(`git diff` empty, confirmed) before committing.

**Linux, both drivers, unaffected** (this fix does not change behavior at
all on Linux — `cpuTimeResolutionUsable` measures `true` there, so the
identical `REQUIRE` fires exactly as before):

| Condition | Driver | N | Fails |
|---|---|---:|---:|
| Standalone (post-fix, false-positive check) | NVIDIA | 40 | 0 |
| Standalone (post-fix, false-positive check) | lavapipe | 40 | 0 |
| Sabotage discrimination | NVIDIA | 5 | 5/5 caught |
| Sabotage discrimination | lavapipe | 5 | 5/5 caught |

**Full suite, both Linux drivers, serial (`ctest -j1`), final state**:
**44/44 green** on lavapipe (142.8s) and **44/44 green** on NVIDIA
(228.0s).

### Scope

Touches only `src/rx_asset/tests/async_import_test.cpp` (74 insertions, 1
deletion) — no engine code changed; this was a test-side measurement-
soundness gap, not a production defect. `Scheduler`, `import_gltf.cpp`,
and every other file this round previously touched are unchanged by this
commit.

Commit: `5691d45` on `task/i76-wine-fix` (base `6dbdd2e`), not pushed.
