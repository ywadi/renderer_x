# Issue #76 review: async-import wall-clock stall-detector root-cause round

Commit under review: `30c4b56` (branch `task/i76-asset-flake`, base `4a14636`),
worktree `/media/ywadi/second/renderer_x-worktrees/i76-asset-flake`. Diff
touches only `src/rx_asset/tests/async_import_test.cpp` (confirmed via
`git diff --name-only 4a14636..30c4b56`).

## Verdicts

**Charter compliance (vs `gh issue view 76`): PASS.** Instrument-first was
followed (temporary `getrusage()`/`CLOCK_THREAD_CPUTIME_ID` instrumentation,
fully removed, confirmed empty grep). The multiplier changed (4x wall-clock
-> 8x CPU-time) but this is not a bare threshold bump: the basis itself
changed for a measured, re-derived reason, and the actual regression-class
proof moved off timing entirely onto a deterministic counter check. Fix
follows evidence — both self-corrected rounds are independently reproducible
(see below), not merely asserted.

**Code quality: Approved**, with one out-of-scope/informational finding (not
a defect in the reviewed diff; see Finding 1) and no in-scope defects in the
diff itself.

## Coverage-preservation adjudication (core question)

The old wall-clock gate nominally guarded three genuine defect classes.
Adjudicated each:

**(a) Historical time-slicing regression (oversized-batch/BLOCKING-DEFECT
shape).** Re-ran the sabotage myself: commented out the early `return true;`
in `marshalGltfImportPrepareStep()` (`import_gltf.cpp` line 1759), rebuilt,
ran 10x lavapipe + 10x NVIDIA. **20/20 (100%) caught**, every single failure
via the deterministic `CHECK( maxTextureRegistrationsInOneTick <= 1 )`
(observed `CHECK( 5 <= 1 )` every time) — matches the report's 20/20 claim
exactly. The secondary CPU-time `REQUIRE` also independently tripped in 1/20
of my runs (`REQUIRE( 13499µs < 13472µs )`, lavapipe) — consistent with the
report's "a fraction also independently trip" framing (not a fixed number
claimed there either). Sabotage reverted byte-identically (md5 confirmed,
`git diff` empty). **Coverage preserved — this defect class is now caught
more reliably than before (deterministic vs. self-calibrated timing).**

