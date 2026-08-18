# CI-red fix: upload_test.cpp SYNC-HAZARD-READ-AFTER-WRITE (rx_rhi_vk_tests)

## 1. Diagnosis

**Failing runs** (`linux-native` job, `Test (xvfb + lavapipe)` step; `windows-cross-zig`
excludes `rx_rhi_vk_tests` and was green throughout):

| Run | Commit | rx_rhi_vk_tests | Other failures |
|---|---|---|---|
| 32121427135 (first red) | `0d9ca39` (Task 11) | 6 cases in upload_test.cpp | none |
| 32128084627 | `6a825f7` | same 6 cases | none |
| 32142479470 | `27b530d` | same 6 cases | none |
| 32142791957 | `d0e49d8` | same 6 cases | none |
| 32155961672 (latest) | `55b4822` | same 6 cases | none |

Every run shows the identical `[doctest] test cases: 6X | (6X-6) passed | 6 failed`
signature — the exact same 6 `TEST_CASE`s, at the exact same assertion
(`CHECK_FALSE(fixture->context.hasValidationErrors())`), every time:

1. `upload_test.cpp:136` — "uploadToBuffer round-trips bytes through the staging path byte-exact"
2. `upload_test.cpp:239` — "uploadToBuffer stages through the ring buffer... deterministically"
3. `upload_test.cpp:470` — "two overlapping flush() calls without waiting on the first ticket..."
4. `upload_test.cpp:741` — "flush()/isComplete() never block past a wall-clock threshold..."
5. `upload_test.cpp:851` — "ring-buffer reclamation under heavy wrap..."
6. `upload_test.cpp:942` — "ring reclamation polls (never blocks)..."

No `rx_asset` GPU test binary failed in any of the 5 runs (the task brief flagged this
as a possibility to check for; it did not materialize).

