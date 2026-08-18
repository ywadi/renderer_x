# CI-red fix report: rx_asset_gltf_gpu_tests SIGSEGV (CI run 32180630087)

**Scope:** `rx_asset` only (`src/rx_asset/registry.cpp`, `src/rx_asset/tests/async_import_test.cpp`), plus a
documentation addition (`README.md`). `rx_task` and `rx_rhi_vk` were investigated but **not modified** — the
persistent-task Scheduler design (commits `da566ed`/`2ed0599`) and 7cc685f's GPU-wait semantics are untouched.

**Base commit:** `aeb61b2` (HEAD at dispatch). **Regression window investigated:** `da566ed..d1fb0d7` (Task 15:
async import pipeline, persistent-task scheduler, rollback fixes) + `7cc685f` (rx_rhi_vk GPU-wait hazard fix).

## 1. Symptom (as reported)

CI run [32180630087](https://github.com/ywadi/renderer_x/actions/runs/32180630087) failed on **both** jobs:

- `windows-cross-zig`, step "Test under wine (xvfb; GPU tests excluded -- no Vulkan under Wine)":
  `rx_asset_gltf_gpu_tests.exe` SIGSEGV.
- `linux-native`, step "Test (xvfb + lavapipe)": `rx_asset_gltf_gpu_tests` SIGSEGV, 13.83s in, test #9,
  19/20 otherwise green (confirms `7cc685f`'s own GPU-wait fix: zero SYNC-HAZARD lines in the `rx_rhi_vk_tests`
  run).

This binary passed on Wine at Task 13/14 heads (`27b530d`, `55b4822`) and failed at `aeb61b2` — narrowing the
regression to the window above.

## 2. Repro transcript

### 2.1 Local Wine repro — did not reproduce as originally framed

The dev machine (8 cores, native lavapipe, wine-11.0) ran `rx_asset_gltf_gpu_tests.exe --validate` under
`xvfb-run -a wine` (mirroring CI's invocation) repeatedly, including under `taskset -c 0,1` (2-core, matching
GitHub's runner class), forced lavapipe (`VK_ICD_FILENAMES`), `LP_NUM_THREADS=1`, and artificial CPU
oversubscription (4-6 busy-loop processes pinned to the same 2 cores). Every run: 55/55, exit 0. Wine itself
turned out **not** to be device-free here — CI installs `mesa-vulkan-drivers`/`vulkan-tools`/`libvulkan-dev`
alongside Wine specifically so `winevulkan` has a real (software) Vulkan implementation to forward to (see
`.github/workflows/ci.yml`'s own comment on that job's "Install system packages" step) — confirmed directly:
the local Wine run's own log shows real `Device::create:` lines, not a skip guard. So this binary genuinely
exercises the real async-import/GPU pipeline under Wine, same as `linux-native` — Wine's overhead just widens
whatever timing window the real bug needs, exactly like CI's own weaker/shared hardware does.

### 2.2 Pivot: pulled the actual CI logs (`gh run view --log`)

```
9/20 Test  #9: rx_asset_gltf_gpu_tests ............***Exception: SegFault 13.83 sec
...
/home/runner/work/renderer_x/renderer_x/src/rx_asset/tests/async_import_test.cpp:646:
TEST CASE:  importGltfAsync: WALL-CLOCK GATE -- deliberately slow decode + a real (DamagedHelmet-scale) payload
in flight, >=300 driven frames, every pumpMain() call bounded (2ms local budget, 10ms CI stall detector),
zero wait-calls-from-async-path, ring never exhausts

/home/runner/work/renderer_x/renderer_x/src/rx_asset/tests/async_import_test.cpp:748: FATAL ERROR:
REQUIRE( pumpDuration < kCiStallDetector ) is NOT correct!
  values: REQUIRE( 12633µs <  10000µs )

[2026-08-18 20:22:14.644] [error] [vulkan validation] Validation Error:
[ VUID-vkEndCommandBuffer-commandBuffer-00059 ] ... vkEndCommandBuffer(): was called in VkCommandBuffer ...
which is invalid because bound VkImage ... was destroyed.
.../async_import_test.cpp:646: FATAL ERROR: test case CRASHED: SIGSEGV - Segmentation violation signal
```

This is the real signal the task brief's "two InvalidFileData lines" description was pointing near (an
earlier, unrelated pair of `InvalidFileData` errors from the *previous* test case, `importGltfAsync:
garbage-bytes import...`, happen to sit a few lines above in the raw log) — the actual crashing test is the
**WALL-CLOCK GATE** (`async_import_test.cpp:644-798`), and the actual trigger is its own hard CI-stall-detector
`REQUIRE` firing on CI's slower hardware (one `pumpMain()` call — registering DamagedHelmet's first real JPEG
texture — took 12.6ms against a 10ms ceiling; the task's own report for the introducing task, section 7 of
`task-15-report.md`, already disclosed this exact assertion is timing-sensitive under contention, just did not
anticipate it firing without `-j4` contention, purely from CI's own weaker single-core throughput).

### 2.3 Isolation

`--test-case`-filtered runs plus a from-scratch ASan/UBSan build (`-fsanitize=address,undefined -O1 -g`,
mirroring Task 15's own methodology) confirmed: every one of the 55 pre-existing test cases passes cleanly and
ASan-clean in isolation. The crash is not "test case N is broken" — it is what happens to a **specific runtime
state** (an async import job abandoned mid-upload) when the surrounding objects unwind, which the
WALL-CLOCK GATE test's own `REQUIRE` failure happens to produce by interrupting its drive loop mid-flight, but
which is not specific to that `REQUIRE` at all (see §3).

## 3. Root cause

**File:line:** `src/rx_asset/registry.cpp:344-383` (`Registry::~Registry()`, pre-fix), interacting with
`src/rx_rhi_vk/src/upload.cpp:615-632` (`Uploader::~Uploader()`) and `src/rx_asset/texture_cache.cpp:106-118`
(`TextureCache::~TextureCache()` — more precisely, its implicit `pool_` member destructor, which unconditionally
destroys every still-"resident" `Texture2D`).

**This is a genuine PRODUCTION-code defect, not a test-only artifact.** Severity is elevated accordingly: any
real host application (Windows included — nothing about the mechanism is Wine- or Linux-specific) that tears
down its `Registry` (for any reason: a thrown exception, an early return, an unrelated fatal error, or simply
choosing to shut down) while an `importGltfAsync()` job is genuinely mid-**upload** (≥1 real GPU resource
already registered via `TextureCache::registerDecoded()`/`GeometryPool::uploadDeferred()`), **without** first
explicitly calling `cancelImport()` and then continuing to call `pumpMain()` until the rollback is confirmed
safe, hits this same corruption. On a fast machine the corrupted-command-buffer state happens to not crash
(confirmed empirically, §5) — it silently produces a `UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-
VkImage` validation error and moves on. On CI's weaker/software-rasterizer/Wine-translated environment, the
driver processing that corrupted state segfaults instead.

### The mechanism

1. `Registry::importGltfAsync()` creates an `AsyncImportJob` (`registry.cpp:29-70`) holding raw
   `pool`/`textures`/`scheduler` pointers and (once compute finishes) a `std::unique_ptr<MarshalPendingImport>
   pending` (`import_pipeline.h:261-291`) — the live handle to whatever real GPU resources
   `marshalGltfImportPrepareStep()` has already registered.
2. Every step of the marshal/upload phase (`runAsyncImportPrepareStep`, `pollAsyncImportUploads`,
   `rollbackAsyncImportWhenSafe`, `registry.cpp:91-194`, pre-fix) advances **only** via
   `Scheduler::postToMain()` closures, drained by `pumpMain()`. There is no other clock driving this state
   machine forward.
3. `Scheduler::~Scheduler()` (`src/rx_task/scheduler.cpp:284-343`) documents, explicitly, that "postToMain()
   work still queued when `pumpMain()` is never called again is simply dropped (its captured closure destroyed
   as an ordinary side effect)" — this is by design, and correct for the Scheduler's own contract in isolation.
4. Before this fix, `Registry::~Registry()` (`registry.cpp:254-276`, pre-fix) only ever set
   `cancelled = true` / `registry = nullptr` on each outstanding job and walked away. It never checked
   whether that job's `pending` still held real, unreleased GPU resources. If nothing ever pumps again
   (§2.2's `REQUIRE` failure is one way this happens; a caller that cancels and then tears down without
   continuing to pump is another, pre-existing way — see §4), the queued rollback closure referenced in step 3
   is simply dropped, and `AsyncImportJob`'s (and its `MarshalPendingImport`'s) plain C++ destructor runs
   instead — which does **not** call `TextureCache::releaseUnpublished()`/`GeometryPool::free()`. The
   texture stays "resident" inside `TextureCache`'s own bookkeeping, indefinitely, with its upload copy command
   still sitting on `Uploader`'s batched, not-yet-ended command buffer.
5. Eventually, the owning `TextureCache` is destroyed. Its own destructor body (`texture_cache.cpp:106-118`)
   only explicitly destroys `VkSampler`s — the actual `Texture2D`/`VkImage` objects live inside its `pool_`
   member, which the compiler-generated implicit destructor tears down unconditionally, including the
   still-"resident", never-released entry from step 4.
6. Later still (in every fixture this project's own tests use, `TextureCache`/`GeometryPool` are declared,
   hence destructed, **before** `Uploader`), `Uploader::~Uploader()` (`upload.cpp:615-632`) runs its own
   documented "auto-flush pending... work" — calling `vkEndCommandBuffer()` on the command buffer that step 4
   left open, which still references the VkImage step 5 just destroyed:
   `VUID-vkEndCommandBuffer-commandBuffer-00059`. The driver then processes this corrupted state; on CI's
   environment, that segfaults.

**This is the identical failure signature `texture_cache.cpp:328-343` (`buildFallbackTextures()`) already hit
and fixed once before** ("A TextureCache that is constructed and then torn down without ever loading a REAL
texture ... would otherwise reach `~TextureCache()` with those 4 images destroyed while Uploader's OWN
destructor still auto-flushes ... reproduced directly, empirically, as a real
`UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkImage` validation error") — for a **different**,
narrower code path (the four D11 fallback textures). Task 15's async pipeline reintroduced the same failure
class for its own, much more general, "abandon a job with real GPU resources already registered" path, and it
was never covered by a test that actually reaches that state without the caller performing an explicit,
fully-drained `cancelImport()` first.

**This bug was already sighted once, out of scope, and misdiagnosed as flakiness.** `.superpowers/sdd/
2026-08-11-phase4-scene-assets/ci-red-fix-report.md` §5 (the report for the immediately-preceding `7cc685f` fix)
records: *"`rx_asset_gltf_gpu_tests` ... failed once locally with the same `CHECK_FALSE(fixture->context.
hasValidationErrors())` shape, at a completely different assertion (`async_import_test.cpp:918`) ... it did
not reproduce consistently either with or without my change — consistent with pre-existing flakiness in that
concurrently-developed code, not something this fix caused."* That is this exact bug, seen once, months before
this CI run, and correctly flagged as out-of-scope for that task — but never independently investigated.

### Registry's own documented contract already promised this was safe

`src/rx_asset/include/rx_asset/registry.h:117-125` ("POOL/TEXTURES/SCHEDULER LIFETIME"): *"destroying the
REGISTRY specifically while one of its own async imports is in flight is safe and defined."* The pre-fix
`~Registry()` only actually delivered on that promise for jobs still in the **compute** phase (nothing
GPU-side registered yet) — exactly the two cases the pre-existing "destroy while in flight" regression tests
cover (`async_import_test.cpp`, both pass `textures == nullptr`, so their jobs never leave the compute phase).
For a job already in the **marshal/upload** phase, the promise was not upheld. This fix makes `~Registry()`
actually honor its own documented contract for every job state, not just the two that happened to be tested.

## 4. Fix

`src/rx_asset/registry.cpp:308-342` adds `drainAndRollbackAbandonedAsyncJob()`: for a job whose `pending` is
still non-null when `Registry::~Registry()` runs, it (a) calls the existing
`marshalGltfImportEnsureRollbackTicketed()` to force a real ticket for anything recorded-but-not-yet-flushed,
(b) polls (bounded, 5s, 1ms sleep — the same idiom every test in this file already uses) the existing
`marshalGltfImportUploadsComplete()` until the GPU confirms every issued ticket is actually done, then
(c) calls the existing `marshalGltfImportRollback()` — the exact same three functions the **live**
`rollbackAsyncImportWhenSafe()` cancellation path already uses, just driven synchronously instead of via
`postToMain()` reposts, because at destructor time nothing is guaranteed to ever pump again. If the bounded
wait times out (a genuine GPU hang/device-loss condition, not something this fix can resolve), it logs loudly
and **leaks** `pending` rather than risk destroying a still-in-flight `VkImage`/`VkBuffer` — a resource leak
is always recoverable at process exit; a use-after-free is not.

`registry.cpp:377` calls it from `Registry::~Registry()`'s existing per-job loop, once, before clearing
`asyncJobs_`.

### Why this is safe, and why it is the minimal, correctly-scoped fix

- **No new assumption.** `registry.h`'s own "POOL/TEXTURES/SCHEDULER LIFETIME" contract already *requires*
  `pool`/`textures`/`scheduler` to outlive any job referencing them — hence to outlive `Registry`, which may
  still hold a live job at its own destruction. `~Registry()` using them directly relies on nothing beyond
  what was already a documented caller obligation.
- **Blocking here is a deliberate, precedented, narrowly-scoped exception to D25's "poll, never block"
  invariant** — the exact same trade-off `Device::~Device()`'s own unconditional `vkDeviceWaitIdle()` already
  makes, and the one `buildFallbackTextures()` already makes (`uploader_.wait(uploader_.flush())`,
  `texture_cache.cpp:343`/`440`) for this identical failure signature. It only ever fires on the **abandonment**
  path (a job whose `pending` was never consumed by a normal finalize/rollback) — never on the live,
  wall-clock-gated async path the existing "zero `wait()` calls" counters (`waitCallCountForTesting()`) verify.
  Confirmed: the fix changes zero counted call sites in `GeometryPool`/`TextureCache`, so every existing
  D25-related assertion is untouched (§5).
- **No `rx_task`/`rx_rhi_vk` changes.** The persistent-task Scheduler design (`2ed0599`) and `7cc685f`'s
  GPU-wait semantics are both left exactly as they were — this closes the gap entirely from the `rx_asset` side,
  by making the one already-safe rollback path reachable synchronously from the one place (`~Registry()`) that
  was the actual gap.
- **Covers both known trigger shapes with one change.** It fixes not only "a driving loop's own `REQUIRE`
  throws mid-upload" (§2.2's proximate trigger) but also the pre-existing, more general "a caller explicitly
  calls `cancelImport()` then tears down without continuing to pump" mistake — `~Registry()` does not care
  *why* `pending` is still non-null, only that it is.

## 5. Regression test

`src/rx_asset/tests/async_import_test.cpp:1279` — new `TEST_CASE`: *"importGltfAsync: abandoning a job
genuinely mid-UPLOAD (>=1 real GPU resource already registered, no explicit cancelImport(), no further
pumpMain()) does not crash and raises no validation errors [Wine/CI SIGSEGV regression, CI run 32180630087]"*.

Drives a real `importGltfAsync()` against DamagedHelmet with a real `TextureCache` until at least one texture
slot is confirmed registered (`liveTextureCountForTesting()` increases — the same observable signal the
pre-existing cancel-mid-upload test uses), then deliberately abandons the job (no `cancelImport()`, no further
`pumpMain()` calls) and explicitly reproduces the exact corrupting sequence (destroy `TextureCache`, then flush
`Uploader`) while `Context` is still alive, so the result is observable via `hasValidationErrors()` rather than
only "did the process crash" (native lavapipe on the dev machine does **not** crash on the corrupted state —
only CI's own environment does, confirmed empirically below — so a bare crash check is too weak a local
discriminator; the validation error itself is the reliable, portable signal both before and after the fix).

**Revert-tested** (temporarily reverted `registry.cpp` to the base-commit version, kept the new test):

```
=== 3/3 runs against PRE-FIX registry.cpp ===
[error] [vulkan validation] Validation Error: [ UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkImage ]
  ... You are adding vkEndCommandBuffer() ... invalid because bound VkImage ... was destroyed.
CHECK_FALSE( fixture->context.hasValidationErrors() ) is NOT correct!  values: CHECK_FALSE( true )
[doctest] test cases:  1 |  0 passed | 1 failed        (3/3 runs, deterministic)
```

```
=== 5/5 runs against FIXED registry.cpp ===
[doctest] test cases:  1 |  1 passed | 0 failed        (5/5 runs, zero validation errors, zero DeletionQueue
                                                          "never flushed" warnings)
```

## 6. Documentation

`README.md`'s "Testing" section gained a new "Windows-cross-zig: verify under Wine locally, not just 'it
builds'" subsection: local verification for any change touching a binary `windows-cross-zig`'s CI job actually
*runs* now always includes the exact CI Wine invocation
(`xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample' --output-on-
failure`) for **every** binary that filter does not exclude — not just the genuinely device-free ones
(`rx_task_tests.exe`, `rx_asset_gltf_tests.exe`, etc.) — closing the exact blind spot this bug slipped through
(CI's filter includes `rx_asset_gltf_gpu_tests.exe`, which constructs a real `VkDevice` under Wine via
`winevulkan`→lavapipe passthrough; a clean *build* was previously treated as sufficient local verification for
it). No plan/spec/ledger document was touched — MANUAL_VERIFICATION.md was left alone (it is scoped to
human-observed `--present` visual checks, a different category from this ctest-under-Wine convention).

## 7. Full verification

All of the following ran on the base checkout (`aeb61b2` + this fix), from-scratch or incremental builds as
noted:

**`rx_asset_gltf_gpu_tests --validate`, linux-native, native lavapipe:**
```
[doctest] test cases:      56 |      56 passed | 0 failed | 0 skipped
[doctest] assertions: 8716302 | 8716302 passed | 0 failed |
[doctest] Status: SUCCESS!
```
(55 pre-existing + 1 new regression test, all passing; was 55/55 before this task.)

**Full serial `ctest --preset linux-native --output-on-failure`:**
```
100% tests passed, 0 tests failed out of 20
Total Test time (real) =  55.86 sec
```

**`cmake --build --preset windows-cross-zig` (full):** clean, zero errors/warnings introduced.

**`xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample' --output-on-
failure` (CI's exact invocation, under Wine):**
```
 1/10 Test  #1: shader_spirv_test ................   Passed    0.05 sec
 2/10 Test  #2: rx_core_tests ....................   Passed   66.76 sec
 3/10 Test  #3: rx_task_tests ....................   Passed    0.86 sec
 4/10 Test  #4: rx_platform_tests ................   Passed    1.60 sec
 5/10 Test  #5: rx_shader_tests ..................   Passed    0.32 sec
 6/10 Test  #6: rx_asset_tests ...................   Passed   10.92 sec
 7/10 Test  #7: rx_asset_gltf_tests ..............   Passed    0.08 sec
 8/10 Test  #8: rx_asset_gltf_gpu_tests ..........   Passed   43.63 sec
 9/10 Test  #9: rx_graph_tests ...................   Passed    0.05 sec
10/10 Test #10: rx_material_tests ................   Passed    0.08 sec
100% tests passed, 0 tests failed out of 10
```
`rx_task_tests.exe` and `rx_asset_gltf_tests.exe` (the two other Wine-run binaries the brief named) both clean.

**Fresh ASan/UBSan build** (`-fsanitize=address,undefined -O1 -g -fno-omit-frame-pointer`, mirroring Task 15's
own methodology, `linux-native` toolchain, excluding only the pre-existing, separately-documented
timing-sensitive `*WALL-CLOCK*` case per Task 15's own sanitizer-timing exclusion):
```
[doctest] test cases:     55 |     55 passed | 0 failed | 1 skipped
[doctest] assertions: 379672 | 379672 passed | 0 failed |
[doctest] Status: SUCCESS!
```
Zero new sanitizer defects from this fix.

## 8. Concerns / residual risk

- **The WALL-CLOCK GATE's 12.6ms spike (§2.2) is itself a separate, minor, already-partially-disclosed
  finding**, not fixed by this task (out of scope: it is a timing assertion, not a lifetime bug) — CI's own
  hardware is now known to occasionally exceed the 10ms hard ceiling on a single texture registration purely
  from its own throughput, without any `-j4` contention (the `task-15-report.md` §7/8 disclosure only
  anticipated the contention case). This fix makes that spike harmless (no crash, clean rollback) rather than
  catastrophic, but the assertion itself could still intermittently fail the WALL-CLOCK GATE test on weak CI
  hardware. That is a pre-existing, disclosed, separate concern for a future task to size a CI-aware budget for
  (D18's own "runner-aware headroom" model already anticipates this class of issue) — not something this fix's
  scope covers or should paper over.
- **GeometryPool has the analogous risk** (an abandoned mid-upload geometry range, not just textures) —
  `drainAndRollbackAbandonedAsyncJob()` covers it identically (`marshalGltfImportRollback()` always handles both
  halves together), but no dedicated regression test isolates the geometry-only case specifically, since the
  CI evidence (§2.2) showed the crash triggering on the very first texture slot, before geometry was ever
  reached. The fix's coverage is not narrower than the bug, only the new test's specific reproduction path is.
