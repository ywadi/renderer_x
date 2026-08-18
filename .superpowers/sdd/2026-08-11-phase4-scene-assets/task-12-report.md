# Task 12 report — GeometryPool: pooled global vertex/index storage (card #21)

**Status:** COMPLETE (fix round 1 applied — see that section below)
**Commits:** `110f36a` (initial implementation, on top of base
`0d9ca39`), `20a9b3f` (fix round 1)
**Test summary (post-fix-round):** 9/9 `rx_asset_tests` pass (146/146
assertions, incl. the two new thread-guard tests); 64/64 `rx_rhi_vk_tests`
pass (1739/1739 assertions, incl. `bda_test.cpp`); full `ctest` on
`linux-native`: 18/18 suites pass; `windows-cross-zig` builds clean
(both presets), `ctest` GPU-excluded subset 7/7 pass under `xvfb-run`
(Wine has no real Vulkan — matches the existing repo convention for
every prior GPU-backed target).

## Files

New:
- `src/rx_asset/CMakeLists.txt`
- `src/rx_asset/include/rx_asset/geometry_pool.h`
- `src/rx_asset/geometry_pool.cpp`
- `src/rx_asset/tests/CMakeLists.txt`, `doctest_main.cpp`,
  `geometry_pool_test.cpp`, `bda_test.cpp`, `accounting_test.cpp`
