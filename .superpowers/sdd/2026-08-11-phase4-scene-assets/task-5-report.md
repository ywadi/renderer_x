# Task 5 report: Public log sink (D23, seed 13)

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
- Sample 01's `--log-callback` proves the underlying `rx_core` mechanism,
  not `rxSetLogCallback()` itself end-to-end in a *running* sample (the
  ABI wrapper is exercised authentically in `rx_material_tests` instead —
  see decision 7's full rationale for why linking `rx_material` into this
  particular sample was judged disproportionate).
