# Task 5 report: Public log sink (D23, seed 13)

## Fix round 1 (post-review)

Review (`task-5-review.md`, commit `7da8ee4`): **Approved with findings** —
spec ❌, 1 Critical + 1 Medium + 2 Low. This section documents the fix; the
rest of this report is the original round-1 account, left intact below for
the record — where the two disagree, this section is authoritative.

**Critical — uninstall/in-flight-callback lifetime race (reproduced UAF).**
The review built a standalone probe against the unmodified sink and
reproduced a real use-after-free (5/5 runs): `sink_it_()` copied
`{callback_, userData_}` out and released `callbackMutex_` *before*
invoking the callback (a deliberate choice at the time, to avoid a
self-logging callback deadlocking on that same mutex) — so `set(nullptr,
...)` (hence `rxSetLogCallback(nullptr, ...)`) could return while the
*previous* callback was still running on another thread with the *old*
`userData`, which a caller following the (at-the-time undocumented) "install
→ uninstall → free" sequence would then free out from under it.

**Fix (the coordinator's required design):** `sink_it_()` now holds
`callbackMutex_` for the callback invocation's ENTIRE duration, not just
while copying the pair out. `set()` takes the same mutex, so it cannot
return until any invocation already in flight has fully finished —
`rxSetLogCallback(nullptr, ...)` now genuinely blocks until drained, and
`userData` is safe to free the instant it returns. This reintroduces the
self-logging-callback deadlock the old design avoided differently: closed
with `base_sink<std::recursive_mutex>` (see the toolchain note below) plus a
`thread_local` re-entrancy flag (`t_insideForward`) that `sink_it_()` checks
first and returns immediately on, dropping the re-entrant forward only —
every *other* sink on the same logger (the real console sink included)
still receives it normally, since spdlog's `logger::log_it_()` calls each
sink independently. The same flag lets `set()` detect and reject (`false`,
no state touched, no lock attempted) a call made from *inside* the
currently-installed callback's own invocation — the one case that would
still self-deadlock (that thread already holds `callbackMutex_` for the
whole invocation, which is deliberately non-recursive) — backed by a
debug-build `assert()` too, though see the honesty note below on why that
assert is not this project's real safety net. `rxSetLogCallback()` maps a
`false` return to `RX_E_FAIL`.

**Toolchain finding worth recording:** `base_sink<std::recursive_mutex>`
does not link against this project's precompiled spdlog (`SPDLOG_COMPILED_LIB`
— only `base_sink<std::mutex>`/`base_sink<null_mutex>` are explicitly
instantiated into `libspdlog.a`, matching spdlog's own built-in sinks).
Fixed by directly `#include <spdlog/sinks/base_sink-inl.h>` in
`log_forward_sink.cpp` — legal regardless of `SPDLOG_HEADER_ONLY` (that
header re-includes `base_sink.h`, idempotently, under `#pragma once`) — so
the compiler instantiates that one specialization locally instead of
expecting it already compiled into the prebuilt library. No spdlog rebuild,
no dep-cache change.

**Medium — missing catch-all.** `rxSetLogCallback()`'s body is now wrapped
in the same `try`/`catch (const std::exception&) { RX_LOG_ERROR(...); return
RX_E_FAIL; }` pattern every other `RxResult`-returning entry point in
`api_impl.cpp` already used — it was the sole exception before this round.

**Low #3 — steady-state cost.** Added `std::atomic<bool> installed_` as a
fast-path hint: `sink_it_()` checks the `thread_local` re-entrancy flag,
then this atomic, and returns before ever touching `callbackMutex_` when
nothing is installed — restoring a genuinely near-zero-cost uninstalled
path (no mutex acquired on this sink's own side at all). Doc comments
(`log_forward_sink.h`, and this report) now state the true cost precisely
instead of the prior "one mutex lock" undercount: spdlog's own
`base_sink<Mutex>::log()` unconditionally takes *its own* lock around every
sink on every record regardless of install state (unavoidable, paid by the
console sink too); this class adds a *second* lock only on the installed
path, never on the uninstalled one.

**Low #4/informational — `std::thread` vs `rx_task`.** Acknowledged, not
changed: the worker-thread delivery test (`log_test.cpp`) still uses a raw
`std::thread`, not `rx::task::Scheduler`. This satisfies the binding
resolution given to the review (generic "worker-thread delivery," verified
via a genuine cross-thread `threadId` assertion) but deviates from the
original brief's more specific "via rx_task" wording — a faithfulness nit
for a future task that touches this file, not addressed in this round.

**Honesty note on the debug assert:** this project's own `CMakePresets.json`
builds both presets `RelWithDebInfo`, and CMake's own defaults define
`NDEBUG` for that build type — confirmed directly against this tree's
`build.ninja` (`-DNDEBUG` is present on every compile line, both presets).
A plain `assert()` therefore compiles out in every build this project
actually configures. The `assert()` added at `set()`'s reentrancy check is
present as documentation-and-defense-in-depth for a genuine Debug build
elsewhere — it is NOT what makes this project's own builds safe. What
actually prevents the self-deadlock here, unconditionally, is the
`thread_local` check returning `false` before ever attempting the lock; the
`assert()` is a secondary, mostly-symbolic layer on top of that, called out
explicitly here rather than presented as real protection it isn't.

### Tests added this round (all device-free, `rx_core`/`rx_material` existing
test files extended, no new `tests/CMakeLists.txt` entries)

- **Reviewer's UAF-reproduction probe** (`log_test.cpp`) — install →
  concurrent storm from 4 threads driving the sink directly (`sink->log()`,
  not through the shared console-carrying default logger, to keep the storm
  quiet and precise) → uninstall → immediately free `userData` and allocate
  a same-size replacement while the storm keeps running a little longer with
  nothing installed → join → assert the replacement was never touched. Run
  20 times in one `TEST_CASE`. ASan/UBSan were re-considered per the
  review's own note and are not usable in this project's zig cc/zig c++
  toolchain (same conclusion the review already reached independently) — this
  uses the same poison/observe pattern the review's own probe used instead.
  Verified stable across 4 separate full-suite runs on this machine (no
  crash, no hang, no corrupted replacement) on top of the 20 internal
  repetitions each run already performs.
- **Re-entrant-callback test** (`log_test.cpp`) — a callback that logs from
  inside itself, run through a temporary logger with two real sinks (a
  capturing stand-in for "console" + the real forward sink), asserting: (1)
  the callback itself is invoked exactly once (the inner, re-entrant record
  is dropped for forwarding), (2) both the outer and inner message text
  reach the other sink, (3) the test completes at all (no deadlock).
- **Inside-callback-uninstall rejection test** — implemented device-free at
  both layers: `log_test.cpp` (`LogForwardSink::set()` directly, asserting
  `false`) and `test_api_contract.cpp` (`rxSetLogCallback()` through the
  real ABI entry point, asserting `RX_E_FAIL`), both triggered by a real
  callback invocation, both proving no deadlock by completing, both
  confirming state is unchanged (still installed, still delivering) after
  the rejection.

### Verification (fix round 1)

Both presets rebuilt clean end-to-end (`cmake --build`, full targets, zero
new warnings beyond the pre-existing unrelated `_WIN32_WINNT` one).
`rx_core_tests`: **16/16 cases, 102/102 assertions** (up from 13/27).
`rx_material_tests`: **13/13 cases, 71/71 assertions** (up from 12/65 — one
net new test; some assertion-count arithmetic shifted with the new case).
`rx_material_gpu_tests` unaffected: **25/25 cases, 344/344 assertions, zero
Vulkan validation errors** — re-ran to confirm no regression from the sink
rework. Full `linux-native` `ctest`: **16/16 suites**. Re-ran both new
binaries under Wine against the real `windows-cross-zig` build output
(`rx_core_tests.exe`/`rx_material_tests.exe`) — identical pass counts on
both platforms. `sample_01_triangle --validate --log-callback` re-verified:
same behavior as round 1, exit 0, every console line still mirrored through
`[log-callback]`. The UAF probe TEST_CASE itself was additionally run 3 more
times standalone (4 total) with no failure, hang, or crash on any run.

---

## Summary

Added `rxSetLogCallback()` — a process-wide, ABI-stable entry point that
lets a consuming engine observe every record RendererX logs through
`rx_core`'s spdlog-backed logging (`RX_LOG_INFO/WARN/ERROR`) from its own
logging/telemetry system, from any thread. The mechanism itself
(`rx::core::log::LogForwardSink`) lives in `rx_core` as a plain spdlog
sink with zero ABI-type knowledge; the ABI wrapper in `rx_material`
(`rxSetLogCallback`, `RxLogSeverity`, `RxLogCallback`) is a direct,
uncasted pass-through to it.

## Design decisions

**1. `RxLogSeverity` follows `RxResult`'s own convention, not a new one.**
`typedef int32_t RxLogSeverity; enum : RxLogSeverity { RX_LOG_TRACE = 0, ...
RX_LOG_ERROR = 4 };` — a plain `int32_t` typedef with named constants,
matching `RxResult`/`RxFormat` exactly rather than introducing a scoped
`enum class` shape into this header. Values are pinned by a `static_assert`
that also documents *why*: they equal `spdlog::level::level_enum`'s own
`trace..err` values 1:1 (verified against `spdlog/common.h`,
`SPDLOG_LEVEL_TRACE..ERROR = 0..4`), so `LogForwardSink` needs no
translation table on either side — a raw `int32_t` crosses from spdlog's
`log_msg.level` straight to the callback. `spdlog::level::critical` (5)
folds into `RX_LOG_ERROR` (this engine has no `RX_LOG_CRITICAL` macro).

**2. `RxLogCallback` needs no cast at the ABI/internal boundary.** Because
`RxLogSeverity` is a plain alias for `int32_t` (not a distinct enum type),
`RxLogCallback` (`void(*)(RxLogSeverity, const char*, const char*, void*)`)
and `rx_core::log::ForwardCallback` (`void(*)(int32_t, const char*, const
char*, void*)`) are the literal same C++ type after alias resolution.
`api_impl.cpp`'s `rxSetLogCallback()` passes `cb` straight through with no
`reinterpret_cast` — avoiding the "calling through an incompatible
function pointer type" UB a cast-based bridge would otherwise carry.

**3. Mechanism lives in `rx_core`, not `rx_material`.** `LogForwardSink`
is a pure `spdlog::sinks::base_sink<std::mutex>` extension with no
knowledge of `rx_api.h` at all — placed at
`src/rx_core/include/rx_core/log_forward_sink.h` +
`src/rx_core/src/log_forward_sink.cpp`, matching `log.h`/`log.cpp`'s own
include/src split (the brief's `src/rx_core/log_forward_sink.{h,cpp}` path
is this convention's shorthand). `forwardSink()` is an idempotent,
`call_once`-guarded lazy singleton, callable from anywhere any number of
times: it constructs the sink and appends it to
`spdlog::default_logger()`'s own sink list exactly once per process,
additively (the console sink is never touched/removed). `log::init()`
calls it too (defensive/eager), so both `log::init()` and
`rxSetLogCallback()` converge on the same instance regardless of which
runs first.

**4. Mutex-guarded `{callback, userData}` pair, not atomics.** Justified
in `log_forward_sink.h`'s own doc comment: `callback`/`userData` must be
observed *together* — a torn read pairing a new callback with stale
userData (or vice versa) would silently corrupt the caller's own context
on the very next racing log record. The critical section on both sides
(a two-pointer copy) is cheap enough that a mutex is immaterial next to
spdlog's own formatting/IO cost, so there's no real incentive for a
lock-free alternative. `sink_it_()` snapshots the pair and **releases the
lock before invoking the callback** — deliberately, to avoid a
self-deadlock if the callback logs anything itself (a re-entrant call back
into this same sink, same thread, same non-recursive mutex) or calls back
into `set()`/`rxSetLogCallback()`.

**5. Throw-disable is precise, not a blunt global kill switch.** On an
exception, `disableIfStillInstalled()` only nulls the callback if it is
*still* the one that just threw (compare-before-clear) — so a legitimate
concurrent `set()` racing the disable can never be clobbered by it. The
one console warning goes directly to `stderr` via `std::fprintf`, never
back through spdlog (re-entering the same logger/sink from inside its own
callback path is exactly what decision 4 above avoids). `disabledByException()`
is a dedicated, mutex-guarded test seam so tests can assert the disable
state deterministically instead of scraping stderr text.

**6. No GUID change; explicitly a free function + PODs.** `rxSetLogCallback()`
adds zero vtable slots to any existing interface — unlike Task 7's
`createTexture2D()`, which *did* need `kIID_IRxMaterialSystem` regenerated
because it changed `IRxMaterialSystem`'s own shape. `rx_api.h` carries an
explicit comment contrasting the two cases; no `kIID_*` constant anywhere
in the header was touched (verifiable directly in the diff — no GUID
literal changed).

**7. Sample 01 demos the underlying mechanism, not the ABI wrapper
directly — with the tradeoff stated up front.** `sample_01_triangle`
links only `rx_rhi_vk`/`rx_platform`, no `rx_material`. Linking
`rx_material` (transitively `slang::slang` + its runtime-library
deployment step) into the smallest, lightest sample purely to prove log
forwarding was judged disproportionate — `rx_api.h`'s `rxSetLogCallback()`
is a direct, uncasted pass-through to `rx::core::log::forwardSink()` (see
decision 2), so the sample calls that same `rx_core` entry point one layer
lower, with zero new link dependencies. The `--log-callback` flag installs
`sampleLogCallback()` (prints `[log-callback] [SEVERITY] message` to
stdout, standing in for a consuming engine's own log sink); the existing
`sample_01_triangle_headless` ctest gate never passes it, so the gate is
unaffected. This is a deliberate scope call, documented at the top of
`main.cpp` and flagged here for visibility rather than silently taken.

## Files changed

- `src/rx_core/include/rx_core/log_forward_sink.h` (new) — `ForwardCallback`,
  `LogForwardSink`, `forwardSink()`.
- `src/rx_core/src/log_forward_sink.cpp` (new) — severity mapping,
  `set()`/`sink_it_()`/`flush_()`/`disableIfStillInstalled()`, singleton.
- `src/rx_core/src/log.cpp` — `init()` now also calls `forwardSink()`
  inside its existing `call_once` block.
- `src/rx_core/CMakeLists.txt` — added `src/log_forward_sink.cpp` to the
  `rx_core` library's sources. No test-file additions (see below).
- `src/rx_core/tests/log_test.cpp` (extended, not replaced) — four new
  `TEST_CASE`s: delivery of severity/category/message, uninstall-stops-
  delivery, worker-thread delivery (`std::thread`, asserts
  `captured.threadId != mainThreadId`), throw-disable path.
- `src/rx_material/include/rx_material/rx_api.h` — `RxLogSeverity`,
  `RxLogCallback`, `rxSetLogCallback()` declaration with full ABI/thread-
  affinity/exception-discipline doc comment (points at `docs/threading.md`
  per that file's own "every new public header carries a one-line
  thread-affinity note" rule).
- `src/rx_material/api_impl.cpp` — `rxSetLogCallback()` definition
  (`log::init()` defensively, then `forwardSink()->set(cb, userData)`,
  always `RX_OK`).
- `src/rx_material/tests/test_api_contract.cpp` (extended) — two new
  `TEST_CASE`s: RX_OK across install/replace/uninstall transitions; an
  authentic end-to-end delivery test that triggers a REAL `RX_LOG_ERROR`
  through `api_impl.cpp`'s own device-free `loadMaterial()` path (not a
  synthetic log call) and verifies uninstall stops it.
- `src/rx_material/tests/test_api_header_self_contained.cpp` (extended) —
  names `RxLogSeverity`/`RX_LOG_*`/`RxLogCallback`/`rxSetLogCallback` in
  the header-alone compilation unit, plus a re-pinned `sizeof(RxLogSeverity)`
  static_assert matching the file's existing pattern.
- `samples/01_triangle/main.cpp` — `--log-callback` flag +
  `sampleLogCallback()` adapter (see decision 7).

**No `tests/CMakeLists.txt` in either `rx_core` or `rx_material` was
touched** — every new test lives in an existing, already-listed `.cpp`
file, per the brief's "prefer extending existing test FILES" guidance.
Nothing to flag for the merge on that front.

## Verification

Both presets configured and built clean (`cmake --preset
linux-native`/`windows-cross-zig`, full build, zero errors; the
`windows-cross-zig` build's only warnings are the pre-existing,
unrelated `_WIN32_WINNT macro redefined` toolchain warning that appears on
every file in that preset, files I never touched included — not a
regression).

`linux-native` full `ctest` run: **16/16 suites passed**, including
`rx_core_tests` (13 cases / 27 assertions, up from 2 cases before this
task) and `rx_material_tests` (12 cases / 65 assertions) covering the new
surface, plus the untouched `rx_material_gpu_tests` (25 cases / 344
assertions, zero validation errors) proving no regression to the existing
device-backed ABI surface.

Ran `sample_01_triangle --validate` (gate-equivalent, exit 0) and
`sample_01_triangle --validate --log-callback` by hand: every console log
line during a full headless run (Vulkan validation warnings, device
creation, `"triangle readback PASSED"`) was mirrored through
`[log-callback] [SEVERITY] message`, confirming end-to-end delivery in a
real running sample, not just under test. Exit code 0 in both runs.

Throw-disable path verified with real output, not just assertions:

```
[error] this record's callback throws
rx_core: log-forward callback threw std::exception("boom") -- disabling it permanently ...
[error] a second record after the callback was disabled
```

(the second record's own console line still prints — via the untouched
console sink — while `disabledByException()` stays `true` and the
throwing callback itself is never invoked again).

## Concerns / follow-ups

- `LogForwardSink` attaches to whichever *specific* logger object is
  `spdlog::default_logger()` the first time `forwardSink()` runs anywhere
  in the process. If some other code permanently swapped the default
  logger to a different instance without restoring it, `RX_LOG_*` calls
  after that point would bypass the forwarding sink. The one existing test
  that swaps loggers (`log_test.cpp`'s original ostream-sink test) already
  restores the original afterward, so this is a pre-existing pattern this
  task relies on rather than one it introduces — flagged for anyone adding
  a *future* logger swap without a restore.
- A callback that logs from inside itself (calling back into `RX_LOG_*`)
  will re-enter `sink_it_()` on the same thread; decision 4's "unlock
  before invoking" avoids a deadlock on that path, but an unconditional
  self-logging callback would still recurse for as long as it keeps
  logging — a caller-side contract issue (same class of footgun as most
  logging libraries' own re-entrancy caveats), not something this task
  engineered a guard against.
  **[Superseded, fix round 1]** This bullet described round 1's actual
  shipped behavior at review time and is kept for the record, but it is no
  longer accurate: the fix round rebuilt the locking model (`callbackMutex_`
  now held across the invocation, closing a separate, more serious Critical
  UAF this bullet did not anticipate) and, as part of closing the
  self-deadlock that new model would otherwise reintroduce, added an actual
  guard — re-entrant forwards are now dropped after exactly one nested
  attempt (never recursing further), not merely avoiding a deadlock while
  still recursing. See "Fix round 1" at the top of this report.
- Sample 01's `--log-callback` proves the underlying `rx_core` mechanism,
  not `rxSetLogCallback()` itself end-to-end in a *running* sample (the
  ABI wrapper is exercised authentically in `rx_material_tests` instead —
  see decision 7's full rationale for why linking `rx_material` into this
  particular sample was judged disproportionate).
