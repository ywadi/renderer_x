# Task 10 report — Memory budget, accounting & eviction-invariant foundation (card #27)

Implementer: Sonnet, main checkout, base `bc879c9`.

## Summary

Implemented category-attributed GPU memory accounting, opportunistic
`VK_EXT_memory_budget` enablement with a heap-size fallback, the
`vmaSetCurrentFrameIndex()` staleness fix, a host-facing `RxMemoryReport`
POD, named/report-attached `VK_ERROR_OUT_OF_DEVICE_MEMORY` handling at all
5 enumerated allocation sites, Tracy plot publishing, teardown leak/balance
logging, and the eviction-invariant CONTRACT text + a synthetic
evict→fallback→reclaim wiring test — all as scoped by the plan's Task 10,
D24, matrix-issue27, and the 2026-08-18 rulings.

## Files

New:
- `src/rx_rhi_vk/include/rx_rhi_vk/memory_report.h` — `MemoryCategory`,
  `MemoryCategoryStats`, `MemoryHeapReport`, `RxMemoryReport`,
  `MemoryBudgetSource`, `MemoryAccounting` (the ledger),
  `AllocationFailureKind` + `classifyAllocationFailure()`, and the
  EVICTION CONTRACT doc-comment (clauses 1-3/(b)/(c)).
- `src/rx_rhi_vk/src/memory_report.cpp` — implementation.
- `src/rx_rhi_vk/tests/memory_report_test.cpp` — rows 1/4/10/16.
- `src/rx_rhi_vk/tests/memory_budget_test.cpp` — rows 2/3.
- `src/rx_rhi_vk/tests/oom_handling_test.cpp` — rows 5-9.
- `src/rx_rhi_vk/tests/eviction_contract_test.cpp` — rows 13-15.

Modified:
- `src/rx_rhi_vk/include/rx_rhi_vk/buffer.h` / `src/buffer.cpp` —
  `Allocator::create()`/`createRaw()` gain `memoryBudgetExtensionEnabled`
  (lockstep with `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT`) and
  `forcedHeapSizeLimitBytes` (test-only OOM forcing via
  `VmaAllocatorCreateInfo::pHeapSizeLimit`); the three buffer factories
  gain a `MemoryCategory category` parameter (defaulted, non-breaking);
  `Allocator` gains `report()`, `setCurrentFrameIndex()`,
  `lastAllocationFailureKind()`/`lastAllocationFailureReport()`,
  `noteAllocationFailure()`/`noteNonMemoryFailure()`, `accounting()`;
  `Buffer` gains `category()`/`allocatedBytes()` + ledger
  record/release wiring; teardown leak check in `Allocator::destroyAll()`.
- `src/rx_rhi_vk/include/rx_rhi_vk/texture.h` / `src/texture.cpp` —
  `Texture2D::create()` gains `category`; vmaCreateImage failures go
  through `noteAllocationFailure()` (memory-classified), the *separate*
  vkCreateImageView failure goes through `noteNonMemoryFailure()`
  (deliberately NOT memory-classified) — the two failure classes stay
  distinguishable per matrix row 9.
- `src/rx_rhi_vk/include/rx_rhi_vk/device.h` / `src/device.cpp` —
  opportunistic `VK_EXT_memory_budget` enable
  (`memoryBudgetExtensionEnabled()`), logged either way, mirroring the
  existing `calibratedTimestampsEnabled` pattern.
- `src/rx_rhi_vk/include/rx_rhi_vk/frame_sync.h` / `src/frame_sync.cpp` —
  `advanceFrame(Allocator* = nullptr)`: when passed an `Allocator*`, calls
  `setCurrentFrameIndex()` — the wired, once-per-frame call site the gate
  ruling requires. Every existing zero-arg call site is unaffected.
- `src/rx_rhi_vk/src/upload.cpp` — `createUploadRingBuffer()` call now
  passes `MemoryCategory::Staging` explicitly.
- `src/rx_rhi_vk/CMakeLists.txt` — new sources/tests registered.
- `docs/threading.md` — new `rx::rhi::Allocator` entry
  (`setCurrentFrameIndex()`/`report()`).

