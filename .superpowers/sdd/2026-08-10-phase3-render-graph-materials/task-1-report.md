# Task 1 Report: rx_graph declarations + compile front-half

## What was built

A new static library `rx_graph` (device-free), matching `rx_shader`'s
CMake/test conventions:

- `src/rx_graph/include/rx_graph/resources.h` -- `QueueClass`, `SizeClass`,
  `AttachmentDesc`, `BufferDesc`, `ResourceAccess`, `PhysicalResource`. Only
  `<vulkan/vulkan_core.h>` + std types.
- `src/rx_graph/include/rx_graph/pass.h` -- `Pass` (fluent declaration API:
  `addColorOutput`/`setDepthStencilOutput`/`addTextureInput`/
  `addStorageBufferOutput`/`addStorageBufferInput`/`setSideEffect`/
  `setExecute`/`name`/`queueClass`), constructed only by `RenderGraph`
  (private constructor + `friend class RenderGraph;`). `PassContext` is
  forward-declared only, per the brief.
- `src/rx_graph/include/rx_graph/render_graph.h` -- `CompileInfo`,
  `CompiledGraph`, `RenderGraph`. `RenderGraph` is pimpl'd (`struct Impl;
  std::unique_ptr<Impl> impl_;`) so the header stays clean and
  implementation stays entirely in `render_graph.cpp`, per the brief's
  ambiguity resolution.
- `src/rx_graph/render_graph.cpp` -- the compile algorithm (see below) and
  all `Pass`/`RenderGraph` method bodies.
- `src/rx_graph/CMakeLists.txt`, `src/rx_graph/tests/CMakeLists.txt`,
  `src/rx_graph/tests/doctest_main.cpp`, `src/rx_graph/tests/test_compile.cpp`.
- Root `CMakeLists.txt`: added `add_subdirectory(src/rx_graph)` after
  `rx_rhi_vk`, with a comment on why it does *not* link `rx_rhi_vk`.

`rx_graph` links only `Vulkan::Headers` and `rx_core` (for `RX_LOG_ERROR`
before every validation throw, matching `rx_shader`/`rx_rhi_vk`'s own
error-reporting convention). `rx_rhi_vk` is not linked -- Task 1 never
touches a `VkDevice`.

## Compile algorithm (render_graph.cpp)

Four passes over the declared graph, run inside `RenderGraph::compile()`:

1. **Collect writers**: iterate all passes in `addPass()` order, building a
   per-resource-name writer list and write-after-write dependency chain
   (later writer depends on the immediately preceding writer of the same
   name) -- this is the ambiguity resolution given ("declaration order
   defines the version chain").
2. **Resolve reads**: iterate all passes again, resolving every texture-
   input/storage-buffer-input declaration against the *complete* writer
   list from step 1 (a read depends on that resource's final declared
   writer, which transitively depends on the whole WAW chain). This is
   deliberately **order-independent with respect to reader-vs-writer
   declaration position** -- a reader may be declared before its writer in
   `addPass()` order (exercised by the `ordering` test's scrambled
   declaration order). Throws here if a read resolves to no writer at all.
3. **Cull + order**: reachability (DFS) from the roots (the backbuffer
   resource's final writer, plus every side-effect pass) over the producer
   edges built in steps 1-2; then Kahn's algorithm over the surviving
   subgraph, breaking ties by ascending raw pass index (declaration order)
   to satisfy "no reordering heuristics, stable among independents."
4. **Resolve resources + accesses**: rescans *only* survivors, in execution
   order, building `PhysicalResource` entries (name -> merged
   attachment/buffer desc, `imageUsage` union, first/last-use as
   *positions* in `executionOrder()`) and each surviving pass's resolved
   `ResourceAccess` list (stage/access/layout per the brief's table).
   Finally resolves `SwapchainRelative` sizes to absolute pixels using
   `CompileInfo`, and forces the backbuffer resource's format/extent/
   samples to mirror the swapchain unconditionally (see decisions below).

## Decisions taken (beyond what the brief pins down verbatim)

1. **Compute-class vs Graphics-class pass classification**: the brief's
   storage-buffer stage table splits on "Compute-class"/"Graphics-class"
   passes, but the `Pass` API has no explicit pipeline-kind flag
   (`QueueClass` is a queue *scheduling* hint per its own enum comment, not
   a pipeline-stage classifier). Resolved by classifying a pass as
   Graphics-class iff it declares at least one color/depth-stencil output,
   Compute-class otherwise. Implemented as private `Pass` member functions
   (`hasAttachmentOutput()`, `resolveAccess()`, `isWriteKind()`) rather than
   free functions in `render_graph.cpp`, because `AccessKind`/`Declaration`
   are private nested types of `Pass` -- a same-translation-unit free
   function is *not* automatically a friend, so this was the actual fix for
   a real compile error hit during implementation (see Test results).
2. **Backbuffer resource's resolved shape always mirrors the swapchain**:
   `compile()` overwrites the backbuffer's `PhysicalResource::attachment`
   (format/size/samples) from `CompileInfo` unconditionally, regardless of
   what `AttachmentDesc` its writing pass declared. This is what gives
   `CompileInfo` an actual reason to exist in a device-free compile step,
   and is covered by its own test.
3. **Validation timing**: all four validation errors (no backbuffer source,
   duplicate pass names, unread-write resource, unwritten backbuffer
   source) are raised inside `compile()`, not eagerly in `addPass()`/
   `setBackbufferSource()` -- matches the brief's "Validation errors are
   exceptions" framing, which groups all four under compile()'s behavior.
   Only two of the four are in the brief's *exact* required test list
   (unwritten read, unwritten backbuffer source); the other two (no
   backbuffer source set, duplicate names) are implemented and tested as
   bonus coverage since the algorithm doc lists all four.
4. **Self-edge guards**: defensively skip adding a write-after-write or
   read-dependency edge when the resolved producer is the reading/writing
   pass itself (e.g. two `addColorOutput()` calls for the same name within
   one pass) -- untested edge case, but a real correctness hazard for
   Kahn's algorithm (a self-loop pass would never reach indegree 0)
   otherwise.
5. **`PhysicalResource::imageUsage` never gets `TRANSFER_SRC`**: the field
   comment mentions "+ TRANSFER_SRC for readback targets," but Task 1's
   `Pass` API has no readback/screenshot declaration to derive that bit
   from -- left unimplemented with a comment explaining why, not silently
   dropped.

## Test results

TDD was followed literally: wrote `test_compile.cpp` + full CMake wiring
first, then temporarily replaced `render_graph.cpp` with an empty stub and
confirmed the expected **red** state -- clean compile against the headers,
then link failure (`undefined symbol: rx::graph::RenderGraph::~RenderGraph()`
and 8 other symbols) -- before restoring the real implementation.

Restoring the implementation initially hit a real compile error (not
anticipated until it happened): the three algorithm helpers were first
written as free functions in an anonymous namespace in `render_graph.cpp`,
which cannot name `Pass`'s private nested types (`AccessKind`, `Declaration`)
even from the same translation unit -- fixed by moving them to private
`Pass` member functions (see Decision 1 above), after which the build went
green.

Final green state:

```
ctest --preset linux-native -R rx_graph --output-on-failure
Test project /media/ywadi/second/renderer_x/build/linux-native
    Start 6: rx_graph_tests
