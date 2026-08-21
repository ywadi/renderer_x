# Task 5 report — Material-path API-gap audit CLOSURE + present-loop centralization (issue #41)

Implementer round. Base: main `e607074` (post T4). Order of authority
followed: rulings (`rulings-2026-08-20.md`, "T5 (#41)") > plan (Task 5,
Stage 0) > gate matrix (`matrix-p5t05-material-audit.md`) > ticket (#41,
incl. the 2026-08-20 owner scope-growth comment).

## Status: COMPLETE

All 12 audit-table rows closed per their ruled disposition. All nine
samples build on both presets and consume `rx::frame_loop::PresentLoop`.
Full suite green on linux-native (lavapipe) and windows-cross-zig (Wine).
Every D17 gate byte-identical (0 failing pixels, references untouched).
Real-NVIDIA WM-close trial performed for every one of the nine samples,
each showing the identical, correctly-centralized Issue #73 sequence.
Two live revert-discrimination proofs performed and confirmed this round
(`PerFrameStorageBuffer::write()`, `RenderGraph::compile()`'s
recompile-skip cache).

## Ruling followed (T5, #41, `rulings-2026-08-20.md:100-105`)

> present-loop helper = NEW small engine module (rx_frame_loop shape,
> layered like rx_debug_ui); per-FIF draw-data helper lands in
> rx_rhi_vk; the extent-unchanged recompile-skip moves INSIDE
> RenderGraph::compile(); grid_layout.h stays sample-local. All nine
> samples convert to the helper in this task (that is the ticket's
> point).

Followed exactly — see per-row proof below.

## What shipped

**New engine module `rx::frame_loop` (`src/rx_frame_loop/`)** — layered
identically to `rx_debug_ui`
(`rx_platform`+`rx_rhi_vk`+`rx_graph`+`rx_core`, PUBLIC; no core library
gains a new dependency; a host embedding RendererX that owns its own
loop never needs to link it). `PresentLoop::create()`/`runFrame()`/
`recreateAndDependents()` is the single orchestration funnel: fence-wait
→ acquire → status-handle (`SurfaceLost`/`Suspended`/`NeedsRecreate`/
`DeviceLost`, in `device.cpp`'s own check-order priority) →
recreate-and-dependents (rebuilds `FrameSync`, per-swapchain-image
`VkImageView`s, and — iff constructed with a `RenderGraph`+`Executor`
pair — recompiles/re-realizes the graph) → frame-body callback → submit
→ present → advance. `CreateInfo::onRecreate`/`allocatorForBudgetRefresh`
cover samples 03/04's non-`RenderGraph` resources (hand-rolled depth
buffer, `FrameSync::advanceFrame(Allocator*)` budget refresh). Absorbs
`window_resize.h`'s three pure decision functions
(`pixelSizeRequiresRecreate`/`f11TogglesFullscreen`/
`shouldSkipTeardownAfterDeviceLoss`) verbatim. Every present-loop call
resolves to one shared four-way `Result` (`Ok`/`Skipped`/`Failed`/
`SurfaceLost`).

**`RenderGraph::compile()`** (`src/rx_graph/`) now returns `bool`
(deliberately not `[[nodiscard]]` — this project has no `-Werror` and
nine samples' existing silent-discard call sites stay valid without
forced changes) and caches the last-applied `CompileInfo` plus a
declaration-generation counter (bumped by `addPass()`/
`setBackbufferSource()`/`reset()`). A call whose generation and
`CompileInfo` are byte-identical to the last successful compile is a
no-op early return (`false`, proven by pointer identity, not content
equality — see Revert-discrimination); anything else recompiles and
returns `true`.

**`rx::rhi::PerFrameStorageBuffer`** (`src/rx_rhi_vk/`) — an
N-buffered, bindless-registered, host-visible storage buffer;
`write(frameSlot, ...)` targets only the caller's own current-frame
physical slot, `bindlessIndex(frameSlot)` hands back the push-
constant/descriptor read handle.