## Per matrix row: how satisfied + which test proves it

| Row | What | Proof |
|---|---|---|
| 1 | Category ledger balance, interleaved create/destroy | `memory_report_test.cpp`: `"MemoryAccounting balances bytes/counts..."` (device-free) + the integration test's real Buffer/Texture2D create+destroy round-trip |
| 2 | `VK_EXT_memory_budget` enablement + fallback, both tested | `memory_budget_test.cpp`: enabled-path test (RealExtension, nonzero budget) + fallback-path test (HeapSizeFallback, `budgetBytes == VkMemoryHeap::size` exactly) |
| 3 | `vmaSetCurrentFrameIndex` staleness/refresh | `memory_budget_test.cpp`'s staleness test — see **Technical finding** below for the precise mechanism verified |
| 4 | `RxMemoryReport` every field | `memory_report_test.cpp`: `"RxMemoryReport: every field..."` |
| 5 | `vmaCreateAllocator` OOM | `oom_handling_test.cpp`: `classifyAllocationFailure`/`allocationFailureKindName` device-free tests — see **Deviation** below |
| 6 | `createHostVisibleBuffer` OOM | `oom_handling_test.cpp`, forced via `pHeapSizeLimit` |
| 7 | `createDeviceLocalBuffer` OOM | same file, also asserts the ledger stayed at 0 (failed alloc never recorded) |
| 8 | `createUploadRingBuffer` OOM | same file |
| 9 | `Texture2D::create` OOM, vmaCreateImage vs vkCreateImageView distinguishable | same file: real forced vmaCreateImage OOM test + a state-isolation test proving `noteNonMemoryFailure()` never touches the memory-failure state `noteAllocationFailure()` set |
| 10 | Composition contract (single choke point → correct attribution) | `memory_report_test.cpp`'s integration test tags GeometryPool/Texture/Transient categories from three separate call shapes (real Buffer, real Texture2D, a Transient-tagged host-visible buffer standing in for a TransientPool-shaped caller) and reads them all back correctly in one `report()` |
| 11 | Never assume heap count | `RxMemoryReport::heaps` is a `std::vector`, sized from `VkPhysicalDeviceMemoryProperties::memoryHeapCount` at every `report()` call; verified for real against llvmpipe (reports exactly 1 heap on this CI-equivalent driver) during test runs |
| 12 | `VK_AMD_memory_overallocation_behavior` → registry | Not implemented (correctly, per ruling: "→ registry (streaming)"). Not registered by me — `.superpowers` ledger/registry files are out of my write scope per the brief; flagging here for the coordinator to record |
| 13-15 | Eviction contract (residency-tolerant resolve, handle-mediated refs, deferred reclaim) | CONTRACT TEXT: `memory_report.h`'s "EVICTION CONTRACT" section. Test: `eviction_contract_test.cpp`, two cases, over a real `rx::core::HandlePool<Tag,T>` + real `rx::rhi::DeletionQueue` |
| 16 | Teardown leak/balance | `Allocator::destroyAll()` logs `RX_LOG_ERROR` if `accounting_->hasOutstanding()`; `MemoryAccounting::hasOutstanding()` tested device-free; integration test proves the ledger genuinely returns to all-zero after real teardown |
| 17 | Tracy plots | `publishTracyPlots()` (buffer.cpp, anonymous namespace) called from every `report()`; 5 category plots + up to 8 heaps × 2 (usage/budget). **MANUAL_VERIFICATION**: not screenshotted in this run (no interactive Tracy client available to this agent) — same honest disclosure precedent as Task 3/17's own report |

## Technical finding beyond the matrix's own wording (row 3)

