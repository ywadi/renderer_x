# Task 2 report: sync2 barrier derivation in rx_graph

## What was built

- `src/rx_graph/include/rx_graph/barriers.h` -- new. Defines `ImageBarrier`,
  `BufferBarrier`, `PassBarriers` (exactly the brief's interface), plus two
  pieces not in the brief's interface list but needed to implement and
  honestly test it:
  - `ResourceBarrierState` -- the per-physical-resource running
    invalidate/flush accumulator (`currentLayout`, `hasPendingFlush` +
    `pendingFlushStages/Access`, `invalidatedStages/Access`, `everAccessed`).
  - `applyAccess(state, isBuffer, stages, access, layout)` -- applies one
    declared access to that state in place and returns
    `std::optional<BarrierTransition>` (the six barrier fields, or
    `nullopt` if no barrier is needed). This is the actual per-resource
    state machine; `buildBarriers()` is a thin driver over it.
  - `buildBarriers(const CompiledGraph&, PassBarriers& outFinalBarriers)`
    -- the brief's per-compile entry point, reading `graph` only through
    its own public accessors (`executionOrder()`/`resources()`/
    `passAccesses()`), never through friend access.
- `src/rx_graph/barriers.cpp` -- new. Implements the above, plus an
  internal `combineByResource()` helper (union-merges a single pass's
  multiple `ResourceAccess` entries against the same physical resource --
  the brief's "same resource read+written by one pass" ambiguity
  resolution) and the write-access-bit mask used to decide what a write
  actually needs to flush later.
- `src/rx_graph/include/rx_graph/render_graph.h` -- modified.
  `CompiledGraph` gets `passBarriers()` (span, indexed by
  `executionOrder()` position) and `finalBarriers()` (single
  `PassBarriers`), backed by two new private members
  (`passBarriers_`/`finalBarriers_`) that only `RenderGraph::compile()`
  writes. Includes `<rx_graph/barriers.h>` for the `PassBarriers` type;
  `barriers.h` only forward-declares `CompiledGraph`, so there is no
  header cycle.
- `src/rx_graph/render_graph.cpp` -- modified. `compile()`'s existing
  step 4 (resource/access resolution) is now followed by a fifth phase
  that calls `buildBarriers(compiled, compiled.finalBarriers_)` and
  stores the result on `compiled.passBarriers_`, immediately before
  `g.compiled = std::move(compiled)`. Top-of-file algorithm comment and
  the class comment on `CompiledGraph` (render_graph.h) updated to
  mention this fifth phase.
- `src/rx_graph/CMakeLists.txt` / `src/rx_graph/tests/CMakeLists.txt` --
  added `barriers.cpp` / `test_barriers.cpp` respectively. Updated the
  library's header comment (it previously said barrier derivation would
  need `rx_rhi_vk`/a device -- corrected, since Task 2 stayed device-free
  as required).
- `src/rx_graph/tests/test_barriers.cpp` -- new. All 7 required doctest
  cases (see below).

## The algorithm, and how it maps to Granite

Granite's `RenderGraph::build_physical_barriers()`
(`renderer/render_graph.cpp`) tracks a `ResourceState` per physical
resource *within one physical (possibly multi-subpass) pass*:
`initial_layout`/`final_layout`/`invalidated_types`/`flushed_types`/
`invalidated_stages`/`flushed_stages`, reset per physical pass, plus a
separate, longer-lived `PipelineEvent` per resource (`to_flush_access`,
`invalidated_in_stage[64]`, `layout`) that carries state *across*
physical passes and drives actual semaphore/event emission against a real
`Vulkan::Device`.

rx_graph has neither subpasses (dynamic rendering: one pass is one
`vkCmdBeginRendering`/`vkCmdEndRendering` pair, no render-pass-compatible
merging) nor, in Task 2, multiple queues/frames-in-flight to bridge with
events -- Task 1 already scopes a compile to one pass topology, and this
task's own ambiguity resolutions (D4: every resource starts a compile walk
at UNDEFINED with empty state; cross-frame carry-over is Task 3's problem)
say a *single* per-resource accumulator, reset once per `buildBarriers()`
call, is exactly what's wanted -- not Granite's two-tier
per-physical-pass-then-cross-pass-event split, which exists to solve
problems (subpass merging, cross-queue events) rx_graph doesn't have here.
So `ResourceBarrierState` collapses Granite's `ResourceState` +
`PipelineEvent` into one struct, and Granite's 64-entry
`invalidated_in_stage[bit]` per-stage-bit array collapses to a single
aggregate `invalidatedStages`/`invalidatedAccess` pair -- a deliberate
simplification: with no subpass merging, a resource's visibility state
never needs to survive past the one barrier that resolves it (see the
"why the WAR test needed its own seam" section below for the one place
this simplification is visible).