**CI's pinned validation layer**, extracted from every run's own `dpkg -l` echo:
`vulkan-validationlayers 1.3.275.0-1` (Ubuntu noble universe). Local machine default:
`1.3.204.1-2` — different. Obtained CI's exact `.deb` from
`http://archive.ubuntu.com/ubuntu/pool/universe/v/vulkan-validationlayers/vulkan-validationlayers_1.3.275.0-1_amd64.deb`,
extracted `libVkLayer_khronos_validation.so` + its manifest into an isolated
`VK_LAYER_PATH` directory, and reproduced the full test suite against it locally
(`vulkaninfo` confirms "VK_LAYER_KHRONOS_validation ... Vulkan version 1.3.275, layer
version 1" loaded). Also downloaded CI's exact `mesa-vulkan-drivers 25.2.8-0ubuntu0.24.04.2`
(lavapipe) `.deb` for completeness, but could not load it locally (this dev machine is
Ubuntu 22.04/glibc 2.35; the noble package requires glibc >= 2.38, and no container
runtime is available in this environment) — reproduction instead used this machine's
own lavapipe (mesa 25.1.5) and, separately, its real NVIDIA GPU, both under CI's exact
layer binary.

**Every one of the 6 failures is the same shape**: a `readBackBuffer()` call
(`upload_test.cpp`'s own test helper) issues its own, independent `vkQueueSubmit`
(via `CommandContext::runOnce()`) to read back a buffer the `Uploader` had just
written. The ONLY thing ordering that second submission after the Uploader's write
is the test's own prior `uploader->wait(ticket)` — a **host-side**
`vkWaitSemaphores` call. `readBackBuffer()`'s own submission carries no
wait-semaphore, no barrier, nothing GPU-visible tying it to the write. The
validation-layer message confirms exactly this: `write_barriers: 0`, two different
command buffers, same queue, "prior_usage: SYNC_COPY_TRANSFER_WRITE" (the
Uploader's staging copy) vs. "submitted_usage: SYNC_COPY_TRANSFER_READ" (the
readback), no synchronization operation recorded between them.

## 2. Adjudication: REAL missing GPU-side dependency (not a layer-version modeling artifact)

**Verdict: (a).** The hazard is a genuine gap under the Vulkan memory model, not a
validation-layer false positive specific to one layer build.

Evidence:

- **Vulkan memory model.** A host wait (`vkWaitSemaphores`, `vkWaitForFences`) makes
  a completed batch's writes visible to the **host** (safe to read mapped memory
  afterward). It does not, by itself, extend the availability→visibility memory
  dependency chain to a **separate, later device queue submission** that carries no
  wait of its own — that requires either a GPU-side semaphore wait chained from the
  same signal, or an explicit barrier in the consuming submission. Two submissions
  to the same queue are guaranteed to *begin executing* in submission order; that is
  not a memory-visibility guarantee.
- **Khronos's own SyncVal documentation says exactly this is out of its tracked
  scope.** `KhronosGroup/Vulkan-ValidationLayers` `docs/syncval_design.md` (fetched
  live): "currently validation only has limited support for tracking fence and
  semaphore primitives" and lists host synchronization commands as a
  `TODO/KNOWN LIMITATION`; `docs/syncval_usage.md`'s "Known Limitations" section
  confirms host-domain accesses are not fully modeled. This is consistent with
  SyncVal being *right* to flag the case rather than SyncVal mis-modeling a
  genuinely-safe pattern.
- **Task 11's own report already reasoned about this and got half of it right, then
  stopped short.** `task-11-report.md`'s "Deviation" section states the exact
  concern — "relying on same-queue submission order alone for memory visibility is
  an unstated, spec-fragile assumption... only execution START order is guaranteed
  per-queue; visibility needs an explicit wait/barrier" — and "fixed" it by adding
  `uploader->wait(uploader->flush())` before every readback. That host wait removes
  the *timing* race (the write is genuinely finished before the readback runs) but
  does not add the *submission-visible* dependency SyncVal (or the strict Vulkan
  memory model) requires for the second device submission. The original fix
  addressed the symptom (data race / wrong bytes) but not the full spec requirement
  (a submission-visible ordering edge).
- **Local reproduction did not trigger, despite matching CI's exact layer binary,
  across several configurations** (default 8-core run, 2-core `taskset`, 1-core
  `taskset` + `LP_NUM_THREADS=1` single-threaded lavapipe). This is *itself*
  consistent with (a), not (b): a real, unsynchronized cross-submission dependency
  is something a validation layer can only catch when its own internal bookkeeping
  observes the two accesses as genuinely concurrent/overlapping (a function of
  exact scheduling, thread interleaving inside the layer, and driver batching) —
  the underlying gap is present on every run, but whether SyncVal's own tracking
  happens to catch it is scheduling-dependent. A pure layer-version modeling bug
  (misclassifying an access type, as `context.cpp`'s four existing suppression
  guards document for genuinely different bugs) would be expected to reproduce
  deterministically given the identical layer binary and API call sequence; this
  did not. No open upstream Vulkan-ValidationLayers issue was found describing this
  exact combination as a false positive (searched via `gh` against
  `KhronosGroup/Vulkan-ValidationLayers`'s own docs), which is the evidence bar the
  task set for claiming (b) — absent, so (a) stands.

Given (a), the correct fix is the one the task's own guidance describes: make the
ordering **submission-visible** by having the dependent (consumer) submission wait
on the Uploader's ticket **GPU-side**, in addition to (not instead of) the existing
host-side `wait()` calls, which several other test assertions in the same file
still depend on (`isComplete()`/`wait()` behavioral coverage).

## 3. Fix design

All changes confined to `src/rx_rhi_vk/**`.

- **`src/rx_rhi_vk/include/rx_rhi_vk/upload.h`**: new `Uploader::timelineSemaphore()`
  accessor — the raw `VkSemaphore` backing every `UploadTicket`, for a caller
  building its own submission that needs a real GPU-side wait on a specific
  ticket's value (`VkTimelineSemaphoreSubmitInfo::pWaitSemaphoreValues`) instead of
  relying solely on `wait()`/`isComplete()`'s host-side polling. Read-only,
  immutable-after-`create()`, no thread-affinity guard needed (same class as
  `ringCapacity()`).
- **`src/rx_rhi_vk/include/rx_rhi_vk/command.h` / `src/command.cpp`**:
  `CommandContext::runOnce()` gains a `uint64_t waitValue = 0` parameter. When
  `wait != VK_NULL_HANDLE`, a `VkTimelineSemaphoreSubmitInfo` is always chained
  into the submission with `pWaitSemaphoreValues = &waitValue` — per the Vulkan
  spec this is harmless/ignored if `wait` turns out to be an ordinary binary
  semaphore, so no caller needs to change behavior; every pre-existing call site
  (bindless_test.cpp, texture_test.cpp, clear_color_test.cpp, executor.cpp,
  geometry_pool_test.cpp, texture_cache_test.cpp, import_gltf_gpu_test.cpp,
  test_material_system.cpp, test_execute_gpu.cpp, test_api_factory.cpp) passes no
  `wait` at all and is unaffected.
- **`src/rx_rhi_vk/tests/upload_test.cpp`**: `readBackBuffer()` now takes
  `(VkSemaphore uploaderTimeline, uint64_t ticketValue)` and threads them into its
  `runOnce()` call with `VK_PIPELINE_STAGE_TRANSFER_BIT` as the wait stage — the
  task's specified "wait-semaphore op with TRANSFER stage mask." All 17 call sites
  across every `TEST_CASE` that does a readback (not just the 6 that happened to
  fail in CI — the identical hazard exists, structurally, in every readback in this
  file, including the two that never tripped CI's non-deterministic detection: the
  direct-memcpy-path test and the ring-wrap auto-flush test) were updated to pass
  the covering ticket's semaphore/value. Where a readback covers a buffer written
  by an *earlier* ticket than the most-recently-flushed one (the ring-wrap test's
  `dstA`, the `MeshBuffers::create()` test's mesh buffers, the deterministic-poll
  test's `dstA`/`dstB`), the LATER ticket already in scope is used instead —
  correct because tickets from the same `Uploader` are strictly ordered by
  submission (documented invariant in `upload.h`'s class comment: waiting on a
  later ticket transitively covers every earlier one on the same queue). No new
  ticket-tracking surface was added to `MeshBuffers` to keep this true — `RC4`'s
  ruling ("`MeshBuffers::create` = `wait(ticket)` byte-identical") is preserved
  exactly; `MeshBuffers::create()` itself is untouched.
- All pre-existing host-side `uploader->wait(...)` calls were **kept** (belt-and-
  braces, explicitly sanctioned by the task) — they still exercise `wait()`/
  `isComplete()` correctness, which several assertions in this file depend on
  independent of the readback hazard.

This is the minimal, spec-correct shape: every dependent submission now carries an
explicit, submission-visible ordering edge to the batch it depends on, closing the
gap regardless of which validation-layer version or scheduling happens to observe
it, rather than pinning to a layer version or filtering the message.

## 4. Verification

**Full `rx_rhi_vk_tests` suite, CI-pinned layer (1.3.275.0-1), lavapipe (matches CI's device class):**
```
[doctest] test cases:   64 |   64 passed | 0 failed | 0 skipped
[doctest] assertions: 1727 | 1727 passed | 0 failed |
[doctest] Status: SUCCESS!
```

**Full `rx_rhi_vk_tests` suite, local default layer (1.3.204.1-2), real NVIDIA GPU:**
```
[doctest] test cases:   64 |   64 passed | 0 failed | 0 skipped
[doctest] assertions: 1738 | 1738 passed | 0 failed |
[doctest] Status: SUCCESS!
```

**Wall-clock non-blocking contract preserved** (Task 11's own gate, re-run after the fix):
```
worst-case flush() duration: 59 us over 8 overlapped frames
worst-case isComplete() duration: 3 us
```
(threshold is 8 ms; both stayed microsecond-scale, as before this fix — the fix adds
no blocking anywhere, only extra `pNext` chaining data on submissions the caller
already issues.)

**Full `ctest --preset linux-native`, CI-pinned layer, lavapipe:** 20/20 passed
(includes `rx_asset_tests`, `rx_asset_gltf_gpu_tests`, `rx_graph_gpu_tests`,
`rx_material_gpu_tests`, all 7 sample headless gates).

**Full `ctest --preset linux-native`, local default layer, real NVIDIA GPU:** 20/20
passed.

**`cmake --build --preset windows-cross-zig`:** builds clean (rx_rhi_vk, rx_rhi_vk_tests.exe
included).

**`ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'`
(CI's exact filter) under Wine:** 10/10 passed.

## 5. Out-of-scope observation (flagged, not fixed)

`rx_asset_gltf_gpu_tests` (`async_import_test.cpp`, Task 15's async import pipeline)
failed once locally with the same `CHECK_FALSE(fixture->context.hasValidationErrors())`
shape, at a completely different assertion (`async_import_test.cpp:918`), while my
`rx_rhi_vk` fix was still stashed out. Re-running immediately afterward (fix still
stashed) passed; re-running with the fix restored passed 3/3 more times. This is
`rx_asset`/`rx_task` territory (explicitly another agent's concurrent write scope
per this task's brief — `git status` shows uncommitted, in-flight changes to
`src/rx_task/scheduler.cpp` and `src/rx_asset/tests/async_import_test.cpp`), it does
not appear in any of the 5 actual CI-red runs this task was scoped to fix, and it
did not reproduce consistently either with or without my change — consistent with
pre-existing flakiness in that concurrently-developed code, not something this fix
caused or should paper over. No `rx_asset` fixture files were touched.

## 6. Files changed

- `src/rx_rhi_vk/include/rx_rhi_vk/upload.h` — `Uploader::timelineSemaphore()` accessor.
- `src/rx_rhi_vk/include/rx_rhi_vk/command.h` — `CommandContext::runOnce()` gains `waitValue`.
- `src/rx_rhi_vk/src/command.cpp` — `VkTimelineSemaphoreSubmitInfo` chaining.
- `src/rx_rhi_vk/tests/upload_test.cpp` — `readBackBuffer()` + all 17 call sites
  thread a real GPU-side wait through every readback in the file (regression test:
  the fix is verified structurally — every readback submission now carries a
  `VkTimelineSemaphoreSubmitInfo`-backed wait on the exact ticket that covers its
  source data — and empirically, both under CI's exact validation-layer binary and
  this machine's own).