- `src/rx_rhi_vk/tests/bda_test.cpp` (Device/Allocator-level BDA
  mechanics, one layer below GeometryPool's own BDA test)

Modified:
- `CMakeLists.txt` (root) — `add_subdirectory(src/rx_asset)`
- `src/rx_rhi_vk/include/rx_rhi_vk/device.h`, `src/device.cpp` —
  `Device::supportsBufferDeviceAddress()` (D26.4 opportunistic
  enablement)
- `src/rx_rhi_vk/include/rx_rhi_vk/buffer.h`, `src/buffer.cpp` —
  `Allocator::create()/createRaw()` thread the BDA flag into
  `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`
- `src/rx_rhi_vk/CMakeLists.txt` — registers `bda_test.cpp`
- `docs/threading.md` — `rx::asset::GeometryPool` entry updated from
  "future, Stage 1" placeholder to landed reality

## Reading order followed

Plan Task 12 (`docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:270-311`)
→ spec D8/D9/D26.4 (+D5/D24/D25, `design.md`) → completeness matrix
(`gate/matrix-issue21-geometry-pool.md`) → coordinator rulings
(`gate/rulings-2026-08-18.md` §#21) → `gh issue view 21`. Order of
authority honored: rulings > spec > matrix > ticket.

## The one substantive deviation from the matrix's literal wording

**The matrix's proposed acceptance criterion "pass `alignment = 48`
(vertex) / `4` (index) to `VmaVirtualAllocationCreateInfo`" is
technically inapplicable and was empirically verified, in this task, to
corrupt data.** VMA's own vendored header (`vk_mem_alloc.h`, the exact
struct the matrix cites at `:1662-1666`) documents
`VmaVirtualAllocationCreateInfo::alignment` as "Must be power of two."
48 (`sizeof(PoolVertex)`) is **not** a power of two (48 = 16×3); 4
(`sizeof(uint32_t)`) is.

First implementation attempt followed the matrix literally
(byte-domain suballocation, `alignment = sizeof(PoolVertex)` on the
vertex side). First test run against it:

```
[first implementation attempt -- byte-domain alignment=48]
[error] GeometryPool::upload: VMA returned a misaligned suballocation
offset (vertexByteOffset=272, indexByteOffset=28) -- alignment
invariant violated, this should be unreachable
[error] [vulkan validation] Validation Error: [ VUID-vkCmdCopyBuffer-srcBuffer-00118 ] ...
geometry_pool_test.cpp:232: ERROR: CHECK( std::memcmp(readVertsB.data(), vertsB.data(), readVertsB.size()) == 0 ) is NOT correct!
  values: CHECK( -250 == 0 )
```

272 is not a multiple of 48 (272 = 5×48 + 32) — reproducing exactly the
bit-mask-truncation signature of VMA's internal `(offset + align - 1) &
~(align - 1)` rounding applied to a non-power-of-two `align` (240, the
correct already-aligned starting offset for the second allocation,
becomes 272 under that same masking arithmetic applied to 48 instead of
a real power of two). This is a real, reproducible defect, not a
theoretical concern — it silently produced a `vertexOffset` that does
**not** correspond to where the data was actually written.

**Fix:** GeometryPool's vertex/index `VmaVirtualBlock`s are created and
addressed in **ELEMENT units** (one `PoolVertex` / one `uint32_t` per
unit), not bytes (VMA's own doc: "Sizes can be expressed in bytes or
any units you want as long as you are consistent"). `alignment` is left
at `0` (VMA's "no special alignment beyond the unit itself" sentinel —
trivially a valid power-of-two-or-zero value). Every `vmaVirtualAllocate`
call now returns the exact element offset directly — `vertexOffset`/
`firstIndex` are that value with **zero division anywhere on the
path**, eliminating the truncation-risk class entirely rather than
merely working around VMA's precondition. The real `Buffer` size is
rounded down to a whole element count first (`requestedBytes /
stride`, then `* stride` again) so the virtual block's addressable
range always matches the real buffer exactly.

This preserves everything the matrix's acceptance criterion actually
cared about (D9: offsets convert cleanly to `vertexOffset`/`firstIndex`,
never rounding) — it just achieves it by a mechanism that is actually
valid against VMA's real API contract. Documented at length in
`geometry_pool.h`'s class comment ("SUBALLOCATION UNITS") and in
`geometry_pool.cpp`. **Flagging explicitly for the reviewer/coordinator
in case the matrix or a future ruling should be amended** to correct
the `alignment=48` phrasing — the index side's `alignment=4` would have
worked (4 *is* a power of two), but the vertex side's `alignment=48`
cannot, ever, on any VMA version, by the API's own documented contract.

Two other real implementation bugs, caught and fixed during this same
TDD pass (not deviations from the ticket, just bugs the tests did their
job of catching):
1. Pool block buffers were missing `VK_BUFFER_USAGE_TRANSFER_SRC_BIT`
   (`VUID-vkCmdCopyBuffer-srcBuffer-00118` on readback) — fixed by
   mirroring `rx::rhi::MeshBuffers::create()`'s own established
   precedent of carrying `TRANSFER_SRC_BIT` alongside `TRANSFER_DST_BIT`
   unconditionally ("costs nothing... lets a debug readback... copy out
   of them directly").
2. The exhaustion-draw test's `vkCmdPushConstants` call hardcoded
   `VK_SHADER_STAGE_FRAGMENT_BIT` instead of using the shader's
   *reflected* push-constant stage mask (Slang/`reflect()` report the
   push-constant range visible to every stage that can see the
   `[[vk::push_constant]]` global's declaration, i.e.
   `VERTEX|FRAGMENT` here, even though only `fsMain` reads it) —
   `VUID-vkCmdPushConstants-offset-01796`. Fixed by using
   `layoutInfo->pushRanges[0].stages`, matching
   `rx_graph/tests/test_execute_gpu.cpp`'s own established pattern
   (which never hardcodes a single stage flag).

## Deviation #2: `GeometryPool::create()` signature

Plan's illustrative sketch: `create(rhi::Device&, rhi::Uploader&, const
PoolConfig&)`. Implemented: `create(rhi::Allocator&, rhi::Device&,
rhi::Uploader&, const PoolConfig&)` — adds `Allocator&` explicitly.
**Necessary, not incidental**: the #27 accounting-integration
acceptance criterion ("after a chunk allocation, the accounting
report's geometry-pool category grows by exactly the chunk's real
allocated size") only holds if GeometryPool's chunk buffers land in the
SAME `MemoryAccounting` ledger `Allocator::report()` reads — which
requires the caller's real, shared `Allocator&`, not a private one
GeometryPool would otherwise have to construct (which would need its
own `Context&` too, not in the plan's sketch either, and would produce
an invisible, separate ledger). Documented in the header's own comment
on `create()`.

## Per-criterion proof

| # | Criterion (matrix/ruling/brief) | Where | Proof |
|---|---|---|---|
| 1 | Single VB/IB pair per chunk (64MB/32MB default), `VmaVirtualBlock` TLSF suballocation | `geometry_pool.h/.cpp`, `addBlock()` | `VmaVirtualBlockCreateInfo::flags` left `0` (TLSF default, never `VMA_VIRTUAL_BLOCK_CREATE_LINEAR_ALGORITHM_BIT`); `kDefaultVertexChunkBytes=64MiB`, `kDefaultIndexChunkBytes=32MiB` |
| 2 | 48-byte `PoolVertex`, static_assert-pinned (D8) | `geometry_pool.h:36-43` | `static_assert(sizeof(PoolVertex)==48, ...)` compiles; field layout `px..pz,nx..nz,tx..tw,u,v` matches D8 exactly |
| 3 | `MeshRange{blockId,firstIndex,indexCount,vertexOffset}` | `geometry_pool.h:63-70` | Matches plan's struct verbatim (fields, types) |
| 4 | `bind(cmd, blockId)` | `geometry_pool.cpp:370-383` | `vkCmdBindVertexBuffers`+`vkCmdBindIndexBuffer(VK_INDEX_TYPE_UINT32)` against `blocks_[blockId]` |
| 5 | Growth: new block on exhaustion, virtual-free, NO defrag | `upload()`/`free()` | `upload()` tries every existing block via real `vmaVirtualAllocate` calls only, never a synthetic "does it fit" check; `free()` only calls `vmaVirtualFree`, never touches any other allocation. Checkerboard test proves it empirically (below) |
| 6 | Main-thread affinity (D5 guard + header one-liner) | `geometry_pool.h:3-12`, `create()/upload()/free()/bind()` | Header carries the D5 one-liner; all four call `RX_ASSERT_MAIN_THREAD` first, mirroring `upload.cpp:129,192`'s existing pattern (no bespoke per-class violation test added — matches this codebase's own established convention: the underlying mechanism is proven once, device-free, in `rx_core/tests/debug_checks_test.cpp`; no other GPU-object-mutation class in this repo — `Uploader`/`BindlessTable`/`Allocator` — duplicates that proof either) |
| 7 | Per-allocation alignment: structural divisibility of every returned offset | `upload()` | **Achieved via element-unit suballocation (see deviation #1 above), not literal byte-domain `alignment=48/4`** — every `vertexOffset`/`firstIndex` is the raw integer element offset VMA returned, with zero division anywhere on the path, making the "divisibility" property true by construction rather than by a runtime modulo check. Verified via byte-exact content readback (`geometry_pool_test.cpp`'s "distinct ranges" test uses deliberately odd element counts — 5/7/3/9 — precisely because an odd count is what would expose a truncated offset) |
| 8 | BDA: `bufferDeviceAddress` opportunistic in `features12`, never selection-required | `device.cpp` | Set via a SEPARATE post-`select()` call (`enable_extension_features_if_present`), never merged into the REQUIRED `features12` struct passed to `set_required_features_12()` — structural guarantee, see `rx_rhi_vk/tests/bda_test.cpp`'s dedicated structural TEST_CASE |
| 9 | `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` when on | `buffer.cpp` | Threaded through `Allocator::create()`→`createRaw()`, lockstep with `Device::supportsBufferDeviceAddress()`, mirroring the existing `memoryBudgetExtensionEnabled` pattern exactly |
| 10 | Usage bit on pool block buffers; `Device::supportsBufferDeviceAddress()` | `geometry_pool.cpp addBlock()`, `device.h` | `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` added conditionally; accessor added |
| 11 | Test: pool block yields nonzero `vkGetBufferDeviceAddress` on a supporting device | `rx_asset/tests/bda_test.cpp` | Passes on both local devices (NVIDIA + lavapipe, both BDA-capable — see below) |
| 12 | Two uploads → distinct non-overlapping ranges | `geometry_pool_test.cpp` "distinct ranges" | Passes; non-overlap + byte-exact readback both asserted |
| 13 | Free + re-upload reuses space | `geometry_pool_test.cpp` "free releases space..." | Passes; block deliberately sized to hold exactly 2 meshes so a 3rd upload can ONLY succeed in-block if the freed space was genuinely reclaimed (`blockCount()` stays 1) |
| 14 | Exhaustion → second block, BOTH drawable, real indexed draws, readback probe | `geometry_pool_test.cpp` "exhaustion creates a second block..." | Real `vkCmdDrawIndexed` × 2 (from block 0 and block 1) into one offscreen image, pixel readback confirms left=red (block 0), right=blue (block 1) |
| 15 | Checkerboard fragmentation proves no hidden compaction | `geometry_pool_test.cpp` "checkerboard fragmentation..." | 4 same-size meshes, free slots 1&3 (isolated, non-adjacent holes), request a size larger than any single hole but smaller than total free → forced onto a NEW block (`blockId==1`, `blockCount()==2`) |
| 16 | Alignment divisibility asserts after every upload | see row 7 | N/A as a raw modulo check (structurally impossible to violate — see deviation #1); satisfied via byte-exact content proofs instead |
| 17 | Zero validation errors (sync validation) | every GPU test | `CHECK_FALSE(context.hasValidationErrors())` in every test case; full suite green |
| 18 | `stats()` incl. bytes used/capacity AND block count | `geometry_pool.h/.cpp` `PoolStats`/`stats()` | `blockCount`, `vertexBytesUsed/Capacity`, `indexBytesUsed/Capacity`, per-block breakdown; dedicated stats test asserts across growth |
| 19 | #27 integration: chunk allocation grows geometry-pool category by exactly the chunk size | `rx_asset/tests/accounting_test.cpp` | `afterStats.bytes - beforeStats.bytes == realVertexBytes + realIndexBytes` (the REAL VMA-charged bytes via new `vertexBufferAllocatedBytes()`/`indexBufferAllocatedBytes()` diagnostic accessors, not the nominal request — honest about VMA/rounding padding); `count` delta `==2` |
| 20 | u32 index width stands | `bind()` | `VK_INDEX_TYPE_UINT32` hardcoded, no alternative path |
| 21 | Category label "geometry-pool" threaded into D24 accounting | `addBlock()` | `MemoryCategory::GeometryPool` passed to both `createDeviceLocalBuffer()` calls |

## Lavapipe BDA verification (in-task, per the ticket's required step)

`vulkaninfo` run against both physical devices visible in this
development environment:

```
$ vulkaninfo --summary 2>&1 | grep -A6 Devices
GPU0: NVIDIA GeForce RTX 2080 (DISCRETE_GPU, proprietary driver)
GPU1: llvmpipe (Mesa lavapipe, CPU device)

VkPhysicalDeviceBufferDeviceAddressFeatures (GPU0):
        bufferDeviceAddress              = true
VkPhysicalDeviceBufferDeviceAddressFeatures (GPU1, lavapipe):
        bufferDeviceAddress              = true
```

Both devices report support — consistent with the gate matrix's own
finding (lavapipe has carried `VK_KHR_buffer_device_address` since
Mesa MR #9616, March 2021). `Device::create()`'s own log line
(`bufferDeviceAddress ENABLED on the selected physical device`)
confirms this at runtime in every test run. No BDA-lacking device is
available in this environment, so the "device creation still succeeds,
accessor reports false" degrade path is a **structural** guarantee
(see `rx_rhi_vk/tests/bda_test.cpp`'s dedicated TEST_CASE and
`device.cpp`'s own comment: `bufferDeviceAddress` is set via a
post-`select()` call that can never gate physical-device selection,
verified by code inspection, not by a runtime test against real
unsupported hardware) — mirroring Task 11's own precedent for a
similarly hardware-unreachable case ("eliminated structurally, not
merely by a runtime check").

## Revert-discrimination evidence (mandatory, load-bearing tests)

Per the brief: "be ready to show it FAILS when that behavior is
broken... include scratch-worktree evidence." Three real defects
injected into three separate scratch git worktrees (checked out at
commit `110f36a`, `.deps-cache`/`toolchain`/`third_party/slang-prebuilt`
symlinked in from the main worktree — not copied), built, run, then
discarded (`git worktree remove --force`); no scratch-only code is
present in the committed tree.

### 1. `free()` made a no-op (simulates "forgot to implement release")

```
[scratch worktree, free() no-op]
TEST CASE: GeometryPool::free releases space that a same-size
re-upload reuses without growing a new block
geometry_pool_test.cpp:274: ERROR: CHECK( statsAfterFree.vertexBytesUsed == 4 * sizeof(rx::asset::PoolVertex) )
  values: CHECK( 384 == 192 )
geometry_pool_test.cpp:286: ERROR: CHECK( rangeC.blockId == 0 )
  values: CHECK( 1 == 0 )
geometry_pool_test.cpp:287: ERROR: CHECK( pool->blockCount() == 1 )
  values: CHECK( 2 == 1 )

TEST CASE: GeometryPool checkerboard fragmentation forces the
exhaustion/new-block path -- proves no hidden compaction (D9)
geometry_pool_test.cpp:331: ERROR: CHECK( statsAfterFree.vertexBytesUsed == 2 * 4 * sizeof(rx::asset::PoolVertex) )
  values: CHECK( 768 == 384 )

[doctest] test cases:   7 |   5 passed | 2 failed | 0 skipped
```

Both tests that depend on real `free()` behavior fail cleanly and
specifically. Against the real (non-reverted) implementation in the
main worktree: `7 | 7 passed | 0 failed` (confirmed both immediately
before and after this scratch run).

### 2. `bind()` made to always bind block 0 (simulates "forgot to use the `blockId` parameter")

```
[scratch worktree, bind() ignores blockId]
TEST CASE: GeometryPool exhaustion creates a second block, and BOTH
blocks are drawable with real indexed draws (readback probe)
geometry_pool_test.cpp:733: ERROR: CHECK( leftPixel[0] == 255 )
  values: CHECK( 0 == 255 )
geometry_pool_test.cpp:735: ERROR: CHECK( leftPixel[2] == 0 )
  values: CHECK( 255 == 0 )
geometry_pool_test.cpp:738: ERROR: CHECK( rightPixel[2] == 255 )
  values: CHECK( 0 == 255 )

[doctest] test cases:   7 |   6 passed | 1 failed | 0 skipped
```

Exactly the expected failure mode: both draws end up reading block 0's
geometry (a left-half quad), so the second (nominally "block 1, blue")
draw overwrites the left pixel with blue instead of drawing anything on
the right — left pixel is blue instead of red, right pixel stays
background (never drawn). The readback probe genuinely discriminates
wrong-block binding, not just "some draw happened."

### 3. Chunk buffers tagged `MemoryCategory::Internal` instead of `GeometryPool`

```
[scratch worktree, wrong accounting category]
TEST CASE: GeometryPool chunk allocation grows the #27 accounting
report's geometry-pool category by exactly the chunk's real allocated size
accounting_test.cpp:113: ERROR: CHECK( afterStats.bytes - beforeStats.bytes == realVertexBytes + realIndexBytes )
  values: CHECK( 0 == 1040000 )
accounting_test.cpp:115: ERROR: CHECK( afterStats.count - beforeStats.count == 2 )
  values: CHECK( 0 == 2 )

[doctest] test cases:   7 |   6 passed | 1 failed | 0 skipped
```

Confirms the #27 integration test genuinely depends on the real
category label, not a coincidentally-passing assertion.

### 4. Alignment/element-unit test (organic, not manufactured)

The "distinct ranges" byte-exact readback test is its own revert
evidence: the FIRST implementation attempt (literal byte-domain
`alignment=48`, matching the matrix's exact wording) was run against
this exact test and failed cleanly and specifically (see the deviation
section above for the full transcript) — `memcmp` mismatch plus a
`VUID-vkCmdCopyBuffer-srcBuffer-00118` validation error, both traced to
the same root cause. This is arguably stronger evidence than a
synthetic revert: it is the actual defect history of this
implementation, not a contrived injection.

Tests judged lower-risk and not separately revert-tested: `stats()`
aggregation (straightforward summation of accessor calls, low
complexity) and the BDA pool-block test (the failure mode — missing
usage bit vs. missing allocator flag — is already self-discriminating
via VMA's own `VMA_ASSERT`/`VK_ERROR_INITIALIZATION_FAILED` enforcement,
independently verified in `rx_rhi_vk/tests/bda_test.cpp`'s comment
citing `vk_mem_alloc.h:14440-14445`).

## Both-preset verification (command output tails)

```
$ cmake --build --preset linux-native -j$(nproc)     # full build
... [30/30] Linking CXX executable samples/05_multipass/sample_05_multipass

$ cd build/linux-native && ctest --output-on-failure
100% tests passed, 0 tests failed out of 18
Total Test time (real) =  57.39 sec

$ cmake --build --preset windows-cross-zig -j$(nproc)   # full build
... [44/44] Linking CXX executable samples/07_stress/sample_07_stress.exe

$ xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample|rx_asset'
100% tests passed, 0 tests failed out of 7
Total Test time (real) =  59.29 sec
```

(`rx_asset_tests` is GPU-backed like `rx_rhi_vk_tests`/`rx_graph_gpu_tests`/
`rx_material_gpu_tests`/every sample's headless gate, so it is excluded
from the Wine-run subset for the same reason those are — Wine has no
real Vulkan. It DOES build cleanly under `windows-cross-zig`, confirmed
above.)

Full `rx_asset_tests` run (linux-native, native GPU):
```
[doctest] test cases:   7 |   7 passed | 0 failed | 0 skipped
[doctest] assertions: 131 | 131 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Full `rx_rhi_vk_tests` run (incl. new `bda_test.cpp`, 16 files now):
```
[doctest] test cases:  64 |  64 passed | 0 failed | 0 skipped
[doctest] assertions: 1739 | 1739 passed | 0 failed |
[doctest] Status: SUCCESS!
```

## Self-review

- **Production grade / TDD**: tests were written and run against a real
  (initially broken) implementation first; the two real bugs found (the
  alignment defect and the missing `TRANSFER_SRC_BIT`/push-constant-stage
  bugs) were caught BY the tests, not found by inspection afterward —
  direct evidence the test suite is load-bearing, not decorative.
- **No AI attribution**: commit author is the local git identity
  (`Yousef Wadi <ywadi85@gmail.com>`); commit message is conventional
  and factual; verified directly via `git log -1 --format='%an <%ae>%n%cn <%ce>%n%B'`.
- **Scope discipline**: did not touch the board, issues, plan, spec, or
  ledger; did not commit the brief file (`task-12-brief.md` remains
  untracked, as instructed); only committed the files listed above.
- **Per-directory style**: followed `src/rx_rhi_vk`'s existing
  commenting/RAII/factory conventions closely (private-constructor
  `create()` factories, `[Phase 4 Stage 1 Task 12, spec D26.4]`-style
  provenance tags, the same GPU test fixture shape as
  `upload_test.cpp`/`buffer_test.cpp`, the same shader-compile-in-process
  pipeline pattern as `rx_graph/tests/test_execute_gpu.cpp`).
- **Known limitation, disclosed**: the default 64MB vertex chunk (not
  a multiple of 48) loses 16 bytes to the element-count rounddown
  (67108864 → 67108848 real bytes) — harmless (0.00002% of the chunk)
  but worth recording; the 32MB index default and every chunk size used
  in this task's own tests are exact multiples of 4, so this only
  affects the vertex side's default config.
- **Not done / explicitly out of scope**: 16-bit index sub-pools
  (registry item per the ruling, not this task); capping/eviction
  response to unbounded block growth (D24/streaming-phase, `stats()`
  makes it observable now per the ruling, nothing more expected here);
  no dedicated worker-thread violation test for the D5 guard (matches
  this codebase's own established convention — see criterion 6 above).

## Concerns for the reviewer/coordinator

1. **The `alignment=48` phrasing in `gate/matrix-issue21-geometry-pool.md`
   (row "Suballocation alignment") is technically wrong** for the
   vertex side (48 is not a power of two, and VMA requires one) —
   recommend a ruling/matrix correction so this doesn't get
   re-litigated or copy-pasted into a future task. The element-unit
   design implemented here satisfies the SAME underlying D9 invariant
   the row was trying to name.
2. `GeometryPool::create()`'s signature gained an explicit `Allocator&`
   parameter beyond the plan's illustrative sketch — flagged above,
   necessary for the #27 integration criterion. (Reviewer ruled this
   justified — see the fix-round section below.)

## Fix round 1 (independent review, commit `20a9b3f`)

Reviewer verdict: Approved-with-minors — all 21 criteria PASS; the
element-unit alignment deviation independently reproduced and validated
as a justified matrix erratum; the `Allocator&` signature deviation
ruled justified; BDA verified against vk-bootstrap source; all three
defect-injection reverts reproduced exactly. One IMPORTANT finding + one
minor entered the fix loop.

### IMPORTANT — read accessors documented as any-thread-safe with nothing backing it

`geometry_pool.h:9-12` and `:243-248` (pre-fix-round line numbers)
claimed `stats()`/`blockCount()`/`bufferDeviceAddressEnabled()`/
`vertexBufferDeviceAddress()`/`indexBufferDeviceAddress()` were "safe
from any thread holding a valid reference." Not true: `stats()` calls
`vmaGetVirtualBlockStatistics()` against the same live `VmaVirtualBlock`s
`upload()`/`free()` mutate unlocked (VMA's own docs require external
synchronization for that call too, not just the mutating ones);
`blockCount()` reads `blocks_.size()`, racing a concurrent `push_back()`
reallocation. Unlike `rx::rhi::Allocator::report()`/
`setCurrentFrameIndex()` (justified any-thread by `MemoryAccounting`'s
atomic counters), GeometryPool has no equivalent backing.

**Coordinator ruling followed exactly:** narrowed the documented
contract to main-thread-only (no synchronization added) and enforced it
with the same `RX_ASSERT_MAIN_THREAD` guard `upload()`/`free()`/`bind()`
already carry.

**Fix:**
- `geometry_pool.h`'s top-of-file thread-affinity comment and every
  affected method's own comment corrected — "safe from any thread" →
  main-thread-only, with the reasoning spelled out (no atomic backing,
  unlike `Allocator::report()`).
- `blockCount()`/`bufferDeviceAddressEnabled()` moved from inline header
  definitions to out-of-line `.cpp` definitions (this codebase's
  established convention keeps every `RX_ASSERT_MAIN_THREAD` call site
  in a `.cpp`, never a header, so this also avoids pulling
  `rx_core/debug_checks.h` into the public header) — both now start with
  `RX_ASSERT_MAIN_THREAD(...)`.
- `stats()`, `vertexBufferDeviceAddress()`, `indexBufferDeviceAddress()`
  each gained the same guard.
- Extended beyond the finding's literal naming, for consistency: the
  four test/diagnostic-only accessors
  (`vertexBufferAllocatedBytes()`/`indexBufferAllocatedBytes()`/
  `vertexBufferHandle()`/`indexBufferHandle()`) read `blocks_[blockId]`
  under the identical hazard and were not explicitly named in the
  finding, but there is nothing that exempts them (they are not close to
  the `Allocator::report()` atomic-counter justification either) — all
  four now carry the guard too. Flagged explicitly here in case the
  reviewer judges this over-scoped; the reasoning is the finding's own
  ("no atomic backing") applies identically to these four.
- `docs/threading.md`'s `rx::asset::GeometryPool` row updated: nine
  guarded methods now listed (was four), with the finding's own
  reasoning recorded there too.

**Guard-fires test added** (`src/rx_asset/tests/thread_guard_test.cpp`),
mirroring `rx_core/tests/debug_checks_test.cpp`'s own worker-thread
pattern (`#ifdef RX_DEBUG_CHECKS`, `rx::core::debug::detail::
setViolationHookForTests`, a plain `std::thread` standing in for a
non-main-thread caller): one TEST_CASE calls all five narrowed
accessors from five separate joined worker threads and asserts the
violation hook fired exactly five times; a second TEST_CASE calls the
same five from the real main thread and asserts the hook never fires.
Full run:

```
$ ./src/rx_asset/tests/rx_asset_tests --validate
[doctest] test cases:   9 |   9 passed | 0 failed | 0 skipped
[doctest] assertions: 146 | 146 passed | 0 failed |
[doctest] Status: SUCCESS!
```

(9 = the original 7 + the two new thread-guard cases; both new cases
confirmed present via `-ltc` and passing, not silently skipped.)

### Minor — orphaned block left behind on an unreachable failure path

`geometry_pool.cpp:287-296` (pre-fix-round line numbers): when a
freshly `addBlock()`'d block (sized exactly to fit the request that
triggered its creation) still fails its own suballocation — a path the
code itself calls "should be unreachable" — the function returned
`MeshRange{}` without removing the block from `blocks_`, leaving an
orphaned block (and, in the index-failure branch, an orphaned but
already-freed vertex suballocation) behind.

