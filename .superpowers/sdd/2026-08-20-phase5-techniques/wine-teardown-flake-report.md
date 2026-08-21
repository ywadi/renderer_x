# Wine-teardown flake report — dedicated round (2026-08-21)

Policy-triggered round: Wine `rx_core_tests` timeout reproduced twice in
Stage 0 (T6 implementer + reviewer, zero diff overlap with `src/rx_core`),
plus a related `rx_platform_tests` "Subprocess killed... after passing all
assertions" incident already ledgered as a watch-item. Base: main `756cd55`.

## Status: COMPLETE (infrastructure fix, substantial mitigation)

## Root cause (one-liner)

Wine's per-session bootstrap — six long-lived processes wineserver spawns
the first time it sees a real, reachable `$DISPLAY` (`services.exe`, two
`winedevice.exe`, `plugplay.exe`, `explorer.exe /desktop`, `svchost.exe`,
`rpcss.exe`) — saturates wineserver's single IPC channel while it forks,
and every wineserver-mediated syscall the test binary makes (thread
create/join, mutex/condvar wait) queues up behind that traffic; on this
job's CI layout that one-time tax lands on whichever ctest test happens to
be running when `xvfb-run -a ctest ...` supplies the job's first real
`$DISPLAY` (mostly `rx_core_tests`, once `rx_platform_tests`) — an
environmental Wine characteristic, not a defect in rx_core's or
rx_platform's own code.

## Investigation (Phase 1-2, systematic-debugging discipline)

1. **CI invocation pattern** (`.github/workflows/ci.yml`, windows-cross-zig
   job): `xvfb-run -a ctest --preset windows-cross-zig -E '...' ` runs each
   test as `wine <binary>.exe` (`CMAKE_CROSSCOMPILING_EMULATOR`), TIMEOUT
   120s per test (`src/rx_core/CMakeLists.txt:53`,
   `src/rx_platform/CMakeLists.txt:31` — both already carry a "windows-cross
   Wine hang, run 31512073559" hang-guard comment from an EARLIER, DIFFERENT
   incident: Tracy's profiler thread not tearing down under cold-prefix
   Wine, already fixed via `RX_TRACY=OFF` for this preset per
   `CMakeLists.txt`'s own note — established precedent that "Wine
   cold-bootstrap under CI" is a real hazard category in this project;
   today's finding is a new, different mechanism in the same category).

2. **Naive reproduction** (`wine rx_core_tests.exe 2>&1 | tail`, first-ever
   wine invocation of a fresh session): appeared to hang forever (5+
   minutes, zero output). Direct `/proc/<pid>/fd` inode inspection proved
   the pipe's write end was held open by wineserver plus six session-helper
   processes (`services.exe`, two `winedevice.exe`, `plugplay.exe`,
   `explorer.exe`, `svchost.exe`, `rpcss.exe`) — all reparented to PID 1
   (daemonized, detached from the invoking process tree, confirmed via
   `ps --forest`) and **never exiting on their own**. The actual test
   binary had already finished: `[doctest] test cases: 22 | 22 passed |
   0 failed`, `Status: SUCCESS!`, exit 0 — confirmed only once
   `wineserver -k` released the pipe. `tail`'s own semantics (block until
   true EOF) made this look like an infinite hang; **real ctest, using
   KWSys's own process-completion detection, does not exhibit this exact
   deadlock** — it correctly detects the direct child's exit and moves on,
   just slowly (see below).

3. **Real elapsed time, not a buffering artifact**: the naive run's own
   RX_LOG_ERROR timestamps (`[HH:MM:SS.mmm]`, formatted at log-call time)
   showed a genuine 51-second gap between two consecutive log records
   inside one `TEST_CASE` — proof the stall is real wineserver-mediated
   syscall contention during the six-process fork burst, not a stdout
   flush-timing illusion.