1/1 Test #6: rx_graph_tests ...................   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

doctest detail: `[doctest] test cases: 11 | 11 passed | 0 failed | 0 skipped`,
`assertions: 55 | 55 passed | 0 failed`. The 11 cases cover the six required
scenarios (culling, ordering, lifetimes, usage-union, diamond, errors) plus
five bonus cases (Pass accessors + reset(), backbuffer shape override,
non-backbuffer swapchain-relative resolution, compute-vs-graphics storage
buffer stage classification, culled-pass empty accesses).

Full-suite regression check, `ctest --preset linux-native` (all 10
targets, including everything from Phases 1-2): **100% passed** --
`shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`, `rx_shader_tests`,
`rx_rhi_vk_tests`, `rx_graph_tests`, and all four sample headless gates.

Both presets build clean:
- `cmake --preset linux-native && cmake --build --preset linux-native` --
  clean build, zero warnings on the new files even under a manual
  `g++ -Wall -Wextra -Wpedantic -Wshadow` recompile.
- `cmake --preset windows-cross-zig && cmake --build --preset windows-cross-zig`
  -- `rx_graph`/`rx_graph_tests` build and link clean via the zig/LLD COFF
  toolchain; full preset build otherwise unaffected (`ninja: no work to do`
  for everything already built).