Before writing the staleness test I read VMA 3.4.0's `GetHeapBudgets()`
implementation directly (`vk_mem_alloc.h:14782-14834`) and found the
matrix's proposed test ("budget values visibly update... vs. stale values
proven when the frame-index call is skipped", read as "usage reflects a
new allocation only after refresh") does not hold for the `usage` field:
VMA computes `usage = frozen_driver_usage_at_last_fetch +
(live_own_blockBytes - own_blockBytes_at_last_fetch)`, and
`blockBytes` is updated immediately on every `vmaCreateBuffer`/
`vmaDestroyBuffer` (`VmaCurrentBudgetData::AddAllocation`/
`RemoveAllocation`, independent of the 30-op/`setCurrentFrameIndex` gate).
So `usage` **always** reflects this allocator's own local allocations
immediately, stale or not — a test asserting otherwise would either be
vacuously true for the wrong reason or flaky. `budget`, by contrast,
applies no such local correction (`VMA_MIN(m_VulkanBudget[heapIndex],
heapSize)`, touched only by `UpdateVulkanBudget()`) and is the field that
genuinely demonstrates staleness. The shipped test exercises `budget`
directly and deterministically (byte-identical across real local
create+destroy activity absent a refresh call, then repeats the same
check relative to a just-refreshed baseline to prove the 30-op counter
really resets on `setCurrentFrameIndex()`, not just once by luck) —
fully driver-independent, no dependency on real system memory pressure
changing between two calls (which would make an `==`/`!=` comparison
against a second live driver query flaky on a real desktop GPU shared
with a compositor).

## Deviation: row 5 (`vmaCreateAllocator` OOM) not forced for real

Verified directly against the vendored VMA source that its constructor
runs `VMA_ASSERT(pCreateInfo->physicalDevice && pCreateInfo->device &&
pCreateInfo->instance)` — forcing a failure via null/invalid handles would
`abort()` the entire test binary (VMA's default `VMA_ASSERT` is a real
`assert()`, not a soft check), not fail one test case gracefully. The
matrix itself marks this row "Lower priority than rows 6-9". Implemented:
a named/classified log message at that call site (unchanged behavior:
still `std::nullopt`, still logged, now with `classifyAllocationFailure()`
naming the kind) plus a device-free test of that classification function
itself. Rows 6-9 (the real per-frame/per-asset OOM path D24(d) is actually
about) are fully exercised via forced failures.

## Scope decision: category defaults, not required parameters

