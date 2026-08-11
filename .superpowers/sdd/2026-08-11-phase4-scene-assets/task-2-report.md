# Task 2 report: enkiTS adoption (`rx_task`) + threading contract doc

Commit: `bcb58b8` (base `f9d0554`), branch `main`, not pushed.

## What was built

- `third_party/CMakeLists.txt` — enkiTS pinned at `v1.12` (verified the
  actual tag via `git ls-remote --tags` against
  `github.com/dougbinks/enkiTS.git`; zlib license, recorded in both the
  commit message and this file's own comment), routed through the
  existing dep-cache (`rx_add_cached_dependency`) rather than vendored as
  source under `third_party/`. **Resolution 1 justification**: enkiTS's
  own `CMakeLists.txt` (`ENKITS_INSTALL=ON`) already produces a clean,
  real CMake package — `enkiTSConfig.cmake` exporting an `enkiTS::enkiTS`
  imported target with correct `INTERFACE_INCLUDE_DIRECTORIES`
  (`<prefix>/include/enkiTS`, no subdirectory prefix needed at the
  `#include` site) and `INTERFACE_COMPILE_DEFINITIONS`
  (`ENKITS_TASK_PRIORITIES_NUM=3`). Verified directly, standalone, before
  wiring it into the project: configured/built/installed this exact tag
  through both `cmake/toolchains/linux-native.cmake` and
  `windows-cross-zig.cmake` from a clean prefix, then linked **and ran** a
  real `enki::TaskScheduler`/`TaskSet` program against each installed tree
  (the windows-cross-zig one under Wine) — both worked with zero
  source-level or link-level changes. That satisfies the resolution's own
  bar ("if its CMake install exports cleanly ... use that"), so there was
  no reason to hand-vendor two source files instead.
  `ENKITS_BUILD_C_INTERFACE`/`ENKITS_BUILD_EXAMPLES=OFF` (this project
  only calls the C++ API); `ENKITS_BUILD_SHARED=OFF` (matches every other
  compiled dependency here being static).
- `CMakeLists.txt` (root) — `add_subdirectory(src/rx_task)`, placed right
  after `rx_core`, independent of the
  `rx_platform`/`rx_shader`/`rx_rhi_vk`/`rx_graph`/`rx_material` chain
  (rx_task depends on neither and nothing yet depends on rx_task).
- `src/rx_task/include/rx_task/scheduler.h` / `scheduler.cpp` —
  `rx::task::Scheduler`, matching the brief's interface exactly:
  - `create(workerCount)`: `0` resolves to
    `hardware_concurrency() - 1` clamped to a minimum of `1`
    (`hardware_concurrency() == 0` also falls through to `1`). Must be
    called on the thread that will act as "main" — it calls
    `enki::TaskScheduler::Initialize()` directly on the calling thread, so
    enkiTS's own internal thread 0 and this Scheduler's "main" are always
    the same thread.
  - `parallelFor(itemCount, grainSize, fn)`: a local `enki::TaskSet`,
    `m_MinRange` set from `grainSize` **before** `AddTaskSetToPipe()`
    (verified against `TaskScheduler.cpp`'s `AddTaskSetToPipeInt()`: it
    recomputes the task's actual runtime split size from `m_MinRange` at
    that call, not at `TaskSet` construction — setting the field after
    construction but before adding to the pipe is correct, not a race).
    Blocking via `WaitforTask()`, which is enkiTS's own documented
    "calling thread runs pending chunks while it waits" behavior — the
    calling thread genuinely participates, not just blocks. This is also
    exactly what makes nested `parallelFor()` (calling it again from
    within a chunk callback) deadlock-free rather than not: the inner
    call's own `WaitforTask()` drains whatever chunks are available
    (inner or outer) on whichever thread reaches it.
  - `runOnIoThread(fn)`: **verified enkiTS's actual pinned-thread idiom
    before implementing anything** — it is enkiTS's own documented
    pattern from `example/WaitForNewPinnedTasks.cpp`
    (`RunPinnedTaskLoopTask`), not something invented for this task.
    `Scheduler::Scheduler()` configures `numTaskThreadsToCreate =
    resolvedWorkerCount + 1` (one extra internal enkiTS thread beyond the
    resolved worker count) and immediately hands that extra thread a
    single long-lived `IoLoopTask` (`enki::IPinnedTask`) whose `Execute()`
    loops `WaitForNewPinnedTasks(); RunPinnedTasks();` until shutdown is
    requested. Because `Execute()` never returns until then, that thread
    can never go back to enkiTS's generic dispatch loop that would
    otherwise let it steal ordinary `parallelFor()` chunks — genuinely
    dedicated, not merely "usually free". Each `runOnIoThread()` call
    heap-allocates a one-shot `IoTask` (`enki::IPinnedTask` pinned to that
    same thread number) and calls `AddPinnedTask()` (documented safe from
    any thread). FIFO comes from enkiTS's own pinned-task list, verified
    directly against `LockLessMultiReadPipe.h`'s
    `LocklessMultiWriteIntrusiveList`: `WriterWriteFront()`/
    `ReaderReadBack()` is a genuine FIFO queue (oldest-inserted dequeued
    first), not a stack — this wrapper needed no queue of its own to get
    that guarantee.
  - **Real bug caught and fixed by reading enkiTS's source before writing
    the wrapper**: `TaskScheduler::RunPinnedTasks(threadNum_, priority_)`
    dereferences the just-executed task **after** `Execute()` returns
    (`m_RunningCount.fetch_sub(...)`, then `TaskComplete()` walks
    `m_pDependents`). A `delete this;` at the end of `IoTask::Execute()`
    would therefore be a use-after-free on every single call — invisible
    without ASan, and impossible to blame on this wrapper by log output
    alone. Fixed by never self-deleting: `Execute()` runs `fn_()` then
    pushes `this` onto a trash list owned by `IoLoopTask`, touched only by
    the IO thread itself, reaped only once `IoLoopTask`'s own call to
    `RunPinnedTasks()` has fully returned to it — by construction, after
    enkiTS's own post-`Execute()` bookkeeping for every task run in that
    call has already happened.
  - `postToMain(fn)` / `pumpMain()`: a plain `std::mutex` +
    `std::vector<std::function<void()>>`, deliberately **not** built on
    any enkiTS mechanism (brief resolution 2) — `postToMain()` locks and
    `push_back()`s; `pumpMain()` swaps the vector out under the lock, then
    runs everything outside it (so a `postToMain()` call arriving mid-drain
    is picked up by the *next* `pumpMain()`, never blocked on or lost).
  - `workerCount()`: returns the resolved value from `create()`, excluding
    both the calling/main thread and the dedicated IO thread (both exist
    regardless of this count — documented in both the header and
    `docs/threading.md`).
  - Public header names **no enkiTS type at all** (pimpl `struct Impl` in
    `scheduler.cpp`) — the D2 "dependency stays swappable" rationale
    realized structurally, not just as a stated intent: `rx_task` links
    `enkiTS::enkiTS` `PRIVATE`, `rx_core` `PUBLIC` (logging only).
- `docs/threading.md` — the D5 contract: main-thread-only list
  (`BindlessTable` registration/`release`, `Uploader` submission/`flush`,
  `MaterialSystem` load/getPipeline (and every other method — not
  internally synchronized at all), `DeletionQueue`, plus forward
  references to `GeometryPool`/`Registry` mutation for Stage 1/2); worker-
  allowed list (pure CPU transforms, parse/decode/transcode/optimize,
  culling, secondary command recording into per-thread pools — forward-
  referencing Task 7's exact pool-per-thread-per-frame-in-flight rule);
  the `postToMain`/`runOnIoThread` handoff pattern with a worked code
  example; the "every new public header carries a one-line thread-
  affinity note" rule restated with a worked example matching the actual
  one added to `deletion_queue.h`.
- One-line thread-affinity notes (doc-only) added to:
  `src/rx_rhi_vk/include/rx_rhi_vk/bindless.h` (`BindlessTable`),
  `upload.h` (`Uploader`), `deletion_queue.h` (`DeletionQueue`),
  `src/rx_material/include/rx_material/material_system.h`
  (`MaterialSystem`, appended right after its existing "not internally
  synchronized" paragraph rather than duplicating it).
- `src/rx_task/tests/scheduler_test.cpp` — 7 device-free `TEST_CASE`s
  (doctest, no VkDevice): `workerCount()` resolution (0 → hw-1 clamped to
  ≥1; explicit value passed through unchanged); `parallelFor` over 10k
  items sums each exactly once (per-item atomic counters) and uses more
  than one worker when `workerCount() > 0` (per-worker touch counts);
  nested `parallelFor` (inner launched from within an outer chunk)
  completes with the correct inner sum on every outer item, no deadlock;
  `postToMain` FIFO for a single calling thread's own sequence, asserted
  via `thread::id` equality to the `pumpMain()`-calling thread, plus
  nothing runs before `pumpMain()` is called; `postToMain` callable safely
  from worker threads (queued from inside `parallelFor` chunks), still
  only ever executes on the `pumpMain()`-calling thread; `runOnIoThread`
  FIFO across 1000 rapid submissions, every execution off the main
  thread and all on the *same* single thread (`thread::id` equality
  across every observed execution); `Scheduler::create()`/destroy repeated
  3× in sequence, exercising `parallelFor`/`runOnIoThread`/`postToMain`
  each cycle (bounded-wait helper, not a fixed sleep, so a real regression
  fails within ~5s rather than hanging CI).

## Test results

`build/linux-native/src/rx_task/tests/rx_task_tests` (native):
```
[doctest] test cases:     7 |     7 passed | 0 failed | 0 skipped
[doctest] assertions: 13692 | 13692 passed | 0 failed |
[doctest] Status: SUCCESS!
```
Same binary, cross-compiled (`build/windows-cross-zig`), run under Wine
(`WINEDEBUG=-all wine rx_task_tests.exe`): identical result, 7/7 cases,
13692/13692 assertions, `Status: SUCCESS!` — this is real evidence, not an
assumption: enkiTS's actual multi-thread behavior (worker fan-out,
dedicated pinned IO thread, clean shutdown/join ×3) was exercised and
verified correct on the real cross-compiled windows-gnu binary, not just
"it linked."

Full repo suite, both presets, zero regressions:
- `ctest --preset linux-native`: 16/16 passed (24.28s) — `rx_task_tests`
  now included alongside every pre-existing test (`rx_core`, `rx_platform`,
  `rx_shader`, `rx_rhi_vk`, `rx_graph` ×2, `rx_material` ×2, 6 sample
  headless gates).
- `ctest --preset windows-cross-zig` (via the configured Wine
  `CMAKE_CROSSCOMPILING_EMULATOR`): 16/16 passed (34.08s).

Both presets' **whole-project** build (not just `rx_task`) reconfigured
and built clean from the existing `.deps-cache` (enkiTS itself: dep-cache
MISS → built once → cached; every other dependency: HIT, zero
recompilation) — `cmake --build --preset linux-native` (36 targets) and
`cmake --build --preset windows-cross-zig` (41 targets), no warnings
introduced, no existing target touched besides the four header comment-only
edits.

## Files

- `third_party/CMakeLists.txt`
- `CMakeLists.txt`
- `src/rx_task/CMakeLists.txt`
- `src/rx_task/include/rx_task/scheduler.h`
- `src/rx_task/scheduler.cpp`
- `src/rx_task/tests/CMakeLists.txt`
- `src/rx_task/tests/doctest_main.cpp`
- `src/rx_task/tests/scheduler_test.cpp`
- `docs/threading.md`
- `src/rx_rhi_vk/include/rx_rhi_vk/bindless.h`
- `src/rx_rhi_vk/include/rx_rhi_vk/upload.h`
- `src/rx_rhi_vk/include/rx_rhi_vk/deletion_queue.h`
- `src/rx_material/include/rx_material/material_system.h`

## Concerns

- **Queued-but-unexecuted work at teardown is dropped, and its heap
  allocation is not specially reclaimed**: if `runOnIoThread()`/
  `postToMain()` is called and the `Scheduler` is destroyed before that
  work ever runs, the enqueued closure (and, for `runOnIoThread()`, its
  heap-allocated `IoTask`) is never freed through this wrapper's own reap
  path — verified directly: `WaitforAllAndShutdown()` sets the shutdown
  flag at its very top, before `WaitforAll()` runs, so `IoLoopTask`'s loop
  can observe shutdown and exit without draining a task added in that
  same narrow window. This is a bounded, process-teardown-only condition
  (not a steady-state leak), and matches the precedent this codebase's own
  `rx::rhi::DeletionQueue` destructor already sets for an equivalent
  "non-empty teardown is a caller-timing choice, not this type's job to
  paper over" case — documented explicitly in both `scheduler.cpp`'s
  destructor comment and `scheduler.h`'s own doc comment, not left
  implicit. No test in this task's list exercises this path (every test
  waits for completion before the `Scheduler` goes out of scope), so
  nothing here is silently asserting zero-leak-in-all-cases.
- **`workerCount() > 1` multi-worker assertion is a strong-evidence, not
  hard, guarantee**: the 10k-item `parallelFor` test asserts more than one
  distinct worker index was observed when `workerCount() > 0`, which is
  correct given enkiTS's real work-stealing behavior on this machine (and
  reproduced identically under Wine on the cross-compiled binary), but is,
  in principle, a scheduling heuristic outcome rather than an enkiTS API
  contract — flagged in the test's own comment rather than presented as an
  absolute guarantee.

  **Resolved in fix round 1 below** — this heuristic assertion was
  removed entirely and replaced with a deterministic proof.

