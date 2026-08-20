# Phase 4 Exit Fix Wave -- Implementer Report

**Base:** main @ 34684f0 (per the exit review; HEAD had advanced to 277531d,
a registry-only docs commit, by the time this wave started -- no functional
drift).
**Scope:** every finding in `phase4-exit-review.md` (C1, I1, I2, I3, I4, M1,
M2, M5). M3/M4 are registry-material, adjudicated by the coordinator
(commit 277531d) -- not touched here, per the brief.
**Verification hardware:** this machine, both drivers used deliberately:
- **lavapipe** (`/usr/share/vulkan/icd.d/lvp_icd.json`, llvmpipe/LLVM 15.0.7)
  -- the enforced D17/discrimination driver, matching CI's own convention
  (`ci.yml` "Test (xvfb + lavapipe)" step).
- The system's other installed ICD -- informational-only runs, matching
  every sample's own existing lavapipe-vs-other-driver gating convention.

---

## C1 (CRITICAL) -- shadow pass never executed

**Root cause (confirmed identical to the review's own three-part mechanism):**
`samples/09_scene/main.cpp`'s `declareGraph()` added the `"shadow"` pass
writing `"shadowmap"` with no reader and no `setSideEffect()` ->
`RenderGraph::compile()` culled it silently -> the forward shader's 9
`SampleCmp` taps sampled a sample-owned raw `VkImage` that was never
written and never left `UNDEFINED` layout.

**Fix (graph-derived consumption, not a `setSideEffect()` band-aid):**
- `declareGraph()`: `"forward"` now declares
  `addTextureInput("shadowmap")` when shadows are enabled -- this both
  un-culls `"shadow"` (compile()'s reachability walk now reaches it via
  `"forward"`'s own reachability from the backbuffer writer) and derives
  the depth-write -> sampled-read barrier the graph already knows how to
  compute for every `addTextureInput()` declaration (D29's mixed-convention
  composition -- reversed-Z forward + standard-Z shadow -- exercised by a
  shipped sample for the first time).
- `setupShadow()`: deleted the sample-owned raw
  `vkCreateImage`/`vkAllocateMemory`/`vkCreateImageView` entirely.
  `"shadowmap"` is now a genuine `rx_graph` transient, owned and
  lifetime-tracked by the Executor's own `TransientPool` (D24 memory
  reporting sees it now, for free).
- `recordForwardChunk()`: chunk 0 (re-)registers `"shadowmap"`'s current
  resolved view into bindless, mirroring `"hdr"`'s own established
  `lastHdrView`/`hdrHandle` re-registration pattern (this file's
  `recordTonemapDraw()`; prior art: `samples/05_multipass`'s
  `recordLitDrawsChunked()`) -- legal only because chunk 0 of a
  `setExecuteChunked()` callback is guaranteed synchronous on the main
  thread [D4 chunk-0 affordance].
- `updateSceneFrame()`'s row population now requires
  `app.shadowMapHandle.isValid()` in addition to `app.shadowEnabled`
  before writing a real `shadowMapTextureIndex` -- before the handle's
  first registration (only ever the very first frame of a run; the
  physical resource never resizes/evicts afterward) rows fall back to the
  same documented `0xFFFFFFFF` fully-lit sentinel the disabled path
  already used. **Never an unregistered/stale bindless read.**

**Library-level guard (generic, not sample-specific):**
`RenderGraph::compile()` (`src/rx_graph/render_graph.cpp`) now emits
`RX_LOG_WARN` naming any pass that survives with a recorded
`setExecute()`/`setExecuteChunked()` callback but gets culled. Two new
`test_compile.cpp` cases prove it fires and prove it stays silent once a
real consumer exists.

**Reference regeneration (real lavapipe, `--write-references`):**
```
sample_09_scene: wrote D17 reference PNG to '/tmp/rx_refs_out'
```
Diff against the OLD (broken) reference: 701 pixels differ, spatially
concentrated at each helmet's ground-contact silhouette -- exactly a
new shadow-contact term appearing, not noise (amplified diff image
inspected directly during this wave; not committed, reproducible via the
commands below).

**Discrimination re-proof (added to the shipped headless gate itself,**
`sample_09_scene_headless`**, so it runs every ctest invocation from now on):**
re-renders the same static-camera frame with a debug
`app.debugForceShadowSentinel` override forced on (the clean,
non-gdb equivalent of the review's own runtime flip) and asserts the
result differs from the real frame by more than a 100-pixel floor,
hard-gated on lavapipe:
```
sample_09_scene: D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true
sample_09_scene: C1 discrimination re-proof (shadows-on vs. forced-off): differingPixels=324/65536 (0.4944%)
sample_09_scene: headless gate PASSED
```
324/65536 -- same order of magnitude as the review's own ~726/65536
gdb-probe measurement (the two probes are not identical: the review's
flip also disabled shadow-list building/recording; this one forces only
the shader-side row sentinel, deliberately, so it exercises the SAME
graph structure every real frame does -- see the field's own comment).

**Revert-and-restore proof (in-tree, this session):** commented out the
single `forward.addTextureInput("shadowmap");` line, rebuilt, ran:
```
[warning] rx_graph: pass 'shadow' has a recorded execute callback but was culled (nothing reads any resource it writes, and it never called setSideEffect()) -- its callback will NEVER run; if this is intentional, remove the callback, otherwise add a reader (addTextureInput()/addStorageBufferInput() on the resource it writes) or setSideEffect()
libc++abi: terminating due to uncaught exception of type std::out_of_range: rx_graph: PassContext: no realized resource named 'shadowmap'
Aborted (core dumped)
```
The new library-level warning fired exactly as designed, immediately, on
the exact regression; the process then aborted (chunk 0's re-registration
now genuinely depends on the graph wiring being correct -- the new code
has no silent-fallback path left, unlike the old raw-image approach it
replaced). Restored the file (`cp` from a pre-edit backup, verified
`git diff --stat` matched the pre-revert byte count); rebuilt; reran:
```
sample_09_scene: D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true
sample_09_scene: headless gate PASSED
```
byte-identical restoration confirmed (0 failing pixels against the
committed, regenerated reference).

**Sponza (real, fetched asset) -- sustained present-mode run, `--validate`:**
```
sample_09_scene: '/home/ywadi/d2/renderer_x/assets/fetched/Sponza/glTF/Sponza.gltf' loaded -- 1 renderable(s), 25 material(s)
```
Ran ~58s under `xvfb` + lavapipe (`--present --scene sponza --validate`),
killed manually (present-mode has no auto-quit and D16 marks `--scene` as
CI-exempt/manual-verification territory): zero unfiltered validation
errors for the whole run (every reported validation message matched one
of the four pre-existing "known false positive" guards already in
`context.cpp`); HUD counters read `recordsIn: 103 drawsSubmitted: 103`
every frame, matching Sponza's real 25-material/103-submesh-instance
shape. A mid-run screenshot (captured via `import -window`, X11) shows
real surface texture detail and lighting gradient, not the uniform
near-black wash C1 produced -- not saved into the repo (throwaway
verification artifact), reproducible via the commands below.

---

## I1 (IMPORTANT) -- frames-in-flight host-write race, both exit samples

**Fix, both samples, identical shape:** the single shared
`drawDataBuffer`/`shadowDrawDataBuffer` became a
`std::array<..., rx::rhi::FrameSync::kFramesInFlight>` (2 physical
buffers), selected each frame by the present loop's own
`frameSync->currentFrameIndex()`. Frame N's write to slot `N % 2` now
only needs frame N-2 (the slot's own last user) to be done -- exactly
what `vkWaitForFences(currentFence)` already proves -- since frame N-1
always used the OTHER slot's buffer.

- **09_scene:** `updateSceneFrame()` split into CPU-side row population
  (kept where it was, before the fence wait -- list-building has no
  GPU-visibility requirement) and a new `uploadSceneFrameGpuBuffers(App&,
  uint32_t frameSlot)`, called only after
  `vkWaitForFences(frameSync->currentFence())` in the present loop.
  Headless mode (fully synchronous `runOnce()`, zero real frame overlap)
  always uses slot 0.
- **08_gltf_viewer:** `updateDrawDataPerPassFields()` already ran after
  its own fence wait (the review's own text: "same class at :2190/:2252"
  -- write-after-wait was already true, buffer duplication was the
  missing half); added the `frameSlot` parameter and per-slot addressing.

**Verification:** both samples' headless AND (09_scene, manually) present
runs pass with `--validate`, zero validation errors, identical rendered
output to before (the fix is purely a buffer-topology change; no visual
delta expected or observed). No live TSAN/timing race reproduction was
attempted -- the phase-exit review's own "What I could not verify" section
states the same limitation on the BUG side ("I1's visible artifact was
not captured on camera... established from the Vulkan host-synchronization
rules plus the code's write/wait ordering"); the FIX is verified the same
way the finding was established: by the same host-synchronization rules
(per-slot buffer + fence-wait-proves-that-slot's-prior-user-done is the
textbook double-buffering argument), by exact-precedent match against
`MaterialSystem::ParamArena`'s own already-shipped, already-reviewed FIF
discipline, and by full-suite green with validation on.

---

## I2 (IMPORTANT) -- Stage-0 F5-remainder guards

`RX_ASSERT_MAIN_THREAD` added to `Executor::execute()`/`realize()`
(`src/rx_graph/executor.cpp`) and `FrameSync::create()`/`advanceFrame()`/
`onSwapchainRecreated()` (`src/rx_rhi_vk/src/frame_sync.cpp`) -- the
literal Stage-0 F5-remainder scope (`stage0-audit.md:136/390`) as
narrowed by the exit review's own I2 text ("Executor::execute()/realize()
and the FrameSync frame-advance surface"); `MaterialSystem::beginFrame()/
onFrameCompleted()` (also named in F5's *original* text) were left
untouched -- not part of the review's own I2 restatement, and no new
finding named them.

**Proof (RX_DEBUG_CHECKS death/abort pattern, matching
`rx_asset/tests/thread_guard_test.cpp`'s established convention -- a
plain `std::thread` stands in for a chunk >= 1 worker, joined before the
next call, test-installed hook records-and-returns instead of aborting):**
new cases in `src/rx_graph/tests/test_execute_gpu.cpp` (minimal
single-pass graph, no shader needed) and
`src/rx_rhi_vk/tests/frame_sync_test.cpp`. Both ran clean under the full
ctest suite (lavapipe) and under Wine (the GPU-backed variants are
excluded from the Wine run by CI's own existing convention -- no Vulkan
under Wine -- unrelated to this change).

---

## I3 (IMPORTANT) -- MaterialSystem::pipelineLayout() from worker chunks

**Call-site fix (D27 pre-resolution, main thread):**
`buildPipelineTokenMap()` now also computes `app.cachedAnyMaterialLayout`
once, at setup, on the main thread; `recordForwardChunk()` reads the
cached value instead of calling `pipelineLayout()` from a worker chunk
(`chunkIndex` can be `>= 1`).

**Guard fix (loud-failure-on-the-next-violation, mirroring GeometryPool's
precedent):** `RX_ASSERT_MAIN_THREAD` added to
`pipelineLayout()`/`layoutInfo()`/`materialParams()`/`paramBlockSize()`
(`src/rx_material/material_system.cpp`). Proof: two new cases in
`test_material_system.cpp` reusing the existing `bindInstance` guard
test's capture hook (plain `std::thread`, no chunked rx_graph/rx_task
machinery needed for these simpler read accessors).

The D27 worker-guard smoke test already present in `sample_09_scene`'s
own headless gate (`#ifdef RX_DEBUG_CHECKS`, `--threads>1` block) still
passes with zero violations recorded -- confirms the call-site fix, not
just the library guard, closed the real violation.

---

## I4 (IMPORTANT) -- docs/threading.md coverage

Added entries for `rx::asset::Registry` (mutation + the full async-import
surface + `~Registry()`'s teardown-drain contract + the M2 read-accessor
guards), `rx::asset::TextureCache` (including the deliberate any-thread
`decodeForUpload()` carve-out), `rx::scene::Scene` (35 guarded call
sites), `rx::scene::DrawListBuilder`/`resolveDrawGroups()` (D27's
main-thread half + the M1 guards), and `rx::shadow::ShadowCasterPipeline`.
Deleted the stale `threading.md:111` line claiming the rx_scene managers
"do not exist yet, not guarded" (false since Task 18; Scene alone has 35
guarded call sites). Also recorded the new Executor/FrameSync entries
(I2) in the same pass.

**Post-report addendum (same session, coordinator-directed):** this
report originally disclosed, but deliberately left unfixed, that
`Registry::importGltf()`'s synchronous overloads and `evictForTesting()`
were documented main-thread-only but carried no `RX_ASSERT_MAIN_THREAD`
anywhere in their own call chain. Per the coordinator's standing
in-round-closure policy (no prerequisite blocked it), this was closed in
the same session: `RX_ASSERT_MAIN_THREAD` added to both `importGltf()`
overloads and both `evictForTesting()` overloads
(`src/rx_asset/registry.cpp`), `docs/threading.md`'s Registry entry
updated to list all nine guarded members, and two new death/abort-pattern
test cases added to `src/rx_asset/tests/thread_guard_test.cpp` (a real
GeometryPool+Scheduler fixture, an intentionally empty byte span --
`importGltf()`'s guard fires as the first statement regardless, and an
empty/malformed document is an already-covered safe-failure path per
`import_gltf_gpu_test.cpp`'s own "malformed-file battery" case, so no
real glTF fixture was needed). Verified: `rx_asset_tests` alone (38/38
test cases, 596/596 assertions) and the full linux-native serial ctest
suite, both under lavapipe -- see the addendum commit below.

---

## M1 (MINOR) -- DrawListBuilder entry-point guards

`RX_ASSERT_MAIN_THREAD` added to `build()`/`buildShadow()`
(`src/rx_scene/draw_list.cpp`), matching `resolveDrawGroups()`'s existing
guard. Proof: two new cases in `src/rx_scene/tests/thread_guard_test.cpp`.
The "fires" case asserts *containment*, not an exact count -- both
methods' own guards fire first (confirmed: `build`/`buildShadow` are
literally the first captured context in each case), then the
non-aborting test hook lets execution continue into every downstream
guarded `Scene`/`DrawListBuilder` accessor the real call internally makes
(10 total violations captured for the two calls combined) -- documented
in the test file so a future reader does not mistake the cascade for a
bug.

---

## M2 (MINOR) -- Registry read-accessor guards

`RX_ASSERT_MAIN_THREAD` added to `mesh()`/`material()`/`texture()`
(`src/rx_asset/registry.cpp`), mirroring the Task-12 GeometryPool ruling
the finding cites. Verified safe: `meshSubmeshesFromRegistry()`/
`materialResolveFromRegistry()` (the production callbacks
`DrawListBuilder::generateRecords()` calls through) run on the main
thread only -- `generateRecords()` itself never calls
`scheduler.parallelFor()` (only the earlier `cullView()` step does, over
already-snapshotted plain spans) -- so this guard cannot fire on any real,
correct call path; confirmed by the full suite staying green. Proof: two
new cases in `src/rx_asset/tests/thread_guard_test.cpp` (a bare
default-constructed `Registry`, no GPU fixture needed -- its D11
fallback assets make even a never-registered handle a safe, defined
read).

**Addendum (post-report, same session):** `importGltf()` (both
overloads) and `evictForTesting()` (both overloads) also guarded now --
see the I4 section's addendum above for the full rationale; these were a
separate, coordinator-directed in-round closure of a gap this report's
I4 section originally disclosed rather than fixed.

---

## M5 (MINOR) -- stale comment

Fixed in the same `samples/09_scene/main.cpp` commit as C1/I1/I3 (the
comment sat immediately above the fields those findings also touched).
Now documents the H1-correct sizing (per-instance submesh totals, not
`scene->renderableCount()`).

---

## Verification summary

| Check | Result |
|---|---|
| linux-native, full serial ctest, lavapipe (CI's own driver) | **29/29 PASSED** (~74-78s) |
| linux-native build | clean, 0 warnings-as-new from this wave |
| windows-cross-zig build | clean |
| Wine, CI's exact ctest invocation (`-E 'rx_rhi_vk\|rx_graph_gpu\|rx_material_gpu\|rx_debug_ui_gpu\|sample'`) | **13/13 PASSED** (~132s) |
| `sample_09_scene_headless` / `--validate` | zero unfiltered validation errors; D17 gate passes against the regenerated reference; C1 discrimination re-proof passes (324/65536, floor 100) |
| `sample_08_gltf_viewer_headless` / `--validate` | zero unfiltered validation errors, unchanged behavior |
| Sponza, `--present --scene sponza --validate`, ~58s sustained | zero unfiltered validation errors; 25 materials / 103 draws every frame; visually not uniformly dark |
| C1 revert-and-restore | new library warning fires on the exact reintroduced bug; restore verified byte-identical (0/65536 against committed reference) |
| I1 revert-and-restore (live race) | not attempted -- see I1 section; same disclosed limitation the phase-exit review itself carries on the bug side |
| New guard tests (I2/I3/M1/M2), death/abort pattern | all pass, both linux-native (lavapipe) and Wine where GPU-independent |

### Reproduction commands (this machine)
```sh
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
xvfb-run -a ctest --preset linux-native --output-on-failure   # 29/29
xvfb-run -a ctest --preset windows-cross-zig \
  -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample' \
  --output-on-failure                                          # 13/13, via Wine
```

---

## Commits (pathspec-scoped, no AI attribution, author = local git config)

1. `1591291` fix(rx_graph): warn on a culled pass that still carries an execute callback -- C1 library guard
2. `c4700ad` fix(rx_graph,rx_rhi_vk): RX_ASSERT_MAIN_THREAD on Executor and FrameSync -- I2
3. `24a1122` fix(rx_material): guard MaterialSystem's remaining read accessors -- I3 (library half)
4. `53817f4` fix(rx_scene): entry-point guards on DrawListBuilder::build()/buildShadow() -- M1
5. `66f6d2a` fix(rx_asset): guard Registry's mesh()/material()/texture() read accessors -- M2
6. `0050540` docs(threading): cover Phase 4's Stage 1/2 surfaces, drop the stale rx_scene line -- I4
7. `e9a59f0` fix(samples): 09_scene shadow pass wiring, FIF draw-data races, worker guard -- C1 (sample half) + I1 (09_scene half) + I3 (call-site) + M5
8. `785559e` fix(samples): 08_gltf_viewer frames-in-flight draw-data buffer race -- I1 (08_gltf_viewer half)
9. `02c14af` docs: Phase 4 exit fix wave report -- per-finding proof, revert evidence
10. `5c3922c` fix(rx_asset): guard Registry::importGltf()/evictForTesting() (in-round closure) -- coordinator-directed, post-report addendum: closes the pre-existing gap this report's I4/M2 sections originally disclosed but left unfixed

No board/plan/ledger edits. `docs/threading.md` and the regenerated
`samples/09_scene/references/grid_scene.png` are the two authorized
non-code touches; both delivered. Not pushed.

**Post-addendum re-verification:** linux-native, full serial ctest,
lavapipe: **29/29 PASSED** (~81s), including the two new
`rx_asset_tests` cases (`rx_asset_tests` alone: 38/38 test cases, 596/596
assertions). Second preset not re-run for this scale, per the
coordinator's own instruction.
