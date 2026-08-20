# Completeness matrix — P5 T05 (issue #41): Material-path API-gap audit CLOSURE (sample-driven)

**Plan task:** Task 5, Stage 0 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:256-285`).

**Binding scope-growth source (verbatim, `gh issue view 41 --comments`, 2026-08-20, author `ywadi`, association `owner`):**
> Scope growth (owner-driven, 2026-08-20): beyond the material path, this
> audit's closure includes PRESENT-LOOP CENTRALIZATION — a shared
> frame-loop helper (acquire → status handling incl.
> NeedsRecreate/suspended/SurfaceLost → recreate-and-dependents → present,
> frame-body callback) consumed by all samples, deleting the nine
> duplicated hand-written loops that produced this phase's recurring
> integration-bug pattern (descriptor sizing, per-FIF buffering, mouse
> capture, node transforms, surface-lost consumption). The render graph's
> swapchain-relative re-compile-on-extent-change contract moves inside the
> helper or the graph itself as part of the same closure. The Stage 0
> primary gate hardens the exact criteria.

This comment is treated as co-equal, binding ticket text alongside the
issue body and plan text below — not an aside. The phase's own SDD ledger
(`.superpowers/sdd/2026-08-20-phase5-techniques/progress.md:3`) already
records it as **settled direction**: "samples consume engine/shared
facilities (present-loop centralization committed in T5/#41)" — so this
matrix's present-loop Open Questions address DESIGN specifics (module
home, contract placement), never whether to do it. Note the ledger's own
"engine/**shared**" phrasing — load-bearing for Open Questions #3. The
same ledger line also records the standing owner decision policy: "the
coordinator takes the best-recommended option autonomously; ONLY
genuinely unclear/owner-level decisions escalate" — this matrix's Open
Questions are written to that policy (decisive recommendations, each with
its rationale).

"Nine duplicated hand-written loops" is verified exactly:
`ls samples/*/main.cpp` returns nine files (01_triangle, 02_hotreload,
03_bindless_mesh, 04_streaming, 05_multipass, 06_materials, 07_stress,
08_gltf_viewer, 09_scene), every one of which defines its own
`runPresent()`.

**Issue #41 body** (plan Task 5 text, `plan:257-270`, which the issue
mirrors): audit sweep of samples 07-09 for hand-rolled engine facilities,
closing `createMaterialParamArena` (duplicated 08/09), the per-FIF
draw-data buffer pattern (exit-review fix I1, hand-built in both exit
samples), `samples/09_scene/{mouse_capture.h,fly_camera.h,grid_layout.h}`
(promote into `rx_platform`/`rx_scene`, or an explicit per-item
sample-local ruling), and sample-recorder worker-side per-frame vector
allocations (`splitByBlockAndGroup`/`resolveDrawGroups` — Phase 4
exit-review registry item (b)). Output must include the audit table
itself.

**Global Constraints binding this ticket** (`plan:55-112`), the one row
directly on point quoted in full (`plan:88-93`):
> **Samples are pure consumers of engine facilities.** No sample
> hand-rolls what the engine provides (the Phase 4 pattern: descriptor
> arenas, per-FIF buffers, mouse capture, node transforms). Any facility a
> Phase 5 sample needs that the engine lacks is an API gap: promote it
> into the engine in the same task (or record an explicit ruling why
> sample-local stands). Task 5 clears the inherited backlog; the rule then
> binds every new task.

Also binding: attribution ban; production grade (no stubs/half-solutions);
zero validation errors with sync validation active; both presets build;
TDD; no-deferred-fixes standing directive (`plan:107-112` — only FEATURE
phase-fits and human-hardware MANUAL_VERIFICATION rows legitimately defer);
performance-is-an-exit-criterion (CLAUDE.md, `plan:100-106`) — the
present-loop helper and zero-alloc recorder work are both directly
performance-shaped, not just correctness-shaped.

**Architecturally binding, found this session, not named in the ticket
text:** the toolchain/RHI design spec's **Main-loop ownership** decision
(`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:500-508`,
decided 2026-08-10): "RendererX is library-model — the consuming
game/engine owns `main()` and the frame loop and calls an explicit frame
API... never the reverse. No required init/frame callbacks, no
engine-owned loop... An optional thin 'runner' convenience (window +
per-frame callback...) may ship later for quick starts, built ON the
library API and never required by it." This directly shapes *where* the
scope-growth comment's shared present-loop helper may legally live — see
Conflicts and Open Questions.

**Sources consulted (in-repo, HEAD at session start; every file below read
in full unless a line range is given):**
- `docs/superpowers/plans/2026-08-20-phase5-techniques.md:1-112` (Global
  Constraints in full) and `:256-285` (Task 5 in full).
- `gh issue view 41 --comments` (fetched this session; body + the single
  2026-08-20 scope-growth comment, both quoted above verbatim).
- `.superpowers/sdd/2026-08-20-phase5-techniques/progress.md:3` (ledger
  header: settled-direction record + owner decision policy, quoted above).
- `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:500-518`
  (Main-loop ownership decision; Phase 4 exit-review registry items (a)/(b)).
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue25-window-hardening.md`
  and `matrix-issue16-imgui-overlay.md` (full files — structural template
  for this matrix, and issue25's own rows 1-9 are the origin of the
  `NeedsRecreate`/`Suspended`/`SurfaceLost` state machine audited below).
- `samples/01_triangle/main.cpp:660-1005` (full `runPresent()`, the
  canonical hand-rolled loop every later sample's own comments point back
  to: "see samples/01_triangle/main.cpp's runPresent() for the full...
  explanation").
- `samples/02_hotreload/main.cpp`, `03_bindless_mesh/main.cpp`,
  `04_streaming/main.cpp`, `05_multipass/main.cpp`, `06_materials/main.cpp`,
  `07_stress/main.cpp`, `08_gltf_viewer/main.cpp`, `09_scene/main.cpp` —
  grepped for `vkAcquireNextImageKHR`/`NeedsRecreate`/`SurfaceLost`/
  `runPresent`/`isSurfaceLost`/`SwapchainStatus::` and the matching
  `runPresent()`/`recordForwardChunk()`/graph-recompile regions read
  directly (line numbers cited per-row in the Present-loop survey below).
- `src/rx_rhi_vk/include/rx_rhi_vk/device.h:9-43` (`SwapchainStatus` enum:
  `Ok`/`NeedsRecreate`/`DeviceLost`/`Suspended`/`SurfaceLost`, each with its
  own contract comment) and `src/rx_rhi_vk/src/device.cpp:611-703,713-828`
  (`acquireNextImage()`/`present()`/`recreateSwapchain()` — the real
  authority on when `surfaceLost_` becomes true and which status each
  function returns).
- `samples/08_gltf_viewer/main.cpp:971-992,1009-1021` and
  `samples/09_scene/main.cpp:1023-1044,1062-1074` — BOTH halves of the
  duplicated material-param machinery: the descriptor-set-LAYOUT
  construction block (identical `VkDescriptorSetLayoutBinding` binding 0 /
  `UNIFORM_BUFFER` / count 1 / `VERTEX|FRAGMENT` +
  `vkCreateDescriptorSetLayout`, 09's own comment: "see
  samples/08_gltf_viewer's own identical block for the full rationale")
  AND the demand-sized arena factory (`createMaterialParamArena()`, both
  full function bodies, byte-identical logic).
- `src/rx_rhi_vk/tests/descriptor_arena_test.cpp:286` (confirms this file
  is a comment cross-reference to the sample's `createMaterialParamArena`,
  not itself a duplication site — it exercises `DescriptorArena::create`
  directly).
- `src/rx_material/include/rx_material/instance.h` (full file) — the
  existing `rx::material::ParamArena` class (a *different* abstraction:
  instance-blob→descriptor-set binding, not the demand-sized
  `rx::rhi::DescriptorArena` wrapper the samples hand-roll).
- `samples/09_scene/mouse_capture.h` (107 lines), `fly_camera.h` (94
  lines), `grid_layout.h` (61 lines), `window_resize.h` (108 lines) — all
  four read in full.
- `src/rx_platform/include/rx_platform/window.h:40-118,166,285-289` —
  existing `setRelativeMouseMode()`/`consumeMouseDelta()`/`isKeyDown()`
  (the low-level facility mouse_capture.h's state machine sits ON TOP of,
  not a duplicate of).
- `src/rx_scene/include/rx_scene/camera.h` (full file, 187 lines) — the
  existing `rx::scene::Camera` (projection/view only; no yaw/pitch/
  fly-rig, confirming `fly_camera.h`'s `FlyCamera` would be a genuine
  extension, not a duplicate).
- `samples/09_scene/draw_recording.h` (102 lines) and `.cpp` (70 lines),
  full files — `splitByBlockAndGroup()`/`materialIndexForSpan()`.
- `src/rx_scene/include/rx_scene/draw_list.h:581` (`resolveDrawGroups()`
  signature: `[[nodiscard]] std::vector<ResolvedDrawGroup>`, fresh
  allocation per call).
- `samples/09_scene/main.cpp:2048-2060` (the one, main-thread,
  once-per-frame `resolveDrawGroups()` call site) and `:2131-2260`
  (`recordForwardChunk()`, the per-chunk-per-worker-per-frame
  `splitByBlockAndGroup()` call site at `:2203`) and `:2350-2352`
  (`setExecuteChunked()` registration) and `:2894-2896`
  (`chunkCountForWorkerCount(scheduler->workerCount())` — chunk count
  scales with worker threads, confirming this is genuinely a
  multi-worker-per-frame allocation site, not merely per-frame).
- `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:665-709`
  (Task 23, full text — the "capacity-snapshot" methodology, gate-hardened
  2026-08-18: "NO global operator-new interposition... capacity-snapshot
  via test-only accessors").
- `src/rx_graph/include/rx_graph/executor.h:100-181`
  (`ExecutorChunkDebugStats`/`debugChunkStats()`/
  `ExecutorAllocationCapacitiesForTesting` — the real, shipped seam Task 23
  built, cited as the reusable pattern) and
  `src/rx_scene/tests/draw_list_test.cpp:1182-1262` ("D26 zero-alloc
  invariant" test — capacity **and** `.data()` pointer identity, both
  checked, with the header's own documented rationale for why capacity
  alone is insufficient).
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/phase4-exit-review.md`
  — read in full; `I1` (`:96-127`, "Frames-in-flight host-write race... in
  BOTH exit samples", naming `samples/09_scene/main.cpp` and
  `samples/08_gltf_viewer/main.cpp` explicitly, never 07_stress), `M4`
  (`:207-211`, verbatim below), and the post-fix-wave re-verdict
  (`:349-357`, "I1 — CLOSED", confirming both samples independently landed
  their own `std::array<Buffer, kFramesInFlight>` fix) and `:379-380`
  ("M3/M4 — registry-recorded by the coordinator").
- `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:510-518`
  (registry items (a)/(b) verbatim, quoted in full in row 6 below).
- `samples/08_gltf_viewer/main.cpp:773-1058,1382-1543` and
  `samples/09_scene/main.cpp:771-1137,1537-1544,2062-2117,2246-2260`
  (`drawDataBuffers[]`/`drawDataBufferHandles[]`/`currentFrameSlot` — the
  duplicated per-FIF pattern both samples independently hand-built).
- `samples/common/reference_gate.h:1-40` and `samples/common/CMakeLists.txt`
  (the one existing precedent for shared-but-not-engine-public sample
  code: a `sample_common` static library, `namespace rx::samples`, linked
  by `rx_core`+`rx_rhi_vk`).
- CMake dependency edges, read directly this session:
  `src/rx_platform/CMakeLists.txt:21` (`rx_platform` links
  `SDL3::SDL3-static`/`Vulkan::Headers`/`rx_core` only);
  `src/rx_rhi_vk/CMakeLists.txt:42,101` (`rx_rhi_vk` does NOT link
  `rx_platform`; only its test binary does);
  `src/rx_graph/CMakeLists.txt:39` (`rx_graph` links
  `rx_core`/`rx_rhi_vk`/`rx_task`, NOT `rx_platform`); and — decisive for
  Open Questions #3 — `src/rx_debug_ui/CMakeLists.txt:13`:
  `target_link_libraries(rx_debug_ui PUBLIC imgui rx_platform rx_rhi_vk
  rx_graph rx_core)` — a production engine module ALREADY layered above
  all three, disproving any "rx_platform has no engine-library dependents"
  premise and establishing the layering precedent the present-loop helper
  needs.
- `src/rx_graph/include/rx_graph/pass.h:1-23` — the header-hygiene
  discipline ("pass.h/render_graph.h/resources.h/barriers.h stay
  device-free headers (no VkCommandBuffer, no volk, no rx_rhi_vk)"),
  relevant to where the recompile-skip logic may live (row 9 / Open
  Questions #4).

---

## Present-loop survey

Required deliverable (ticket text: "output includes the audit table
itself"). All nine `samples/*/main.cpp` hand-roll the identical
acquire→status-handle→recreate→present shape — eight of them (01-08) are
near-verbatim copies of `01_triangle`'s own loop (each carries a comment
pointing back to it, e.g. `02_hotreload/main.cpp:866`: "see
samples/01_triangle/main.cpp's runPresent() for the full... rationale");
09_scene diverges structurally (Issue #33/#36/#73 hardening layered on
top of the same base shape, never backported to 01-08).

| Sample | `runPresent()` | `Suspended` handled | `NeedsRecreate` handled | Direct `SurfaceLost` status branch | Live drag-resize (proactive, not reactive-only) | Graph recompile on recreate |
|---|---|---|---|---|---|---|
| 01_triangle | `:680` | Yes (`:793`, nested `isSurfaceLost()` check `:812`) | Yes (`:839` acq, `:960` present) | **No** — only nested `device->isSurfaceLost()` inside Suspended/NeedsRecreate | No (not resizable) | N/A — no RenderGraph |
| 02_hotreload | `:856` | Yes (`:1019`) | Yes (`:1054`,`:1176`) | **No** | No | N/A — no RenderGraph |
| 03_bindless_mesh | `:1267` | Yes (`:1455`) | Yes (`:1493`,`:1642`) | **No** | No | N/A — no RenderGraph |
| 04_streaming | `:1610` | Yes (`:1772`) | Yes (`:1807`,`:1936`) | **No** | No | N/A — no RenderGraph |
| 05_multipass | `:1963` | Yes (`:2139`) | Yes (`:2180`,`:2285`) | **No** | No | **Unconditional** — `compileForExtent()` lambda (`:2067`) recompiles on every recreation, incl. a pure vsync toggle |
| 06_materials | `:1587` | Yes (`:1796`) | Yes (`:1837`,`:1939`) | **No** | No | **Unconditional** — same `compileForExtent()` shape (`:1700`) |
| 07_stress | `:1512` (`Args&`) | Yes (`:1662`) | Yes (`:1706`,`:1771`) | **No** | No | **Never** — comment (`:1666-1668`) states this sample "never re-runs graph.compile() on resize" |
| 08_gltf_viewer | `:2138` (`Args&`) | Yes (`:2308`) | Yes (`:2351`,`:2426`) | **No** | No | **Never** — identical comment (`:2312-2314`) |
| 09_scene | `:3062` (`Args&`) | Yes (`:3576`, routed through `recreateSwapchainAndDependents()` `:3341`) | Yes (`:3585`,`:3651`) | **Yes** — explicit branches at `:3563` (acquire) and `:3645` (present), labeled "[Issue #73] Defense-in-depth" | **Yes** — `pixelSizeRequiresRecreate()` (`:3517`, window_resize.h) drives proactive recreation from `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` | **Conditional, correct** — `graphNeedsRecompileForExtent()` (`:3382`, window_resize.h) skips compile()/realize() when the extent didn't actually change |

**Findings, not just a status table:**

1. **A real, if narrow, correctness gap in samples 01-08's `SurfaceLost`
   handling.** `Device::acquireNextImage()`/`Device::present()`
   (`device.cpp:630-631,671-672`) check `surfaceLost_` **first** and
   return `SwapchainStatus::SurfaceLost` directly, before even checking
   `isSuspended()` — a real, standalone status value, not merely a flag
   nested inside another status. Samples 01-08 never write
   `if (acquire.status == SwapchainStatus::SurfaceLost)` anywhere (grep
   confirmed, zero hits in all eight files) — they only call
   `device->isSurfaceLost()` (a method, not the status enum) nested
   **inside** the `Suspended` and `NeedsRecreate` branches, immediately
   after calling `recreateSwapchain()`. In every path these eight samples'
   own code can reach today this happens to work, because
   `recreateSwapchain()` is the only call site that ever sets
   `surfaceLost_ = true` (`device.cpp:803`), and both branches that call it
   check `isSurfaceLost()` in the same iteration. But it is fragile, not
   structurally guaranteed the way 09_scene's direct top-level branch is:
   e.g. `01_triangle`'s own pre-loop `--vsync` `recreateSwapchain()` call
   (`:742`, before the loop starts) never checks `isSurfaceLost()` at all —
   if the surface were already gone at that point, the loop's first
   `acquireNextImage()` would return `SurfaceLost` directly, matching none
   of the three branches present (`Suspended`/`NeedsRecreate`/`DeviceLost`),
   and fall through into normal frame recording against a dead swapchain.
   09_scene's own `:3563-3572`/`:3645-3650` comments call their identical
   direct branches "defense-in-depth... not expected to be reachable in
   this loop's own control flow" — i.e., 09_scene's author already
   identified this exact class of gap and closed it defensively; 01-08
   were never updated to match.
2. **Three independently-invented, mutually inconsistent answers to "does
   the render graph need to recompile after `recreateSwapchain()`
   succeeds?"** — see the table's last column. 05/06 always pay the
   recompile+realize cost (correctness-safe, wasteful on a pure
   present-mode toggle). 07/08 never pay it (cheap, but silently depends
   on "this sample's extent never truly changes across a recreation" being
   true forever — true today only because neither sample's window is
   resizable and neither has a runtime fullscreen toggle; the CLI
   `--fullscreen` flag is applied once, before the graph's first
   `compile()`, so it never exercises this gap). 09_scene alone computes
   the right answer per-recreation. This is exactly the "swapchain-relative
   re-compile-on-extent-change contract" the scope-growth comment names.
3. **`runPresent()`'s own signature has already drifted twice**: 01-06
   take three positional bools/enums (`bool enableValidation,
   PresentMode vsyncMode, bool fullscreen`); 07-09 take one
   `const Args&`. Not a correctness bug, but relevant to designing the
   shared helper's frame-body-callback signature — it cannot assume one
   calling convention already exists to build on.

---

## The matrix

| # | Hand-roll item | Sites (verified) | Disposition | Evidence / promoted-API shape | Proposed acceptance criterion |
|---|---|---|---|---|---|
| 1 | `createMaterialParamArena` + the material-param descriptor-set-LAYOUT block | TWO duplicated halves, both verified side-by-side: (a) the arena factory — `samples/08_gltf_viewer/main.cpp:1009-1021`, `samples/09_scene/main.cpp:1062-1074` — **byte-identical** logic (only the log-message sample name differs): `sets = max(materialCount, 1)`; `DescriptorArena::Capacities{maxSets=sets, uniformBuffers=sets}`; `DescriptorArena::create(device, framesInFlight=1, capacities)`; (b) the set-layout construction — `08:971-992`, `09:1023-1044` — identical `VkDescriptorSetLayoutBinding` (binding 0, `UNIFORM_BUFFER`, count 1, `VERTEX\|FRAGMENT`) + `vkCreateDescriptorSetLayout`, 09's own comment: "see samples/08_gltf_viewer's own identical block for the full rationale". | `promote` | Clean promotion, both halves together — zero sample-specific state (only `App.device`, the returned arena, and the layout handle). `rx_material` already has a **different**, non-conflicting "arena" concept (`rx::material::ParamArena`, `instance.h`, an instance-blob→descriptor-set binder) — the promoted API is a small factory (e.g. a free function or a `MaterialSystem`-adjacent static, see Open Questions #1 for naming) returning the layout+arena bundle, not a merge into `ParamArena` (different abstraction level). | Both samples call the promoted API; grep-enforced deletion of BOTH halves: `grep -rn "DescriptorArena::Capacities{.*uniformBuffers" samples/` AND `grep -rn "vkCreateDescriptorSetLayout" samples/` (for the material-param block specifically) return zero hits outside the new engine call site; existing GPU pixel gates (08/09 D17) byte-identical. |
| 2 | Per-FIF draw-data buffer pattern (exit-review fix I1) | `samples/08_gltf_viewer/main.cpp:773-1058,1382-1543` and `samples/09_scene/main.cpp:771-1137,1537-1544,2062-2117` (09 duplicates the pattern a **second** time internally, for `shadowDrawDataBuffers`). Both: `std::array<std::optional<Buffer>, kFramesInFlight>` + a parallel `std::array<BindlessHandle, kFramesInFlight>`, created+bindless-registered per slot at setup, `memcpy`+`flush()` into `[currentFrameSlot]` after `vkWaitForFences(currentFence)`, release+reset at teardown. | `promote` | Non-trivial GPU-resource-lifecycle logic independently re-implemented twice (three times counting 09's own internal shadow-buffer duplicate) — exactly the pattern phase4-exit-review's I1 fix closed per-sample instead of once, engine-side. Genuinely reusable shape: "N-buffered (`kFramesInFlight`), bindless-registered, host-visible storage buffer; caller writes only the current-frame-slot after its own fence wait." Composes ONLY `rx_rhi_vk` types (`Buffer`, `BindlessTable` handle, `FrameSync::kFramesInFlight`) — zero scene-specific content, which decides its home (Open Questions #2). | A generic helper (recommended home `rx_rhi_vk`, Open Questions #2) used by both 08 and 09 for `drawDataBuffers` AND 09's `shadowDrawDataBuffers`; GPU test proves the double-buffering discipline (write slot N never touches a buffer whose slot-N fence hasn't been waited); existing I1 fix stays byte-identical in behavior. |
| 3 | `mouse_capture.h` (`FlyThroughCaptureState`, `mouseDeltaDrivesCamera()`, `escTogglesCapture()`) | `samples/09_scene/mouse_capture.h`, full file (107 lines), consumed at `main.cpp:3427-3475`. | `promote` → `rx_platform` | Fully device-free/SDL-free already (own header comment, `:4`: "no rx::platform::Window dependency"). Sits cleanly ON TOP of the existing low-level `Window::setRelativeMouseMode()`/`consumeMouseDelta()` (`rx_platform/window.h:102,118`) — it is the missing **policy** layer (when to call the existing mechanism), not a duplicate of the mechanism itself. No DamagedHelmet/grid/scene dependency of any kind; the "FlyThrough" name is the only sample-flavored artifact, trivially renamed on promotion. | Promoted as (e.g.) `rx::platform::MouseCaptureToggle`, consumed by 09_scene; `samples/09_scene/mouse_capture.h` deleted (grep-enforced); existing `samples/09_scene/tests/test_mouse_capture.cpp` moves/adapts to `rx_platform`'s own test target, asserting the same toggle-transition matrix. |
| 4 | `fly_camera.h` (`FlyCamera` struct, `flyCameraLocalMoveDelta()`, `keyboardDrivesCamera()`) | `samples/09_scene/fly_camera.h`, full file (94 lines), consumed at `main.cpp` (`updateFlyCamera()`). | `promote` → `rx_scene` | `rx_scene::Camera` (`camera.h`, full file read) has `position`/`orientation`/`forward()`/`right()`/`up()`/projection helpers but **no** yaw/pitch state, no `moveLocal()`, no fly-rig at all — `FlyCamera` is a genuine extension (wraps `rx::scene::Camera`, adds `yawRadians`/`pitchRadians`/`applyLookDelta()`/`moveLocal()`), not a reimplementation of something that already exists. Zero scene/grid/material dependency — pure glm + `rx_scene/camera.h`. The plan's own "Files" list (`plan:271-274`) explicitly names "fly-camera... facilities per ruling" under `src/rx_scene`, anticipating this exact promotion. | Promoted as `rx::scene::FlyCamera` (or similar), consumed by 09_scene and available to Stage 1's 10_lights/11_surfaces samples (both will need free-look camera rigs); `samples/09_scene/fly_camera.h` deleted (grep-enforced); `test_fly_camera.cpp`'s W/S-inversion regression assertions move with it. |
| 5 | `grid_layout.h` (`gridTransform()`, `gridInstanceTransform()`) | `samples/09_scene/grid_layout.h`, full file (61 lines), consumed only by `populateHelmetGrid()` (the default-scene demo-content generator). | `rule-sample-local-stands` | Genuine per-item ruling, not a default. The two functions are mechanically pure/generic (grid-cell translation + compose-with-asset-node-transform), **but** the *concept* they encode — "arrange N instances of one imported asset into a procedural showcase grid" — is demo-content generation for this one sample's fallback scene, not a facility a host game embedding RendererX consumes as an engine API (a real game places its content via its own scene data/tooling, not a "grid demo" helper). Reinforced structurally: `grid_layout.h`'s own header comment and its dedicated test (`test_grid_layout.cpp`) are written entirely in terms of one hard-coded external asset's node quaternion (DamagedHelmet.gltf), the opposite of the asset-agnostic framing `fly_camera.h`/`mouse_capture.h` both carry. Nor does it even hand-roll an engine facility in the binding rule's own sense — its only dependencies are glm math (the rule's named examples are descriptor arenas, per-FIF buffers, mouse capture, node transforms *as engine machinery* — not content-placement arithmetic). No other current or near-term sample (10_lights/11_surfaces/12_bistro, per the Stage 1-4 task list) needs a demo-grid layout function — Bistro is a curated authored scene, not a proceduralized instance grid. | No code change required by this ruling; if a future sample needs the identical demo-grid pattern, it is a fresh, cheap promotion call at that time (a two-function pure-math header), not a cost this ticket is paying to defer. |
| 6 | `splitByBlockAndGroup()`/`resolveDrawGroups()` — worker-side per-frame vector allocations | `samples/09_scene/draw_recording.h/.cpp` (`splitByBlockAndGroup`, called once **per chunk per worker per frame** from `recordForwardChunk()`, `main.cpp:2203`, chunk count = `chunkCountForWorkerCount(workerCount)`, `:2894-2896`); `rx_scene/draw_list.h:581` `resolveDrawGroups()` (called once per frame, main-thread, `main.cpp:2054`). Both `[[nodiscard]] std::vector<...>` return-by-value, fresh heap allocation every call — confirmed exactly by phase4-exit-review's own **M4** finding (`phase4-exit-review.md:207-211`, quoted in full): "`splitByBlockAndGroup()` returns a fresh `std::vector` per chunk per frame..., and `resolveDrawGroups()` returns a fresh vector per frame... Sample-side, outside the Task-23 zero-alloc mandate's letter, but contrary to its spirit on the hottest path. Registry-material." Master registry item (b) (`toolchain-platform-rhi-design.md:513-518`, verbatim): "Sample-recorder worker-side per-frame vector allocations (`splitByBlockAndGroup`/`resolveDrawGroups` return values) — extend the zero-alloc discipline to the sample/consumer recording path when the scene path is next reworked (geometry or techniques phase)." | `promote` (zero-alloc, into `rx_scene`) | `resolveDrawGroups()` already lives in `rx_scene` (engine-owned) — its own allocation is in-scope for the SAME zero-alloc conversion, not just `splitByBlockAndGroup()` (the ticket text names both explicitly; M4's "Sample-side" framing describes the call-site load, not the function's ownership). `splitByBlockAndGroup()` (currently sample-local) is the higher-severity site (per-chunk × per-worker × per-frame, not merely per-frame) and should be promoted alongside its zero-alloc conversion, per the plan's "zero-alloc recorder helpers in `rx_scene`" text (`plan:267-268`). **Methodology is prescribed, not open**: reuse Task 23's own gate-hardened "capacity-snapshot via test-only accessor, NOT global operator-new interposition" pattern (`docs/superpowers/plans/2026-08-11-phase4-scene-assets.md:692-698`), matching the shipped precedent `ExecutorAllocationCapacitiesForTesting`/`debugChunkStats()` (`rx_graph/executor.h:100-181`) and `rx_scene`'s own existing "D26 zero-alloc invariant" test (`draw_list_test.cpp:1182-1262`) — **both** `.capacity()` **and** `.data()` pointer identity checked across N steady-state frames (capacity alone is an empirically-verified-insufficient signal per that test's own documented rationale). | Both functions convert to write into caller-owned persistent scratch (or an `Impl`-held buffer, matching Executor's own precedent); capacity-snapshot test (steady-state N-frame run, zero growth after warm-up, pointer identity held) for both; existing byte-identical draw-order/grouping behavior proven by existing 09 pixel/counter gates; worker-safety re-verified (the promoted `splitByBlockAndGroup` still runs on worker chunks ≥ 1 — persistent scratch must be per-worker-slot, not a single shared buffer, or this reintroduces a data race the current fresh-`std::vector`-per-call design accidentally avoided). |
| 7 | `window_resize.h` (`f11TogglesFullscreen()`, `pixelSizeRequiresRecreate()`, `graphNeedsRecompileForExtent()`) — **ticket-enumeration gap**: the same extracted-pure-logic pattern as rows 3-5's three named files, but absent from the ticket's own enumeration | `samples/09_scene/window_resize.h`, full file (108 lines) — its own header comment (`:2-7`) states it follows "the same 'device-free pure logic gets its own header' precedent" as fly_camera.h/mouse_capture.h. Consumed at `main.cpp:3382,3450,3517`. | `promote` (absorbed by the present-loop centralization, rows 8-10 — not a separate standalone promotion) | This file IS the already-factored-out decision logic the scope-growth comment's helper needs internally: `graphNeedsRecompileForExtent()`/`pixelSizeRequiresRecreate()` are the recompile-on-extent-change contract (row 10) and the proactive drag-resize trigger; `f11TogglesFullscreen()` is present-mode UX policy of the same shape as `escTogglesCapture()` (row 3's promoted home). Its real destination is inside the shared helper (or `RenderGraph::compile()` itself, for the recompile-skip half — Open Questions #4), not an independent header promotion. Flagged as a genuine gap in the ticket's own enumeration so the audit table has zero undispositioned hand-rolls. | `samples/09_scene/window_resize.h` deleted once rows 8-10 land (grep-enforced); its three functions' existing device-free unit tests move to the helper's (or `rx_graph`'s) own test target with assertions intact. |
| 8 | Present-loop shared helper — overall shape (acquire → status handling → recreate-and-dependents → present, frame-body callback) | Scope-growth comment, quoted in full above; prototype-in-practice is 09_scene's own `recreateSwapchainAndDependents()` lambda (`main.cpp:3341-3400`) + its call sites (`:3488,3518,3539,3578,3586,3652`) — the closest existing thing in this repo to the requested helper's "recreate-and-dependents" half, though it is itself sample-local, not shared. The status-handling PRIMITIVES (`SwapchainStatus`, `AcquireResult`, `isSuspended()`/`isSurfaceLost()`, `recreateSwapchain()`) are already one shared, well-documented `rx_rhi_vk` surface — what is duplicated nine times is the ORCHESTRATION around them, which is exactly where the scope-growth comment's named integration bugs lived. | `needs-coordinator-decision` (module home only — the helper itself is settled direction per `progress.md:3`) | See Conflicts and Open Questions #3 for the module-home tension and its resolution. Frame-body-callback placement (verified against 08/09's inner loops): the genuinely per-sample-varying part is narrow — which scene-update/HUD/record functions run between "acquire succeeded, recreate-and-dependents resolved" and "submit+present" — everything else (fence discipline, status branches, recreation, view/FrameSync rebuild, present-status handling) is identical across all nine and belongs to the helper. The callback receives at minimum the current frame-slot index and extent; exact signature is implementation detail. | Coordinator rules on the module home (Open Questions #3) before implementation; once ruled, acceptance is: one shared present-loop entry point, a frame-body callback parameter, consumed by all nine samples' `runPresent()`, with a one-line D5 thread-affinity statement (main-thread-only, matching `Device`'s own `RX_ASSERT_MAIN_THREAD` surfaces); the helper's pure decision functions (when to recreate/recompile/stop) are device-free unit-testable, exactly like the window_resize.h logic being absorbed (row 7) already is. |
| 9 | Present-loop — `NeedsRecreate`/`Suspended`/`SurfaceLost` status handling, consistently | Present-loop survey above, finding 1: 01-08 have **no** direct `SwapchainStatus::SurfaceLost` branch (only nested `isSurfaceLost()` checks that work today by structural coincidence, not by an explicit contract); 09_scene alone has the direct, defense-in-depth branch. | `promote` (fold into the shared helper) | The shared helper is the correct, single place to fix this once instead of patching eight call sites individually — matches 09_scene's own already-proven-correct shape (top-level branches for `SurfaceLost`/`Suspended`/`NeedsRecreate`/`DeviceLost`, in that priority order, matching `device.cpp`'s own check order at `:630-634`/`:671-675`). | Helper's status-handling switch has an explicit `SurfaceLost` case (not merely a nested method check) as its own binding acceptance criterion; a targeted regression test constructs a `Device` already in the surface-lost state and asserts the helper's very first `acquireNextImage()` call is handled without falling through to frame recording. |
| 10 | Present-loop — swapchain-relative re-compile-on-extent-change contract | Present-loop survey finding 2: three independently-arrived-at behaviors (05/06 unconditional recompile, 07/08 never recompile, 09 conditional/correct — `pixelSizeRequiresRecreate()`/`graphNeedsRecompileForExtent()`, `window_resize.h`, full file read). | `promote` (see Open Questions #4 for helper-vs-graph placement) | The scope-growth comment explicitly offers both placements ("moves inside the helper or the graph itself"). `RenderGraph::CompileInfo` (`render_graph.h:20-38`) is a small, trivially-comparable POD (`width`/`height`/`format`/`backbufferFinalLayout`) — `RenderGraph::compile()` could itself cache the last-applied `CompileInfo` and early-return when unchanged, which would fix 07/08's silent gap and 05/06's wasted work for **every** caller automatically, not only ones that adopt the new shared present-loop helper. Whichever placement wins, the ONE canonical implementation adopted is 09_scene's hardened conditional logic — a real, in-passing correctness/performance improvement for 8/9 samples, an intended positive side effect of centralization, not scope creep. | Whichever placement the coordinator rules (Open Questions #4), the acceptance criterion is behavioral, not structural: a present-mode-only recreation (same extent) triggers zero `RenderGraph::compile()`/`Executor::realize()` calls; a genuine extent change triggers exactly one of each; both proven by a counting test (matching this project's own established counter-assertion convention, e.g. Task 23's `ExecutorChunkDebugStats`). |
| 11 | Present-loop — deletion of the nine duplicated hand-written loops | Scope-growth comment's explicit deliverable. | `consume-now` (once 8-10 land) | The Phase-4 gate-matrix precedent for "prove a duplicated pattern is actually gone, not just superseded" is direct grep enforcement (matrix-issue16's own row 12 CMake dependency-boundary proposal is the same discipline one level up — a mechanism check, not a promise). | `grep -rln "SwapchainStatus::NeedsRecreate" samples/*/main.cpp` returns **zero** files after the migration (every sample's own status-handling code is gone, replaced by a call into the shared helper) — this exact grep is the report-time proof, run and pasted into the closing report, not merely asserted. |
| 12 | Present-loop — CLI-signature unification (`bool,bool,PresentMode` vs `const Args&`, survey finding 3) | Present-loop survey finding 3. | `promote` (fold into helper design, not a separate item) | Not named explicitly in the ticket text but directly load-bearing for designing one shared entry point across nine call sites with two different existing calling conventions today. Standardize on the `Args`-struct shape (07-09's — the more recently evolved, richer convention the samples' own history has been trending toward); the six older samples migrate to it as part of adopting the helper (mechanical signature change, not behavior change), rather than the helper supporting two calling conventions. | The shared helper's entry-point/callback signature is designed against the `Args`-struct shape; acceptance is a design-review note, not a new automated test. |

---

## Conflicts

1. **The scope-growth comment's "shared frame-loop helper... consumed by
   all samples" sits in tension with the Main-loop-ownership decision's
   "no engine-owned loop... never the reverse"**
   (`toolchain-platform-rhi-design.md:500-508`, decided 2026-08-10, ten
   days before this scope growth landed). The same decision's own text
   also supplies the resolution shape: an "optional thin 'runner'
   convenience (window + per-frame callback)... built ON the library API
   and never required by it" is explicitly permitted — so an engine-side
   helper is legal **iff** it is optional, layered ON the libraries (not
   inside `rx_graph`/`rx_rhi_vk`'s required surfaces), and never required
   by the library API. The plan's Global Constraint pushes toward "promote
   into the engine" (`plan:88-93`), while the ledger's own phrasing is
   "engine/**shared** facilities" (`progress.md:3`) — the ticket text
   itself never names a module for the helper (unlike its precise "Files"
   list for every other promoted item). Resolved as a recommendation, not
   silently assumed: Open Questions #3.
2. **The registry's own SDK-phase "optional thin runner convenience"
   item** (`toolchain-platform-rhi-design.md:506-508`) **and this ticket's
   present-loop helper are the same shape, pulled six phases earlier, not
   two different things** — a "runner" is exactly "window + per-frame
   callback... built ON the library API and never required by it," which
   is precisely acquire→status→recreate→present + a frame-body callback.
   If the coordinator's ruling on Open Questions #3 lands the helper as an
   engine module (the recommendation), this ticket should be understood as
   **seeding** that SDK-phase registry line early, not as independent,
   additional scope — the registry entry should be updated to say so
   rather than left to be "discovered" stale later.
3. **05/06's `compileForExtent()` (always recompile) and 07/08's "never
   recompile" comment are themselves in quiet, unacknowledged conflict
   with each other** — neither pair's own code or comments engage with
   the other pair's choice; both were arrived at independently, and
   neither cites 09_scene's later, correct, conditional answer (09_scene
   postdates both, per its own "[Issue #73 round-review hardening]"
   framing). Not a conflict between binding project artifacts, but a
   conflict between shipped sample code that the audit table above
   surfaces for the first time as a named, deliberate finding rather than
   an assumption.

---

## New gaps

- **Samples 01-08's missing direct `SwapchainStatus::SurfaceLost` branch**
  (present-loop survey finding 1; matrix row 9) is not named in the
  ticket text, the plan, `feature-gap-audit.md`, or the master registry —
  a genuine correctness gap this audit surfaced by reading `device.cpp`'s
  real check order rather than trusting the samples' own comments (which
  claim parity with 01_triangle's "for the full... rationale" without
  those samples ever having been updated for 09_scene's later Issue #73
  hardening). Proposed fit: closed in-round by this ticket (rows 9/11),
  not deferred — it is a direct, mechanical consequence of building the
  shared helper correctly, not separable extra work.
- **`samples/09_scene/window_resize.h` is absent from the ticket's own
  enumeration** (matrix row 7) despite being the same
  extracted-pure-logic pattern as the three files the ticket names — and
  it is not incidental: it holds exactly the recompile-on-extent-change
  decision logic the scope-growth comment's centralization must absorb.
  Dispositioned in-table (row 7), flagged here so the ticket's
  enumeration is corrected rather than silently extended.
- **`RenderGraph::compile()` has no self-knowledge of "was this recompile
  actually necessary"** — every caller (05/06's `compileForExtent()`,
  09's `graphNeedsRecompileForExtent()`, 07/08's total silence) currently
  reimplements or omits this decision independently, because
  `RenderGraph`/`CompileInfo` itself carries no memory of the last
  `compile()` call. Not previously named anywhere as a `rx_graph`-level
  gap (window_resize.h frames it as sample-local pure logic, not as
  something missing from the graph). Proposed fit: this ticket's own
  scope (row 10), per the scope-growth comment's explicit "moves inside
  the helper or the graph itself" — not a new registry line.
- **`rx_material` already has an unrelated type also named "arena"**
  (`rx::material::ParamArena`, `instance.h`) distinct from the
  `rx::rhi::DescriptorArena` the promoted `createMaterialParamArena`
  wraps — not a functional gap, but a naming-collision risk this ticket's
  implementation should actively avoid (Open Questions #1), surfaced here
  because nothing in the ticket text or plan flags it.

---

## Open Questions

1. **Naming for the promoted material-param-arena API (row 1).**
   **Recommend a free function under a name that does NOT contain
   "ParamArena"** (e.g. `rx::material::createDemandSizedDescriptorArena`
   or similar), explicitly distinct from the existing
   `rx::material::ParamArena` class — the two are genuinely different
   abstractions (a raw demand-sized `rx::rhi::DescriptorArena` factory +
   set-layout bundle vs. an instance-blob→descriptor-set binder) and
   sharing a name invites a future reader to conflate them. A
   `MaterialSystem`-owned static factory is an acceptable alternative
   shape (MaterialSystem already owns the per-material param-UBO
   convention); the binding part of this recommendation is only the
   naming-collision avoidance. Decisive, low-stakes.
2. **Module home for the promoted per-FIF draw-data buffer helper (row
   2).** **Recommend `rx_rhi_vk`** — revised from this matrix's earlier
   draft (which said `rx_scene`) after the dependency check settled it:
   the helper composes ONLY `rx_rhi_vk` types (`Buffer`, `BindlessTable`
   handle, `FrameSync::kFramesInFlight`) and has zero scene-specific
   content; `FrameSync` — the pattern's natural sibling — already lives at
   that layer, and an RHI-level home keeps it available to future
   non-scene consumers (e.g. a compute-dispatch parameter ring). Housing
   an rhi-primitive composition in `rx_scene` because its first two call
   sites happen to be scene-shaped would put the idiom at the wrong layer.
3. **Module home for the shared present-loop helper (rows 7-8, Conflicts
   #1-2) — the single highest-leverage decision in this matrix.**
   **Recommend a NEW small engine module (e.g. `src/rx_frame_loop` or
   `src/rx_runner`), following `rx_debug_ui`'s exact, verified layering
   precedent** (`src/rx_debug_ui/CMakeLists.txt:13`:
   `target_link_libraries(rx_debug_ui PUBLIC imgui rx_platform rx_rhi_vk
   rx_graph rx_core)` — an optional engine module already layered above
   `rx_platform` + `rx_rhi_vk` + `rx_graph`, proving this shape is
   established practice in this codebase, not new ceremony). This
   satisfies all three binding texts simultaneously: the Global
   Constraint's "promote into the engine" (`plan:88-93`), the ledger's
   "engine/shared facilities" (`progress.md:3`), and the
   Main-loop-ownership decision's "optional... built ON the library API
   and never required by it" (`toolchain:506-508` — a separate optional
   module is exactly "ON the library API"; per Conflicts #2, this seeds
   the SDK-phase runner registry line early and that line should be
   updated to say so). **Rejected alternatives, with reasons:** (a) new
   files inside `rx_graph` with a new `rx_graph`→`rx_platform` PUBLIC
   dependency — rejected because it would force every `rx_graph` consumer
   (including host engines that own their own windowing, the project's
   PRIMARY audience per the library-model decision) to link
   `rx_platform`/`SDL3-static`, entangling the graph library with
   windowing permanently for a convenience only samples/quick-starts
   need; the premise under which this option was originally argued
   ("rx_platform has zero engine-library dependents") is factually wrong
   (`rx_debug_ui` links it), so the option's "no precedent for layering
   above rx_platform" implication collapses too. (b) `samples/common/`
   (the `reference_gate.{h,cpp}`/`sample_common` precedent) — a legitimate
   fallback if the coordinator wants zero new public surface this phase,
   but it satisfies the Global Constraint's "promote into the engine" only
   under a strained reading of the ledger's "engine/shared" phrasing, and
   it would leave the SDK-phase runner to be built a second time later;
   demoted to fallback, not recommended.
4. **Placement of the extent-recompile-skip logic (row 10): inside the new
   present-loop helper only, or inside `RenderGraph::compile()` itself.**
   **Recommend inside `RenderGraph::compile()` itself** (cache the last
   applied `CompileInfo`, early-return when byte-identical), not only
   inside the helper: it fixes 07/08's silent gap and 05/06's wasted work
   for every present-mode-affecting caller automatically, including any
   future sample or host code that never adopts the shared present-loop
   helper, rather than making correctness depend on helper adoption. The
   helper (wherever it lands, Open Questions #3) still owns *calling*
   `compile()` unconditionally after every successful
   `recreateSwapchain()` — it just stops needing to compute the skip
   decision itself. This placement is compatible with `rx_graph`'s
   header-hygiene discipline (`pass.h:9-22` — `render_graph.h` stays
   device-free; `CompileInfo` caching adds no Vk device dependency, only
   POD comparison). One caveat worth the coordinator's explicit sign-off:
   this changes `compile()`'s current "always recompiles" contract, so a
   hypothetical caller that intentionally wants to force a same-extent
   recompile (e.g. after mutating graph topology with unchanged
   `CompileInfo`) would need a new explicit force parameter — not observed
   as a real need anywhere in this codebase today, but worth naming rather
   than silently foreclosing. The helper-only alternative (09_scene's
   `graphNeedsRecompileForExtent()` moving into the helper verbatim) is
   the smaller change and remains acceptable if the coordinator prefers
   zero `rx_graph` contract movement this ticket.
5. **`grid_layout.h` disposition (row 5) — decisive ruling, not a survey.**
   **Rule sample-local stands.** Restated plainly per the task's own
   instruction to be decisive: this is demo-content-generation logic for
   one sample's fallback scene, evidenced by its own test suite being
   written against one hard-coded external asset's node transform, not a
   facility any current or near-term (Stage 1-4) sample needs — and its
   only dependency is glm math, so it does not even hand-roll engine
   machinery in the binding rule's own sense. Two independent research
   passes over this ticket (this matrix and the concurrent pass it
   supersedes, see Verification health) reached this same ruling
   separately. If a future ticket disagrees, promoting it later costs one
   small, pure-function header move — this ruling is not foreclosing
   anything expensive.

---

## Verification health

**Supersession note:** this file supersedes a concurrent, shorter
(174-line) research pass over the same ticket that briefly occupied this
path on 2026-08-20 (~22:45); that version had no present-loop survey and
a two-part table. Everything of verified value in it was merged here;
its claims were cross-checked against the real files first, with these
outcomes:
- **Merged (re-verified first-hand):** the `progress.md:3`
  settled-direction citation (quoted verbatim in the header — including
  its "engine/shared" phrasing and the owner decision policy line); the
  duplicated material-param descriptor-set-LAYOUT block (its cited line
  ranges were slightly off — corrected here to `08:971-992`/`09:1023-1044`
  after a direct read; 09's own "identical block" comment confirms the
  duplication is deliberate copy-paste), which extends row 1 beyond the
  arena factory alone; `window_resize.h` as an explicit
  ticket-enumeration-gap audit row (row 7 — my earlier draft discussed the
  file but left it undispositioned as a hand-roll item); the three CMake
  dependency-edge facts (`rx_platform:21`, `rx_rhi_vk:42,101`,
  `rx_graph:39` — all re-read and confirmed); the `pass.h:1-23`
  header-hygiene citation; the frame-body-callback placement analysis
  (row 8); the `Args`-struct signature-unification recommendation (row 12
  — both passes converged independently); and its `rx_rhi_vk`
  recommendation for the per-FIF helper home, which on re-examination is
  better-grounded than this matrix's own earlier `rx_scene` call (Open
  Questions #2 revised accordingly — the deciding fact, verified: the
  helper composes only `rx_rhi_vk` types).
- **Rejected (checked and found wrong or superseded):** its claim that
  "`rx_platform` is a genuine leaf with zero engine-library dependents in
  production code (only `rx_rhi_vk_tests` links it)" — **false**,
  `src/rx_debug_ui/CMakeLists.txt:13` links `rx_debug_ui PUBLIC ...
  rx_platform ...`; its module-home recommendation built on that premise
  (new `rx_graph` files + an `rx_graph`→`rx_platform` PUBLIC dependency)
  — rejected in Open Questions #3 with the corrected evidence; its
  "Conflicts: None found" — this matrix's three Conflicts stand (it did
  not engage the Main-loop-ownership spec decision at all); its
  uncertainty about the other eight samples' recreate/recompile behavior
  ("not individually read... the implementing task should confirm") —
  superseded by this matrix's per-sample verified survey table; and its
  implication that every sample has a "recreateSwapchainAndDependents()-
  shaped function" — imprecise (only 09_scene has that function; 05/06
  have `compileForExtent()`; 07/08 have inline realize-only calls; 01-04
  have no graph at all).

**Verified first-hand this session (read/grepped directly, not carried
over from any prior artifact):**
- Full contents of `mouse_capture.h`, `fly_camera.h`, `grid_layout.h`,
  `window_resize.h`, `draw_recording.h`, `draw_recording.cpp`.
- Both halves of the material-param duplication in both 08 and 09:
  `createMaterialParamArena()`'s full body (byte-identical modulo the
  log-message sample name) and the descriptor-set-layout construction
  block (identical, deliberately cross-referenced by 09's own comment).
- The real `SwapchainStatus` enum and `acquireNextImage()`/`present()`/
  `recreateSwapchain()` implementations in `device.h`/`device.cpp` — the
  present-loop survey's "no direct SurfaceLost branch in 01-08" finding
  and its "why this happens to still work today" reasoning are both
  derived directly from this code, not inferred from sample comments
  alone.
- Every one of the nine samples' `runPresent()` acquire/present status
  blocks, at the specific line numbers cited in the survey table (grepped
  then read in context, not grep-output-only).
- `resolveDrawGroups()`'s call site (once per frame, main thread) and
  `splitByBlockAndGroup()`'s call site (per chunk, `setExecuteChunked`,
  chunk count scales with worker count) — confirmed by reading the
  registration and call sites directly, not assumed from the ticket
  text's own framing.
- Phase 4 exit-review's `I1`/`M4` sections and their post-fix-wave
  re-verdict (`phase4-exit-review.md`), confirming (a) "both exit
  samples" = 08_gltf_viewer and 09_scene, never 07_stress, and (b) I1's
  fix is the exact duplicated pattern this ticket's row 2 promotes.
- Task 23's gate-hardened capacity-snapshot methodology
  (`2026-08-11-phase4-scene-assets.md:692-698`) and its real, shipped
  implementation (`executor.h`'s `ExecutorAllocationCapacitiesForTesting`/
  `debugChunkStats()`, `draw_list_test.cpp`'s own "D26 zero-alloc
  invariant" test) — both read directly, not paraphrased.
- The Main-loop-ownership decision and the two Phase-4-exit-review
  registry items (a)/(b), both read at their cited lines in
  `toolchain-platform-rhi-design.md`; `progress.md:3`'s ledger header.
- All four CMake dependency-edge declarations cited above, including
  `rx_debug_ui`'s (the one the superseded pass missed).
- `samples/common/reference_gate.h`/`CMakeLists.txt` — the structural
  precedent weighed (and demoted to fallback) in Open Questions #3.

**Inferred / lower-confidence (flagged explicitly, not presented as
fact):**
- The claim that 07_stress/08_gltf_viewer's "never recompile the graph on
  resize" is safe TODAY specifically because neither sample's window is
  resizable and their `--fullscreen` flag only applies once, pre-`compile()`
  — verified by reading their own CLI parsing and setup-order code, but
  not exercised by an actual runtime resize/fullscreen-toggle repro this
  session (no GPU/display session available in this research task's
  scope); flagged as a design-level inference from the code's own
  structure, not an empirically reproduced failure.
- Whether any other, not-yet-written Phase 5 sample content would need
  `grid_layout.h`'s specific demo-grid shape — the "rule sample-local
  stands" call (Open Questions #5) is a judgment against the currently
  known Stage 1-4 task list (10_lights/11_surfaces/12_bistro), not a proof
  no future need can arise.

**Dead links / access failures:** none — `gh issue view 41 --comments`
and every in-repo file read succeeded on the first attempt.

**Version ambiguities:** none found — all evidence is in-repo, HEAD at
session start; no external library/version claims are made in this
matrix. One concurrency note: this path was overwritten once mid-session
by the parallel pass described above; the merged version you are reading
was written after re-reading the file on disk and is the intended
authoritative version for the Stage 0 primary gate.