## Fix round 1 (`task-2-review.md`: 1 Important, 1 Minor)

Commit: `7c91eb1` (base `bcb58b8`), branch `main`, not pushed.

### Important — flaky `distinctWorkersUsed > 1` heuristic (reproduced 2/27 under `taskset -c 0,1`)

Removed the heuristic assertion entirely (the 10k-item `parallelFor` test
now only checks the exact-once-touch property; per-worker touch tracking
was deleted along with it — nothing left in that test relies on how many
workers happened to get used). Added a new, separate test case: a
barrier-style proof. `Scheduler::create(2)` (explicit, independent of
machine core count — the fix's own instruction), then
`parallelFor(2, 1, fn)` where `fn` increments a shared atomic once and then
busy-waits (yielding) until it observes the atomic equal to 2, bounded by
a 20s per-unit timeout that records a timeout flag instead of hanging.

Why this can't flake the way the old one did: if `parallelFor` ever
delivered the whole `[0, 2)` range to a single thread as one sequential
chunk, the first item's wait would have nothing else to run and would
hang until *its own* timeout — a thread cannot service a second chunk
while stuck inside the first chunk's callback. Completion is therefore
only possible when genuine concurrent progress happens; there is no
"got lucky with work-stealing" middle ground left to flake on. The test
asserts zero timeouts and `arrived == 2`; a non-asserting `RX_LOG_INFO`
records how many distinct thread ids actually participated (kept purely
so a human can still see it — matching the fix instruction's "if you
like").

Verification requested explicitly — both tallies, same binary:

```
Unconstrained, 20 consecutive runs:      pass=20 fail=0
`taskset -c 0,1`, 20 consecutive runs:   pass=20 fail=0
```
(Full shell loop and per-run logs were produced directly against
`build/linux-native/src/rx_task/tests/rx_task_tests`; every one of the 20
`taskset -c 0,1` runs' own `RX_LOG_INFO` line reported "2 distinct thread
id(s) observed for 2 concurrent unit(s)" — the barrier resolved via real
concurrency every single time, not merely without crashing.)

### Minor — abandoned `IoTask` leaked silently at teardown

Before reproducing anything, built a standalone probe directly against
the pinned, installed enkiTS v1.12 library (not this wrapper) to check
the review's own theorized mechanism empirically rather than assume it:

- Probe 1: one long-running (800ms) pinned task submitted, followed
  immediately by 4 trivial ones, then `WaitforAllAndShutdown()` called
  with zero delay. Across 5 runs: **all 5 tasks ran to completion every
  time** — `WaitforAllAndShutdown()`'s own `WaitforAll()` step genuinely
  blocks until the IO thread's pinned-task list is observed empty, which
  only happens after draining everything queued (including tasks added
  while an earlier one is still executing).
- Probe 2: a second thread hammering `AddPinnedTask()` in a tight loop
  while the main thread concurrently calls `WaitforAllAndShutdown()`,
  across 200 trials (125,263 total submissions): **zero ever dropped by
  enkiTS itself**.

Conclusion: "enqueue several long tasks, destroy immediately" (the fix
instruction's suggested repro) does not, in practice, reach the narrow
race the original report theorized — enkiTS's own shutdown sequencing is
more robust than that report assumed. Implemented the fix anyway, as
genuine defense-in-depth (correct regardless of how rarely the underlying
race could ever trigger), and adjusted the test strategy to match what is
actually, honestly reproducible:

- `runOnIoThread()` now checks a new `acceptingIoTasks` flag (set false as
  literally the first statement in `~Scheduler()`, before
  `WaitforAllAndShutdown()` runs) and refuses + counts any submission
  once it is false, rather than handing a task to a scheduler no longer
  guaranteed to run it.
- Every accepted submission is tracked in a mutex-guarded `outstandingIo`
  list (`runOnIoThread()` adds, `IoTask::Execute()` removes itself the
  instant it actually runs — before doing anything else observable).
  `~Scheduler()`, after `WaitforAllAndShutdown()` returns (every thread
  joined, so nothing can ever call `Execute()` again), deletes whatever
  is still in that list directly — safe for exactly that reason — and
  logs once, loudly (`RX_LOG_WARN`), with the total dropped count
  (intake-refused + drain-time-found).
- Added `rx::task::detail::debugLastDroppedIoTaskCount()` — a test-only
  seam mirroring `rx::material::detail::debugCompileCount()`'s own
  carve-out convention, since there is no other way to observe this
  count once the `Scheduler` that produced it no longer exists.
- **Corrected the report's DeletionQueue-parity claim** (this section
  replaces the old "Concerns" bullet above): the original report
  described the leak as "matching the precedent `DeletionQueue`'s
  destructor sets," which the review correctly flagged as overstating the
  parity — `DeletionQueue` never actually loses memory (captured RAII
  destructors still run as an ordinary side effect of its vector being
  destroyed) and logs loudly regardless. This fix makes `Scheduler` match
  `DeletionQueue`'s *posture* now too (loud log, nothing silently
  unaccounted for, on non-empty teardown), not just gesture at it —
  though the mechanics necessarily differ (deletion instead of execution,
  since a dropped task's `fn` genuinely never runs).

New tests (both safe to run — see below for why a naive "race a second
thread against `scheduler.reset()`" design was rejected):

- **Positive case** (`"Scheduler teardown lets several already-queued
  runOnIoThread() tasks all run to completion..."`): one 300ms task
  followed immediately by 4 trivial ones, `Scheduler` destroyed at end of
  scope while the first is still sleeping. Asserts all 5 ran and
  `debugLastDroppedIoTaskCount() == 0` — this is the ACTUAL, verified
  behavior for the fix instruction's literal repro (see Probe 1 above),
  not the drop path.
- **Drop-path case** (`"Scheduler::runOnIoThread refuses (and counts) a
  call made from within an already-running IO task's own body..."`):
  rather than racing a second, independently-running thread against
  `scheduler.reset()` — which the report's own comment explains is
  impossible to do without either leaving `acceptingIoTasks` untested
  (join the racer first) or introducing a genuine use-after-free in the
  TEST ITSELF once `.reset()` actually frees the `Scheduler` (no internal
  flag can make calling a method on freed memory safe) — the second
  `runOnIoThread()` call is issued from INSIDE the first task's own
  `Execute()` body. That is safe: it executes as part of `~Scheduler()`'s
  still-in-progress `WaitforAllAndShutdown()` call, which by definition
  has not returned while it is running, so `impl_` is still guaranteed
  alive. Since stopping intake always happens-before
  `WaitforAllAndShutdown()` is even called, the nested call is
  guaranteed to already see `acceptingIoTasks == false`. Asserts the
  nested call's own side effect never happened and
  `debugLastDroppedIoTaskCount() >= 1` — confirmed directly in the actual
  run: `[warning] rx::task::Scheduler: dropped 1 runOnIoThread() task(s)
  at teardown (1 refused at intake, 0 queued but never executed)`.

### Re-verification after both fixes

`build/linux-native/src/rx_task/tests/rx_task_tests` — 10/10 cases,
13703/13703 assertions, `Status: SUCCESS!` (native and, identically, under
Wine on the windows-cross-zig cross-compiled binary).

Full repo suite, both presets, zero regressions:
- `ctest --preset linux-native`: 16/16 passed (24.66s).
- `ctest --preset windows-cross-zig` (Wine emulator): 16/16 passed
  (34.52s).

Both presets' whole-project build reconfigured/rebuilt clean (only
`rx_task`/`rx_task_tests` needed recompiling; nothing else was touched by
either fix).

### Fix-round concerns

- The "drop-path" test's timing-based ordering (a 50ms sleep inside the
  first task, before its nested call) is deterministic given
  `WaitforAllAndShutdown()`'s own blocking behavior (Step 1 always
  happens-before Step 2 starts, and Step 2 cannot return while that task
  is still sleeping/running), not a race — but it is still a sleep-based
  test, and a hypothetical future change to `~Scheduler()`'s own
  sequencing (e.g., reordering Steps 1 and 2) would silently invalidate
  the reasoning this test relies on. The comment directly above the test
  says so explicitly.