What ported directly: the *shape* of the decision (need a barrier when
layout differs, or a write follows any prior access, or a read isn't yet
covered by established visibility while a write is still unflushed;
srcStage/access come from the still-pending write, or 0 for
read-after-read/first-use), and the *purpose* of `pendingFlush` vs
`invalidated` (available-but-maybe-not-yet-visible vs
already-established-visible) as two distinct pieces of state -- both are
Granite's core insight and this task's brief's own framing of it. What's
new/re-derived: every field name, the buffer-vs-image split (Granite
special-cases swapchain/transient images inline in the loop; rx_graph
just branches on `PhysicalResource::isBuffer`), the write-access bit mask
(`kWriteAccessMask`) used to strip a depth/storage-buffer output's paired
READ bit out of what gets remembered as "needs flushing" (Granite does the
analogous thing with `flush_access_to_invalidate`/`flush_stage_to_invalidate`,
but in the opposite direction -- expanding a flush's access into what it
implicitly invalidates -- rather than narrowing a raw declared access down
to its write-only portion; the underlying reason, "reads don't dirty
anything, so don't count them as needing a flush," is the same idea
either way), and the same-pass union-access handling (`combineByResource`)
for a case Granite's own model doesn't need to handle the same way
(Granite tracks `get_color_inputs()`/`get_color_outputs()`/etc. as
separate per-kind lists on each logical pass and merges them into a single
`Barrier` per resource via its own `get_flush_access`/`get_invalidate_access`
lookup helpers -- conceptually the same merge, expressed differently
because rx_graph's `ResourceAccess` list is already flat by the time
`buildBarriers()` sees it).

No Granite source was copied; comments in `barriers.h`/`barriers.cpp`
reference Granite by file/function name only, the same brief-reference
convention already used elsewhere in `rx_graph` (e.g. `render_graph.cpp`'s
own comment citing Granite's `traverse_dependencies`/
`depend_passes_recursive` for Task 1).

## Where I deviated from the brief, and why

1. **Added `ResourceBarrierState`/`applyAccess` to the public interface**
   (the brief's own interface section only lists
   `ImageBarrier`/`BufferBarrier`/`PassBarriers` plus the two
   `CompiledGraph` accessors). This was forced by a real, verified gap
   between the brief's `war-execution-only` test description and Task 1's
   actual, already-in-tree dependency semantics -- see the next section.
   `buildBarriers()` still exists exactly as specified and is what
   `RenderGraph::compile()` calls; the two extra names are additive, not a
   replacement.

2. **Four of the seven required tests build the scenario through a pass
   that "doubles as the present pass"** (`shadow-then-sample`,
   `hdr-tonemap`, `compute-to-draw-buffer`, `culled-contributes-nothing` --
   e.g. `shadow-then-sample`'s `forward` pass both samples `sm` *and*
   writes `bb` directly) rather than a separate side-effect-only reader
   pass. This was necessary to make the reading pass Graphics-class (Task
   1's `Pass::hasAttachmentOutput()` is keyed on that *same* pass's own
   declared outputs) so its texture-input stage resolves to
   `FRAGMENT_SHADER_BIT` as the brief's test descriptions expect, rather
   than `COMPUTE_SHADER_BIT`. The side effect: that pass also owns a
   second, unrelated first-use image barrier for its own backbuffer write
   -- **but only in three of those four** (`shadow-then-sample`,
   `hdr-tonemap`, `culled-contributes-nothing`) does that contamination
   land in a list the test actually asserts on (`imageBarriers`);
   `compute-to-draw-buffer`'s "draw" pass also writes `bb`, but that test
   only ever checks `bufferBarriers`, so no relaxation was needed there.
   Rather than assert an exact list size of 1 for those three (which would
   be false and would have meant weakening the per-field assertions to
   make it true), each asserts the list's real size (2) and locates the
   barrier under test by `physicalIndex` before checking all six fields
   exactly -- still a full, exact-field assertion, just not also asserting
   "and nothing else is in this list." (Corrected in fix round 1 below --
   the original text here said "six of the seven" needed the pattern,
   which was wrong; see that section.)

