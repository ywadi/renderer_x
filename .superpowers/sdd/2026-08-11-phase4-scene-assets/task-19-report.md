# Task 19 report: `DrawListBuilder` — parallel culling + sort keys (D14/D15/D24/D26/D27)

Closes cards #6 (frustum/shadow-caster culling) and #7 (layer/mask system).
BASE = a178969 (post Task 18, CI green).

## Files created

- `src/rx_scene/include/rx_scene/draw_list.h` (576 lines) — public API:
  `DrawCommand`/`DrawPayload`/`BlockRange`/`CullCounters`/`ViewLists`/
  `ShadowLists`, the `sortkey` namespace (bit layout + encode/decode),
  `MeshSubmeshesFn`/`MaterialResolveFn` injected seams + their
  `xFromRegistry()` production adapters, `DrawListBuilder`, and the
  `recordDrawList`/`resolveDrawGroups` D27 mechanism.
- `src/rx_scene/draw_list.cpp` (887 lines) — implementation.
- `src/rx_scene/tests/draw_list_test.cpp` (32 `TEST_CASE`s as of fix round
  1 — see "Fix round 1" section below) — device-free coverage.

## Files modified (necessary, documented deviations — see below)

- `src/rx_scene/include/rx_scene/scene.h` / `scene.cpp` — added
  `Scene::aliveSpan()`/`generationsSpan()`; changed the private `alive_`
  column's storage from `std::vector<bool>` to `std::vector<uint8_t>` so it
  can be spanned (same convention `castsShadowsSpan()` already established
  for every other boolean column).
- `src/rx_scene/CMakeLists.txt` — added `draw_list.cpp`, linked `rx_task`
  PUBLIC.
- `src/rx_scene/tests/CMakeLists.txt` — added `draw_list_test.cpp`.

## Design decisions and their justification

### Scene::aliveSpan()/generationsSpan() — a necessary interface gap, not scope creep