`MemoryCategory` is a defaulted parameter (`Internal`) on all 4 factory
methods, not a required one. The plan's own Task 10 file list is narrow
(`buffer.cpp`, `device.cpp`, new `memory_report.h`+impl); GeometryPool
(Task 12) and TextureCache (Task 14) — the two subsystems matrix row 10
names as the real GeometryPool/Texture taggers — don't exist yet.
`TransientPool`/`MeshBuffers`/`ParamArena`/samples were left uncategorized
(default `Internal`) rather than retrofitted, since none of those files
are in this task's scope and the matrix explicitly allows a mock/synthetic
caller to prove the composition contract (row 10) instead of requiring
every existing call site rewritten. The one real, in-scope call site this
task owns (`Uploader`'s staging ring buffer, `upload.cpp`) is explicitly
tagged `MemoryCategory::Staging`.

## Test evidence

```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json SDL_VIDEODRIVER=x11 ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests
...
[doctest] test cases:  51 |  51 passed | 0 failed | 0 skipped
[doctest] assertions: 976 | 976 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Zero un-filtered `[vulkan validation]` lines (84/84 matched this
codebase's existing documented false-positive guards; every
`CHECK_FALSE(context.hasValidationErrors())` in the new tests passed) —
confirmed by grep over the full run's captured log, not just the
assertion count.

Sample OOM log lines from the new tests (confirms "loud, named,
report-attached", and the two Texture2D failure classes staying
distinguishable):

```
[error] Allocator::createHostVisibleBuffer failed: VkResult=-2 (OutOfDeviceMemory) category=Internal -- memory report at failure: categories={GeometryPool=0/0,...} heaps={#0:usage=0/budget=4096(of 4096)} budgetSource=HeapSizeFallback
[error] Allocator::createDeviceLocalBuffer failed: VkResult=-2 (OutOfDeviceMemory) category=GeometryPool -- ...
[error] Allocator::createUploadRingBuffer failed: VkResult=-2 (OutOfDeviceMemory) category=Staging -- ...
[error] Texture2D::create(vmaCreateImage) failed: VkResult=-2 (OutOfDeviceMemory) category=Texture -- ...
[error] test-seed failed: VkResult=-2 (OutOfDeviceMemory) category=Texture -- ...
[error] Texture2D::create(vkCreateImageView) failed: VkResult=-11 -- NOT a memory-budget event (device/API-misuse failure class); memory accounting unaffected
```

No spurious `"Allocator::destroyAll: torn down with outstanding..."` or
`DeletionQueue`-style leak lines anywhere in the run (confirmed via grep)
— every ledger genuinely returned to zero.

```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a ctest --preset linux-native
...
100% tests passed, 0 tests failed out of 17
Total Test time (real) =  25.41 sec
```

```
$ cmake --build build/windows-cross-zig
... (53/53, no errors)

$ xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'
...
100% tests passed, 0 tests failed out of 7
Total Test time (real) =  62.13 sec
```

(`rx_rhi_vk_tests`/GPU-backed targets are excluded from ctest under
`windows-cross-zig` in this project's own CI, same as every prior task —
Wine has no real Vulkan device; the full binary still cross-compiles and
links cleanly, verified above.)

Compiler warnings: re-touched every modified/new `rx_rhi_vk` source and
rebuilt both the library and `rx_rhi_vk_tests` targets from clean object
files — zero warnings emitted (grepped for `warning`/`error` in the
build log).

## Self-review findings

- Confirmed `Buffer`/`Texture2D` move-assignment operators correctly
  move `accounting_`/`category_`/`allocatedBytes_` and null out the
  moved-from object's copy (a moved-from object's ledger release is a
  no-op, guarded by `buffer_`/`image_ == VK_NULL_HANDLE`).
- Confirmed `Allocator`'s move-assignment carries the new
  `physicalDevice_`/`budgetSource_`/`accounting_`/`lastFailureKind_`/
  `lastFailureReport_` fields, not just `allocator_` (the pre-Task-10
  baseline only had `allocator_` to move) — a move that dropped the new
  fields would have left `report()`/failure-state silently stale after
  any `Allocator` move, so this was checked deliberately, not assumed.
- No AI attribution anywhere in new/modified files (grepped
  case-insensitively for `claude|anthropic|\bai\b|co-authored|generated
  with` across every touched file — zero matches).
- `.superpowers/sdd/.../progress.md` and `task-10-brief.md` are NOT
  included in this commit (pre-existing/coordinator-owned dispatch
  records, out of my write scope per the brief's "do NOT touch ... the
  ledger files").
- Registry item flagged, not filed: `VK_AMD_memory_overallocation_behavior`
  + `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT` per the ruling's "→ registry
  (streaming)" — I have no write access to the master registry document;
  the coordinator should record this from this report.

## Concerns for the reviewer

- The row-3 staleness test's technical basis (usage self-corrects locally,
  budget does not) is a first-hand finding against the vendored VMA
  source, not something I could cross-check against a second source —
  worth an independent read of `vk_mem_alloc.h:14782-14834` /
  `15036-15095` during review.
- `FrameSync::advanceFrame(Allocator*)` is wired and tested at the
  mechanism level, but no sample's real frame loop was updated to pass its
  `Allocator*` in (out of this task's file-list scope, per the plan) — the
  per-frame budget refresh is available and default-safe, not yet
  exercised by a running sample. A one-line follow-up when a sample
  adopts `Allocator::report()` for its HUD.

## Fix round 1 (independent review response)

Commit `f210e73`, on top of `9bd5be2`. All three findings addressed.

### C1 (Critical, BLOCKING) — row-3 staleness test did not discriminate

Root cause confirmed exactly as the reviewer described: every assertion in
the original `"vmaSetCurrentFrameIndex staleness regression"` test compared
`report()`'s own `budget`/`usage` numbers before/after a refresh, and on a
quiet driver (lavapipe, and presumably the reviewer's own environment) the
real value never moves either way, so `CHECK(stale == before)` and the
"proves the reset" second round both pass whether or not
`vmaSetCurrentFrameIndex` actually fires.

**Fix:** added `Allocator::setCurrentFrameIndexCallCount()`
(`buffer.h`/`buffer.cpp`) — a test-only diagnostic counter (same carve-out
convention as `DeletionQueue::pendingCount()`), incremented unconditionally
at the top of `setCurrentFrameIndex()`, before the real
`vmaSetCurrentFrameIndex()` call. New test case in `memory_budget_test.cpp`,
`"FrameSync::advanceFrame(Allocator*) wiring..."`: drives `FrameSync::
advanceFrame(&*allocator)` through 5 frames — the REAL production call
chain (`FrameSync::advanceFrame(Allocator*)` → `Allocator::
setCurrentFrameIndex()` → `vmaSetCurrentFrameIndex()`), not a shortcut
calling the leaf method directly — and asserts the counter reads exactly
5, plus that a subsequent no-argument `advanceFrame()` call (matching
every pre-Task-10 call site) leaves it unchanged. The original
budget-equality test was kept (its own comment now explicitly says it is
NOT discriminating on its own, for the documentation/evidence value it
still carries) but is no longer the only row-3 coverage.

**Discrimination proof (mandatory), scratch worktree, main checkout
untouched:**

```
$ git worktree add --detach /media/ywadi/second/renderer_x_revert_check_wt HEAD
HEAD is now at f210e73 fix: memory-budget review fix round 1 ...
```

Hardlink-copied `.deps-cache/`, `third_party/slang-prebuilt/`, and
`toolchain/zig/` from the main checkout into the worktree (both on the
same filesystem/device, `cp -al`) purely to avoid re-fetching/re-building
~1.1 GiB of already-built third-party dependencies from scratch — zero
source files were touched by that copy.

Baseline (unmodified worktree, real wiring) — passes:

```
$ cmake --preset linux-native && cmake --build build/linux-native --target rx_rhi_vk_tests
... (43/43, clean)
$ VK_ICD_FILENAMES=.../lvp_icd.json SDL_VIDEODRIVER=x11 ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests \
    --test-case="*FrameSync::advanceFrame(Allocator*) wiring*"
[doctest] test cases: 1 | 1 passed | 0 failed | 51 skipped
[doctest] assertions: 8 | 8 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Reverted `Allocator::setCurrentFrameIndex()` (worktree copy only, never
committed) to:

```cpp
void Allocator::setCurrentFrameIndex(uint32_t frameIndex) {
    // REVERT-CHECK ONLY: a no-op that never calls vmaSetCurrentFrameIndex
    // (and never increments the counter either).
    (void)frameIndex;
}
```

Rebuilt (incremental, `buffer.cpp` only) and re-ran the SAME test —
**fails**:

```
$ cmake --build build/linux-native --target rx_rhi_vk_tests
[1/3] Building CXX object .../buffer.cpp.o
[2/3] Linking CXX static library src/rx_rhi_vk/librx_rhi_vk.a
[3/3] Linking CXX executable src/rx_rhi_vk/rx_rhi_vk_tests
$ VK_ICD_FILENAMES=.../lvp_icd.json SDL_VIDEODRIVER=x11 ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests \
    --test-case="*FrameSync::advanceFrame(Allocator*) wiring*"
...
TEST CASE:  FrameSync::advanceFrame(Allocator*) wiring: Allocator::setCurrentFrameIndex() fires exactly once per advanced frame -- the mandatory discriminating regression guard [fix round 1, reviewer C1]

memory_budget_test.cpp:303: ERROR: CHECK( allocator->setCurrentFrameIndexCallCount() == static_cast<uint64_t>(kFrames) ) is NOT correct!
  values: CHECK( 0 == 5 )

memory_budget_test.cpp:309: ERROR: CHECK( allocator->setCurrentFrameIndexCallCount() == static_cast<uint64_t>(kFrames) ) is NOT correct!
  values: CHECK( 0 == 5 )

[doctest] test cases: 1 | 0 passed | 1 failed | 51 skipped
[doctest] assertions: 8 | 6 passed | 2 failed |
[doctest] Status: FAILURE!
```

Full-suite run with the same revert applied confirms the failure is
isolated to exactly this one test case (nothing else regresses/masks it):

```
$ ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests
[doctest] test cases:  52 |  51 passed | 1 failed | 0 skipped
[doctest] assertions: 984 | 982 passed | 2 failed |
[doctest] Status: FAILURE!
$ echo $?   # the binary's own real exit code, captured directly (not a pipe's)
1
```

Worktree removed afterward (`git worktree remove --force`); main checkout
confirmed unmodified throughout (`git status`/`git diff HEAD` both clean
for `src/rx_rhi_vk` before and after) and re-verified green on its own,
real (non-reverted) `buffer.cpp`:

```
$ ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests
[doctest] test cases:  52 |  52 passed | 0 failed | 0 skipped
[doctest] assertions: 984 | 984 passed | 0 failed |
[doctest] Status: SUCCESS!
```

### M1 — false claim about a VMA runtime assert

Reviewer is correct: re-read `vk_mem_alloc.h:13355-13368` directly — the
`VMA_ASSERT` cited sits behind `#if !(VMA_MEMORY_BUDGET) ||
!(VMA_GET_PHYSICAL_DEVICE_PROPERTIES2)`, a COMPILE-TIME macro gate (whether
VMA's own headers detect the extension's symbols at build time), never a
runtime check against whether `VK_EXT_memory_budget` was actually enabled
on the device. Since this project's vendored headers are current, that
macro is always-true here, so the assert can never fire regardless of what
`memoryBudgetExtensionEnabled` is passed. Corrected the three affected
comments (`buffer.h`'s `create()`/`createRaw()` doc comments,
`buffer.cpp`'s `createRaw()` body, `device.h`'s
`memoryBudgetExtensionEnabled()` doc comment) to state plainly that there
is NO VMA-side runtime enforcement of the lockstep at all —
`Device::memoryBudgetExtensionEnabled()` is this engine's own single
source of truth, enforced only by `Allocator::create(Context&, Device&)`
always reading it directly rather than trusting a caller-supplied value —
and to name the real (spec-undefined-behavior /
validation-layer-VUID-class, not assert-class) hazard a mismatched flag
would actually cause.

### M2 — missing D5 thread-affinity one-liners

Added the "Thread-affinity (D5, Phase 4)" convention (matching
`deletion_queue.h:59`/`bindless.h:149`'s exact shape) immediately above
the class declaration in all four files: `buffer.h` (`Buffer` AND
`Allocator` — two classes gained relevant new API, so both got a note),
`texture.h` (`Texture2D`), `device.h` (`Device`), `frame_sync.h`
(`FrameSync`).

### Verification after all three fixes

```
$ cmake --build build/linux-native --target rx_rhi_vk rx_rhi_vk_tests   # clean, zero warnings
$ VK_ICD_FILENAMES=.../lvp_icd.json SDL_VIDEODRIVER=x11 ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests
[doctest] test cases:  52 |  52 passed | 0 failed | 0 skipped
[doctest] assertions: 984 | 984 passed | 0 failed |
[doctest] Status: SUCCESS!

$ VK_ICD_FILENAMES=.../lvp_icd.json xvfb-run -a ctest --preset linux-native
100% tests passed, 0 tests failed out of 17

$ cmake --build build/windows-cross-zig   # full engine, all 51 targets, clean
```

No AI attribution in any fix-round file (grepped
`claude|anthropic|\bai\b|co-authored|generated with` case-insensitively
across every file touched this round — zero matches). Commit `f210e73`
author/committer both `Yousef Wadi <ywadi85@gmail.com>`, unchanged local
git config.

### New concern surfaced by this round

The counter-based discriminator (`setCurrentFrameIndexCallCount()`) proves
`FrameSync::advanceFrame(Allocator*)` → `Allocator::setCurrentFrameIndex()`
→ `vmaSetCurrentFrameIndex()` fires. It cannot by itself distinguish a
revert that guts the WHOLE `setCurrentFrameIndex()` body (what was
reverted above, and what the reviewer's own C1 finding described) from a
hypothetical, more surgical revert that removes only the
`vmaSetCurrentFrameIndex()` line while leaving the counter increment in
place — an inherent limitation of any counter placed as a sibling
statement to the thing it is meant to prove happened, not specific to this
implementation. Flagging this honestly rather than overclaiming the new
test's coverage; closing it fully would need either a build-time
substitution seam or accepting that one-line leaf function as
low-enough-risk to review by inspection (it is a single, direct pass-
through call with nothing else in its body).