**`rx::material::createDemandSizedMaterialParamArena()`**
(`src/rx_material/`) — the promoted, byte-identical-logic
descriptor-set-LAYOUT + demand-sized `DescriptorArena` factory both
08/09 hand-rolled independently. Named to avoid colliding with the
existing, unrelated `rx::material::ParamArena` (matrix Open Questions
#1).

**`rx::platform::MouseCaptureToggle`** (`src/rx_platform/`, renamed
from `FlyThroughCaptureState`) + `mouseDeltaDrivesCamera()`/
`escTogglesCapture()`.

**`rx::scene::FlyCamera`** (`src/rx_scene/`) — wraps `rx::scene::Camera`
with yaw/pitch fly-rig state + `flyCameraLocalMoveDelta()`/
`keyboardDrivesCamera()`.

**`rx::scene::splitByBlockAndGroup()`/`resolveDrawGroups()`**
(`src/rx_scene/draw_list.{h,cpp}`) converted to zero-alloc out-param
APIs (capacity-snapshot + `.data()` pointer-identity tested, Task 23's
own methodology). New `RecordSpan`/`materialIndexForSpan()` promoted
from `samples/09_scene/draw_recording.{h,cpp}`.

**All nine samples** (`samples/0{1..9}_*/main.cpp`) migrated to
`rx::frame_loop::PresentLoop`. Samples 01-06's
`runPresent(bool,PresentMode,bool)` positional signature unified onto
the `Args`-struct shape 07-09 already used (matrix row 12).
`grid_layout.h` stays sample-local (matrix row 5 ruling); its own test
(`test_grid_layout.cpp`) is unchanged.

## Per-row disposition proof (all 12 audit-table rows)

| # | Item | Disposition (ruled/matrix) | Delivered | Evidence |
|---|---|---|---|---|
| 1 | `createMaterialParamArena` + material-param descriptor-set-LAYOUT block | promote → `rx_material` | Yes | `src/rx_material/include/rx_material/param_arena_factory.h`, `.cpp`; both 08/09 call it; `grep -rn "vkCreateDescriptorSetLayout" samples/08_gltf_viewer/main.cpp samples/09_scene/main.cpp` returns zero material-param-block hits (verified below) |
| 2 | Per-FIF draw-data buffer pattern (I1) | promote → `rx_rhi_vk` | Yes | `src/rx_rhi_vk/include/rx_rhi_vk/per_frame_storage_buffer.h`, `.cpp`; 08's `drawDataBuffer` and 09's `drawDataBuffer`+`shadowDrawDataBuffer` (the internal second duplicate) all converted; live revert-discrimination proof performed (see below) |
| 3 | `mouse_capture.h` | promote → `rx_platform` | Yes | `src/rx_platform/include/rx_platform/mouse_capture_toggle.h`; `samples/09_scene/mouse_capture.h` deleted (`git rm`); 13 device-free `TEST_CASE`s |
| 4 | `fly_camera.h` | promote → `rx_scene` | Yes | `src/rx_scene/include/rx_scene/fly_camera.h`; `samples/09_scene/fly_camera.h` deleted |
| 5 | `grid_layout.h` | rule-sample-local-stands | Yes (no code change, per ruling) | `samples/09_scene/grid_layout.h` untouched; `tests/test_grid_layout.cpp` still green (part of `sample_09_scene_tests` 5/5) |
| 6 | `splitByBlockAndGroup()`/`resolveDrawGroups()` zero-alloc + promote | promote → `rx_scene` | Yes | `src/rx_scene/include/rx_scene/draw_list.h` out-param APIs + `RecordSpan`/`materialIndexForSpan`; `samples/09_scene/draw_recording.{h,cpp}` deleted; capacity+pointer-identity tests + a concurrent per-worker-slot-safety test in `draw_list_test.cpp` |
| 7 | `window_resize.h` (ticket-enumeration gap) | absorbed into present-loop centralization (rows 8-10) | Yes | `samples/09_scene/window_resize.h` deleted; its 3 functions live in `rx_frame_loop/present_loop.{h,cpp}`, tested by `pure_decisions_test.cpp` |
| 8 | Present-loop shared helper — overall shape | needs-coordinator-decision → **ruled**: NEW module, `rx_frame_loop` shape | Yes | `src/rx_frame_loop/` (see "What shipped"); D5 main-thread-only statement in the header's own top comment |
| 9 | Present-loop — consistent `SurfaceLost` handling | promote (fold into helper) | Yes | `PresentLoop::runFrame()`/`recreateAndDependents()` both check `Device`'s surface-lost state as an explicit top-level branch (`Result::SurfaceLost`), not a nested check; every one of the 9 samples now has this (01-08 previously did not); 9-way WM-close trial below is the targeted regression proof, run for real rather than only asserted |
| 10 | Present-loop — extent-recompile-skip contract | **ruled**: inside `RenderGraph::compile()` itself | Yes | `src/rx_graph/render_graph.cpp:299-322`; counting-test proof: `test_compile.cpp`'s "returns false (skips)... unchanged topology" + "returns true again... genuine extent change" pair; live revert-discrimination proof performed (see below) |
| 11 | Present-loop — deletion of the nine duplicated hand-written loops | consume-now (once 8-10 land) | Yes | `grep -rln "SwapchainStatus::NeedsRecreate" samples/*/main.cpp` → **zero files** (run this round, output below) |
| 12 | Present-loop — CLI-signature unification | promote (fold into helper design) | Yes | Samples 01-06 converted from `runPresent(bool,PresentMode,bool)` to `runPresent(const Args&)`, matching 07-09's pre-existing shape |

Row 1 grep proof (material-param block; both sites now call the
promoted factory, no local descriptor-set-layout construction remains):
```
$ grep -n "vkCreateDescriptorSetLayout" samples/08_gltf_viewer/main.cpp samples/09_scene/main.cpp
(no output)
```

Row 11 grep proof (run this round, HEAD `4d5caeb`):
```
$ grep -rln "SwapchainStatus::NeedsRecreate" samples/*/main.cpp
(no output, exit code 1 — zero files)
```

## Migration table — all nine samples

| Sample | Signature unified | PresentLoop adopted | `onRecreate`/budget hook used | Headless gate (lavapipe) | WM-close trial (real NVIDIA) |
|---|---|---|---|---|---|
| 01_triangle | Yes (was 3 positional bools) | Yes | No (no RenderGraph, no extra resources) | readback PASSED | Issue #73 sequence, clean exit — confirmed |
| 02_hotreload | Yes | Yes | No | `hotreload headless gate PASSED` | confirmed |
| 03_bindless_mesh | Yes | Yes | Yes (hand-rolled depth buffer) | `bindless mesh headless gate PASSED` | confirmed |
| 04_streaming | Yes | Yes | Yes (hand-rolled depth buffer + budget refresh) | `streaming headless gate PASSED` | confirmed |
| 05_multipass | Yes | Yes | No | `multipass headless gate PASSED` | confirmed |
| 06_materials | Yes | Yes | No | `materials headless gate PASSED` | confirmed |
| 07_stress | Already `Args&` | Yes | No | `stress headless gate PASSED` | confirmed (Issue #74 fast-exit path, appropriate for its heavy GPU load) |
| 08_gltf_viewer | Already `Args&` | Yes | No | D17 `loading_state`/`loaded_scene`: 0/65536 both | confirmed |
| 09_scene | Already `Args&` | Yes (the proving ground — see below) | No (RenderGraph-based) | D17 `grid_scene`: 0/65536; stress-v2 gate PASSED; C1 shadow discrimination re-proof intact (240/65536 differing, expected non-zero) | confirmed |

09_scene's migration additionally consumed rows 1, 2, 4, 6 in the same
sample (`materialParamArena`, `drawDataBuffer`/`shadowDrawDataBuffer`,
`flyCamera`, `recordSpanScratch`) — the "proving ground" the ticket
names: the helper had to express its F11/live-resize/vsync-checkbox
proactive-recreation triggers (novel among the nine samples — 01-08 have
none of these) cleanly via `PresentLoop::recreateAndDependents()`/
`shouldRecreateForPixelSize()`, which it does without any sample-local
fallback logic.

## Command tails

Full suite, linux-native, lavapipe (post revert-discrimination re-fix,
HEAD `4d5caeb`):
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DISPLAY=:1 ctest -j4 --output-on-failure
...
100% tests passed, 0 tests failed out of 31
Total Test time (real) =  26.04 sec
```

windows-cross-zig, full build + Wine ctest (T2/T3/T4's own exclusion
pattern, extended this round with `rx_frame_loop_gpu`):
```
$ cmake --build --preset windows-cross-zig      # 73/73, zero errors
$ xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|rx_frame_loop_gpu|sample' --output-on-failure
...
100% tests passed, 0 tests failed out of 14
Total Test time (real) = 108.70 sec
```

09_scene headless gates, direct (lavapipe, isolated ICD):
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DISPLAY=:1 ./sample_09_scene --validate
  D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true
  C1 discrimination re-proof (shadows-on vs. forced-off): differingPixels=240/65536 (0.3662%)
  headless gate PASSED

$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DISPLAY=:1 ./sample_09_scene --validate --stress --stress-draws 64
  headless gate PASSED
```

08_gltf_viewer headless gate, direct:
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DISPLAY=:1 ./sample_08_gltf_viewer --validate
  D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true
  D17 loaded_scene gate:  failingPixels=0/65536 (0.0000%) pass=true
  headless gate PASSED
```

Interactive verification, 09_scene, real NVIDIA driver (DISPLAY=:1,
xdotool): mouse-drag orbit/fly-through (camera responded correctly to
injected relative-mouse deltas), F11 fullscreen toggle (1280x720 →
3840x1080, correct aspect ratio, HUD confirms "Window: FULLSCREEN"),
live drag-resize (`xdotool windowsize` 1280x720 → 900x600, swapchain
recreated at the new extent, no artifacts), HUD vsync checkbox click
(present mode switched off, frame time dropped from ~8ms to ~0.6ms) —
all screenshotted and confirmed correct this round.

Real-NVIDIA WM-close trial, all nine samples (this round, batch run —
launched together, closed via `xdotool windowclose`), each log showing
the identical sequence now emitted by `rx_frame_loop` itself (not
per-sample duplicated code):
```
Device::recreateSwapchain: surface-lost inferred from VkResult=-3 ...
Device::recreateSwapchain: surface capabilities query failed ... entering the surface-lost terminal state ...
rx_frame_loop: the present window's native handle is gone -- stopping without touching the surface further [Issue #73]
<sample>: window closed cleanly    (or, for 07_stress: the Issue #74 fast-exit std::_Exit() path — its heavy 30000-draw
                                     workload means VkDevice reports lost by the time vkDeviceWaitIdle() runs post-surface-loss,
                                     the exact compound condition shouldSkipTeardownAfterDeviceLoss() exists for)
```
Zero unfiltered validation errors across all nine logs (every "error"
substring found is either an already-classified `[vulkan validation]
(known false positive: ...)` line or the Issue #73 log message's own
literal mention of `VK_ERROR_SURFACE_LOST_KHR`/
`VK_ERROR_INITIALIZATION_FAILED` inside an informational/warning line,
not an actual validation-layer report). No process left running after
close (`pgrep -af "sample_0[1-9]_"` → empty).

## Revert-discrimination (load-bearing ordering/cache logic)

**`RenderGraph::compile()`'s recompile-skip cache** (row 10) — the
skip condition (`render_graph.cpp:316-322`) was temporarily disabled
(`false &&` prepended), rebuilt, and run against
`test_compile.cpp`'s "compile() returns false (skips) on a second call
with byte-identical CompileInfo and unchanged topology" case:
```
CHECK( graph.compile(kInfo) == false ) is NOT correct!  values: CHECK( true == false )
CHECK( findResource(graph.compiled(), "bb") == firstResourcePtr ) is NOT correct!
[doctest] test cases: 1 | 0 passed | 1 failed | 59 skipped
```
Both assertions failed exactly as predicted. Reverted
(`git diff src/rx_graph/render_graph.cpp` empty afterward), rebuilt,
`rx_graph_tests`: 60/60 cases, 379/379 assertions pass again. Full
project rebuild + full ctest re-run (31/31) confirms no collateral
damage from the probe/revert cycle.

**`PerFrameStorageBuffer::write()`** (row 2) — `write()`'s
`buffers_[frameSlot]` target was temporarily hardcoded to `buffers_[0]`,
rebuilt, run against the cross-slot-isolation test (which memcmps raw
slot bytes via a `detail::debugSlotBufferData()` test-only friend seam,
not merely "the call succeeded"): exactly the predicted 3 assertions
failed with the predicted values. Reverted, rebuilt, all 95 test cases
in `rx_rhi_vk_tests` pass again.

**`MouseCaptureToggle` composition** (row 3, pre-existing coverage
carried forward with the promotion) —
`test_mouse_capture_focus_composition.cpp`'s own top comment documents
an equivalent proof already performed for `applyCaptureTransition()`'s
`!=` vs. `==` condition (inverting it makes both composition test cases
fail at their first post-toggle assertion); not re-performed this round
since the logic and its test moved verbatim, unchanged by the
promotion.

## Self-review

- **TDD discipline**: `RenderGraph::compile()`'s cache and
  `PerFrameStorageBuffer`'s per-slot isolation were both test-designed
  before being trusted, then proven by live mutation rather than static
  reasoning alone (see Revert-discrimination).
- **No deferred fixes**: every hand-roll site the matrix identified
  (rows 1-12) is closed in this round; row 5 (`grid_layout.h`) closes
  via its ruled non-action, not a skip.
- **Zero unfiltered validation errors**: confirmed across the full
  ctest suite (both presets) and the nine-sample interactive/WM-close
  sweep — every `[vulkan validation]` line matches this codebase's
  pre-existing documented false-positive classifier set.
- **Both presets green**: linux-native 31/31 lavapipe; windows-cross-
  zig 73/73 build + 14/14 Wine ctest (device-free suites only, per the
  established GPU-test exclusion pattern, now including
  `rx_frame_loop_gpu`).
- **No AI attribution**: verified directly against all 9 commit
  messages this round (`git log` + grep for
  claude/anthropic/co-authored/generated-by/ai-assistant) — none found.
- **Commit scope**: 9 pathspec-scoped commits (module, graph change, rhi
  helper, 4 further promotions, samples 01-08, sample 09) — see SHAs
  below. `.superpowers/sdd/2026-08-20-phase5-techniques/progress.md` is
  concurrently maintained by the coordinator in this shared tree
  (confirmed via `git status` throughout — it already carries this
  task's own "T5 DISPATCHED" line, written before this round started)
  and is deliberately excluded from every commit, per the "no board/
  plan/spec/ledger edits" binding constraint. One self-correction this
  round: the first `rx_frame_loop` commit attempt accidentally swept in
  the (already index-staged, from earlier in this same session)
  09_scene file deletions; caught before it left the working tree via
  `git diff --cached --stat` review discipline, fixed with `git reset
  --soft HEAD~1` + `git restore --staged` on exactly the 9 deletion
  paths, and re-committed cleanly — no force-push, no lost work, still
  entirely local/unpushed.
- **Scope discipline**: no push performed; no board/plan/spec/ledger
  file edited; `grid_layout.h` left untouched per its explicit ruling
  rather than opportunistically promoted.
- **Concerns for the coordinator**: (1) matrix row 9's "targeted
  regression test constructs a Device already in the surface-lost state"
  acceptance criterion is satisfied functionally (every sample's WM-
  close trial exercises exactly this path against a REAL surface-lost
  Device, on real NVIDIA hardware, nine times this round) but there is
  no NEW device-free unit test asserting `PresentLoop::runFrame()`'s
  very first call against an already-surface-lost `Device` short-
  circuits without attempting a frame — `present_loop_gpu_test.cpp`'s
  existing GPU suite covers the Suspended-retry path with a real live
  window but does not construct a pre-lost `Device` synthetically; flag
  for a future hardening round if the coordinator wants a device-free
  (or GPU, pre-seeded) regression specifically for that entry
  condition, distinct from the live WM-close proof already performed.
  (2) `runPresent()`'s new `handleRecreateResult()` lambda in every
  sample is small, near-identical boilerplate repeated across all nine
  `main.cpp` files (mapping `rx::frame_loop::Result` onto each sample's
  own local `ok`/`running` pair) — deliberately NOT promoted into
  `PresentLoop` itself this round, since `ok`/`running` are each
  sample's own local state, not something the helper should own or
  name; noting it in case a future ticket wants a shared adapter
  utility. (3) 07_stress's WM-close trial this round exited via the
  Issue #74 fast-exit `std::_Exit()` path rather than the ordinary
  `shouldSkipTeardownAfterDeviceLoss()`-negative "window closed cleanly"
  path every other sample took — expected given its default 30000-draw
  workload (the GPU is still busy when the window disappears, so
  `vkDeviceWaitIdle()` observes `VK_ERROR_DEVICE_LOST` on top of the
  already-known surface loss), not a regression, but worth the
  coordinator's awareness since it is the one sample among nine whose
  trial this round exercised BOTH of `PresentLoop`'s
  `shouldSkipTeardownAfterDeviceLoss()`-consuming branches in practice
  rather than just the common one.

## Commit SHAs (base `e607074`, all local, none pushed)

1. `8bae5d3` — feat(rx_frame_loop): new present-loop engine module (#41)
2. `0a4bdd3` — feat(rx_graph): RenderGraph::compile() skips a redundant recompile (#41)
3. `97d8bbf` — feat(rx_rhi_vk): add PerFrameStorageBuffer, close row 2 (#41)
4. `088e071` — feat(rx_material): promote createDemandSizedMaterialParamArena (#41)
5. `42c25e3` — feat(rx_platform): promote MouseCaptureToggle, close row 3 (#41)
6. `e5770de` — feat(rx_scene): promote FlyCamera, close row 4 (#41)
7. `d9466fd` — perf(rx_scene): zero-alloc splitByBlockAndGroup/resolveDrawGroups (#41)
8. `2f84947` — refactor(samples): migrate 01-08 to rx::frame_loop::PresentLoop (#41)
9. `4d5caeb` — refactor(samples/09_scene): migrate to PresentLoop, consume all promoted primitives (#41)
10. `4283286` — chore: p5 task 5 SDD report
11. `d23a047` — build(cmake): configure-enforce rx_frame_loop absence from core targets (#41)
12. `e8e1875` — test(rx_frame_loop): regression-test the pre-lost-Device short-circuit (#41)
13. `47c9b16` — chore: p5 task 5 SDD report -- review round addendum
14. `aaa51ed` — fix(rx_frame_loop): sync2 vkQueueSubmit2 for PresentLoop's own submission (#41)

## Review round addendum (2 Medium findings, both closed in-round)

Verdict: spec PASS, Approved, 2 Mediums. Both closed this round; see
`task-05-review.md` for the full review.

**Medium #1 — the Task-21 CMake configure-time link-boundary check was
never extended to `rx_frame_loop`.** Link closure was already clean, but
nothing FAILED configure if a core target ever started linking it.
Fixed by mirroring the existing `imgui` boundary check exactly (same
core-target list, same `rx_assert_target_excludes_dependency()`
mechanism, `CMakeLists.txt`) and generalizing that function's own
FATAL_ERROR message, which had hardcoded "core-libraries-stay-ImGui-free"
regardless of which forbidden dependency actually triggered it
(`cmake/DependencyBoundaryCheck.cmake`).

Proven load-bearing, exactly like Task 21's own imgui check was proven:
```
$ # injected: target_link_libraries(rx_material PUBLIC rx_frame_loop), right after
$ # rx_material's add_subdirectory in CMakeLists.txt
$ cmake .
...
CMake Error at cmake/DependencyBoundaryCheck.cmake:138 (message):
  [dependency-boundary-check] 'rx_material' transitively depends on something
  matching 'rx_frame_loop' -- this violates a core-library dependency
  boundary (see the rx_assert_target_excludes_dependency(rx_material
  rx_frame_loop) call site in the root CMakeLists.txt for which boundary and
  its rationale).  Dependency chain: rx_material -> rx_frame_loop
```
Injection removed, reconfigured clean (`-- Configuring done` /
`-- Generating done`, no errors). One real finding surfaced while
picking the injection target: a first attempt via `rx_platform`
(`target_link_libraries(rx_platform PUBLIC rx_frame_loop)`) did NOT
produce the expected FATAL_ERROR — it hit the dependency-closure walk's
own recursion-depth guard instead (`_rx_dep_closure_contains`'s
documented non-visited-set-deduplicated design,
`DependencyBoundaryCheck.cmake:28-44`), because `rx_platform` is one of
`rx_frame_loop`'s OWN dependencies, so that injection created a genuine
`rx_platform -> rx_frame_loop -> rx_platform` cycle the walk is not
designed to short-circuit. Not a bug against this project's real
(acyclic) target graph — recorded as an in-code usability note
(`CMakeLists.txt`'s own comment on the new check) for picking a future
injection target: use a core target `rx_frame_loop` does NOT itself
depend on (`rx_material`/`rx_task`/`rx_shader`/`rx_asset`/`rx_scene`),
not one of `rx_platform`/`rx_rhi_vk`/`rx_graph`/`rx_core`.

**Medium #2 — matrix row 9's literal regression-test artifact was
missing.** Row 9's own acceptance criterion: "a targeted regression test
constructs a Device already in the surface-lost state and asserts the
helper's very first `acquireNextImage()` call is handled without falling
through to frame recording." Closed via a new
`rx::rhi::detail::forceSurfaceLostForTesting(Device&)` test-only friend
seam (`device.h`/`.cpp`, mirroring the `detail::debugSlotBufferData()`/
`debugFrameBufferData()` convention already used elsewhere this task) —
the real end-to-end path (a genuinely destroyed native window) is
MANUAL_VERIFICATION-only per `surface_loss_test.cpp`'s own header
comment (no CI driver/display backend this repo's fixtures use can
destroy a live window out from under a running process), so this is the
GPU-under-lavapipe fallback tier the review comment itself anticipated.

New `present_loop_gpu_test.cpp` TEST_CASE forces a real `Device`
surface-lost BEFORE `PresentLoop`'s own first `runFrame()` call (an
ordering `PresentLoop::surfaceLost_`'s own top-of-function guard does
NOT catch, since it has never itself observed a loss yet — the actual
catching branch is the acquire-status check inside `runFrame()`) and
asserts: `Result::SurfaceLost` returned, `frameBody` never invoked,
`Device::acquireCallCount()` unchanged (Device's own
`surfaceLost_`-short-circuit in `acquireNextImage()` deliberately never
increments it — the same call-count contract Task 17's Suspended-path
tests already rely on), and `loop->isSurfaceLost()` latches true
afterward. Result: `rx_frame_loop_gpu_tests` 8/8 cases (up from 7),
117/117 assertions (up from 106), zero unfiltered validation errors.

Revert-proved: temporarily disabled `runFrame()`'s
`acquire.status == SwapchainStatus::SurfaceLost` branch
(`present_loop.cpp`), rebuilt, ran the new test standalone. It did not
merely fail an assertion — it produced a genuine, UNFILTERED Vulkan
validation error and then hung (had to be `SIGKILL`ed):
```
[error] [vulkan validation] Validation Error: [ VUID-vkQueueSubmit-pWaitSemaphores-03238 ]
  ... Queue ... is waiting on semaphore (VkSemaphore ...) that has no way to be signaled.
```
— because with the short-circuit removed, `runFrame()` falls through to
`vkQueueSubmit` waiting on `frameSync_->currentImageAvailableSemaphore()`,
which `Device::acquireNextImage()`'s OWN (untouched) `surfaceLost_`
short-circuit never actually submitted for signaling (no real
`vkAcquireNextImageKHR` ran) — a genuine, reproduced GPU deadlock, the
exact hazard the disabled branch exists to prevent. Reverted
(`git diff` against the committed version was empty), rebuilt,
`rx_frame_loop_gpu_tests`: 8/8 cases, 117/117 assertions pass again.

**Verification after both fixes:** full project rebuild clean on both
presets; `rx_rhi_vk_tests`/`rx_frame_loop_tests`/`rx_frame_loop_gpu_tests`
(the touched suites) green under lavapipe; full serial lavapipe ctest:
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DISPLAY=:1 ctest --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 31
Total Test time (real) = 132.80 sec
```
windows-cross-zig: clean configure + full build (87/87 targets,
including the new `rx_frame_loop_gpu_tests.exe`), plus `rx_rhi_vk_tests`/
`rx_frame_loop_tests` (the touched device-free/GPU-boundary suites) run
green under Wine.

07_stress's WM-close-trial latency (17s from `xdotool windowclose` to
the Issue #73 log sequence, noted separately by the coordinator) is
ruled a pre-existing `device.cpp` characteristic outside this diff — a
watch item, not addressed in this round.

## CI-failure addendum (post-push, run 32463376885)

**Status: fix committed (`aaa51ed`), CI is the verification arbiter** —
local repro of the exact hazard was attempted with real effort and not
achieved (full account below); the push after this fix, and CI's own
conclusion on it, is what confirms or reopens this.

### Root cause

Fetched the real failure log directly (`gh run view 32463376885
--log-failed`), not paraphrased. Two related SyncVal hazard classes in
`rx_frame_loop_gpu_tests`, both genuine gaps `PresentLoop::runFrame()`'s
ONE submission call site had left unproven to the validator (present
since the very first sample migrated, never CI-present-tested before
this task's own new GPU test suite existed):

1. **SYNC-HAZARD-WRITE-AFTER-READ** — the acquired swapchain image's
   own first-touch layout-transition barrier (`seq_no: 1`,
   `SYNC_IMAGE_LAYOUT_TRANSITION`) hazards against the presentation
   engine's synthetic acquire-read
   (`SYNC_PRESENT_ENGINE_SYNCVAL_PRESENT_ACQUIRE_READ_SYNCVAL`, whose
   own `read_barriers` showed only `COLOR_ATTACHMENT_OUTPUT_BIT|
   BOTTOM_OF_PIPE_BIT` as proven-covered). Confirmed via CI's own exact
   file:line failures (`present_loop_gpu_test.cpp:83/131/336/372`,
   mapped against this session's own `grep -n "^TEST_CASE"`) that this
   hits BOTH the no-RenderGraph test fixture's own hand-rolled barrier
   AND every RenderGraph-based test. The RenderGraph path already
   carries a narrower, TARGETED fix for exactly this class
   (`src/rx_graph/executor.cpp:537-551`, "Backbuffer acquire chaining"
   comment, `srcStage` overridden to `COLOR_ATTACHMENT_OUTPUT_BIT` for
   the backbuffer's first UNDEFINED-oldLayout transition, explicitly
   dated in its own comment to an EARLIER "layers >= ~1.3.240"
   threshold) — CI's own log proves that fix is no longer sufficient by
   itself against 1.3.275.
2. **SYNC-HAZARD-PRESENT-AFTER-WRITE** (no-RenderGraph test case only) —
   `vkQueuePresentKHR` not provably ordered-after the submission's own
   final layout-transition write via the render-finished semaphore's
   coverage (`write_barriers: 0` on the prior write in CI's own log).

Both stem from the SAME root cause: `PresentLoop::runFrame()` used
legacy `vkQueueSubmit`/`VkSubmitInfo`, whose wait/signal semaphore
coverage is implicit/ambient ("waits/signals the whole batch") rather
than an explicit, per-stage-provable sync2 chain — and `frameBody` is
OPAQUE to `PresentLoop` (a RenderGraph's `Executor::execute()` or any
hand-rolled recording), so this class can never safely assume which
stage a caller's first/last touch of the acquired image happens at.

### Fix

`present_loop.cpp`'s one submission call site switches from
`vkQueueSubmit`/`VkSubmitInfo` to sync2 `vkQueueSubmit2`/`VkSubmitInfo2`/
`VkSemaphoreSubmitInfo` (`synchronization2` is already an unconditional
Vulkan 1.3 core feature this project enables,
`device.cpp:223 features13.synchronization2 = VK_TRUE`) — both the wait
and signal `VkSemaphoreSubmitInfo` now use the explicit, spec-documented,
maximally-conservative `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT` stage
mask, a strict superset of every narrower mask any caller's own barriers
already use (including the RenderGraph-side partial fix, which stays in
place, now redundant-but-harmless rather than the sole line of defense).
The sync-chain invariant this fix documents is written directly into
`present_loop.cpp`'s own comment block, per the module's explicit
ownership of this concern.

Verified no sample retains its own present-submission path with this
gap:
```
$ grep -rln "vkQueueSubmit\b\|VkSubmitInfo\b" samples/*/main.cpp
samples/04_streaming/main.cpp
samples/07_stress/main.cpp
```
Both are unrelated HEADLESS (no swapchain, no acquire) submissions —
`04_streaming/main.cpp:1496`'s own comment: "No wait/signal semaphores
-- there is no swapchain and no cross-queue dependency in this mode";
`07_stress/main.cpp:1400` is a prose comment, not code. Every present-
mode `runPresent()` across all nine samples goes exclusively through
`PresentLoop::runFrame()`'s one call site.

### Local repro attempt (full account, per the coordinator's explicit ask)

Local dev machine runs `vulkan-validationlayers 1.3.204.1-2` (Pop!_OS/
jammy apt) and `mesa-vulkan-drivers 25.1.5` — CI's `ubuntu-latest`
(noble/24.04) installs `vulkan-validationlayers 1.3.275.0-1` and
`mesa-vulkan-drivers 25.2.8` from Ubuntu's own archive. Both were
side-loaded locally, WITHOUT touching the system package manager:

```
$ curl -sL -o vvl275.deb http://mirrors.kernel.org/ubuntu/pool/universe/v/vulkan-validationlayers/vulkan-validationlayers_1.3.275.0-1_amd64.deb
$ dpkg-deb -x vvl275.deb extracted   # libVkLayer_khronos_validation.so + VkLayer_khronos_validation.json, colocated
$ curl -sL -o mesa.deb http://security.ubuntu.com/ubuntu/pool/main/m/mesa/mesa-vulkan-drivers_25.2.8-0ubuntu0.24.04.2_amd64.deb
$ dpkg-deb -x mesa.deb extracted     # libvulkan_lvp.so + lvp_icd.json, colocated
$ VK_LAYER_PATH=.../layer_dir vulkaninfo --summary | grep khronos_validation
  VK_LAYER_KHRONOS_validation ... 1.3.275  version 1     # confirmed loaded, exact CI version
```

`rx_frame_loop_gpu_tests` run against the CURRENT (pre-fix) committed
code under every combination tried:
- CI-matched layer (1.3.275) + local ICD, real `DISPLAY` — 8/8 pass.
- CI-matched layer + CI-matched ICD (25.2.8), real `DISPLAY` — 8/8 pass.
- CI-matched layer + CI-matched ICD, `xvfb-run -a` (matching CI's exact
  wrapper) — 8/8 pass.
- CI-matched layer + CI-matched ICD, `xvfb-run -a taskset -c 0,1`
  (CPU-constrained, 3 runs) — 8/8 pass, all 3.

**None reproduced the hazard.** SyncVal's cross-batch hazard tracking
depends on genuine GPU-completion timing (when a software rasterizer's
prior batch is actually observed complete relative to the next
submission), which a version-matched layer+driver combination does not,
by itself, guarantee reproduces — this is the same class of
non-forceable, timing-sensitive divergence the Phase 4 Stage 1
`7cc685f` round already hit and documented ("Revert-check could not
force the scheduling-sensitive hazard locally... CI green is the final
confirmation"). Per the coordinator's own explicit instruction, this is
reported honestly rather than claimed as a verified repro: **CI's own
next run on this push is the arbiter.**

### Verification after the fix (regression-only, since local repro was not achieved)

Full serial lavapipe ctest (local 1.3.204 layer):
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DISPLAY=:1 ctest --output-on-failure -j1
...
100% tests passed, 0 tests failed out of 31
Total Test time (real) = 146.87 sec
```
`rx_frame_loop_gpu_tests` under the side-loaded CI-matched layer+ICD:
8/8 cases, 117/117 assertions, clean (no regression from the fix
itself, though this combination never reproduced the original hazard
either). D17 gates re-confirmed byte-identical post-fix:
`sample_09_scene --validate`: `grid_scene` 0/65536; `sample_08_gltf_viewer
--validate`: `loading_state`/`loaded_scene` both 0/65536. Real-NVIDIA
present run (`sample_09_scene --present --validate`, DISPLAY=:1): zero
unfiltered validation errors, "window closed cleanly". windows-cross-zig:
clean configure + full build, all targets including
`rx_frame_loop_gpu_tests.exe`.

### Commit

`aaa51ed` — fix(rx_frame_loop): sync2 vkQueueSubmit2 for PresentLoop's own submission (#41)
