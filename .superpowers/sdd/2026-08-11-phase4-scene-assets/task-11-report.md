# Task 11 report — Uploader completion tickets: non-blocking flush invariant (card #28)

Implementer: Sonnet, main checkout, base `2e1b55d`.

Commits:
- `37d9466` — `feat: Uploader completion tickets -- non-blocking flush via timeline semaphore`
- `ee01d7d` — `fix: size the wall-clock non-blocking test to actually discriminate`

## Summary

`Uploader::flush()` now returns a pollable `UploadTicket` (timeline-
semaphore value + ring generation) instead of blocking, per spec D25 as
amended by gate ruling RC4. `isComplete(ticket)`/`wait(ticket)` added.
Ring reclamation now keys off ticket completion (D3D12-pattern
poll-reclaim-then-block-only-on-the-oldest-in-flight-ticket), not off
having already blocked. `Device::create()` acquires an optional
dedicated transfer queue via `get_dedicated_queue` (never `require_`),
acquisition only. Every existing `Uploader::flush()` call site in the
tree (samples 03/04/05/06/07, `rx_material`) was audited and migrated —
most to an explicit `wait(ticket)` (preserving byte-identical blocking
behavior, out of this task's primary scope), sample 04's eviction cycle
to genuine `isComplete()` polling (the demonstrated non-blocking
migration target, per plan Task 11's own text and matrix row 11).

A structural hazard not named in the matrix, but necessary to fix for the
redesign to be correct at all, is called out under **Self-review findings**
below: reusing a single command buffer across un-awaited `flush()` calls
is the same class of hazard as the fence-reuse one row 1 names, just for
the command buffer. Fixed with a lazily-grown command-buffer rotation.

## Files

Modified (no new files):
- `src/rx_rhi_vk/include/rx_rhi_vk/upload.h` / `src/upload.cpp` —
  `UploadTicket`; `flush()` returns it instead of `void`;
  `isComplete()`/`wait()` added; one `VK_SEMAPHORE_TYPE_TIMELINE`
  semaphore replaces the single reused `VkFence`; a lazily-grown
  command-buffer rotation (`CmdSlot`) replaces the single reused command
  buffer; `pendingRegions_` FIFO (D3D12-pattern) drives ring reclamation;
  `ringWrapCount()`/`blockingRingWaitCount()`/`ringCapacity()`
  test/diagnostic accessors added.
- `src/rx_rhi_vk/include/rx_rhi_vk/device.h` / `src/device.cpp` —
  `features12.timelineSemaphore = VK_TRUE` (required unconditionally —
  see **Technical finding** below); optional dedicated transfer queue
  acquisition (`hasDedicatedTransferQueue()`/`transferQueue()`/
  `transferQueueFamily()`), logged either way.
- `src/rx_rhi_vk/include/rx_rhi_vk/mesh_buffers.h` / `src/mesh_buffers.cpp`
  — `MeshBuffers::create()` keeps its blocking contract explicitly via
  `wait(flush())` after each of its two uploads.
- `src/rx_material/material_system.cpp` — `MaterialSystem::createTexture2D()`
  (the public, synchronous ABI entry point) keeps its blocking contract
  the same way — see **Deviation: call-site audit widened beyond sample
  04** below for why this was in scope.
- `samples/{03_bindless_mesh,04_streaming,05_multipass,06_materials,
  07_stress}/main.cpp` — every discovered `Uploader::flush()` call site
  migrated (see the same Deviation section).
- `docs/threading.md` — `Uploader` entry: `flush()`/`isComplete()`/`wait()`
  now `[guarded]`.
- `src/rx_rhi_vk/tests/{upload_test.cpp,device_test.cpp,texture_test.cpp}`
  — new tests (below) + existing readback-dependent tests updated to
  `wait(flush())` explicitly (see **Deviation: existing upload_test.cpp/
  texture_test.cpp assertions touched**).

## Per matrix row: how satisfied + which test proves it

| Row | What | Proof |
|---|---|---|
| 1 | Reused-fence hazard eliminated; overlapping flush(), first ticket reports its own true status | `upload_test.cpp`: `"...two overlapping flush() calls without waiting on the first ticket..."` — distinct/monotonic ticket values, `wait(ticketA)` succeeds independent of `ticketB`, `isComplete(ticketA)` stays true after `ticketB` also completes, zero validation errors. Eliminated **structurally** (no reset step exists for a timeline semaphore at all), not just by a runtime check — see below for why this specific row has no scratch-worktree revert (the pre-existing API shape, `void flush()`, cannot host this test at all) |
| 2 | Primitive choice: timeline semaphore | RC4 ruling adopted verbatim; `upload.h`/`upload.cpp` |
| 3 | `vkGetFenceStatus`-equivalent poll semantics (row 3 describes the fence variant; row 4 the chosen timeline variant) | N/A — fence variant not chosen |
| 4 | `vkGetSemaphoreCounterValue` poll primitive, one call, no spin-loop | `Uploader::isComplete()` — one call, `current >= ticket.value` |
| 5 | Ring reclamation, D3D12 (ticket, ring-offset) queue pattern, partial reclaim proven | `upload_test.cpp`: `"...ring-buffer reclamation under heavy wrap..."` — `blockingRingWaitCount() < flush() calls issued` (observed 0-4 blocking waits across 80 flush() calls issued, run-to-run — see Test evidence) |
| 6 | Ring exhaustion: block-on-oldest, never silent growth | Same test: `ringCapacity()` asserted constant every iteration; `ringWrapCount() > 0` (observed 9 wraps into a 512-byte ring across 80×64-byte uploads) |
| 7 | Image-only ticket never trivially complete, incomplete until GPU copy finishes | `upload_test.cpp`: `"...a ticket covering ONLY an image upload is never the trivially-complete sentinel..."` — `ticket.value != 0` deterministically; `CHECK_FALSE(isComplete(ticket))` immediately after `flush()` on a 16 MiB upload (best-effort timing probe, not load-bearing — see the test's own comment) |
| 8 | Direct-path-only batch = already-complete ticket, zero semaphore machinery touched | `upload_test.cpp`: `"...a direct-path-only batch...returns an already-complete ticket..."` — `ticket.value == 0`, `isComplete()` true with no Vulkan call made (see `isComplete()`'s own `value == 0` short-circuit); also the empty-flush test |
| 9 | Mixed batch: ticket covers only recorded work | `upload_test.cpp`: `"...mixing a direct-path buffer upload with a staging-path upload..."` — one nonzero ticket, both destinations readback-correct once it completes |
| 10 | Transfer-queue acquisition via `get_dedicated_queue`, never `require_` | `device_test.cpp`: `"...acquires an optional dedicated transfer queue via get_dedicated_queue..."` — this dev machine's discrete GPU exposed a dedicated family (`hasDedicatedTransferQueue()==true`, distinct family index asserted); the `else` branch (graceful degrade) is exercised on hardware without one, per the test's own documented either-way assertions |
| 11 | Sample 04 call-site migration, list not claimed exhaustive | Audited the whole file (`grep -n "flush("`): only two `flush()` sites exist (919, 1052), not three — line 1281/1580 are `Uploader::create()` sites with no local `flush()` call of their own. Line 919 (eviction cycle) migrated to genuine `isComplete()` polling; line 1052 (initial fill, pre-frame-loop) migrated to explicit `wait(flush())`, documented as deliberate |
| 12 | `MeshBuffers::create()` blocking convenience via `wait(ticket)` | `mesh_buffers.cpp`; existing MeshBuffers-specific assertions in `upload_test.cpp` (`indexCount()`, buffer sizes, `mesh->vertexBuffer()`/`indexBuffer()` readbacks) are byte-for-byte unmodified — see the Deviation section for the one line in the SAME test case that did change (unrelated to `MeshBuffers::create()` itself) |
| 13 | Wall-clock methodology: timer around the call, must fail against pre-D25 code | `upload_test.cpp`: `"...never block past a wall-clock threshold..."` + **mandatory scratch-worktree revert evidence**, below |
| 14 | Validation-layer #5997 false positive cannot trigger under D5 | Not applicable by construction (main-thread-only, single-threaded test suite) — zero validation errors observed across the entire suite, every run |
| 15 | Cross-queue submission out of scope | Verified: `grep -rn "transferQueue()"` across `src/`/`samples/` shows only the accessor definition/test — no call site submits to it |

## Revert-discrimination evidence (mandatory, row 13)

Per the brief: "the wall-clock test must FAIL against pre-D25 code to
prove it discriminates." Done via a scratch git worktree (toolchain/
`.deps-cache` symlinked in, not copied) checked out at commit `37d9466`
(the real fix), with the pre-D25 defect **reintroduced surgically**
inside `flush()`'s real submission path — a `vkWaitSemaphores` call
right before the ticket is returned, simulating the old
always-blocking contract on top of the new ticket-shaped API (the old
`void flush()` signature cannot host the new test at all, so a literal
"check out the pre-Task-11 commit" revert does not apply here — this is
the mechanically closest equivalent, and it is what the wall-clock test
actually needs to prove: that ITS OWN measured invariant fails when the
underlying block is reintroduced).

Sizing the payload took iteration: this dev machine's discrete GPU is
fast enough that a genuine blocking round trip for 2 MiB only cost
~670 µs, and even 32 MiB only ~4.2 ms — both under an 8 ms threshold,
i.e. the test would have PASSED against the reverted code too (a false
negative). Final sizing (128 MiB × 8 frames) was found empirically in
the scratch worktree until the reverted case failed cleanly:

```
[scratch, reverted flush() blocks]
TEST CASE: Uploader::flush()/isComplete() never block past a wall-clock
threshold across many overlapped batches ...
upload_test.cpp:692: ERROR: CHECK( flushDuration < kThreshold ) is NOT correct!
  values: CHECK( 14054280ns <  8ms )
  ... (8 of 8 iterations failed, worst-case 19135 us)
worst-case flush() duration: 19135 us over 8 overlapped frames
worst-case isComplete() duration: 17 us
[doctest] test cases:  1 |  0 passed | 1 failed | 59 skipped
[doctest] Status: FAILURE!
```

Same sizing against the real (non-reverted) implementation in the main
worktree:

```
[main worktree, real non-blocking flush()]
TEST CASE: Uploader::flush()/isComplete() never block past a wall-clock
threshold across many overlapped batches ...
worst-case flush() duration: 47 us over 8 overlapped frames
worst-case isComplete() duration: 3 us
[doctest] test cases:  1 |  1 passed | 0 failed | 59 skipped
[doctest] Status: SUCCESS!
```

47 µs vs. 14-19 ms — roughly a 300-400× margin, non-flaky, and it
directly demonstrates why `flush()`'s wall-clock cost is now
independent of upload size (isComplete() stayed at 3-17 µs in both runs
regardless of payload — only `flush()`'s own reintroduced block scaled
with data). The scratch worktree was discarded (`git worktree remove
--force`) after capturing this evidence; no scratch-only code is present
in the committed tree.

The overlapping-flush test (row 1, the single highest-priority gate) has
no equivalent revert run: its discrimination is structural, not
timing-based — the pre-Task-11 `void flush()` signature cannot host a
ticket-returning test at all, so there is no mechanical way to run this
exact test body against the old API. Its proof is the design argument
itself (a timeline semaphore has no reset step, eliminating the
reset-while-referenced hazard by construction) plus zero validation
errors under `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`
(confirmed active — `context.cpp:281`, every test fixture passes
`enableValidation=true`).

## Technical finding beyond the matrix's own wording

`VkPhysicalDeviceVulkan12Features::timelineSemaphore` is a feature BIT
that must be explicitly requested at device-creation time — despite
`VK_KHR_timeline_semaphore`'s functionality being fully promoted into
Vulkan 1.2 core, a device is not required to expose it just because API
version ≥ 1.2 is requested. This was not caught until the first test run:
`VkCreateSemaphore` hard-failed every fixture with
`VUID-VkSemaphoreTypeCreateInfo-timelineSemaphore-03252` until
`features12.timelineSemaphore = VK_TRUE` was added alongside this
project's other required Vulkan 1.2 feature bits in `device.cpp`.
Verified present on both this project's own dev drivers (NVIDIA
proprietary and Mesa llvmpipe/lavapipe via `vulkaninfo`, both showing
`VkPhysicalDeviceTimelineSemaphoreFeatures.timelineSemaphore = true`) and
mandated by the Vulkan Roadmap 2022 profile this project's Steam Deck
RADV floor hardware already targets — required unconditionally, no
optional/fallback path needed (unlike `VK_EXT_calibrated_timestamps`/
`VK_EXT_memory_budget`, genuinely optional extensions, not core-promoted
feature bits).

## Deviation: call-site audit widened beyond sample 04

The brief/matrix explicitly scope call-site migration to sample 04
(matrix row 11). Auditing the whole tree for `Uploader::flush()` (not
just sample 04) surfaced ELEVEN more pre-existing call sites relying on
the old always-blocking contract: `src/rx_material/material_system.cpp`
(`MaterialSystem::createTexture2D()`, a public synchronous ABI entry
point — Task 4's own GPU test depends on the texture being fully
uploaded the instant this call returns) and one setup-time + one
per-frame call site each in `samples/03_bindless_mesh`,
`samples/05_multipass`, `samples/06_materials`, plus one setup-time site
in `samples/07_stress`. Left as bare `flush()` (discarding the ticket),
these would have silently regressed: the per-frame ones upload data a
SAME-frame draw immediately reads, and without an explicit `wait()` or a
GPU-side semaphore wait wired into that draw's own submission, this
becomes a genuine Vulkan memory-visibility hazard (not just a timing
race) that sync validation is specifically designed to catch — leaving
them unfixed risked exactly the kind of regression this task's own
"zero validation errors with sync validation" global constraint forbids,
in code Task 11 did not author but does affect by changing
`flush()`'s contract underneath it.

All eleven were migrated to explicit `wait(flush())` — preserving their
EXACT prior (already-blocking) behavior byte-identically, matching the
plan's own "existing callers that immediately wait() are byte-identical
in behavior" text — except sample 04's eviction cycle (line 919), which
is the one call site the sources explicitly name as the non-blocking
migration target. The full test suite (all 17 ctest entries, including
every headless sample gate with `--validate`) passes unchanged after
this — see Test evidence.

## Deviation: existing upload_test.cpp/texture_test.cpp assertions touched

Six pre-existing `upload_test.cpp`/`texture_test.cpp` test cases called
`uploader->flush()` directly, then immediately read back GPU-written data
in a SEPARATE command-buffer submission with no other synchronization.
Before this task, this was safe because `flush()` blocked (a host fence
wait establishes a real memory-availability guarantee for anything
submitted after it). Once `flush()` stops blocking, relying on
same-queue submission order alone for memory visibility is an unstated,
spec-fragile assumption — not a real Vulkan guarantee (only execution
START order is guaranteed per-queue; visibility needs an explicit
wait/barrier). All six were updated to `uploader->wait(uploader->flush())`
before their readback. The ONE line inside the `"MeshBuffers::create..."`
test case that changed is a STANDALONE Uploader call used for a separate
correctness check (not part of `MeshBuffers::create()`'s own call or its
own assertions, which remain byte-for-byte unmodified) — flagging this
explicitly since plan Task 11's text says "existing tests pass
UNMODIFIED" for `MeshBuffers::create()`'s own behavior specifically.

## Deviation: TDD process note

For a redesign this architecturally interconnected (the ticket type, the
new `flush()` signature, the command-buffer rotation, and the ring
reclamation FIFO are all mutually dependent), a strict file-by-file
red-green-refactor cadence was impractical — e.g. no test asserting
ticket behavior can compile before `UploadTicket`/the new `flush()`
signature exist. What was actually done: design the whole mechanism
first, implement it, write the full test set against it, THEN verify
discrimination empirically via the scratch-worktree revert (above) —
substituting "prove the tests fail without the fix" for literal
"write the failing test first." Flagging this honestly rather than
claiming strict TDD literalism.

## Self-review findings

- **Command-buffer reuse hazard (not named in the matrix, found and
  fixed during implementation)**: reusing a single command buffer across
  un-awaited `flush()` calls is the same class of defect as the
  fence-reuse hazard matrix row 1 names, just for the command buffer
  instead of the completion primitive — `vkResetCommandBuffer`/re-record
  into a buffer whose previous submission may still be pending on the
  GPU is a hard Vulkan violation the moment `flush()` stops blocking.
  Fixed with a lazily-grown rotation of command buffers
  (`beginRecordingIfNeeded()` picks any slot whose last ticket
  `isComplete()`, or allocates a fresh one rather than block) — grow,
  never block, matching the timeline semaphore's own "no artificial
  in-flight depth" character. The overlapping-flush test exercises this
  directly (two flushes with no wait between them force two DIFFERENT
  command buffers to be in flight simultaneously); zero validation
  errors confirm no reset-while-pending violation.
- `activeCmd()` returns `VK_NULL_HANDLE` (not an out-of-bounds vector
  index) on the rare `beginRecordingIfNeeded()` allocation-failure path
  — checked deliberately after noticing the naive `cmdSlots_[activeSlot_]`
  form would be UB when `activeSlot_` stays `kInvalidSlot`.
  `vkQueueSubmit` failure inside `flush()` deliberately does NOT roll
  `lastQueuedValue_` back (documented in the code: simpler and equally
  correct to skip a value than risk reusing one).
- Confirmed `~Uploader()` waits on `lastQueuedValue_` (the highest ticket
  ever queued), not merely the ticket its own final `flush()` call
  returns — the latter could be the trivially-complete sentinel even
  while an earlier, never-waited-on batch is still genuinely running,
  which would have left teardown racing real GPU work.
- Confirmed move constructor/assignment carry every new member
  (`timelineSemaphore_`, `lastQueuedValue_`, `cmdSlots_`, `activeSlot_`,
  `lastFlushRingCursor_`, `pendingRegions_`, `ringGeneration_`,
  `blockingRingWaitCount_`) and null/reset the source — a dropped field
  here would have silently corrupted ticket bookkeeping after any move.
- Confirmed `MemoryCategory::Staging` tagging on the ring buffer
  (Task 10 lockstep, brief's own note) is unchanged — `createUploadRingBuffer`
  call site in `Uploader::create()` was not touched beyond formatting.
- `logDescriptorIndexingFeatureGaps()` was deliberately NOT extended to
  name `timelineSemaphore` gaps specifically — it is scoped to
  descriptor-indexing per its own existing design (Phase 2), and every
  other required Vulkan 1.1/1.2/1.3 bit (`shaderDrawParameters`,
  `dynamicRendering`, `synchronization2`, etc.) already goes through the
  same generic vk-bootstrap error path without individual diagnosis —
  `timelineSemaphore` verified present on both dev drivers, consistent
  with that existing risk profile, so no new precedent was introduced.
- No AI attribution anywhere in new/modified files or commit messages
  (grepped case-insensitively for `claude|anthropic|co-authored|generated
  by|ai-generated` across the full diff — zero matches).
- `.superpowers/sdd/.../task-11-brief.md` and the plan/spec/matrix/rulings
  files are untouched (out of write scope per the brief).
- Local environment note (not a code issue): the FIRST `windows-cross-zig`
  configure attempt failed with a vk-bootstrap dispatch-table compile
  error (`unknown type name 'PFN_vkShutdownLatencyDeviceLegacyNV'`) —
  traced to a STALE `build/windows-cross-zig` directory left over from a
  prior, unrelated session on this machine (confirmed via a scratch
  worktree at the base commit: a fresh configure there succeeded
  immediately, building vk-bootstrap from a MISS with no errors). Deleting
  `build/windows-cross-zig` and reconfiguring fresh in the main worktree
  resolved it; unrelated to this task's changes (`git diff` against
  `third_party/CMakeLists.txt`/`CMakePresets.json` is empty).

## Test evidence

Full `rx_rhi_vk_tests` binary, Uploader/MeshBuffers/transfer-queue subset
(16 test cases; the other 44 are skipped windowed-fixture guards that
route through the same binary per this target's own doctest_main.cpp
convention):

```
$ xvfb-run -a ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests -tc="*Uploader*,*MeshBuffers*,*transfer*"
...
ring wrapped 9 times across 80 uploads into a 512-byte ring
blocking ring-reclaim waits: 0 (flush() calls issued: 80)
[doctest] test cases:  16 |  16 passed | 0 failed | 44 skipped
[doctest] assertions: 830 | 830 passed | 0 failed |
[doctest] Status: SUCCESS!
```

(`blockingRingWaitCount()` varies 0-4 run to run depending on how far
ahead the GPU stays of the CPU issuing loop; always well below the
80 flush() calls issued, satisfying matrix row 5's own instrumented
criterion every run observed.)

Full linux-native suite:

```
$ xvfb-run -a ctest --preset linux-native --output-on-failure
...
100% tests passed, 0 tests failed out of 17
Total Test time (real) =  52.65 sec
```

windows-cross-zig, full build then the CI's own ctest subset:

```
$ cmake --build --preset windows-cross-zig
... 115/115, no errors

$ xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'
...
100% tests passed, 0 tests failed out of 7
Total Test time (real) =  88.37 sec
```

(`rx_rhi_vk_tests`/GPU-backed/sample targets excluded from ctest under
`windows-cross-zig` per this project's own CI — Wine has no real Vulkan
device; the full binary set, including every sample and `rx_rhi_vk_tests.exe`,
still cross-compiles and links cleanly, confirmed in the build log above.)

Compiler warnings: touched every modified `rx_rhi_vk`/`rx_material` source
and test file's mtime and rebuilt `rx_rhi_vk`, `rx_rhi_vk_tests`, and
`rx_material` from a forced recompile — zero `warning`/`error` lines in
the build output (grepped, excluding the pre-existing documented
"known false positive" validation-layer log lines).

Validation errors: `grep`-verified zero un-filtered `[vulkan validation]`
lines across every run above; every new test's own
`CHECK_FALSE(hasValidationErrors())` passed, and CI's own `--validate`
flag on every headless sample test confirms the sample-level migrations
(including the eviction-cycle poll redesign) are exercised under
`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` specifically,
not just generic validation.