4. **Code-defect ruled out directly**: `grep -rn "\.detach()" src/rx_core/
   src/rx_platform/` — zero hits. Every `std::thread` in both test binaries
   (`log_test.cpp`'s worker-thread test, the 20x storm-thread UAF probe,
   `debug_checks_test.cpp`'s worker-thread violation test) is `.join()`ed.
   `log_forward_sink.cpp`'s exception handling (the "throwing callback"
   test previously flagged as the "hang point" in T6's own reports) is a
   correctly-scoped `try`/`catch` with no thread or async work involved —
   a red herring: it's simply the last *unbuffered* stderr write (`fprintf`)
   before the process's *buffered* stdout got flushed all at once whenever
   the pipe/session finally unblocked, not the actual point of failure.

5. **Real historical CI log corroboration** (`gh run view --log`, 3
   independent windows-cross-zig job runs: 32476017557/job 96752403550,
   32475019428/job 96749462367, 32180630087/job 95852551423): every one
   shows the same signature — `rx_core_tests` and/or `rx_platform_tests`
   running 4-10x slower than sibling Wine tests in that same run (6.81s /
   10.27s vs ~1.4-1.7s baseline; 1.57s / 10.56s vs ~1.5-1.6s; 1.86s / 5.72s
   vs ~1.6-1.9s) — which specific test absorbs it varies run to run,
   matching "intermittent." One real run's own log (32476017557) directly
   shows the mechanism: the "Wine toolchain smoke check" step (runs with
   **no** `$DISPLAY`) logs `nodrv_CreateWindow... "The explorer process
   failed to start."` and `err:ole:start_rpcss Failed to open RpcSs
   service` — confirming that step does NOT pre-warm the X11-dependent
   part of the session; the real bootstrap is deferred into the ctest step,
   which supplies the job's first real `$DISPLAY`.

## Reproduction rate

- **`rx_core_tests`, 20 cold-start loops** via the real `ctest --preset
  windows-cross-zig -R rx_core_tests` invocation (CI's own mechanism,
  `wineserver -k` before each iteration to force a genuinely fresh
  session, matching a fresh CI container): **0/20 outright ctest
  failures** on this development machine (more headroom than a throttled
  CI runner), but **100% reproduction of the underlying wall-clock tax**:
  durations `50,50,50,20,34,32,34,26,95,28,28,49,65,25,16,30,11,24,7,7`
  (seconds) — mean **34.0s**, max **95s** (79% of the 120s TIMEOUT budget
  consumed by environmental tax alone in the worst trial).
- **`rx_platform_tests`, 12 cold-start loops** (same methodology): 0/12
  failures; durations `13,17,11,10,9,10,8,8,9,8,8,9` — mean 10.0s, max 17s
  — same signature, smaller magnitude for this binary.
- Real CI history (`gh run list --workflow=ci.yml --limit 100`): 0
  windows-cross-zig failures attributable to this mechanism in the last
  100 runs (the two Aug-21 failures were `linux-native`; one Aug-18
  `windows-cross-zig` failure was an unrelated, already-understood
  `rx_asset_gltf_gpu_tests` SegFault) — consistent with a rare-tail flake
  that has not yet crossed 120s on GitHub's own runners in the sampled
  history, while T6's implementer/reviewer both hit it locally under
  otherwise-normal dev-machine conditions.

## Fix

**Infrastructure-only** (`.github/workflows/ci.yml`, windows-cross-zig job
only — `linux-native` never invokes wine, confirmed unaffected). Not a
code change, correctly: the hang/tax lives entirely in wineserver's own
session-helper process tree, unrelated PIDs our binaries cannot see, join,
or influence, and both test binaries are independently proven
detached-thread-free.

Adds a "Warm up Wine session under Xvfb" step between the existing "Wine
toolchain smoke check" and "Test under wine" steps: starts one **persistent**
Xvfb on `:77` (`nohup`/background, `$GITHUB_ENV`-exported `DISPLAY`,
survives into later steps — GH Actions does not kill orphaned background
processes at step boundaries), then runs the existing smoke-check binary
again under that real `$DISPLAY` before ctest starts. "Test under wine"
drops its own `xvfb-run -a` wrapper (would otherwise spin up a second,
unrelated Xvfb/`$DISPLAY` and defeat the fix) and runs `ctest` directly
against the already-exported `$DISPLAY`.

**Honest disposition — substantial mitigation, not a mathematical
guarantee.** wineserver's own session-settling time is an external,
Wine-internal characteristic this step can front-load but not fully
control (confirmed: even a warm-up run of the full `rx_core_tests.exe`
binary itself, discarded, did not reduce every subsequent run to near-zero
— one trial still showed 41s after warm-up). This was not a blind
ctest-TIMEOUT bump (never touched, per project policy) — it is a
documented, narrowly-scoped step that measurably and repeatably shifts
most of the tax out of any individual test's timed window.

## Verification

**20-run proof, post-fix, cold-start each iteration (same methodology as
the baseline above, `ctest -R rx_core_tests` after the new warm-up
sequence):**
```
run=1  test_dur=14s  test_rc=0
run=2  test_dur=21s  test_rc=0
run=3  test_dur=21s  test_rc=0
run=4  test_dur=17s  test_rc=0
run=5  test_dur=2s   test_rc=0
run=6  test_dur=8s   test_rc=0
run=7  test_dur=14s  test_rc=0
run=8  test_dur=1s   test_rc=0
run=9  test_dur=6s   test_rc=0
run=10 test_dur=6s   test_rc=0
run=11 test_dur=13s  test_rc=0
run=12 test_dur=41s  test_rc=0
run=13 test_dur=13s  test_rc=0
run=14 test_dur=17s  test_rc=0
run=15 test_dur=16s  test_rc=0
run=16 test_dur=20s  test_rc=0
run=17 test_dur=19s  test_rc=0
run=18 test_dur=15s  test_rc=0
run=19 test_dur=21s  test_rc=0
run=20 test_dur=23s  test_rc=0
```
20/20 green. Mean **34.0s -> 15.4s** (~55% reduction), max **95s -> 41s**
(worst-case tail cut from 79% to 34% of the 120s TIMEOUT budget). A prior,
less-controlled batch of 20 runs (immediately following ~30 other wine
invocations in the same session, so OS-page-cache-warmed) measured 0-4s;
disclosed but NOT used as the headline number — the cold, decontaminated
20-run set above is the honest, representative measurement.

- **Full serial lavapipe ctest (linux-native, unchanged)**: `100% tests
  passed, 0 tests failed out of 31`, `Total Test time (real) = 161.43 sec`
  — confirms linux behavior is unaffected (expected: zero code changed,
  and linux-native never invokes wine).
- **windows-cross-zig build**: `ninja: no work to do` — clean, unaffected
  (this fix touches only `.github/workflows/ci.yml`, no source).
- **Revert-discrimination**: not applicable in the code-revert sense (this
  is an infrastructure accommodation, not a code fix) — the discriminating
  evidence is the direct before/after timing comparison above (same
  machine, same cold-start methodology, only the CI step sequence differs).
- **YAML validity**: `python3 -c "import yaml; yaml.safe_load(...)"` passes
  after every edit.

## Concerns for the coordinator

1. The fix substantially reduces but does not provably eliminate the
   flake's probability — wineserver's own internal session-settling time
   remains externally variable. Recommend keeping this on watch: if
   `rx_core_tests`/`rx_platform_tests` still occasionally time out under
   CI after this lands, the next escalation (not applied here, to stay
   narrowly scoped) would be a second, heavier warm-up probe or an
   explicit condition-based wait for wineserver's session processes to
   settle, rather than a bigger fixed delay.
2. `rx_asset_gltf_gpu_tests` is NOT excluded by the windows-cross-zig
   job's `-E` filter and legitimately takes 34-53s under Wine (real,
   unrelated decode work) — noted only so it isn't mistaken for a
   recurrence of this flake in future logs.
3. No code in `src/rx_core` or `src/rx_platform` was touched; no
   board/issue/plan/spec/ledger file was edited, per scope.

## Commit

- `<COMMIT_SHA_PLACEHOLDER>` — `ci(windows-cross-zig): pre-warm Wine
  session under Xvfb before ctest (Wine session-bootstrap flake)`
  — `.github/workflows/ci.yml` only.