## Files touched

- `CMakeLists.txt` (root) -- added `add_subdirectory(src/rx_graph)`.
- `src/rx_graph/CMakeLists.txt` (new)
- `src/rx_graph/include/rx_graph/resources.h` (new)
- `src/rx_graph/include/rx_graph/pass.h` (new)
- `src/rx_graph/include/rx_graph/render_graph.h` (new)
- `src/rx_graph/render_graph.cpp` (new)
- `src/rx_graph/tests/CMakeLists.txt` (new)
- `src/rx_graph/tests/doctest_main.cpp` (new)
- `src/rx_graph/tests/test_compile.cpp` (new)

## Commit

`16ccad4863eca3d285cc185c7a8d871e8f2f5c87` -- "feat: add rx_graph pass
declarations and compile front-half" (author: local git config, Yousef
Wadi <ywadi85@gmail.com>; no Co-Authored-By or AI attribution of any kind
-- verified directly against `git log -1` output and a grep for
claude/anthropic/co-authored-by strings across the changed files before
committing). Committed to `main` locally; **not pushed**, per instructions.

Only the code deliverable was staged and committed (`CMakeLists.txt` +
`src/rx_graph/`) -- `progress.md` and `task-1-brief.md` in this materials
directory were left untracked, as ledger bookkeeping outside this task's
scope.

## Concerns