- The theoretical enkiTS-internal race (a task whose `runOnIoThread()`
  call passes the `acceptingIoTasks` check but doesn't reach
  `AddPinnedTask()` until after every thread is already joined) remains
  exactly that — theoretical. Step 3's drain closes it if it ever
  happens, but nothing in this fix round forced it to happen even once
  across 325 total probe attempts (5 + 200 + 20 + 20 test/CI runs
  combined) targeting adjacent scenarios.

## Fix round 2 — auto-grain default (spec D4 amendment) + host-engine coexistence

Commit: `5034036` (base `39e4f9c`), branch `main`, not pushed.

Note on base: 5 coordinator-authored doc/spec commits (`0155e62` through
`39e4f9c`) landed on `main` between fix round 1 and this round while this
session had round-2 edits in progress in the working tree. Checked
directly before doing anything else: `git diff 7c91eb1..HEAD` for those 5
commits touches only `docs/superpowers/plans/` and
`docs/superpowers/specs/` — never `docs/threading.md` or `src/rx_task/`
— so this round's in-progress edits were never at risk of conflicting
with them; this round's own commit is based on top of all five, not
around them.

### 1. `parallelFor` grain becomes optional, 0 = AUTO

- `Scheduler::parallelFor(itemCount, grainSize, fn)`: `grainSize == 0`
  now means AUTO (previously meant "grain of 1" — the old
  behavior-before-this-round). A nonzero `grainSize` is still used
  verbatim, now documented as a measurement affordance for controlled
  tuning (the stress benchmark's own instrument), not a general mode.
- New overload: `parallelFor(itemCount, fn)` — delegates to
  `parallelFor(itemCount, 0, fn)`. This is the path ordinary callers use;
  they never see `grainSize` at all.
- `Scheduler::autoGrainSize(itemCount, workerCount)`: a public, static,
  pure function — `max(kMinGrain, itemCount / (workerCount * 4))` — so
  the formula is directly unit-testable without constructing a live
  Scheduler (which would spin up real OS threads per test case). Guards
  `workerCount == 0` to behave like `1` (no real `Scheduler::workerCount()`
  is ever 0 — `create()`'s `resolveWorkerCount()` clamps to a minimum of
  1 — but this is a pure function callable with arbitrary input).
- `Scheduler::kMinGrain = 64`: a public `static constexpr`, documented
  with the rationale the fix instructions specified (4 chunks/worker
  balances stealing granularity vs. per-task overhead; 64 floors the
  per-item cost for trivial bodies).
- Effective grain computation moved into `parallelFor()` itself
  (`grainSize > 0 ? grainSize : autoGrainSize(itemCount,
  resolvedWorkerCount)`), still assigned to `taskSet.m_MinRange` before
  `AddTaskSetToPipe()` — unchanged mechanism from round 1, just fed a
  different value when the caller passes/defaults to 0.

### 2. `docs/threading.md`: "Parallelism is the default, not a mode"

New section (placed between "Worker-allowed" and "The handoff pattern")
mirroring spec D4's amended text verbatim in spirit: no on/off switch, no
caller-chosen chunk count anywhere engine-owned work runs; chunked passes
record in parallel unconditionally with grain-based scaling making small
workloads effectively serial at the cost of one task submission;
`--threads` exists only in the stress benchmark (sample 07) as a
measurement instrument, never an engine-wide switch. Ties the prose
directly to the real mechanism: `autoGrainSize()`'s formula and the
`parallelFor(itemCount, fn)` overload.

### 3. Tests

- New: auto-grain overload exactly-once coverage for itemCount
  `{0, 1, 63, 64, 65, 10000}` (reuses the existing atomic-touch-count
  pattern from the pre-existing 10k-item test, now parameterized).
- New: explicit `grainSize == 0` via the three-argument form produces the
  same exactly-once result — locks in "0 literally means AUTO," not just
  the convenience overload's own delegation.
- New: `autoGrainSize()` unit-tested directly against known
  itemCount/workerCount pairs, including: below-the-floor cases (`kMinGrain`
  wins), an exact-boundary case (`10000/39 == 64` exactly), above-the-floor
  cases (formula's own value wins), and the `workerCount == 0` defensive
  guard (`autoGrainSize(x, 0) == autoGrainSize(x, 1)`).
- All pre-existing tests using explicit nonzero `grainSize` (64, 4, 16,
  10, 1, 8 across the various cases) are unaffected — nonzero grain
  behavior is unchanged by this round.

### 4. Profiling instrumentation — deferred, not added

Checked `ls src/rx_core/include/rx_core/profile.h` directly before writing
any code: **it does not exist yet** (confirmed again immediately before
committing, in case the concurrent profiling task landed mid-session — it
had not). Per instructions, did not create it. Added a one-line,
TODO-free comment inside `parallelFor()` itself (`scheduler.cpp`) stating
where the RX_ZONE/RX_PLOT instrumentation point belongs and that it lands
as the profiling task's own follow-up once `rx_core/profile.h` exists —
no zone/plot code added.

### 5. Supplement (user-raised): "Host-engine coexistence" section

Added to `docs/threading.md`, covering the three points requested:

1. **Idle behavior — verified directly against the pinned enkiTS v1.12
   source** (not assumed), citing the exact mechanism:
   - `TaskScheduler::TaskingThreadFunction` (`TaskScheduler.cpp:281-317`):
     each internal worker's dispatch loop spins with an increasing
     backoff for at most `gc_SpinCount = 10` attempts
     (`TaskScheduler.cpp:75`) after finding no work, then calls
     `WaitForNewTasks()`.
   - `TaskScheduler::WaitForNewTasks()` (`TaskScheduler.cpp:686-715`):
     double-checks for a just-arrived task first (avoiding a lost-wakeup
     race), then, if genuinely idle, calls `SemaphoreWait()` — a real
     blocking wait, not a spin.
   - `SemaphoreWait()`'s actual OS primitive, checked for both of this
     project's target platforms: `WaitForSingleObject(sem, INFINITE)` on
     Windows (`TaskScheduler.cpp:1336-1341`), `sem_wait()` on POSIX/Linux
     (`TaskScheduler.cpp:1421-1424`) — both genuine kernel-level blocking
     calls (near-zero CPU while parked), confirming "spin-then-semaphore-
     sleep" was the right characterization, not an assumption.
   - The dedicated IO thread's own idle wait (`IoLoopTask::Execute()`'s
     `WaitForNewPinnedTasks()` call) uses the identical semaphore
     mechanism (already traced in round 1's own work on this file), plus
     blocks on real I/O inside whatever `fn` a caller submitted.
   - Documented: renderer-core occupancy is bursty (wakes for a
     `parallelFor()` fan-out, runs to completion, sleeps again), never a
     perpetually-spinning background thread competing with a host
     engine's own subsystems.
2. **Worker count is the consumer's budget**: documented in both
   `docs/threading.md` and a new paragraph on `Scheduler::create()`'s own
   doc comment — the `hardware_concurrency() - 1` default is for a
   standalone consumer; an embedding engine is expected to pass its own
   granted budget, and `parallelFor()` self-scales to whatever
   `workerCount()` results, via `autoGrainSize()`.
3. **External-thread participation — recorded, not implemented**: tied
   directly to the master-registry entry the coordinator had just
   committed (`39e4f9c`, `docs/superpowers/specs/2026-08-09-toolchain-
   platform-rhi-design.md`'s "Scheduler sharing with host engines" item)
   — enkiTS's own `RegisterExternalTaskThread()`/`numExternalTaskThreads`
   mechanism is named as the actual end-state mechanism, explicitly
   scoped to the SDK phase, not touched in this task.

No behavior change from the supplement — confirmed directly: `git diff`
against the round-2 code commit shows the only non-comment lines added
to `scheduler.h`/`scheduler.cpp` since round 1 are the three round-2
feature declarations (the new overload, `kMinGrain`, `autoGrainSize()`);
everything in the supplement is comment/doc-only.

### Re-verification after round 2 (including the supplement)

`build/linux-native/src/rx_task/tests/rx_task_tests` — 13/13 cases,
33936/33936 assertions, `Status: SUCCESS!` (native and, identically,
under Wine on the windows-cross-zig cross-compiled binary, both before
and after the supplement's doc-only edits).

Flake discipline (round 2's actual code change touches `parallelFor`,
which several timing-adjacent tests depend on — re-ran the full binary,
not just the new cases):

```
Unconstrained, 20 consecutive runs:      pass=20 fail=0
`taskset -c 0,1`, 20 consecutive runs:   pass=20 fail=0
```

Full repo suite, both presets, zero regressions, re-run twice (once after
the grain-default code change, once more after the supplement's doc-only
edits):
- `ctest --preset linux-native`: 16/16 passed both times.
- `ctest --preset windows-cross-zig` (Wine emulator): 16/16 passed both
  times.

### Round-2 concerns

- None new. The auto-grain formula's floor/boundary behavior is
  exhaustively unit-tested directly (not just observed indirectly through
  `parallelFor()`'s output), and every pre-existing timing-sensitive test
  was re-confirmed flake-free under the same constrained-CPU conditions
  round 1's fix was validated against.