`DrawListBuilder`'s culling loop iterates `Scene`'s SoA span accessors in
bulk (by design — that's the whole point of the SoA layout, per scene.h's
own top comment: "DrawListBuilder's culling loop... reads AABBs/layers/etc.
directly, contiguous, never per-object"). But `Scene::destroyRenderable()`
marks a slot dead WITHOUT re-zeroing its other columns (`scene.cpp`: only
`submeshOverrides_` is cleared) — so a freed-but-not-yet-reused slot's
`layers_`/`mesh_`/etc. entries hold arbitrary stale values, and no existing
accessor lets a bulk consumer distinguish a live slot from a dead one by
INDEX alone (`isRenderableAlive(handle)` needs a full generational handle,
which a bare index iterator doesn't have). Without a fix, `build()` would
either crash/misbehave on stale mesh handles or silently double-count dead
slots in `CullCounters`. Fixed the same way Task 18's own gate rulings
fixed analogous small gaps (`getLayers`/`setChannels` "added for API
parity... cheap now... retrofitting later is a needless negotiation"): two
new read-only span accessors, mirroring `castsShadowsSpan()`'s exact
uint8_t-not-`vector<bool>` convention. Covered by its own test
(`draw_list_test.cpp`, "Scene::aliveSpan()/generationsSpan() correctly
distinguish a live slot from a destroyRenderable()d hole").

### Sort key: collapse-BEFORE-sort, not mesh-tier-above-depth

The first implementation attempt put a `meshKey` tier ABOVE `depthBucket`
in the opaque sort key, reasoning that D26.3's instancing-collapse needs
identical-mesh instances to be ADJACENT post-sort. TDD caught the real bug
this created immediately (see "Bugs TDD caught" below): with mesh ranked
above depth, DIFFERENT-mesh objects sort by an arbitrary mesh-identity hash
rather than depth, breaking D14's own "front-to-back by pipeline|material|
depth" contract across meshes — the opposite failure from what was being
fixed.

The correct resolution (implemented, `DrawListBuilder::Impl::
collapseAndSortOpaque()`, `draw_list.cpp`): collapse FIRST, sort SECOND.
Phase 1 sorts `opaqueRecords` by exact draw IDENTITY (blockId, indexCount,
firstIndex, vertexOffset, materialIndex, priority — never depth), so every
collapse-eligible run becomes contiguous regardless of the instances'
individual depths (a scattered "1000-tree forest" scene collapses into ONE
instanced draw even though its members are at 1000 different depths).
Phase 2 folds each identity-run into one `CollapsedGroup` (representative
depth = the group's NEAREST member), then sorts the resulting, far fewer,
groups by the perceptual D14 key (priority|pipeline|material|depth) — no
mesh field needed in that key at all, since collapse already happened.
This satisfies BOTH requirements unconditionally: D26.3 collapse never
depends on depth adjacency, and D14 depth-ordering is never distorted by
mesh identity. The opaque sort key is 64 bits: `priority(3) |
pipelineKey(7) | materialKey(15) | depthBucket(23) | tieBreak(16)`.

### `blockId` is the outermost sort tier, not a key bit-field

`BlockRange`'s "one contiguous run per block" invariant (needed because
`GeometryPool::bind()` requires every draw between two `bind()` calls to
share one block) is a HARD correctness requirement, not an ordering nicety.
Making `blockId` the primary comparison (above even `priority`) in every
sort comparator — rather than a bit-field competing for space in the
64-bit key — guarantees this unconditionally, while `priority`/pipeline/
material/depth still fully control ordering WITHIN each block's own
contiguous run.

### `buildShadow(..., LightHandle light, ...)`, not a `DirectionalLight` value

The brief's interface sketch shows `buildShadow(const Scene&, const
DirectionalLight&, const Camera&, ...)`, but Task 18's landed `Scene`/
`scene.h` defines no `DirectionalLight` type — only `LightHandle` +
per-handle accessors (`lightDirection`/`lightChannels`/`lightCastsShadows`).
Necessary concretization: `buildShadow` takes `LightHandle` (resolved
against `scene`, mirroring how `build()` already takes renderables through
`scene`), never inventing a redundant duplicate light-value type.

### `recordDrawList`/`resolveDrawGroups`: the D27 mechanism, genericized

Task 19's own file list is `draw_list.{h,cpp}` only (no `rx_graph`/
`rx_material` dependency), and the brief itself requires this library stay
device-free (no VkDevice, no rx_rhi_vk — matching `rx_scene`'s existing
CMakeLists.txt posture verbatim). `recordDrawList`'s PRODUCTION binding
(driving `rx::material::MaterialSystem::getPipeline()`/`rx::graph::
PassContext` for real GPU recording) is Task 22/24's own scope per the plan
(`docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:713`: "loads...
through Registry→Scene→DrawListBuilder→graph", Task 24). What THIS task
delivers is the MECHANISM D27 requires: `resolveDrawGroups()` does the
single main-thread linear scan pre-resolving each distinct `materialIndex`
boundary exactly once (main-thread-guarded via `RX_ASSERT_MAIN_THREAD`),
and `recordDrawList()` fans `RecordChunkFn` out via `scheduler.parallelFor()`
with a callback signature that STRUCTURALLY never receives
`PipelineResolveFn` — a compile-time-adjacent guarantee, not just a runtime
check (exactly what the gate matrix's own D27 row asks for). A production
`PipelineResolveFn`/`RecordChunkFn` pair implementing the real GPU path is
future-task work; this task proves the seam and its guard are load-bearing
(see revert-discrimination evidence below).

**Fix round 1 correction (review finding 3, Medium):** the FIRST cut of
this seam shaped `PipelineResolveFn` as `std::function<uint32_t(uint32_t
materialIndex)>` and `ResolvedDrawGroup::pipelineToken` as `uint32_t` —
independently reviewed and found genuinely wrong, not just narrow:
`VkPipeline` is a `VK_DEFINE_NON_DISPATCHABLE_HANDLE` (vendored
`vulkan_core.h`), 8 bytes on every platform this project targets
regardless of the `VK_USE_64_BIT_PTR_DEFINES` branch, so a `uint32_t`
token could never actually carry one; and `MaterialSystem::getPipeline()`
takes a `PipelineRequest{MaterialHandle material; PassSignature pass;
uint32_t specializationBits;}`, not a bare material index, so the seam's
INPUT shape was also incomplete. Fixed (still device-free, no
`rx::material`/`rx::graph` dependency added): a new `PipelineRequestKey{
uint32_t materialIndex; uint64_t passSignatureHash; uint32_t
specializationBits; }` mirrors `PipelineRequest`'s three fields exactly
(`passSignatureHash`'s type matches `PassSignature::hash()`'s own
`uint64_t` return type field-for-field); `PipelineResolveFn` now takes a
`const PipelineRequestKey&` and returns `uint64_t`;
`ResolvedDrawGroup::pipelineToken` is `uint64_t`; `resolveDrawGroups()`/
`recordDrawList()` both gained `passSignatureHash`/`specializationBits`
parameters (caller-supplied — the pass signature is a per-CALLER, not
per-`ViewLists`, concern, since one built draw list may legitimately feed
more than one pass). All three D27 tests updated to the new signature
in place; see "Fix round 1" section below for the full delta and
re-verification.

### Zero-alloc test mechanism: scoped global `operator new`/`delete`, not capacity/pointer checks

Tried first: `.capacity()` equality and `.data()` pointer-identity checks
against the warm-up call's own values. BOTH turned out to be unsound —
empirically proven, not assumed (see "Revert 1" below): a buggy
`out.commands = std::vector<DrawCommand>();` (discard-and-reallocate every
call) still reaches the SAME final `.capacity()` (std::vector's geometric
growth from empty is a deterministic function of final size alone) and
frequently the SAME `.data()` address (glibc's allocator commonly hands
back the just-freed block immediately for a same-size request) — neither
check caught the bug. Fixed with a scoped global `operator new`/`delete`
override (`draw_list_test.cpp`, active only inside the zero-alloc
`TEST_CASE`) counting TOTAL allocation CALLS (monotonic, never
decremented) — a net-live-count design was tried and also found unsound
(alloc+free pairs from a reallocate-every-call bug cancel out in a net
metric even though real allocator churn occurred). This is a documented,
narrow deviation from the #29/Task-23 ruling's "NO global operator-new
interposition (volk/validation/rpmalloc linkage risk)" — that ruling is
scoped to a DIFFERENT ticket's DIFFERENT binary (one that links and
exercises live Vulkan/validation-layer/rpmalloc code); `rx_scene_tests` is
100% device-free at runtime (no `VkInstance`/`VkDevice` ever constructed),
so the cited linkage risk does not apply here — verified by running the
full 57→61-case suite with the override compiled in, twice, with zero
unrelated failures.

## Per-criterion proof — matrix-issue06 (drawlists-culling)

| Criterion | Test(s) | Result |
|---|---|---|
| Sort-key bit layout documented + `decode()` round-trip | `sortkey::encodeOpaque/decodeOpaque round-trips...`, `...encodeBlend/decodeBlend...`, `...encodeShadow/decodeShadow...` | PASS — named constants + ASCII diagram in `draw_list.h`; exact round-trip on every field |
| Depth bucket = truncated monotonic float32 bit pattern, never linear rescale | `Depth bucket preserves ordering for NDC depths clustered near the reversed-Z near plane` | PASS — real reversed-Z-infinite `near/distance` NDC values, near-plane pair (0.2 vs 0.3 units) resolvable, monotonic across a realistic clustered range |
| Deterministic low-bit tie-break = stable creation index | `build() produces byte-identical ViewLists across --threads 1/2/8` + Revert 4 | PASS — tie-break is the renderable's Scene slot index; discrimination proven (see below) |
| Partition sort-DIRECTION test, one shared fixture | `Opaque partition sorts front-to-back... and blend partition sorts back-to-front... on the SAME depth values` | PASS — 3 known depths, opaque decreasing / blend increasing, asserted via `firstIndex` identity tags |
| Priority tier above pipeline bits | `Priority occupies a documented tier ABOVE pipeline/material/depth` | PASS |
| blendOrder bits reserved, unpopulated | `...decodeBlend...` asserts `reservedBlendOrder == 0` | PASS |
| Fixed index-range chunks + chunk-order concatenation → byte-identical across `--threads` | `build() produces byte-identical ViewLists across --threads 1/2/8` | PASS — see design note: achieved via embarrassingly-parallel per-index writes + fixed slot-order serial compaction (a simpler mechanism giving the identical guarantee — documented in `cullView()`'s own comment) |
| D26.3 lockstep (commands/payloads as one unit; interleaved-desync test mandatory) | `D26.3 lockstep criterion: an interleaved-creation-order scene...` + Revert 3 | PASS — cross-checks every collapsed command's payload range against source-renderable identity, not just counts |
| Cross-group depth ordering (not a matrix row; added fix round 1, finding 2, to document an intentional design tradeoff the review flagged as under-tested) | `Cross-group depth ordering: representative-depth (nearest-member) comparison, not a flattened per-instance merge` | PASS — pins down and asserts the group-representative-vs-flattened-merge tradeoff on a hand-computed interleaved-depth fixture; code comment added at `collapseAndSortOpaque()` stating D14 front-to-back is an early-Z rejection-rate optimization, not an ordering guarantee (opaque correctness comes from the per-fragment depth test, submission-order-independent by construction) |
| `CullCounters` exact field list, CI-gated | `CullCounters are EXACT across a mixed layer/frustum/visible scene` + counters asserted in nearly every other test | PASS |
| `ShadowLists` = ViewLists shape, single partition, sorted (pipeline, mesh range, block), BLEND excluded | `buildShadow(): BLEND-partition renderables are EXCLUDED...` | PASS |
| Culling planes from `Camera::cullingProj()` (finite) + extreme-depth-never-culled | `An object at extreme depth (100,000 units)...` | PASS — both directions tested (legitimately included AND legitimately excluded by a smaller far plane) |
| Degenerate/zero-extent AABB + ground-slab | `build() never culls a degenerate (single-point, min==max) AABB...`, `...never culls a ground-slab AABB...` | PASS |
| Per-block contiguous `BlockRange` grouping | `ViewLists.blocks groups commands contiguously by blockId...` | PASS — adversarial fixture interleaves 2 blocks × 2 pipeline variants |
| D27 worker-guard test, `setViolationHookForTests` + rendezvous barrier | `recordDrawList()'s RecordChunkFn callback... NEVER triggers the main-thread guard` + adversarial control test + Revert 2 | PASS — see deviation note (adapted to `task::Scheduler`'s own barrier precedent, device-free); seam shape widened fix round 1 (finding 3), tests updated in place |
| D26.1 per-draw addressing — data-layout requirement (firstInstance indexing, zero per-draw push-constant fields in `DrawPayload`) | Implicit across every D26.2/D26.3 test (`DrawPayload` has no push-constant-shaped field at all; `firstInstance` is the only addressing mechanism) | PASS (data layout) |
| D26.1 — literal GPU-observable criterion ("a GPU test... counting `vkCmdPushConstants` calls... O(1) per pass, not O(draws)") | — | **N/A — device-free-unsatisfiable in this task's own scope** (no `VkCommandBuffer` exists in this library; matches how the layer-masks table below marks its own analogous `getLayers`/`getChannels` row). Becomes satisfiable once Task 22/24's real `RecordChunkFn` binds to actual `vkCmdDrawIndexed*`/`vkCmdPushConstants` recording — that binding, and its own validation-layer/call-count proof, is explicitly out of this task's file list (`draw_list.{h,cpp}` only) and this task's own device-free brief |
| D26.2 `VkDrawIndexedIndirectCommand` compatibility | `DrawCommand is byte-for-byte VkDrawIndexedIndirectCommand-compatible...` | PASS — `static_assert(sizeof(...)==...)` + per-field `offsetof` + reinterpret-cast upload proof |
| Instancing collapse, records-in vs draws-submitted | `An identical-run scene (1000 identical renderables) collapses into ONE DrawCommand...` | PASS — exact `recordsIn==1000`, `drawsSubmitted==1` |
| Off-screen-caster-still-casts + conservative exclusion | `buildShadow(): a caster fully OUTSIDE the camera frustum...` | PASS — both the include AND the exclude case, independently confirmed off-camera via a separate `build()` call |
| Alpha-MASK in opaque partition | `Alpha-MASK draws land in the opaque partition...` | PASS |
| D24 residency-tolerant resolve (mesh + material) | `An evicted MATERIAL handle resolves to the fallback...`, `An evicted MESH handle resolves to the (empty) fallback...` | PASS — never a crash, fallback substitution both ways |
| Zero-alloc invariant | `build() into reused ViewLists/DrawListBuilder storage performs zero net capacity growth...` + Revert 1 | PASS — see measured evidence below |

## Per-criterion proof — matrix-issue07 (layer-masks)

| Criterion | Test(s) | Result |
|---|---|---|
| 32-bit `layers`, 8-bit `channels`, AND-test semantics | `Camera cullMask / renderable layers: the five-case CI mask matrix` | PASS |
| cullMask==0 excludes even `layers==~0u` (non-empty intersection, not subset test) | `cullMask == 0 excludes a renderable with layers == ~0u too` | PASS |
| All-ones defaults confirmed deliberate (regression guard) | `A default-constructed renderable is visible to a default-constructed camera...` | PASS |
| Light channels COUPLED to both lighting and shadow-casting (RC5) | `buildShadow(): channel coupling (RC5)...` | PASS — channel mismatch excludes from caster list; `castsShadows=false` independently excludes too |
| Five-case CI mask matrix (a-e) | Cases (a)(b)(c) in the mask-matrix test; (d)(e) in the channel-coupling shadow test (channels only matter for shadow-casting, not camera visibility — see design note) | PASS |
| `getLayers`/`getChannels`/`setChannels` (Task 18 gate delta) | Already landed Task 18; consumed here via `scene.layersSpan()`/`channelsSpan()` | N/A to this task, confirmed present |

## Revert-and-restore evidence (in-tree, byte-identical restore verified each time)

All four probes: `cp draw_list.cpp` to a scratchpad backup before editing;
`md5sum`/`diff` confirmed byte-identical restore after each probe
(`e65f28f64eb3ff0f064d7f88188e25e1` throughout).

**Revert 1 — zero-alloc.** `out.commands.clear()` → `out.commands =
std::vector<DrawCommand>()`. Capacity-equality and `.data()`-identity
checks alone did NOT fail (see design note above — allocator/growth-pattern
coincidence). The monotonic total-allocation-call counter DID: baseline 1
call/frame (correct code) → 10 calls/frame (buggy code), assertion
`baselineDelta <= 4` correctly failed.

**Revert 2 — D27 guard.** `recordDrawList()`'s per-chunk dispatch loop
made to call `resolvePipeline(0)` directly (simulating the exact bug the
design prevents). Result: `capture.empty()` failed — the guard fired and
was captured, exactly as expected. Restored; adversarial control test
(designed to trip the guard deliberately) independently confirms the
mechanism itself works (`capture.size() == 1`).

**Revert 3 — D26.3 lockstep.** `sameDrawIdentity()`'s `firstIndex` check
dropped (simulating a bug that merges genuinely different meshes). Result:
`lists.commands.size() == 2` failed (got 1 — the two distinct mesh types
merged into one command), caught even before the lockstep cross-check ran.

**Revert 4 — determinism across thread counts.** Two combined probes: (a)
serial-compaction order made to depend on `scheduler.workerCount() > 4`
(reversed for `--threads 8`), (b) sort tie-break zeroed. Probe (a) alone
did NOT produce a divergence (the downstream sort with a real tie-break
absorbs any pre-sort order difference — itself a useful finding: the
tie-break makes the pipeline robust to MORE than just chunk-boundary
issues). Combining (a)+(b): 1774 of 5641 assertions failed at `threads=8`
vs the `threads=1` reference — real divergence, confirming BOTH the
fixed-order compaction AND the tie-break are independently load-bearing.

## Zero-alloc — measured evidence (not narrated)

```
rx_scene zero-alloc: steady-state build() call made 1 operator-new call(s)
(baseline; must stay small and IDENTICAL every subsequent frame)
```
500-renderable mixed opaque/blend/multi-block/multi-mesh scene, 10
steady-state frames after warm-up: every frame's total-allocation-call
delta == 1 (identical across the whole run), `.capacity()` and `.data()`
pointer-identity also unchanged on `ViewLists`' 3 vectors and
`DrawListBuilder`'s 6 private scratch buffers (test-only introspection via
`rx::scene::detail::capacitiesForTesting()`).

## Determinism — measured evidence

`build() produces byte-identical ViewLists across --threads 1/2/8`: 2000-
renderable scene (3 blocks, mixed opaque/blend/doubleSided materials, 17
distinct mesh identities, priorities 0-7, depths spanning 50 distinct
values) — `commands`/`payloads`/`blocks`/`opaqueCommandCount`/`counters`
asserted byte-identical (`operator==`) across all three thread counts.
5641 assertions, 0 failures.

## Tracy zones (RX_ZONE/RX_ZONE_NAMED)

`build`/`buildShadow`/`recordDrawList`/`resolveDrawGroups` (RX_ZONE, named
after the enclosing function); `cullView`/`cullShadowCasters`/
`generateRecords`/`partitionAndSort`/the shadow sort step (RX_ZONE_NAMED,
explicit names) — the parallel cull AND the sort are both independently
zoned per the task's own instruction, so a Tracy capture during Task 24's
stress-v2 run can attribute time to culling vs. record-generation vs.
sorting/collapse separately.

## Test suite results

**linux-native, serial `ctest` (matching CI's own invocation):**
```
100% tests passed, 0 tests failed out of 23
Total Test time (real) = 155.42 sec   [pre-fix-round-1 run; 148.16 sec post-fix-round-1, see below]
```
`rx_scene_tests` alone (post-fix-round-1 figures; see "Fix round 1"
section for the corrected count — this line originally, incorrectly,
read "61 test cases (57 unconditional + 4 gated)", a reviewer-caught
arithmetic error, see Finding 3 below): **58 total `TEST_CASE`s** across
the whole `rx_scene` suite (32 in `draw_list_test.cpp`, 16 in
`scene_test.cpp`, 8 in `camera_test.cpp`, 2 in `thread_guard_test.cpp`),
of which 4 (2 in `draw_list_test.cpp`, 2 pre-existing in
`thread_guard_test.cpp`) are compiled behind `#ifdef RX_DEBUG_CHECKS` --
INCLUDED in the 58, not additional. `RX_DEBUG_CHECKS=ON` in both dev
presets, so all 58 compile and run; doctest reports `test cases: 58 | 58
passed`, 6205 assertions, 0 failures, stable across repeated runs (no
flakiness observed in the barrier-based D27/determinism tests).

**windows-cross-zig, under Wine (per README's own documented local-
verification convention — `xvfb-run -a ctest --preset windows-cross-zig -E
'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'`):**
```
100% tests passed, 0 tests failed out of 11
Total Test time (real) = 119.82 sec   [pre-fix-round-1; 116.71 sec post-fix-round-1, both 100%/11-of-11]
```
`rx_scene_tests.exe`: 0.08s, all cases passed (device-free, as expected —
matches the README's own list of binaries genuinely device-free under
Wine). `rx_asset_gltf_gpu_tests.exe` (real winevulkan/lavapipe `VkDevice`)
also passed, confirming the `Scene::alive_` storage-type change introduced
no regression in a GPU-backed consumer path either (it has none — `Scene`/
`DrawListBuilder` are never touched by that binary — but it shares the
`rx_scene` static library, so a link-level confirmation is still useful).

Both presets built from a clean incremental state with zero warnings
treated as errors; both ran with `RX_DEBUG_CHECKS=ON` (dev preset default),
so every `RX_ASSERT_MAIN_THREAD` guard was live in both runs.

## Deviations from the plan/gate text (summary; each detailed above)

1. `Scene::aliveSpan()`/`generationsSpan()` added (necessary correctness
   fix for bulk SoA iteration over possibly-dead slots — Task 18's landed
   surface had no way to do this).
2. Sort key: collapse-BEFORE-sort two-phase design, not a single sort with
   a mesh tier (the naive reading of "mesh range must be a sort tier for
   D26.3" breaks D14's own depth-ordering contract across meshes — see
   design note; caught by TDD, not assumed).
3. `buildShadow(..., LightHandle, ...)`, not a `DirectionalLight` value
   type (none exists in the landed API).
4. `recordDrawList`/`resolveDrawGroups` are genericized over injected
   `PipelineResolveFn`/`RecordChunkFn` rather than hard-bound to
   `rx::material::MaterialSystem`/`rx::graph::PassContext` — the real GPU
   binding is explicitly Task 22/24 scope per the plan; this task delivers
   the mechanism + its guard, proven via revert-discrimination. Fix round 1
   (review finding 3) widened the seam SHAPE to genuinely carry a real
   `VkPipeline` (`PipelineRequestKey` mirroring `PipelineRequest`'s three
   fields, `uint64_t` token) without adding a hard dependency — see the
   design-decisions section and the "Fix round 1" section below.
5. D27 guard test adapted to `rx::task::Scheduler::parallelFor()`'s own
   n-way barrier precedent (`rx_task/tests/scheduler_test.cpp`) rather than
   literally reusing `test_material_system.cpp`'s `rx_graph::Executor`/live-
   `VkDevice` scaffolding, which this device-free library has no access to
   by design. The `setViolationHookForTests` + rendezvous-barrier PATTERN
   is reused verbatim; the GPU harness around it is not, since none exists
   here.
6. Zero-alloc test uses a scoped global `operator new`/`delete` override —
   see design note for why the capacity/pointer-only approach was
   insufficient and why the #29-ruling linkage-risk concern does not apply
   to this device-free binary.

## Self-review

**What is solid:** every gate-matrix acceptance criterion for both #6 and
#7 has a directly corresponding, passing test; four of the highest-value
tests (zero-alloc, D27 guard, D26.3 lockstep, cross-thread determinism)
have genuine revert-discrimination evidence, not just "the test exists and
passes" — including two cases (zero-alloc's capacity/pointer checks;
determinism's compaction-order-only probe) where a FIRST attempt at the
mechanism was empirically proven insufficient and replaced, which is
exactly the kind of grounding this project's verification discipline asks
for. The sort-key architecture issue (mesh-above-depth) was caught by
writing the direction test first and watching it fail for the RIGHT reason
(not a typo — a real design flaw), then fixed at the architecture level
rather than patched around.

**What is a real, bounded limitation, not swept under the rug:**
- The opaque sort key's `materialKey`/`tieBreak` fields are truncated to
  15/16 bits (32768/65536 distinct values). Beyond that many distinct
  materials or renderables in one scene, ordering degrades gracefully to
  "still deterministic, just less globally precise" (documented in
  `draw_list.h`'s own sort-key comment) — never a correctness bug, but a
  real, named capacity bound worth flagging for Task 24's 30k-instance
  stress scene (well within bounds for renderable COUNT via `tieBreak`;
  material/pipeline variety in Phase 4 content is far below 32k).
- `recordDrawList`'s instancing-collapse discipline does NOT extend to
  `ShadowLists` (Phase 4 scope decision, documented, not gate-required) —
  a future task could add it for the same draw-call-count benefit shadow
  passes get too, but nothing in the gate criteria requires it now.
- `generateRecords()` (mesh/material resolution + payload expansion) is
  deliberately NOT parallelized — only the AABB-vs-frustum test is, per
  D15's own explicit "AABB-vs-planes batched in parallelFor chunks"
  wording. This is O(visible) work, cheap relative to culling 30k
  candidates, and touches the injected resolver callbacks whose
  thread-safety this task makes no claim about — parallelizing it would be
  a genuine scope expansion beyond what the gate criteria ask for.
- No published wall-clock benchmark for a 30k-object stress scene in this
  report — Task 19's own criteria (this task, device-free, synthetic
  scenes) don't name one; the project's phase-4-exit benchmark-publication
  policy (CLAUDE.md's "every phase exits with published benchmark numbers")
  applies at Task 23/24 (after the executor's own zero-alloc work lands and
  a real stress sample exists to measure) — flagging this explicitly rather
  than fabricating a number now.

## Known limitations / what later tasks still own

- Task 22 (shadow bridge): the real GPU shadow-pass projection matrix,
  depth-bias, and `AttachmentDesc` depth-convention wiring — `buildShadow()`
  only decides WHICH casters are in the list, never touches a
  `VkPipeline`/render pass.
- Task 22/24: a real `PipelineResolveFn`/`RecordChunkFn` pair binding
  `recordDrawList()` to `MaterialSystem::getPipeline()` and actual
  `vkCmdDrawIndexed`/`vkCmdDrawIndexedIndirect` recording.
- Task 24: the 30k-instance stress-v2 sample and its published A/B
  benchmark numbers vs. sample 07 (desktop + Steam Deck), consuming this
  task's `DrawListBuilder` end to end through `Registry→Scene→
  DrawListBuilder→graph`.

## Fix round 1 (independent review, `task-19-review.md`)

Verdict: spec PASS, quality Approved, 4 findings (1 Medium, 3 Low), no
blockers. All 4 closed in-round per project policy.

**Finding 1 [Low] — report's own test-count arithmetic wrong ("61
(57+4)").** Confirmed: the suite was 57 total `TEST_CASE`s, of which 4
(RX_DEBUG_CHECKS-gated) are INCLUDED in that 57, not additional — the
report's own "61" figure double-counted them. Fixed throughout the report
(the "Files created"/"Test suite results" sections above are corrected in
place, not left as a separate erratum, since the wrong figure appeared in
more than one place). The suite is now 58 total (57 pre-existing + 1 new
test added for finding 2 below), same discipline: 4 gated, included not
additional. Re-verified directly this round: `grep -c '^TEST_CASE'
src/rx_scene/tests/*.cpp` → `draw_list_test.cpp:32 scene_test.cpp:16
camera_test.cpp:8 thread_guard_test.cpp:2` = 58; `./rx_scene_tests` →
`test cases: 58 | 58 passed`, `assertions: 6205 | 6205 passed`.

**Finding 2 [Low] — missing adversarial cross-group depth-ordering
test.** Added `TEST_CASE("Cross-group depth ordering: representative-depth
(nearest-member) comparison, not a flattened per-instance merge...")` to
`draw_list_test.cpp` (after the D26.3 lockstep test): two collapse-groups
(distinct meshes, same pipeline/material) with genuinely interleaved
per-instance depths (group A: world z -0.5 and -50; group B: world z -1
and -80; near=0.1 camera) — hand-computed representative depths (A=0.2,
B=0.1, via `ndc = near/distance`) predict group A's WHOLE collapsed
command (2 instances, including its own far member at true depth 0.002)
sorts entirely before group B's (2 instances, including B's near member
at true depth 0.1 — nearer than A's own far member). Test asserts exactly
this; ran and matched the hand computation exactly on the first attempt
(no debugging needed — the design's own arithmetic is what the review
predicted). Added a code comment at `collapseAndSortOpaque()` (immediately
above the phase-1/phase-2 description) stating the reviewer's own framing
verbatim in substance: D14 front-to-back is an early-Z rejection-rate
OPTIMIZATION, not an ordering guarantee — opaque correctness comes from
the per-fragment depth TEST, which is submission-order-independent by
construction; group-level (not flattened per-instance) ordering is the
only way to keep D26.3's collapse guarantee unconditional.

**Finding 3 [Medium] — `PipelineResolveFn`'s `uint32_t` return cannot
carry a real `VkPipeline`; resolver input shape didn't mirror
`PipelineRequest`.** Both confirmed against source, not just the review's
word: `vulkan_core.h`'s `VK_DEFINE_NON_DISPATCHABLE_HANDLE` macro (both
branches — pointer-typed and raw-`uint64_t`) resolves to an 8-byte type on
every platform this project targets; `MaterialSystem::getPipeline()`
(`material_system.h:381`) takes `PipelineRequest{MaterialHandle material;
PassSignature pass; uint32_t specializationBits;}`, confirmed by direct
read this round. Fixed at the shape level, not patched:
- New `struct PipelineRequestKey { uint32_t materialIndex; uint64_t
  passSignatureHash; uint32_t specializationBits; }` in `draw_list.h` —
  field-for-field mirror of `PipelineRequest`'s three inputs, with
  `passSignatureHash` typed to match `PassSignature::hash()`'s own
  `uint64_t` return exactly (confirmed via direct read of
  `rx_graph/pass_signature.h`). No `rx::material`/`rx::graph` dependency
  added — `PipelineRequestKey` is this library's own device-free type.
- `PipelineResolveFn` is now `std::function<uint64_t(const
  PipelineRequestKey&)>`; `ResolvedDrawGroup::pipelineToken` is `uint64_t`.
- `resolveDrawGroups()`/`recordDrawList()` both gained
  `passSignatureHash`/`specializationBits` parameters — CALLER-supplied
  (constant for one call, folded into every group's `PipelineRequestKey`
  alongside that group's own resolved `materialIndex`), since a built
  `ViewLists` is pass-agnostic and may legitimately feed more than one
  pass with different signatures; `ViewLists` itself gained no new field.
- All three D27 tests (`resolveDrawGroups()`'s own linear-scan test, the
  legal-path worker-guard test, the adversarial illegal-call test) updated
  to the new signature in place; the linear-scan test was additionally
  STRENGTHENED (not just recompiled) to assert `passSignatureHash`/
  `specializationBits` are threaded into every resolve call unchanged,
  proving the new fields actually flow through, not just compile.
- Re-verified: `rx_scene`/`rx_scene_tests` rebuilt clean (zero warnings),
  full suite green (58/58, 6205/6205) on `linux-native`; both presets'
  full `ctest` runs re-executed after the fix (see below).

**Finding 4 [Low] — D26.1's device-free-unsatisfiable
`vkCmdPushConstants`-count row missing from the proof table.** Added an
explicit N/A row to the matrix-issue06 proof table, immediately above the
existing D26.2 row, mirroring exactly how the layer-masks table already
handles its own analogous `getLayers`/`getChannels` row: states the
criterion is unsatisfiable in a `VkCommandBuffer`-free binary, names where
it becomes satisfiable (Task 22/24's real `RecordChunkFn` binding), and
adds a companion row confirming the underlying DATA-LAYOUT requirement
(zero push-constant-shaped fields in `DrawPayload`, `firstInstance`-only
addressing) IS delivered and covered, implicitly, by the D26.2/D26.3 rows
already in the table.

**Full re-verification after all four fixes (both presets, foreground,
matching the exact commands the original report used):**
```
linux-native, serial ctest:
100% tests passed, 0 tests failed out of 23
Total Test time (real) = 148.16 sec

windows-cross-zig, under Wine (xvfb-run -a ctest --preset windows-cross-zig
-E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'):
100% tests passed, 0 tests failed out of 11
Total Test time (real) = 116.71 sec
```
`rx_scene_tests`/`rx_scene_tests.exe` both green in these runs (58/58
cases, 6205/6205 assertions on linux-native; Wine run confirms the
`.exe` builds and runs identically under the cross-compiled target).

No new revert-and-restore probe was run for finding 2's own new test this
round (not requested; the test's correctness is independently grounded in
the hand-computed `ndc = near/distance` arithmetic matching the observed
result exactly, which is itself a form of falsifiable evidence — a wrong
implementation would not have matched the prediction). Findings 1 and 4
were pure documentation corrections with no code path to revert-probe.
Finding 3's shape widening was verified by full clean rebuild + full suite
re-run on both presets (above), not a revert-probe (there is no "narrower
seam" bug to revert to — the old shape simply could not compile against a
real `VkPipeline`, which is exactly the point).