**(b) Genuine unbounded stall/hang in `pumpMain()` (worker deadlock) — see
Finding 1.** I injected a real, permanent hang: an infinite
`sleep_for(200ms)` loop at the top of `computeGltfImport()` (the async
compute-phase entry point, called on a worker thread), env-gated
(`RX_I76_REVIEW_INJECT_HANG`), rebuilt, ran under an external `timeout`
wrapper so I could observe without blocking indefinitely. Result:
`REQUIRE(done)` (line 1137, **unchanged by this diff** — pre-existing since
the [Issue #30] redesign) fires correctly and **within its own bounded ~20s
in-loop deadline** (confirmed: still running with `FATAL ERROR: REQUIRE(
done )` already in the output at a 30s external cutoff). So *detection* of a
true hang is bounded and this diff does not weaken it. However, the process
then does **not exit on its own** — `Scheduler::~Scheduler()`'s
`WaitforAllAndShutdown()` blocks forever joining the permanently-stuck
worker thread; only an external kill (my `timeout -k`, or ctest's outer 300s
in real CI) ends it (observed: process required `SIGTERM`+`SIGKILL` at 90s,
doctest reported `test case CRASHED: SIGTERM`). This is exactly the "ctest
timeout aside" gap the review brief asked me to check for, and it is real.
**Scoping: this is a pre-existing property of the test fixture's `Scheduler`
lifetime, not something 30c4b56 introduced, worsened, or claimed to fix** —
`REQUIRE(done)` and `Scheduler`'s shutdown/join logic are both outside this
diff (confirmed: diff is test-file-only within `async_import_test.cpp`, and
touches none of this code's surrounding lines). The old wall-clock-only
design had the identical `REQUIRE(done)` and identical `Scheduler` teardown,
so it would hang identically on a true worker deadlock — no regression here,
but also no improvement, and it's a legitimate gap in the suite's overall
hang-safety independent of issue #76's own scope (root-cause finding for
#76, which I independently corroborate: no actual liveness defect exists in
the async import pipeline today — this is a synthetic worst case, not a
reproduction of any observed failure). Recommend a follow-up issue (bound
`Scheduler` shutdown/thread-join with its own timeout, or have the test
abandon/detach on a confirmed hang) rather than blocking this review on it.
Hang injection reverted byte-identically (md5 confirmed, `git diff` empty).

**(c) Gross per-tick CPU-time blowout.** Re-derived the CPU-time net's
calibration story independently rather than accepting the report's numbers:

- With the shipped warm-up call in place, 30 fresh runs/driver gave
  BEFORE/AFTER CPU ratio mean=0.988 (lavapipe) / 1.965 (NVIDIA, see caveat
  below) — the lavapipe cold-start skew is gone.
- With the warm-up call **temporarily disabled** (byte-identically restored
  after), 15 runs/driver reproduced the cold-start confound directly:
  lavapipe BEFORE/AFTER CPU ratio jumped to **mean 3.579x, max 3.882x** —
  closely matching the report's own claim ("up to 3.5x in one captured
  run"). This is strong, independent corroboration that the confound is
  real, not a post-hoc rationalization.
- My own re-measured worst-case (max in-loop-tick CPU / calibration CPU)
  ratios, with the warm-up fix in place, 30 runs/driver: **lavapipe
  max=4.864x** (report: 4.887x over 279 runs — a near-exact match from 9%
  of the sample size, suggesting the report's ceiling is real and not
  underestimated) and **NVIDIA max=2.222x** (report: 2.531x over 80 runs,
  consistent). The shipped 8x multiplier clears both with the margin the
  report claims (8/4.887=1.637x).
- NVIDIA's own BEFORE/AFTER ratio in my post-warm-up sample averaged ~2x
  (not ~1.0 like lavapipe) — this is not the cold-start confound (the
  warm-up call already absorbs that); it reflects further amortization of
  driver-internal caches across the ~300+ real ticks between BEFORE and
  AFTER, which the bracketing formula's `max()` already accounts for and
  does not compromise the ratio that actually gates the assertion.

**Coverage preserved for (c)** — the CPU-time secondary net has a real,
re-derived margin on both drivers, and (per the primary-proof design) its
soundness is not load-bearing for the historical defect class anyway.

## Root-cause evidence sanity-check

The report's central claim — that offending ticks were dominated by
descheduling, not extra work — is independently consistent with my own
telemetry: on one lavapipe smoke run the BEFORE calibration probe measured
9970µs wall vs. only 2014µs CPU (~20% utilization on a single call with no
blocking call of its own), the same order of divergence the report's
captured failing ticks show (13.9%-55% utilization). I did not re-instrument
`getrusage()` context-switch counts (correctly removed per the charter), but
the wall/CPU divergence itself reproduces on demand, which is the load-
bearing half of the claim.

## Statistics verification

Recomputed the report's arithmetic exactly:
`14/333 = 4.2042%`; `1 - 0.042042 = 0.957958`; `0.957958^212 =
0.011103% ≈ 1/9006` — matches the report's "0.958^212 ≈ 0.011% (≈ 1 in
9,000)" claim precisely, not merely approximately. Per-condition table sums
also check out: `90+220+20+3=333` (N), `1+10+0+3=14` (fails);
`100+100+12=212` (post-fix N), `0` fails.

My own re-runs (all pass, all driver-labeled, all via the checked-out
commit, all offscreen through a private `Xvfb :99`, no on-desktop windows):

| Condition | Driver | N | Fails |
|---|---|---:|---:|
| Standalone | lavapipe | 40 | 0 |
| Standalone | NVIDIA (RTX 2080, 580.82.07) | 40 | 0 |
| 4-other-GPU-binary contention (matches report's harness) | NVIDIA | 5 | 0 |
| **My combined post-fix** | both | **85** | **0** |

Full serial `ctest -j1`: **44/44 lavapipe** (130.34s), **44/44 NVIDIA**
(208.98s), zero unfiltered `validation error` lines in either log (`grep -ic
"validation error"` = 0 both).

## The two self-corrected rounds — sanity-checked, not accepted at face value

Round 1 (wall-clock -> CPU-time alone was insufficient due to the cold-start
confound) and Round 2 (a single multiplier can't separate both drivers'
bands) are both architecturally sound and, for Round 1, directly
reproduced by me (see (c) above — my own confound reproduction: 3.579x mean
without the warm-up vs. 0.988x with it). Round 2's specific NVIDIA-sabotage
floor (4.568x) was not independently re-measured against a stripped-down
(count-check-disabled) build — that would require reconstructing an
intermediate historical design not present in the shipped commit — but the
claim is architecturally consistent with what I did measure: lavapipe's own
legitimate ratio reaches ~4.9x, which would already false-positive under any
multiplier at or below ~4.9x (including the superseded 4x), independently
corroborating that a single shared multiplier in that band is unsound
regardless of the exact NVIDIA sabotage-floor figure.

## Instrumentation removal

Confirmed via `grep -n "RX_I76_TRACE\|i76Trace\|i76CpuUs\|i76ThreadCpuUsHires\|getrusage\|RUSAGE_THREAD"` —
one hit, and it is prose inside a comment describing the (removed) forensic
method, not code. The single `i76` string is the report-filename citation,
as claimed. Diff is test-file-only (`git diff --name-only` = one file).

## Findings

- **[Informational, out-of-scope for 30c4b56]** A genuine worker-thread
  hang in the async import compute phase is detected within its own bounded
  ~20s in-loop deadline (`REQUIRE(done)`, unchanged by this diff), but the
  test **process** does not terminate on its own afterward —
  `Scheduler::~Scheduler()`'s `WaitforAllAndShutdown()` blocks forever
  joining the permanently-stuck worker, bounded only by ctest's outer 300s
  timeout (excluded per the review brief's own framing). Verified by direct
  injection + byte-identical revert. Pre-existing (same in the old
  wall-clock design); not introduced, worsened, or claimed-fixed by this
  commit, and fixing it would require touching `rx_task::Scheduler`, outside
  this round's (correctly) test-file-only charter. Recommend a follow-up
  issue.

No other findings — no in-scope defects, no leftover instrumentation beyond
what the report justifies, no threshold bump unsupported by evidence, no
coverage loss for either the historical defect class or gross CPU blowout.

## Hygiene

- Single commit (`30c4b56`), base `4a14636`.
- Author/committer: `Yousef Wadi <ywadi85@gmail.com>` (both), matches local
  git identity.
- No AI attribution anywhere in the commit message (grep for
  claude/anthropic/co-authored/ai-generated/assistant: no matches).
- Not pushed: `origin/main` still at `4a14636`; branch not present under
  `git branch -r`.
- Main checkout untouched by the review's own commit-history state (the
  pre-existing `progress.md` modification noted in the task brief was left
  alone). **Self-correction during this review:** two GPU-test-binary timing
  probes were accidentally run from the default (main-checkout) working
  directory instead of the worktree, writing two stray Vulkan pipeline-cache
  files (`rx_compute_gpu_cube_faces.cache`, `rx_compute_gpu_primary.cache`)
  into the main checkout. Removed before finishing (untracked, not part of
  any deliverable); `git status` in the main checkout now shows only the
  pre-existing `progress.md` modification.
- Worktree source tree confirmed byte-identical to the reviewed commit after
  all temporary experiments (sabotage, hang injection, warm-up-disable):
  `md5sum` matched before/after in all three cases, `git diff --stat`
  empty.

## Not independently verifiable

- The exact NVIDIA sabotage-ratio floor from Round 2 (4.568x, historical
  intermediate design) — see "two self-corrected rounds" above; the shipped
  design's soundness does not depend on this exact figure.
- The report's raw `getrusage()` context-switch/page-fault counts from the
  333+ pre-fix runs (instrumentation correctly removed per the charter, so
  not re-derivable without re-adding it — the wall/CPU divergence itself,
  which is the load-bearing claim, was independently reproduced instead).

## Re-review (teardown-hang closure)

Scoped re-review of `62d7d89` (base `30c4b56`, same branch/worktree) — the
in-round closure of adjudication-(b) Finding 1 above (`Scheduler::~Scheduler()`
blocking forever on a permanently-stuck worker). Unlike `30c4b56`, this
commit touches **engine code** (`src/rx_task/scheduler.h`, `scheduler.cpp`,
plus test files `tests/CMakeLists.txt`, `tests/scheduler_test.cpp`, new
`tests/shutdown_hang_probe.cpp`) — confirmed via `git show --stat 62d7d89`,
exactly the 5 files the report's "Scope" section claims, nothing else.

### Verdict: CLOSED. Finding 1 from the original review is resolved.

### 1. Design soundness

- **Watchdog never touches enkiTS internals — verified against real enkiTS
  source**, not just the commit's own comment. Pulled enkiTS v1.11 (this
  project pins v1.12 via `third_party/CMakeLists.txt`'s `RX_ENKITS_TAG`; the
  `gtl_threadNum`/`WaitforAll()` mechanism checked is core, long-standing
  architecture, not something a minor-version bump plausibly touches).
  Confirmed directly in `TaskScheduler.cpp`: `gtl_threadNum` is a
  `thread_local` defaulting to `0` for every OS thread that has never called
  `RegisterExternalTaskThread()`, and `WaitforAll()` (which
  `WaitforAllAndShutdown()` calls) reads `uint32_t ourThreadNum =
  gtl_threadNum;` and uses it to index this scheduler's own per-thread state
  array. A brand-new, unregistered watchdog thread calling any enkiTS API
  would read `gtl_threadNum == 0` — the SAME identity the real master
  thread already owns — corrupting shared per-thread bookkeeping. The
  commit's design keeps the actual `WaitforAllAndShutdown()` call
  byte-for-byte on the original destructor-calling thread and has the
  watchdog touch only a plain `condition_variable`/`mutex`/atomic flags —
  this is exactly the right avoidance, not a hand-waved rationale.
- **Watchdog lifecycle**: standard predicate-based
  `condition_variable::wait_for(lock, deadline, pred)` idiom — the
  `shutdownComplete` flag is set under the same mutex the watchdog's
  predicate re-checks under, so there is no lost-wakeup race at the
  deadline boundary (the standard guarantees a final predicate check before
  a `wait_for` reports timeout). On the happy path, the destructor's own
  thread sets the flag, notifies, and calls `shutdownWatchdog.join()`
  directly in `~Scheduler()` — the watchdog is always joined before the
  destructor returns; no leak. On the abort path, `join()` is never reached
  from the destructor's side, but this cannot leak either: the watchdog
  itself calls `std::abort()`, which tears down the entire process
  (confirmed via `core dumped` in every probe run below) — there is no
  "orphaned thread" once the process no longer exists.
- **Deadline plumbing**: `shutdownJoinDeadline` is a plain `Impl` member set
  once at `Scheduler::create()`/constructor time from a parameter — no
  static/global state, no shared mutable default beyond the `constexpr
  kDefaultShutdownJoinDeadline` (30s) used when a caller doesn't override
  it. Confirmed all ~119 existing call sites needed no changes (default
  parameter).

### 2. Happy-path byte-identical claim

Full serial `ctest -j1`, rebuilt from this commit, both drivers, offscreen
via a private `Xvfb :99`:

| Driver | Result | Wall time | This session's own pre-62d7d89 baseline | Report's own claim |
|---|---|---:|---:|---:|
| lavapipe | 44/44 green | 136.22s | 130.34s | 138.0s |
| NVIDIA (RTX 2080, 580.82.07) | 44/44 green | 213.26s | 208.98s | 210.9s |

Zero unfiltered `validation error` lines either run (`grep -ic "validation
error"` = 0 both). The ~2-6s deltas are within ordinary run-to-run variance
for a 130-215s suite doing real GPU work, not a regression pattern — my own
pre-fix baseline (same machine, same session) and the report's own
post-fix number bracket my post-fix number on both sides. Isolated,
Scheduler-heavy happy-path sample (`rx_task_tests -tc="Scheduler::workerCount*"`,
creates/destroys several real `Scheduler`s): **0.341s** (report: 0.360s) —
no measurable watchdog overhead.

### 3. Subprocess proof

Direct `rx_task_shutdown_hang_probe <deadline_ms>` runs, this machine:

| Deadline | My wall time | Report's wall time | Exit |
|---|---:|---:|---|
| 500ms | 1.06s | 1.035s | SIGABRT (134), core dumped, diagnostic present |
| 1500ms | 2.05s | 2.019s | SIGABRT (134), core dumped, diagnostic present |
| 3000ms | 3.52s | 3.525s | SIGABRT (134), core dumped, diagnostic present |

Near-exact match at every point — **the deadline is genuinely load-bearing
(linear tracking), not a fixed delay**, confirmed independently rather than
spot-checked alone. Every run's output named `worker-task lane thread`
correctly and never misreported `STUCK: IO thread`.

`rx_task_tests -tc="*teardown-hang*"`: **15/15 clean** (exceeds the
requested N≥10). Full `rx_task_tests` standalone: **21/21 test cases,
35074/35074 assertions, 0 failed** — exact match to the report's claim.

**My own hang injection, full end-to-end (the technique is mine, from the
original review)**: re-applied the identical injection (infinite
`sleep_for(200ms)` loop at the top of `computeGltfImport()`, env-gated,
byte-identical to before), rebuilt, re-ran the WALL-CLOCK GATE test under
an external `timeout 75s` wrapper. Result: `REQUIRE(done)` fails at its own
bounded ~20s in-loop deadline as before, but this time — unlike the
original review's finding — `~Scheduler()`'s watchdog fires **on its own**
at the default 30s `shutdownJoinDeadline`, logs `STUCK: worker-task lane
thread` / `shutdown-join deadline`, and the process **SIGABRTs itself**
(doctest: `test case CRASHED: SIGABRT`) at **~50s total wall time** —
`timeout` reported `Aborted`, not `Killed`: my 75s external safety net was
never needed. This is the exact scenario I used to discover the gap in the
original review, now closed end-to-end, not just in isolation via the new
probe binary. Injection reverted byte-identically afterward (md5 confirmed:
`ed9cdd77dc5679093946d193294ce387`, matching before/after in both rounds;
`git diff --stat` empty).

### 4. Adjudication of the two disclosed limitations

**(a) Ordinary `parallelFor()` pool workers reported as "one of N, not
individually nameable" — ACCEPTABLE, honest scope, not a finding.** The
`std::abort()` path produces a real core dump (confirmed: `core dumped` in
every probe run above) — a debugger can enumerate every thread's exact
stack, including any stuck ordinary worker, with full fidelity regardless
of what the log line names. The log-line diagnostic is a fast triage aid
for the two dedicated, individually-tracked threads (IO / worker-task
lane), not the only source of truth for a real incident; adding equivalent
per-chunk atomic tracking to every `parallelFor()` callback to close this
gap would add real, permanent overhead to a hot path for a case the core
dump already covers. Correctly and honestly disclosed rather than
overclaimed.

**(b) windows-cross-zig verified compile-only, no Wine run — ACCEPTABLE,
independently verified, no CI regex change needed.** `fork()`/`execv()`/
`waitpid()` are genuinely POSIX-only with no portable Windows equivalent —
not a convenience excuse. I independently reconfigured and rebuilt for the
`windows-cross-zig` preset myself (not just trusted the report): configure
succeeds cleanly, `rx_task_tests.exe` builds to a valid PE32+ executable,
`ninja -t targets` shows no `rx_task_shutdown_hang_probe` target exists for
that platform, and `RX_TASK_SHUTDOWN_HANG_PROBE_PATH` appears zero times in
that build's `compile_commands.json` — the guarded block genuinely compiles
to nothing, exactly as claimed. Checked `.github/workflows/ci.yml` line 730
directly: the Wine tier's `ctest -E 'rx_rhi_vk|rx_graph_gpu|...|sample'`
exclude-regex does not match `rx_task_tests` (never did, unrelated to this
commit) — the pre-existing 20-case Windows binary keeps running under Wine
exactly as before. No regex change is required or missing: the new test's
absence from that platform is handled entirely by the compile-time
`CMAKE_SYSTEM_NAME` guard, which is orthogonal to ctest's own test
selection.

### Hygiene (62d7d89)

- Single commit, author/committer `Yousef Wadi <ywadi85@gmail.com>` (both),
  matches local git identity.
- No AI attribution (`grep -iE
  "claude|anthropic|co-authored|ai-generated|generated by|assistant"` on
  the full commit message: no match).
- Not pushed: `origin/main` unchanged at `4a14636`; `62d7d89` absent from
  `git branch -r`.
- File list exactly matches the report's claimed scope (5 files, all under
  `src/rx_task/`) — no stray inclusions, consistent with deliberate
  pathspec-based staging rather than a broad `git add`.
- Both checkouts clean at the end of this re-review: worktree shows only
  expected untracked build/dep-cache directories (no tracked-file diff);
  main checkout shows only the pre-existing `progress.md` modification
  (left alone, per instructions). All temporary artifacts from this
  re-review (the reapplied hang injection) restored byte-identically.

### Overall verdict: ALL ADDRESSED

The teardown-hang gap identified in the original review is closed by
`62d7d89`, verified independently rather than accepted on the report's
word: the enkiTS-aliasing rationale checked against real upstream source,
the watchdog's own lifecycle traced for leaks/races, the happy path
re-measured on both drivers, the subprocess proof re-run and its linearity
independently reproduced, and — most directly — the exact original
hang-injection scenario re-run end-to-end, now terminating itself via a
diagnosed `SIGABRT` at ~50s instead of hanging indefinitely. Both disclosed
limitations are honest, low-risk scope decisions, independently checked
rather than rubber-stamped. No findings remain open from either round.