## The `war-execution-only` test: a genuine Task 1/Task 2 brief conflict

Before writing this test I verified, empirically, that it cannot be built
through `RenderGraph`'s public API as literally described ("pass reads T
(sampled), later pass depth-writes T"). Task 1's `compile()` (already
in-tree, already reviewed) resolves every reader of a declared resource
name against that name's **final** declared writer
(`render_graph.cpp`, step 2: `uint32_t writer = it->second.back();`) --
not the nearest preceding one, and with no resource versioning. If a
second pass writes the same name later, every reader of that name
(wherever it was declared) becomes dependent on *that* writer instead,
which forces the reader after *both* writes in any valid topological
order -- never between them. I confirmed this by compiling and running a
small standalone program directly against the already-built
`librx_graph.a`/`librx_core.a` (using the project's own
`zig-cxx-linux` wrapper and the exact flags from
`compile_commands.json`, plus the prebuilt `libspdlog.a` the real test
binaries link against) with the pass topology
`writer1 -> reader(reads T, side effect) -> writer2(writes T again, side
effect) -> present`, declared in that order. Result:
`execution order: 0 2 1 3` -- the reader always ends up *after* both
writers, confirmed by direct compile()/executionOrder() inspection, not
by re-reading the source a second time.

Given that, `RenderGraph::compile()` can never produce a `CompiledGraph`
whose `executionOrder()` puts a read of resource X before a later write
of the same X -- and `CompiledGraph` has no public constructor other than
through `compile()`, so there is no way to hand-construct one for a test
either. The WAR *accounting rule* is still real and still needed (a
future Task 1 extension adding resource versioning, or a resource
genuinely read-then-written across two different *physical* resources
that happen to alias once Phase 3 gets aliasing, would need it) --
implementing it and then being unable to honestly test it felt worse than
adding one small, clearly-documented additional seam. `applyAccess()` is
that seam: it's also exactly the reusable unit `buildBarriers()` is built
out of, so exposing it isn't pure test scaffolding bolted on top -- it's
the natural decomposition either way.

## Test results