- The "Compute-class vs Graphics-class" pass classification (Decision 1) is
  an inference from the brief's prose, not a literal API field -- worth
  confirming with whoever writes Task 2/3's briefs (and rx_material's
  pass-signature derivation, which reads pass declarations too) that
  deriving pipeline kind from "has an attachment output" is the intended
  long-term rule, in case a future compute pass legitimately needs a color
  attachment for some reason (e.g. a compute pass that also writes an image
  via `imageStore` semantics modeled as a color output -- not something
  Task 1's test suite exercises either way).
- No other known gaps against the brief's exact interfaces/tests. All
  six required test scenarios pass with the exact expectations given
  (culling/ordering/lifetimes/usage-union/diamond numeric results all
  match the brief's worked example verbatim).

## Fix round 1

Applied the review's Important finding plus the coordinator's two rulings
on the Minor findings (`task-1-review.md`).

### 1. Cycle detection (Important, must-fix)

`compile()` previously had no cycle check: since a reader may legally be
declared before its writer (the exact mechanism the `ordering` test
exercises), a 2-pass cycle reachable from the backbuffer's writer used to
compile to a silently empty `executionOrder()` with the backbuffer resource
simply absent from `resources()` -- no exception, no diagnostic anywhere.

Root cause (confirmed against the review's own trace): Kahn's algorithm's
`ready` set starts empty whenever every reachable pass has indegree > 0,
so the `while (!ready.empty())` loop body never runs for a fully-cyclic
reachable subgraph, and nothing downstream noticed the mismatch.

Fix: track `reachableCount` (incremented alongside the existing
reachability DFS) and compare it to `executionOrder.size()` right after
Kahn's algorithm. On a mismatch, a second DFS -- this time with a
recursion-stack marker (`visitState`: unvisited/on-path/done) over the
same `dependsOn` producer edges, restricted to the reachable subgraph --
finds a genuine back-edge (a producer still on the current DFS path) and
reports the exact cycle suffix from that revisited pass to the end of the
path, not just "some pass never got emitted" (which could name a pass
merely *downstream* of the cycle, e.g. the "present" pass in the new test,
rather than an actual cycle member -- verified this distinction matters
by working through the two-independent-writers-plus-cycle case by hand
before writing the DFS).

New test `"a circular pass dependency throws, naming passes in the
cycle"`: A reads "y"/writes "x", B reads "x"/writes "y" (the 2-cycle),
"present" reads "x"/writes "bb" (roots it to the backbuffer). Asserts the
throw and that the message names both "A" and "B". Observed message:
`rx_graph: dependency cycle detected among passes: 'A' -> 'B' -> 'A'`.

### 2. Texture-input stage split by pass kind (mapping extension, per coordinator ruling)

The brief's own stage/access table gave texture-input access one fixed
`FRAGMENT_SHADER` row with no Compute-class variant, unlike the
storage-buffer rows. Per the coordinator's ruling accepting the existing
attachment-output-based Compute/Graphics classification as the intended
rule, extended the identical split to texture inputs:
`computeClass ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT :
VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT` in `Pass::resolveAccess()`;
access (`SHADER_SAMPLED_READ`) and layout (`SHADER_READ_ONLY_OPTIMAL`) are
unchanged and identical for both classes, per the ruling.

New test `"texture input stage resolves per pass kind, like storage
buffers already did"`: a Compute-class pass ("sampler", no attachment
output) reading a texture resolves `COMPUTE_SHADER`; a Graphics-class pass
("present", has a color output) reading the same resource resolves
`FRAGMENT_SHADER` -- both asserted in one test case against the same
underlying resource.

### 3. `CompiledGraph` class-comment tightened (Minor)

The class-level comment used to say an unpopulated `CompiledGraph` "simply
reports empty spans and treats every pass index as absent," which reads as
implying a graceful non-throwing query -- inconsistent with the correct,
already-accurate per-method comments on `isCulled()`/`passAccesses()`
("Out-of-range `passIndex` throws `std::out_of_range`"). Reworded the
class comment to state the split precisely: `executionOrder()`/
`resources()` report empty spans pre-`compile()`, but `isCulled()`/
`passAccesses()` throw `std::out_of_range` for any index at all. No
behavior changed -- doc-only fix.

### Verification

```
ctest --preset linux-native -R rx_graph --output-on-failure
Test project /media/ywadi/second/renderer_x/build/linux-native
    Start 6: rx_graph_tests
1/1 Test #6: rx_graph_tests ...................   Passed    0.00 sec
100% tests passed, 0 tests failed out of 1
```

doctest detail: `test cases: 13 | 13 passed | 0 failed`, `assertions: 66 |
66 passed | 0 failed` (up from 11 cases / 55 assertions -- the two new
cases above). Full-suite regression (`ctest --preset linux-native`, all 10
targets): 100% passed, no change to any pre-existing test. Manual strict
recompile (`g++ -Wall -Wextra -Wpedantic -Wshadow`) of the modified
`render_graph.cpp`: zero warnings. Both presets rebuilt clean
(`cmake --build --preset linux-native` / `--preset windows-cross-zig`,
targeted at `rx_graph_tests` then a full preset build -- `ninja: no work
to do` confirming nothing else was disturbed).

### Files touched (fix round 1)

- `src/rx_graph/include/rx_graph/render_graph.h` -- `CompiledGraph` class
  comment (fix 3).
- `src/rx_graph/render_graph.cpp` -- cycle detection (fix 1), texture-input
  stage split (fix 2).
- `src/rx_graph/tests/test_compile.cpp` -- two new test cases.

### Commit

`089af6858581e356f343a38c0a87304c1becf32e` -- "fix: detect render graph
pass dependency cycles and split texture-input stage by pass kind" (author:
local git config, Yousef Wadi <ywadi85@gmail.com>; no AI attribution --
verified via `git log -1` and a grep for claude/anthropic/co-authored-by
across the changed files before committing). Committed to `main` locally;
**not pushed**.

### Concerns

None outstanding. The review's Important finding is resolved with a real
cycle-finding DFS (not just a "some pass never ran" heuristic), both
Minor findings are resolved per the coordinator's rulings, and no
regressions were introduced (full suite + both presets green).