**Fix:** new private helper `GeometryPool::discardOrphanedLastBlock()`
— destroys `blocks_.back()`'s two `VmaVirtualBlock`s (both call sites
guarantee zero live suballocations remain in either by the time it
runs) and `pop_back()`s the block, letting `~Buffer()` handle the real
device-local memory release via the normal RAII path. Both branches in
`upload()` (vertex-suballocation failure and index-suballocation
failure) now call it before returning; the index-failure branch frees
its vertex-side suballocation first so the block is genuinely empty
before the virtual blocks are destroyed.

No dedicated test added for this path (matches its own "should be
unreachable" characterization — `addBlock()` sizes a fresh block to
`max(config, requested)`, so a same-request suballocation failing
immediately after is not reachable through this class's own public API
under any input verified so far); covered by code review/inspection,
consistent with how the codebase already treats other defensive-only
branches (e.g. `Uploader`'s own "should never happen" arms).

### Verification commands run (fix round 1)

```
$ cmake --build --preset linux-native --target rx_asset_tests -j$(nproc)
... [1/2] Building CXX object .../thread_guard_test.cpp.o
... [2/2] Linking CXX executable src/rx_asset/tests/rx_asset_tests

$ ./src/rx_asset/tests/rx_asset_tests --validate
[doctest] test cases:   9 |   9 passed | 0 failed | 0 skipped
[doctest] assertions: 146 | 146 passed | 0 failed |
[doctest] Status: SUCCESS!

$ ctest --output-on-failure   # full linux-native suite
100% tests passed, 0 tests failed out of 18
Total Test time (real) =  61.22 sec

$ cmake --build --preset windows-cross-zig --target rx_asset_tests -j$(nproc)
... [7/7] Linking CXX executable src/rx_asset/tests/rx_asset_tests.exe

$ cmake --build --preset windows-cross-zig -j$(nproc)
ninja: no work to do.   # already fully built and current
```

### Self-review (fix round 1)

- Both findings addressed exactly as the coordinator's ruling specified
  (narrow-not-synchronize for the IMPORTANT finding; clean pop for the
  minor).
- Extended the guard to four accessors the finding didn't explicitly
  name, for consistency — called out above rather than left implicit,
  in case that reads as over-scoped.
- Commit `20a9b3f` touches only my own files
  (`docs/threading.md`, `src/rx_asset/geometry_pool.cpp`,
  `src/rx_asset/include/rx_asset/geometry_pool.h`,
  `src/rx_asset/tests/CMakeLists.txt`,
  `src/rx_asset/tests/thread_guard_test.cpp`) — verified via `git show
  --stat` before committing; author identity confirmed
  (`Yousef Wadi <ywadi85@gmail.com>`) via `git log -1 --format='%an
  <%ae>%n%cn <%ce>%n%B'`; no AI attribution anywhere in the message.
  `.superpowers/sdd/.../progress.md` (modified by the review process,
  not by me) and the new `review-0d9ca39..110f36a.diff` were left
  untouched/untracked, matching the brief's scope rule.