`ctest --preset linux-native -R rx_graph --output-on-failure`: 1/1 test
binary passed. Full doctest summary from `rx_graph_tests` (test_compile.cpp's
20 existing Task 1 cases + this task's 7 new ones):

```
[doctest] test cases:  20 |  20 passed | 0 failed | 0 skipped
[doctest] assertions: 149 | 149 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Full project regression check, both presets:
- `ctest --preset linux-native --output-on-failure`: 10/10 passed
  (shader_spirv_test, rx_core/rx_platform/rx_shader/rx_rhi_vk/rx_graph
  tests, 4 headless samples).
- `windows-cross-zig`: built `rx_graph`/`rx_graph_tests` clean, then ran
  the full suite under Wine: `ctest --preset windows-cross-zig
  --output-on-failure`: 10/10 passed.

## Files

- `src/rx_graph/include/rx_graph/barriers.h` (new)
- `src/rx_graph/barriers.cpp` (new)
- `src/rx_graph/tests/test_barriers.cpp` (new)
- `src/rx_graph/include/rx_graph/render_graph.h` (modified)
- `src/rx_graph/render_graph.cpp` (modified)
- `src/rx_graph/CMakeLists.txt` (modified)
- `src/rx_graph/tests/CMakeLists.txt` (modified)

## Commit

`feat: derive sync2 barriers in rx_graph compile` -- see commit hash in
the coordinator-facing status reply for this task (this report was
written and finalized before the commit was created, per the task's own
step order: green tests, then commit).

## Concerns for the coordinator

1. The `war-execution-only` deviation above (extra public names,
   `ResourceBarrierState`/`applyAccess`) is the one thing worth a second
   look -- I'm confident in the empirical finding (reproduced it directly
   against the built library, not just by re-reading source), but the
   *response* to it (expose a lower-level seam and test that instead of
   the brief's literal scenario) was my own call, not something the brief
   anticipated. If a future task adds resource versioning/aliasing, this
   is also the point where a real end-to-end WAR scenario would start
   being constructible through the public API, and it would be worth
   adding a `RenderGraph`-driven version of this test at that time.
2. Four of the seven tests needed the "reader pass doubles as present"
   restructuring described above (three of which needed the list-size
   relaxation too -- see deviation 2's corrected count), which makes those
   tests slightly less visually literal than the brief's one-line
   descriptions suggest (e.g. `shadow-then-sample`'s `forward` pass is
   described as just "fragment sample" but also writes `bb`). Every
   six-field assertion the brief asks for is still exact; only the "and
   this is the *only* barrier in the list" framing had to be relaxed to
   "and this exact barrier is in the list, which also (correctly) contains
   one unrelated entry."
3. No new validation was added for the same-pass read+write-of-one-resource
   case, per the brief's own instruction; `combineByResource()`'s
   write-layout-wins tie-break for that case is documented in
   `barriers.cpp` but is untested (no required test exercises it, and the
   brief said not to add validation, which I read as "don't add tests for
   it either" -- happy to add one if that reading is wrong).

## Fix round 1 (review: task-2-review.md, commit df40111)

The review found 1 Critical, 1 Important, 2 Minor findings. All four
applied.

### Critical: the state machine under-synchronized a second reader in an uncovered pipeline stage, and it compounded into an under-synchronized WAR write

The reviewer's own probe (independent of anything in this repo) showed
that after a resource is written and then read once, a *second* read of
the same resource in a *different* pipeline stage got **zero** barriers --
`COMPUTE_SHADER`'s cache was never named as a `dstStage` in any sync2
barrier for that resource, a genuine GPU synchronization hazard. It
compounded: a subsequent write-after-read (WAR) after both reads produced
a `srcStage` that silently omitted the second reader's stage entirely, so
the write was not guaranteed to wait for that reader to finish.

Root cause, confirmed against Granite's actual source (not just the
brief's paraphrase of it): the first port collapsed two fields Granite
deliberately keeps separate --
- Granite's `pipeline_barrier_src_stages` (the last real write's stage)
  **persists across reads** -- nothing but a new write ever changes it.
- Granite's `to_flush_access` (the last write's access still needing to be
  made available) **is** cleared by any resolving read.
- Granite's `invalidated_in_stage[64]` tracks visibility **per
  pipeline-stage bit**, not as one aggregate.

The first port's `ResourceBarrierState` had one `pendingFlushStages/Access`
pair that got *fully* cleared (stage included) the moment any read
resolved it, and one *aggregate* `invalidatedStages/Access` pair. Once the
pending-flush pair was cleared, the old `applyAccess()`'s read-needs-
barrier check (`!write && hasPendingFlush && !coveredByInvalidated(...)`)
could never fire again for that resource until the next write --
regardless of whether the new read's own stage had ever actually been
covered.

**Fix**: `detail::ResourceBarrierState` (`barriers.h`) now keeps these as
genuinely separate, persisted-vs-transient fields:
- `lastWriteStages` -- persists across reads (Granite's
  `pipeline_barrier_src_stages`).
- `hasPendingFlush`/`pendingFlushAccess` -- transient, cleared by any
  resolving barrier (Granite's `to_flush_access`).
- `invalidatedInStage` -- a real `std::array<VkAccessFlags2, 64>`, indexed
  by stage-bit position exactly like Granite's own array, plus a
  companion `invalidatedStagesUnion` (not a Granite field -- see below).

The read-needs-barrier check is now `!write &&
!coveredByInvalidated(state, stages, access)` -- no `hasPendingFlush` gate
at all, per the reviewer's own fix direction -- where `coveredByInvalidated`
checks per-stage-bit coverage (Granite's `need_invalidate()`). A read's
barrier chains `srcStage` off the persisted `lastWriteStages` (correct
whether or not the flush was already resolved to some *other* stage) and
`srcAccess` off `pendingFlushAccess` (0 if already flushed).

One deliberate departure from Granite's own literal behavior, called out
explicitly rather than silently: tracing Granite's actual write-side
barrier construction, a WAR write's `srcStage` there would come from
`pipeline_barrier_src_stages` -- i.e. the **original write's** stage, not
either reader's. That does not actually wait for the readers at all (the
original write already trivially precedes them), which would not satisfy
this fix round's explicit requirement ("the final WAR sees the union of
both reader stages as its src") and would not be correct synchronization
for this pattern regardless. So a WAR write's `srcStage` here is
`invalidatedStagesUnion` -- the union of every stage that has read this
resource since the last write -- which is also exactly what the original
brief's own ambiguity resolution already said ("WAR ... srcStage = the
prior read stages"), just generalized from one reader to the union of
however many there were. This is flagged as a divergence from Granite's
literal source, made to satisfy the coordinator's own explicit correctness
requirement for this exact pattern.

Added tests (both pass):
- `multi-stage-read-gets-own-barrier` (API-level): pass `W` writes
  texture `T`; Graphics-class pass `A` samples it (`FRAGMENT_SHADER`);
  Compute-class pass `B` also samples it (`COMPUTE_SHADER`), both
  reachable. Asserts `A` gets the write->read flush barrier and `B` gets
  its own barrier (no layout change, `srcAccess = 0`, `srcStage` chained
  off the same persisted write source `A`'s barrier used) -- this exact
  case used to produce zero barriers for `B`.
- `war-unions-all-reader-stages` (unit-level, direct `detail::applyAccess()`
  reproduction of the reviewer's own probe): write, read (`VERTEX_SHADER`),
  read (`COMPUTE_SHADER`, previously the silently-dropped case), write
  (WAR). Asserts every step's exact six fields, especially that the WAR
  write's `srcStage` is `VERTEX_SHADER_BIT | COMPUTE_SHADER_BIT`.

### Important: the added public surface wasn't clearly scoped

`ResourceBarrierState`/`BarrierTransition`/`applyAccess` moved into
`namespace rx::graph::detail` in `barriers.h`, with a comment stating they
are exposed for unit tests and Task 3's executor, not a second API-stable
surface alongside the brief's locked `ImageBarrier`/`BufferBarrier`/
`PassBarriers`/`buildBarriers()` interface (which is unchanged).
`test_barriers.cpp` updated (`using rx::graph::detail::applyAccess;` /
`using rx::graph::detail::ResourceBarrierState;`) accordingly; no other
file references either name.

### Minor: report's deviation-2 count was wrong

Corrected above, in place: four tests use the "reader pass doubles as
present" pattern (`shadow-then-sample`, `hdr-tonemap`,
`compute-to-draw-buffer`, `culled-contributes-nothing`), not six; three of
those four needed the list-size relaxation (`compute-to-draw-buffer`'s
contamination lands in `imageBarriers`, a list that test never asserts
on).

### Minor: relaxed tests only proved "at least one" match

`shadow-then-sample`, `hdr-tonemap`, and `culled-contributes-nothing` now
assert `countMatchingImageBarriers(barriers, physicalIndex) == 1` before
locating and checking the barrier's six fields, so a hypothetical future
regression that duplicated the barrier under test while also dropping the
pass's own unrelated one can no longer slip past a bare list-size check.

### Verification

`ctest --preset linux-native -R rx_graph --output-on-failure`: pass.
Full doctest summary:

```
[doctest] test cases:  22 |  22 passed | 0 failed | 0 skipped
[doctest] assertions: 203 | 203 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Both presets rebuilt clean and the full project suite re-run on each:
`ctest --preset linux-native --output-on-failure` and `ctest --preset
windows-cross-zig --output-on-failure` (the latter's `rx_graph_tests.exe`
run under Wine) both report 10/10 passed.

### Files touched this round

- `src/rx_graph/include/rx_graph/barriers.h` (modified)
- `src/rx_graph/barriers.cpp` (modified)
- `src/rx_graph/tests/test_barriers.cpp` (modified)
- `.superpowers/sdd/2026-08-10-phase3-render-graph-materials/task-2-report.md`
  (this file, modified)

No change to `render_graph.h`/`render_graph.cpp`/CMakeLists this round --
`buildBarriers()`'s own signature and behavior for every previously-passing
test are unchanged; only the internal state machine and test-facing
namespace changed.

### Commit

See commit hash in the coordinator-facing status reply for this fix round.

### Remaining concerns

- The WAR-src-uses-union-of-readers departure from Granite's literal
  behavior (described above) is a deliberate, explicit call made to
  satisfy this fix round's own stated requirement and the original
  brief's ambiguity resolution, not an oversight -- but it means
  "faithfully ported from Granite" is now true for the invalidate/
  per-stage-visibility side of the model and *not* literally true for one
  specific WAR-src-selection detail, where a stricter (and, I'd argue,
  more correct) rule was substituted instead. Flagging this explicitly in
  case the coordinator wants it recorded differently or reconciled with
  Granite's literal behavior for some other reason I'm not seeing.
