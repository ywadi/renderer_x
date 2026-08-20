# Phase 4 Exit Review — Cross-Stage Seams

**Reviewer:** phase-exit reviewer (fresh whole-phase pass, per plan §"Phase exit")
**Date:** 2026-08-20
**Tree:** main @ 34684f0 (clean)
**Baseline sanity:** full serial ctest, linux-native, `xvfb-run -a`: **29/29 PASSED** (131s).
All findings below are NEW cross-stage-seam defects — none restate closed per-task findings
or ledgered limitations.

**VERDICT: EXIT-BLOCKED** — one Critical (C1) and one Important correctness race (I1) sit in
the shipped exit-sample surface; both are one-fix-wave-sized. Remaining Importants are
mechanical (guards, doc-contract, a one-line worker-affinity fix).

---

## Critical

### C1 — Sample 09's shadow pipeline never executes on the GPU; the forward pass comparison-samples an uninitialized image; the committed D17 reference bakes the broken output

**Severity: Critical. Phase-exit-blocking** (correctness of the flagship exit deliverable;
the shadow bridge is a named Stage 2 exit feature).

This is a pure cross-stage seam defect: rx_graph's dead-code cull (Phase 3/Stage 0
semantics) × rx_shadow + sample 09 (Stage 2 wiring). No per-task review could see it —
Task 22 proved PCF inside rx_material's own GPU rig (which renders its own real shadow map,
not through the graph), and Task 24's review verified counters/packaging/crash-freedom, not
shadow ground truth.

**Mechanism (three independent wiring errors that mask each other):**

1. `samples/09_scene/main.cpp:2062-2066` — the `"shadow"` pass writes `"shadowmap"`, but
   **no pass ever reads `"shadowmap"`** (the forward pass at :2068-2074 declares only
   `hdr`/`depth`) and the pass never calls `setSideEffect()`. Cull roots are: backbuffer
   final writer + side-effect passes + history writers (`src/rx_graph/render_graph.cpp:431-464`).
   The shadow pass is unreachable → **culled by `compile()`**, silently (compile emits no
   log for culled passes).
2. `samples/09_scene/main.cpp:1685-1764` (`setupShadow`) — the sample creates its **own**
   `VkImage` shadow map (raw `vkCreateImage`/`vkAllocateMemory`, outside the D24-accounted
   Allocator) and registers **that** view in bindless as
   `SHADER_READ_ONLY_OPTIMAL`. Nothing anywhere renders to, transitions, or clears this
   image — even if the graph pass ran, it would render into the graph's *transient*, a
   different image.
3. `shaders/material/material.slang:295-317` — every frame, every lit fragment runs 9
   `SampleCmp` taps (compareOp LESS) against that never-initialized, never-transitioned
   image: **spec-level UB** (sampling an UNDEFINED-layout image). Silent under validation
   because bindless update-after-bind descriptors are exempt from core layout tracking —
   which is why every `--validate` run in the phase stayed "zero validation errors".

**Empirical proof (reproduced on this machine):**

- gdb breakpoint at `recordShadowPass` **entry** (`main.cpp:1899`, before any early
  return) with `shadowEnabled == true` (headless grid mode calls `setupShadow`,
  `main.cpp:2355`): **never hit**; program runs to a passing gate. Breakpoint on
  `rx::shadow::ShadowCasterPipeline::bindAndSetDepthBias`: never hit. The pass callback is
  never invoked — culled, not merely empty.
