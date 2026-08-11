# Task 1 report: render-graph history resources + PassSignature bounds check

Commit: `f9d0554` (base `6b1654a`), branch `main`, not pushed.

## What was built

- `src/rx_graph/include/rx_graph/pass_signature.h` — `PassSignature::
  kMaxColorAttachments = 8` (named constant replacing the array's own magic
  `8`), the carried Phase 3 final-review bounds check's ceiling.
- `src/rx_graph/include/rx_graph/pass.h` / `render_graph.cpp` —
  `Pass::addHistoryInput(name)` (sampled read of a history resource's
  PREVIOUS frame's contents) and `Pass::setHistoryOutput(name, desc)`
  (persistent written attachment, color or depth/stencil chosen by
  `desc.format`, exactly like `addColorOutput`/`setDepthStencilOutput`).
  Two new `AccessKind` values (`HistoryInput`/`HistoryOutput`).
  `RenderGraph::compile()` grows:
  - the bounds check itself (>`kMaxColorAttachments` color outputs on one
    pass throws, naming the pass; a color-format `HistoryOutput` counts, a
    depth/stencil one never does, matching `setDepthStencilOutput`'s own
    unbounded treatment);
  - namespace-mixing validation (a name used by both a history and a
    non-history declaration throws, naming it) and exactly-one-writer
    validation (>1 `setHistoryOutput()` pass for the same name throws,
    naming both);
  - a `HistoryInput`-specific read-resolution pass, deliberately separate
    from `addTextureInput`'s: resolves against a `historyOutputWritersByName`
    map instead of `writersByName`, and adds **no** `dependsOn` edge —
    reading last frame's contents must never create a same-frame ordering
    dependency on this frame's writer, since they are two different
    physical images (ping-pong slots), not one. Verified directly: a test
    declares one pass with BOTH `addHistoryInput("hist")` and
    `setHistoryOutput("hist", ...)` (the canonical accumulator pattern) and
    confirms `compile()` succeeds with the expected execution order.
  - cull-step treatment of every `setHistoryOutput()`-declaring pass as an
    implicit root (like `setSideEffect()`) — its effect crosses the frame
    boundary, so "nothing in this graph reads it" must never cull it away.
  - `Pass::isWriteKind()` deliberately excludes `HistoryOutput` (keeps it
    out of the `dependsOn` edge graph while `hasAttachmentOutput()`/
    `resolveAccess()` still correctly treat it as a real write).
  - a device-free `isDepthOrStencilFormat()` (same six-format table as
    `executor.cpp`'s/`rx_rhi_vk/texture.cpp`'s own `aspectMaskForFormat()`,
    an independent small copy for the same "not exported past its own .cpp"
    reason those two already document).
- `src/rx_graph/include/rx_graph/resources.h` — `PhysicalResource::
  isHistory` (mutually exclusive with `isBackbuffer`/buffer-ness); a
  history resource unions `VK_IMAGE_USAGE_SAMPLED_BIT` unconditionally
  (it is always eventually sampled, even in a compiled graph with no
  `addHistoryInput()` for it at all).
- `src/rx_graph/transient_pool.h` / `.cpp` — the pinned pool, a genuinely
  separate category from the existing shape-keyed `images_`/`buffers_`:
  `PinnedHistoryEntry` (looked up by **name**, never shape — two
  differently-shaped or same-shaped history resources never alias) holding
  two `PinnedHistorySlot`s (ping-pong slot A/B), each with its own
  `detail::ResourceBarrierState` — **reused verbatim** from `barriers.h`'s
  existing per-resource invalidate/flush state machine rather than a new
  one, and never reset between `execute()` calls (unlike `buildBarriers()`'s
  own per-compile-walk instance) — that persistence alone *is* design
  contract point 4's "barrier state machine initialized from tracked
  last-frame layout instead of UNDEFINED". `TransientPool::acquireHistory()`
  creates both slots fresh (or replaces them wholesale, retiring the old
  ones via `DeletionQueue`, on a shape change/resize) and reports
  `freshlyCreated` so the caller knows to run the one-time init-clear.
  `sweepStale()` never touches `pinned_` at all (documented: the simplest
  choice that still satisfies "never recycled" — retired only by
  `retireAll()` at shutdown or superseded wholesale by a resize).
- `src/rx_graph/executor.cpp` / `include/rx_graph/executor.h` —
  - `PassContext::historyValid(name) -> bool`: false until the resource's
    current read slot has been the target of a REAL `setHistoryOutput()`
    write in some past `execute()` call (`PinnedHistorySlot::
    everWrittenByRealPass`, latched, never set by the init-clear).
  - The one-time black-clear-at-creation init (design contract point 3):
    implemented as a **small init submission** (`initializePinnedHistoryEntry()`,
    a dedicated `rx::rhi::CommandContext::runOnce()` call at `realize()`
    time) rather than clear-on-first-write-load — the latter cannot cover
    the actual hazard at all: under this task's ping-pong parity (write
    slot = `frameCounter % 2`, read slot = `(frameCounter + 1) % 2`), the
    very first `execute()` call reads a slot that will not be written by
    any real pass until the frame *after* next, so there is no first write
    for a `LOAD_OP` to piggyback on before that first read. Deliberately
    does **not** route through `detail::applyAccess()` for the clear's own
    barrier (a real gap found while implementing this: `barriers.cpp`'s
    write-classification mask does not recognize
    `VK_ACCESS_2_TRANSFER_WRITE_BIT`, so doing so would mis-seed
    `lastWriteStages`/`pendingFlushAccess` — harmless only by the accident
    of `runOnce()`'s synchronous `vkQueueWaitIdle`, not something to rely
    on) — hand-writes the (fully known, since a fresh slot is always
    `UNDEFINED`/never-accessed) transition and hand-seeds the resulting
    state as a real write instead.
  - `applyHistoryAccesses()`: history resources bypass `compile()`'s own
    device-free barrier derivation (`allBarriers[pos]`) entirely. Reasoned
    and empirically necessary: a single `physicalIndex`, when a pass
    declares both a read and a write of the same history name, would need
    `barriers.cpp`'s `combineByResource()` to merge them into one
    transition with only one `newLayout` — structurally incapable of
    describing two different real images (read slot vs. write slot) at
    once. Instead walks the **unmerged** per-declaration `accesses`
    directly, resolves each history access's own real slot (write slot for
    `COLOR_ATTACHMENT_OPTIMAL`/`DEPTH_ATTACHMENT_OPTIMAL`, read slot for
    `SHADER_READ_ONLY_OPTIMAL` — exhaustive, unambiguous), and applies a
    real barrier chained off *that slot's own* persisted
    `ResourceBarrierState` via `detail::applyAccess()` (this call site's
    accesses are all real resolved `ResourceAccess` values, so the
    write-classification mask gap above does not apply here).
  - `resolveAttachmentView()`: the dynamic-rendering attachment-building
    loop binds the **write slot**'s view for a history resource.
    `PassContext::imageView()/image()` resolve to the **read slot**
    instead — the only two roles anything ever needs; there is no
    "give me the write slot by name" accessor since nothing needs one
    (the write slot is bound automatically as the pass's own attachment,
    same as any other `addColorOutput()`).
  - The generic `applyBarriers()` / cross-frame `finalStageThisExecute`
    accumulation both explicitly skip `isHistory` physical indices (their
    `poolIndex` addresses `pinned_`, not `images_`/`buffers_` — processing
    them through the generic path would misindex that vector).
  - `Executor::Impl` gained `graphicsQueue`/`graphicsQueueFamily` (needed
    for the init-clear's own `CommandContext`) — the first Executor state
    that needed a real queue for something other than the caller-supplied
    `execute()` command buffer.
- `src/rx_graph/tests/test_compile.cpp` — 12 new device-free `TEST_CASE`s:
  color/depth resolution by format; compute/graphics stage split (mirrors
  the existing `addTextureInput` case); namespace-mixing throws;
  exactly-one-writer throws; undeclared-history-read throws; a
  history-output pass survives culling with zero readers; the
  accumulator-pattern (read+write same name, one pass) compiles with no
  cycle and no dependency edge; the bounds check itself (throws over the
  limit, accepts at the limit, a color-format `HistoryOutput` counts, a
  depth-format one never does).
- `src/rx_graph/tests/test_execute_gpu.cpp` — one new GPU `TEST_CASE`
  (`buildFillPipeline`/`kFillShaderSource`, a small new push-constant-color
  pipeline with no descriptor sets, added for the history-output writer;
  the existing "invert" pipeline/shader from the file's very first test
  case is **reused verbatim** for the history-input reader rather than
  writing a third near-duplicate texture-sampling shader — its channel
  inversion is simply accounted for in the expected-pixel arithmetic
  below). Two `execute()` calls against one realized graph:
  - Frame 1: `write_history` fills red into write slot 1; `read_history`
    samples read slot 0 (never written, only black-cleared) — asserts
    `historyValid("hist") == false` and the inverted-black readback
    (`{255,255,255,255}`).
  - Frame 2: `write_history` fills green into write slot 0; `read_history`
    samples read slot 1 (= frame 1's red) — asserts
    `historyValid("hist") == true` and the inverted-red readback
    (`{0,255,255,255}`), i.e. frame 2's readback contains frame 1's
    pattern, not frame 2's own still-being-written one.
  - `CHECK_FALSE(context.hasValidationErrors())` at the end, matching every
    other case in the file.

## Design choices within the given contract

- **Ping-pong slot selection**: `writeSlot = frameCounter % 2`,
  `readSlot = (frameCounter + 1) % 2`, read directly off
  `Executor::Impl::frameCounter` (already incremented at the top of every
  `execute()` call) — no separate counter.
- **First-frame contract (point 3)**: small init submission, chosen over
  clear-on-first-write-load for the reason above (documented at length in
  `initializePinnedHistoryEntry()`'s own comment) — this is the one place
  the brief explicitly left as an implementation choice to make and
  justify.
- **Cross-frame barrier state (point 4)**: reused `detail::
  ResourceBarrierState`/`detail::applyAccess()` verbatim per-slot rather
  than inventing a parallel mechanism — its persistence across
  `execute()` calls (never reset, unlike `buildBarriers()`'s own instance)
  realizes "initialized from tracked last-frame layout instead of
  UNDEFINED" with no new state-machine code at all. `barriers.h`/`.cpp`
  are completely untouched.
- **PassContext accessor split**: `imageView()`/`image()` → read slot;
  automatic attachment binding → write slot. No accessor for "the write
  slot by name" exists because nothing needs one.

## Test results

`ctest --preset linux-native -R rx_graph --output-on-failure`:
```
Test project .../build/linux-native
    Start 6: rx_graph_tests
1/2 Test #6: rx_graph_tests ...................   Passed    0.00 sec
    Start 7: rx_graph_gpu_tests
2/2 Test #7: rx_graph_gpu_tests ...............   Passed    1.92 sec
100% tests passed, 0 tests failed out of 2
```
`rx_graph_tests` lists 33 cases (15 pre-existing, unmodified, all still
passing; 18 new for this task). `rx_graph_gpu_tests`: 5 cases (4
pre-existing unmodified + 1 new), 84/84 assertions passing.

Newer validation layer (per task instructions):
```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json VK_LAYER_PATH=/home/ywadi/sponza/vvl \
  xvfb-run -a ctest --preset linux-native -R rx_graph --output-on-failure
Test project .../build/linux-native
    Start 6: rx_graph_tests
1/2 Test #6: rx_graph_tests ...................   Passed    0.00 sec
    Start 7: rx_graph_gpu_tests
2/2 Test #7: rx_graph_gpu_tests ...............   Passed    0.55 sec
100% tests passed, 0 tests failed out of 2
```
Ran the new history test case directly under that same newer layer with
full doctest output (`-tc="*Phase 4 Task 1*" --success`): **zero**
validation/error/hazard log lines at all — including the
`SYNC-HAZARD-READ_AFTER_WRITE` message this repo's own `context.cpp`
already documents as a known false positive of the apt-packaged
(1.3.204.1) layer for this exact separate-sampler `Texture2D.Sample()`
shape, "verified clean against a newer layer build" — confirming that
documented claim held for this task's reused "invert" shader/pipeline too,
not just filtered by the older layer's known-bug matcher.

Both presets build clean: `cmake --build build/linux-native` (whole repo,
12 targets, including `rx_material`/samples 05/06 that transitively depend
on `rx_graph`) and
`cmake --build build/windows-cross-zig --target rx_graph rx_graph_tests rx_graph_gpu_tests`.

Full repo suite (`ctest --preset linux-native`, lavapipe): 15/15 passed,
0 regressions anywhere (rx_core, rx_platform, rx_shader, rx_rhi_vk,
rx_material ×2, all 6 sample headless gates).

## Files

- `src/rx_graph/include/rx_graph/pass_signature.h`
- `src/rx_graph/include/rx_graph/pass.h`
- `src/rx_graph/include/rx_graph/resources.h`
- `src/rx_graph/include/rx_graph/executor.h`
- `src/rx_graph/render_graph.cpp`
- `src/rx_graph/transient_pool.h`
- `src/rx_graph/transient_pool.cpp`
- `src/rx_graph/executor.cpp`
- `src/rx_graph/tests/test_compile.cpp`
- `src/rx_graph/tests/test_execute_gpu.cpp`

## Concerns

- **Multi-reader-same-frame optimality, not correctness**: if several
  passes read the same history name's read slot within one frame (not
  exercised by any current caller or test), `applyHistoryAccesses()`
  correctly avoids redundant barriers via the persisted
  `ResourceBarrierState`'s own per-stage invalidation tracking — no gap,
  just noting it was reasoned about rather than assumed away.
- **Resize discards history content**: `TransientPool::acquireHistory()`
  recreates both slots (and resets `everWrittenByRealPass`) on a shape
  change. Not exercised by a dedicated test this task (the existing
  resize-rerealize GPU test covers the *mechanism* for ordinary pooled
  resources only) — a reasonable, documented behavior for Phase 4 (nothing
  yet resizes a history resource mid-use) but worth a dedicated test if/when
  a real consumer does.
- Load-preserve history output (partial/incremental writes to a persistent
  resource) is explicitly out of scope per the design contract and is
  documented as future work on `setHistoryOutput()` itself, not silently
  dropped.