- Discrimination experiment on lavapipe (`VK_ICD_FILENAMES=.../lvp_icd.json`):
  - baseline run: D17 gate `failingPixels=0/65536` — the committed reference exactly
    matches the *broken* state;
  - same binary with `app.shadowEnabled` flipped to `false` at runtime via gdb (forcing the
    shader's documented `0xFFFFFFFF` → fully-lit sentinel path):
    `failingPixels=726/65536 (1.11%) pass=false`.
  The committed reference (`samples/09_scene/references/grid_scene.png` — visibly
  near-black, ambient/emissive only) therefore encodes "every lit pixel forced into
  shadow by an uninitialized (zero-read) map", and a *correct* render fails the gate.
  This is the exact self-generated-reference blindness class the helmet P0 ledgered as a
  standing lesson (progress.md, 2026-08-19) — recurring one stage later.
- On non-lavapipe (NVIDIA), the same run reports 706-726 failing pixels *informational* —
  i.e., real-driver output already visibly disagrees with the reference, driven partly by
  whatever uninitialized device memory contains (nondeterminism hazard on real hardware).

**Consequences beyond the image:** (a) `--scene sponza` present mode ships with direct
lighting suppressed by phantom shadows — the "sustained clean run" evidence in Task 24's
review was validation-clean, not ground-truth-checked; (b) **D29's mixed-convention frame
(reversed-Z main + standard-Z shadow in one graph) is exercised by zero shipped samples at
runtime** — it survives only in rx_graph's unit test; (c) the HUD reports healthy
shadow-caster counters every frame (CPU-side `buildShadow()` works fine), actively masking
the dead GPU pass; (d) 16 MB of raw-allocated shadow memory is invisible to the D24 memory
report.

**Fix shape (one wave):** forward pass declares `addTextureInput("shadowmap")` (this both
un-culls the shadow pass and buys the depth-write→sampled-read barrier from the graph);
delete the sample-owned image/memory/view entirely and register the graph transient's view
in bindless via the existing `lastHdrView` re-registration pattern (`recordTonemapDraw`,
`main.cpp:2008-2015`) — legal from chunk 0 of the chunked forward pass (D4 chunk-0
affordance) or from a tiny whole-pass prologue; regenerate `grid_scene.png` from the fixed
build; add a discriminating assertion the current state fails (e.g., assert spatial shadow
contrast: a known-occluded grid position darker than a known-lit one — today all lit pixels
are uniformly shadowed, so it discriminates), plus a graph-level guard (assert the shadow
pass survives compile — e.g. via a culled-pass query or a `RX_LOG_WARN` on culled passes
with executes attached, which would have made this loud).

---

## Important

### I1 — Frames-in-flight host-write race on the D26.1 draw-data buffers in BOTH exit samples

**Severity: Important. Phase-exit-blocking recommended** (a genuine Vulkan host-synchronization
violation in the shipped samples that consumers will copy as the reference D26.1 integration).

Both exit samples stream per-frame per-draw data into a **single** host-visible
`STORAGE_BUFFER` shared by all frames in flight (`FrameSync::kFramesInFlight == 2`):

- `samples/09_scene/main.cpp:719-722, 1839-1880` — `drawDataBuffer` **and**
  `shadowDrawDataBuffer` are memcpy'd + flushed every frame in `updateSceneFrame()`, which
  the present loop calls **before even the frame-slot fence wait**
  (`main.cpp:2879` update → `:2882` `vkWaitForFences(currentFence)`).
- `samples/08_gltf_viewer/main.cpp:1386-1402, 2190/2252` — `updateDrawDataPerPassFields()`
  rewrites the whole buffer after the *slot* fence wait — which with 2 frames in flight
  only proves frame N-2 finished; frame N-1's GPU work is still reading the same buffer.

Frame N's CPU write therefore overlaps frame N-1's GPU vertex/fragment reads with no
synchronization: torn `viewProj`/`model`/`lightViewProj`/`materialIndex` rows mid-frame
(payload row *order* also changes as culling/collapse changes), i.e., sporadic wrong-frame
draws. Invisible to sync validation (host-side races are not modeled) and to the D17 gates
(headless paths serialize with waits/`vkDeviceWaitIdle`).

The seam: the engine's own streaming path carries exactly the missing discipline —
`MaterialSystem`'s `ParamArena` is created with `kFramesInFlight` slots and `beginFrame()`
documents the "only after the caller's own fence wait for this slot" contract
(`material_system.h:432-453`). D26.1's new fast path (Tasks 16/24) replaced `bindInstance()`
with sample-owned buffers and dropped the FIF discipline; no gate matrix row required it.

**Fix shape:** per-frame-in-flight buffer copies (2×) selected by the frame slot, written
**after** `vkWaitForFences(currentFence)`; in sample 09 move the buffer-population half of
`updateSceneFrame()` below the fence wait (list building can stay where it is). Both
buffers in 09, one in 08. Small, mechanical.

### I2 — Stage-0 audit ruling F5-remainder ("execute/realize/beginFrame guards → Stage 1 acceptance") was never delivered

**Severity: Important. Policy-blocking under the owner's no-deferred-fixes standing
directive** (a ruled carry whose acceptance point passed without delivery).

`stage0-audit.md:136, :390` rules the F5 remainder — thread-affinity guards on
`Executor::execute()/realize()` and the FrameSync frame-advance surface — into "Stage 1
acceptance". Verified at HEAD: **zero** `RX_ASSERT_MAIN_THREAD` in `src/rx_graph/executor.cpp`
and `src/rx_rhi_vk/src/frame_sync.cpp` (grep; `FrameSync` documents main-thread-only by
comment only, `frame_sync.h:72-79`). `Executor::execute()` is precisely the entry point
whose chunk pools + scheduler assume the Scheduler's main thread (D4); a host calling it
from the wrong thread today fails silently/corruptingly instead of loudly. Fix is
mechanical (the same guard every other D5 surface carries).

### I3 — `MaterialSystem::pipelineLayout()` called from worker chunks — a sibling of the Task-24 `GeometryPool::bind()` violation, silent because read accessors are unguarded

**Severity: Important (contract violation, currently benign; loud-failure gap).**

`samples/09_scene/main.cpp:1957` (`recordForwardChunk`, executed by every chunk including
workers ≥ 1):
```cpp
VkPipelineLayout anyLayout = app.materialSystem->pipelineLayout(app.pipelineTokenToBinding.begin()->second->handle);
```
`docs/threading.md:67-71` declares MaterialSystem main-thread-only wholesale ("and every
other method on this type — it is not internally synchronized at all"). `pipelineLayout()`
(`material_system.cpp`) reads the same `impl_->materials` HandlePool that `loadMaterial()`/
`reloadChanged()` mutate — and `reloadChanged()` destroys the old `VkPipelineLayout`
immediately. Benign today only because nothing mutates MaterialSystem during `execute()`;
it is exactly the violation class Task 24 caught and fixed for `GeometryPool::bind()`
(the sample even documents that workaround at `main.cpp:666-685` — and then reaches the
un-cached sibling one page later). Unlike `bind()`, no guard fires, so the D4 "must fail
loudly in dev builds" promise is unmet for this class.

**Fix shape:** hoist `anyLayout` into main-thread-cached App state next to
`blockBufferCache` (one line), and extend guards to MaterialSystem's read accessors
(`pipelineLayout()`/`layoutInfo()`/`materialParams()`/`paramBlockSize()`) so the next
sibling fails loudly instead of silently.

### I4 — docs/threading.md (the D5 canonical contract) covers none of the Phase 4 Stage 1/2 library surfaces

**Severity: Important (documentation-contract; D5 names this file as the contract of record).**

At HEAD the file has **no entries** for: `rx::asset::Registry` (including the entire async
import surface — `importGltfAsync`/`cancelImport`/`importProgress` and the ~Registry
teardown-drain contract), `rx::asset::TextureCache` (including the deliberately
any-thread `decodeForUpload()` carve-out — the one *exception* a host most needs written
down), `rx::scene::Scene`, `rx::scene::DrawListBuilder`/`resolveDrawGroups`/`recordDrawList`
(D27's main-thread half), and `rx::shadow::ShadowCasterPipeline`. Worse, `threading.md:111`
still states the rx_scene managers "do not exist yet — not guarded", which is now false in
both halves (they exist; Scene is fully guarded, 35 call sites). Last touched at Task 21
(bbd1df1); Tasks 13/14/15/18/19/22 added guarded surfaces without updating the contract
document their own headers cite.

---

## Minor

### M1 — `DrawListBuilder::build()`/`buildShadow()` carry no entry-point guard
`draw_list.cpp:735/753`: both are documented main-thread-only and mutate unsynchronized
Impl scratch, but only `resolveDrawGroups()` (`draw_list.cpp:848`) carries
`RX_ASSERT_MAIN_THREAD`. Coverage today is transitive (the first `scene.aliveSpan()` call
fires Scene's guard) — but the caller-owned `out.commands.clear()` mutation happens before
any guarded call, and the transitive guard disappears for any future code path that
touches scratch before Scene. One guard per entry point matches the library's own posture.

### M2 — Registry read accessors unguarded despite the Task-12 ruling on the sibling class
`registry.cpp:620-646`: `mesh()/material()/texture()` are the per-frame hot resolve path
(via `meshSubmeshesFromRegistry`/`materialResolveFromRegistry`, invoked per visible
renderable per build). Task 12's review ruling narrowed GeometryPool's read accessors to
guarded main-thread-only precisely because unguarded reads against unlocked mutable state
invite silent misuse; Registry's identical-posture reads never got the same treatment.

### M3 — D27 "resolve once per distinct key" holds only for the opaque partition
`resolveDrawGroups()` (`draw_list.cpp:846-884`) groups by materialIndex **adjacency**; the
blend partition is depth-sorted, so materials interleave and `resolvePipeline` re-fires per
run (cache hits, main thread — cost only, not correctness). Registry-material: worth a
memoized key→token map inside the scan when blend-heavy scenes arrive (techniques phase).

### M4 — Worker-side per-frame allocations in the sample recorder
`splitByBlockAndGroup()` returns a fresh `std::vector` per chunk per frame
(`draw_recording.h/.cpp`, called from `recordForwardChunk`), and `resolveDrawGroups()`
returns a fresh vector per frame (`main.cpp:1889`). Sample-side, outside the Task-23
zero-alloc mandate's letter, but contrary to its spirit on the hottest path. Registry-material.

### M5 — Stale load-bearing comments in sample 09
`main.cpp:713-716` still documents `drawDataBuffer` as "sized ONCE …
(`scene->renderableCount()` at setup time)" — the exact sizing the H1 heap-overflow fix
(1ea8a01) replaced with per-instance submesh totals (`main.cpp:1384-1435`). A future
reader "restoring" the comment's claim reintroduces the overflow. Fix in wave (comment-only).

---

## Seam-by-seam verification record (what was checked and found sound)

**A. Registry ↔ Scene handle lifetimes — no new defect found.** Verified by code-walk at
HEAD: the entire mutation + resolve chain (import marshal, `pumpMain` closures,
`Scene` mutators, `DrawListBuilder` build, D27 resolve) is serialized on the one pump
thread, so no cross-thread stale-handle window exists by construction. `~Registry()`'s
cancel→`drainAndRollbackAbandonedAsyncJob()`→null sequence (registry.cpp:344-390), the
`cancelledAndDrained` reap gate (:392-436), `rollbackAsyncImportWhenSafe()`'s
ensure-ticketed + poll-reposting (:136-146), and the bounded leak-not-UAF teardown timeout
all match the ruled shapes from the Task 15 rounds. D24 resolve: evicted mesh → fallback's
empty submesh list → zero records (`draw_list.cpp:501-503`); evicted material → fallback
content at the stable index (documented, safe — index is never dereferenced).
`destroyRenderable` vs in-flight lists: lists are rebuilt each frame before recording in
every consumer; slot-columns never shrink, so even a stale index reads defined memory.
The one same-frame teardown-ordering member (`ImportResult` handles before scene binding)
is main-thread sequential. `registerRealTexture()`'s failure paths now flush+wait before
destroy (texture_cache.cpp:188-236) — verified in source, both branches.

**B. Parallel recording under real scene loads — no race found in the engine; one contract
violation in the sample (I3).** Task 23's Impl-persistent executor scratch audited
field-by-field: everything except `chunkBuffersScratch` is touched only on the serial
per-pass loop (main); `chunkBuffersScratch[frameSlot]` is `assign()`d before fan-out and
workers write disjoint `[chunkIndex]` elements with no resize during flight;
`validChunkBuffersScratch` is main-only after the blocking `parallelFor` join;
`totalChunkPoolAllocations` is atomic; per-(frameSlot,threadIndex) command pools are
single-writer by workerIndex identity. Chunk-0-on-main guarantee intact
(`executor.cpp:1693`). Sample 09's chunk callback reads only main-frozen state
(`viewLists`/`resolvedGroups`/`blockBufferCache`/`pipelineTokenToBinding`) — except I3.
Determinism: chunk partition is index-arithmetic; stitching is chunk-index order.

**C. Threading-contract adherence — I2/I4/M1/M2 above.** Full guard census run across all
libs (grep): Scene 35, TextureCache 18, GeometryPool 17, Registry 6, Overlay 4,
ShadowCasterPipeline 2 (the Task-22 F5 closure), Window 10, Uploader 6, MaterialSystem 4,
BindlessTable 5, Device 5, DeletionQueue 1, Scheduler 1 — versus zero on
Executor/FrameSync (I2). No *call-site* violation of any guarded API found anywhere in
samples or libs besides I3's unguarded-API case; `Registry::importGltfAsync(path,…)`'s
IO-thread closure calls only any-thread-safe primitives (`runOnWorkerThread`,
`postToMain`); `computeGltfImport` on the worker lane touches TextureCache only through
the deliberately any-thread `decodeForUpload()` (texture_cache.cpp:378-384) and
main-snapshotted fallback handles.

**D. Upload/timeline seams — no host-wait-implies-device-visibility assumption found in
Stage 2 consumers.** Prepare-step time-slicing, `flushPendingUploads()` ticketing,
`isUploadComplete` pure polls, and the finalize gate all verified in source; sample 09's
imports are fully synchronous (`importGltf`) so its first frame renders only
already-waited content; rx_debug_ui's font upload keeps the at-most-once QueueWaitIdle
seam; rx_shadow performs no uploads; the 7cc685f GPU-side-wait pattern is confined to
readbacks and unchanged. (I1 is a host-vs-device *frame-overlap* race, not a
ticket-semantics defect — the ticket layer itself holds.)

**E. D29 composition — mechanism sound, sample-level exercise vacuous (folded into C1).**
Both executor derivation sites route through the single `depthClearValueFor()`
(executor.cpp:77-79 → :1342 attachment clear, :837 pinned-history init-clear via the
convention parameter realize() forwards at :1108). Convention is a per-resource property,
so transient-pool reuse across conventions cannot mis-clear (clear value is re-derived per
pass from the resource's own declaration). The mixed-frame case is covered by Task 22's
rx_graph test (417d440) — but after C1's fix, sample 09 becomes the first *shipped* frame
actually mixing conventions; re-verify zero validation errors then.

**F. Sample 09 as integration proof — fails on C1/I1/I3; otherwise contract-conformant.**
Checked against each subsystem's documented contract: Scene/DrawListBuilder main-thread
build-before-execute; D27 pre-resolution on main before fan-out with workers consuming
plain data; `GeometryPool::bind()` main-thread-only respected (whole-pass shadow recorder;
cached raw handles for chunks); Overlay event/beginFrame/addPass ordering per gate ruling
#16; H1 capacity sizing verified correct at `main.cpp:1401-1435`; A/B stress contract
unchanged. The headless gate's counter assertions are CPU-side only — which is how C1
stayed invisible; the fix wave should add at least one GPU-side shadow discriminator.

---

## What I could not verify

- **No TSAN pass over the full sample-09 present loop** (chunked recording + overlay +
  import concurrency as one process). Seam-B conclusions are from exhaustive code-walk of
  the shared state, not a sanitizer run; the rx_task layer itself carries its own
  adversarial TSAN harnesses from Task 15 (unchanged since).
- **Windows/Wine behavior of the findings** — all empirical work here is linux-native
  (lavapipe + NVIDIA). Nothing found is platform-conditional, but the fixes should ride
  the normal both-preset verification.
- **Sponza visual ground truth vs an external renderer** — C1's impact on Sponza is
  inferred from the proven mechanism (shadow term forced to 0 on all lit pixels), not from
  a side-by-side render; the grid-mode proof is fully empirical.
- **Deck-hardware rows** — unchanged MANUAL_VERIFICATION convention (noise per mandate).
- **I1's visible artifact** was not captured on camera (it needs a mid-frame preemption
  window); the race is established from the Vulkan host-synchronization rules plus the
  code's write/wait ordering, both cited above.

---

# FINAL PHASE VERDICT (post fix wave, 2026-08-20)

## EXIT-READY

**Flip statement:** every finding of the exit review is verifiably closed at
main @ `034b201` (12 commits, `34684f0..034b201`; `277531d` is the
coordinator's registry recording of M3/M4). The Critical and the
frames-in-flight race — the two exit-blockers — are fixed at the root
(graph-derived consumption, not a side-effect pin; per-FIF buffer topology,
not a wait reshuffle), each now carries a standing regression discriminator,
and the phase may tag v0.4.0-phase4.

## Per-finding re-verification (this reviewer's own evidence, not the report's)

- **C1 — CLOSED, root-cause fix, empirically flipped.** `"forward"` now
  declares `addTextureInput("shadowmap")` (sample_09 main.cpp:2207 —
  genuine graph consumption; the culled state is unreachable through the
  API, not pinned via `setSideEffect()`); the sample-owned raw
  VkImage/VkDeviceMemory/VkImageView are deleted — `"shadowmap"` is a real
  TransientPool transient (D24-visible), re-registered into bindless only
  from chunk 0 (`chunkIndex == 0` gate verified, main.cpp:2015) via the
  established `lastHdrView` pattern, with a validity-gated sentinel so the
  first frame never reads an unregistered slot. **My original gdb proof now
  flips:** the `ShadowCasterPipeline::bindAndSetDepthBias` breakpoint that
  never fired at 34684f0 now hits with a backtrace through
  `recordShadowPass` (main.cpp:1994). D17 reference regenerated:
  independently diffed old→new = 701 px, changed pixels **brighter**
  (mean 38→65 — direct lighting restored), confined to the helmet rows.
  The shipped headless gate now carries a **standing** shadows-on/off
  discrimination sub-check, hard-gated on lavapipe
  (`kMinDiscriminatingPixels = 100`, observed 324/65536; `gateOk = false`
  below floor — verified in source, main.cpp:2842-2850, and observed
  live). **Revert spot-proof (my choice, re-run myself):** commenting out
  the single `addTextureInput` line → the new `RenderGraph::compile()`
  culled-pass-with-live-callback warning fires naming the pass, and the run
  aborts loudly on `PassContext: no realized resource named 'shadowmap'`
  (no silent-fallback path remains); restored via `git checkout`, gate
  green again (0/65536 + 324 discrimination), tree clean. D29's
  mixed-convention frame is now genuinely exercised by a shipped sample.
- **I1 — CLOSED.** Both samples: `std::array<Buffer, kFramesInFlight>`
  draw-data (and 09's shadow draw-data) buffers with per-slot bindless
  handles; GPU writes moved after `vkWaitForFences(currentFence)`
  (09: wait :3075 → upload :3121; 08: wait :2237 → update :2307 with
  `frameSync->currentFrameIndex()`); record-time push constants read the
  per-slot handle via `currentFrameSlot` set before `execute()`. The
  double-buffering argument is exactly the ParamArena precedent; verified
  by code-read at every write/read site. (No live race repro attempted —
  same limitation both directions, as my review disclosed.)
- **I2 — CLOSED.** `RX_ASSERT_MAIN_THREAD` on `Executor::execute()/realize()`
  and `FrameSync::create()/advanceFrame()/onSwapchainRecreated()`
  (guard census re-run), with death/abort-pattern tests in
  `test_execute_gpu.cpp`/`frame_sync_test.cpp`. Disclosed residual, accepted:
  `MaterialSystem::beginFrame()/onFrameCompleted()` (named only in F5's
  *original* Stage-0 wording, not in my I2 restatement) remain unguarded —
  a one-line hygiene item for any future pass, not a blocker.
- **I3 — CLOSED.** Call site fixed (`cachedAnyMaterialLayout` resolved once
  on main in `buildPipelineTokenMap()`, main.cpp:1752/2077) AND the loud-
  failure gap closed (guards on `pipelineLayout()/layoutInfo()/
  materialParams()/paramBlockSize()` + tests).
- **I4 — CLOSED.** `docs/threading.md` now covers Registry (all nine guarded
  members incl. the 5c3922c follow-up), TextureCache (with the deliberate
  any-thread `decodeForUpload()` carve-out), Scene, DrawListBuilder/D27,
  ShadowCasterPipeline, Executor, FrameSync; the stale "does not exist yet"
  line is gone.
- **M1 — CLOSED.** Entry-point guards on `build()/buildShadow()` + cascade-
  documented guard tests.
- **M2 — CLOSED.** Guards on `Registry::mesh()/material()/texture()`; the
  follow-up `5c3922c` also guards both `importGltf()` overloads and both
  `evictForTesting()` overloads (in-round closure of a disclosed gap).
- **M3/M4 — registry-recorded** by the coordinator (`277531d`), matching my
  own classification of both as registry-material.
- **M5 — CLOSED.** Comment now documents the H1-correct per-instance
  submesh-total sizing.

## Empirical record for this re-verdict

Full serial ctest at 034b201 (`xvfb-run -a`, linux-native): **29/29 PASSED**
(134s). Sample 09 headless gate re-run on lavapipe: D17 0/65536 against the
regenerated reference + discrimination 324/65536 (enforced). gdb flip-proof
and the C1 revert-and-restore spot-proof re-run by this reviewer (transcripts
above). Commit hygiene: 12 commits, sole author = the user's local git
identity, zero AI attribution in any message or body, file inventory exactly
the finding surfaces + tests + docs + regenerated reference, **nothing
pushed** (no remote ref contains 034b201).

**Residual notes (non-blocking, for the record):** (a) the
`MaterialSystem::beginFrame()/onFrameCompleted()` guard sliver above;
(b) chunk-0 shadowmap re-registration writes a row-carried bindless index
that is one frame stale across a view change — unreachable in practice here
(the shadowmap transient is absolute-size and touched every frame, so its
view is stable across swapchain recreates, and every recreate path reaches
device-idle first), and the identical risk profile as the pre-existing hdr
pattern it mirrors.
