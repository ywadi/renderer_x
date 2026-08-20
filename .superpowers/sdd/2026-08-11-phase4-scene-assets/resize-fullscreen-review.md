# Independent review: Issue #36 (resize + fullscreen) / Issue #73 (window-close race)

Reviewer: independent pass, not the author of this code. Scope: 7 local
commits on `main`, not pushed (`370b7a6..952b868`):

```
370b7a6 feat(rx_platform): add resizable-window flag to Window::create
7f94dc4 test(rx_rhi_vk): prove a real SDL_SetWindowSize() resize drives Device::recreateSwapchain() cleanly
204873b feat(samples/09_scene): resizable present window + F11 fullscreen toggle
a739a40 fix(rx_rhi_vk): Device enters a surface-lost terminal state instead of hard-failing when the native window is gone
9aa578c fix(rx_platform): Window::abandonNativeHandle() skips SDL_DestroyWindow() on an already-gone handle
7feced3 fix(samples/09_scene): stop touching a dying surface the moment Device::isSurfaceLost() fires
952b868 fix(samples): sweep Device::isSurfaceLost()/Window::abandonNativeHandle() consumption to samples 01-08
```

Inputs read in full: `gh issue view 36`/`73`, `resize-fullscreen-report.md`
(including its two embedded addenda), the three NVIDIA screenshots, and
`review-4cddc26..952b868.diff` (all 2686 lines, all 20 changed files).

Hardware for this review's own empirical checks: real NVIDIA GeForce RTX
2080 (driver-resident, confirmed via `nvidia-smi --query-compute-apps`,
default ICD, `DISPLAY=:1`, gnome-shell) alongside Mesa llvmpipe (lavapipe,
`VK_ICD_FILENAMES` forced) for the headless/CI-representative gates. Every
result below is driver-labeled.

## Verdicts

### 1. Spec compliance vs #36 and #73 — PASS

**#36** ("sample 09 present mode must support live window resizing and
fullscreen... verified on real NVIDIA and lavapipe"): met. `Window::create()`
grew a `resizable` flag (defaulted false, every pre-existing call site
unaffected — confirmed by grep and by the full `rx_platform_tests` re-run
below). Sample 09's `--present` window is resizable; F11 toggles fullscreen
through the exact same recreation path the `--fullscreen` CLI flag and
driver-signaled `NeedsRecreate` already used. HUD/camera track the live
extent (aspect ratio recomputed from `swapchainExtent()` every frame,
confirmed correct in this review's own real-hardware screenshots).
Minimize/zero-extent handling is unchanged (routed through the pre-existing
`isSuspended()` guard, still gated correctly through the new shared
recreation lambda). Verified this round on both drivers (below), matching
the issue's own stated verification bar.

**#73** ("handle close-requested before any further surface queries...
orderly teardown... verified with an xdotool-driven WM close exiting 0 on
real NVIDIA"): met. The investigation correctly identified that the
issue's own initial framing (a late-handled close *event*) was wrong for
`xdotool windowclose` specifically (no SDL event fires at all for a
third-party `XDestroyWindow()`) and pivoted to a reactive fix at the
Vulkan-call layer — `Device::isSurfaceLost()` — plus a second, only-found-
by-testing layer (`Window::abandonNativeHandle()`, which prevents the
teardown-time `SDL_DestroyWindow()` crash on the same dead handle). Fixed
at the `rx_rhi_vk`/`rx_platform` engine seam (reusable), consumed at the
sample layer (sample-specific loop wiring), matching the "samples consume
engine" layering this codebase already uses elsewhere. Verified this round
with independent `xdotool windowclose` runs, below — all exit 0.

### 2. Code quality — Approved, 2 Minor findings, 0 Major/Critical

No correctness, safety, or architecture defects found. Two Minor,
non-blocking findings (detail in Findings below). The "one shared
`recreateSwapchainAndDependents()` call site" rule holds with no second
recreation site smuggled into the frame loop; the render-graph recompile
fix and the vsync mid-frame deferral are both correct; the surface-lost
classifier does not swallow device-lost or OOM; `abandonNativeHandle()`
cannot leak the SDL window on any normal path traced. The samples 01-08
sweep is a faithful, non-mechanical per-file adaptation — read all 8 in
full, no `||`-chain short-circuit bug found in any of them.

## Empirical verification (this review's own runs, all driver-labeled)

| Check | Driver | Result |
|---|---|---|
| Full serial ctest | lavapipe (`VK_ICD_FILENAMES` forced, `xvfb-run`) | **29/29 passed**, 77.9s |
| `surface_loss_test.cpp` (3 cases) | lavapipe | 3/3 pass |
| `window_state_test.cpp` fullscreen double-toggle + new live-resize case | lavapipe | 4/4 test cases, 31/31 assertions pass; live resize genuinely granted under this Xvfb (128x112 → matches queried extent, 0 validation errors) |
| `rx_platform_tests` `resizable`/`abandonNativeHandle` cases | lavapipe | 3/3 test cases, 18/18 assertions pass |
| `sample_09_scene_tests` `f11TogglesFullscreen`/`pixelSizeRequiresRecreate` | lavapipe | 8/8 test cases, 15/15 assertions pass |
| Total test-case counts | lavapipe | `rx_rhi_vk_tests` 79, `rx_platform_tests` 32, `sample_09_scene_tests` 43 — exactly matches the report's claimed deltas |
| Sample 09 combined interactive run: resize (1280x720→1600x900, granted) → F11 fullscreen (3840x1080) → F11 back (1600x900, exact restore) → `xdotool windowclose` | **real NVIDIA RTX 2080** (354 MiB resident, confirmed via `nvidia-smi`) | **exit 0**; 1402 validation-layer lines, **0 unfiltered** (all carry this codebase's own `(known false positive: ...)` tag); log ends with the exact documented sequence (`entering the surface-lost terminal state` → `the present window's native handle is gone -- stopping...` → `window closed cleanly`) |
| `xdotool windowclose` spot-check, `02_hotreload` (report's own "inspection-only" list) | real NVIDIA | exit 0, process reaped, 0 unfiltered validation errors, correct surface-lost→clean-close log sequence |
| `xdotool windowclose` spot-check, `06_materials` (report's own "inspection-only" list) | real NVIDIA | exit 0, process reaped, 0 unfiltered validation errors, correct surface-lost→clean-close log sequence |
| Revert-discrimination: commented out the `compileInfo.*`/`graph.compile(compileInfo)` lines inside `recreateSwapchainAndDependents()` (samples/09_scene/main.cpp), rebuilt, ran, resized 1280x720→1600x900, screenshotted before/after, then restored the file byte-identically (`git diff` empty) and rebuilt again | real NVIDIA | **Reverted build**: resize succeeds, exit 0, **0 unfiltered validation errors either way** (this bug is silent, not validation-detectable) — but the scene visibly renders at the stale 1280x720 internal resolution upscaled to fill the new 1600x900 window (softer/blurrier model detail, confirmed by side-by-side screenshot comparison). **Fixed build**: same resize produces visibly sharper, natively-resolved geometry at the new extent, exit 0, 0 unfiltered validation errors. This is a real, visually confirmable regression signature specific to the reverted line, and it is *not* caught by validation layers or ctest — screenshot/inspection is the only way to see it, which matches why the report needed real-hardware screenshots to prove the fix rather than relying on the automated gates alone. |

## Attention-lens walkthrough

**Resize / single call site.** Confirmed by direct grep of the current
`samples/09_scene/main.cpp`: the only `Device::recreateSwapchain()` calls
inside `runPresent()`'s frame loop are the one inside
`recreateSwapchainAndDependents()`. The two other call sites in that
function (`--vsync off` / `--fullscreen` CLI-flag application) run once,
before the loop even starts and before the graph's first `compile()`/
`realize()` — they are one-time startup configuration, not a second
in-loop recreation path, so they don't violate Task 17's rule. Every
in-loop trigger (acquire/present `NeedsRecreate`, the suspended retry, F11,
live drag-resize via `pixelSizeRequiresRecreate()`, the deferred vsync
toggle) funnels through the one lambda.

**RenderGraph::compile() re-run.** Correct against the graph's own
contract (`RenderGraph::compile()` is the only place a `SwapchainRelative`
`AttachmentDesc` resolves into real pixels; `Executor::realize()` alone
does not react to a new extent) and confirmed correct empirically via the
revert-discrimination test above. Not wasteful per frame: the lambda that
does the recompile is only invoked from event-driven triggers (an actual
`NeedsRecreate`, an actual F11 press, an actual `pixelSizeRequiresRecreate()
== true` observation, an actual vsync-checkbox change) — none of these
fire on every frame, so `compile()` does not run continuously during
steady-state rendering. One minor inefficiency noted below (Finding 2).

**Vsync-toggle deferral cannot lose a toggle.** The application is
level-triggered (`app->hud.vsyncOn != vsyncOnAppliedLastFrame`), not
edge/event-consumed, so if a frame's recreation attempt is skipped for any
reason the comparison simply re-fires next frame — there is no path that
clears `app->hud.vsyncOn` without also being caught by this check on a
later iteration.

**F11 routing / ImGui gating / no Esc conflict.** Confirmed in the current
file: F11 and Esc are two independent `else if` branches on distinct
`SDL_SCANCODE_*` values inside the same `pumpEvents()` callback, each
independently gated on its own `!imguiWantsKeyboard`-shaped predicate
(`f11TogglesFullscreen()` / `escTogglesCapture()`). Esc's mouse-capture
semantics are untouched by this round. F11 routes through
`recreateSwapchainAndDependents()`, the same shared path as every other
trigger.

**Surface-lost classification.** `isSurfaceLossResult()` returns true only
for `VK_ERROR_SURFACE_LOST_KHR` and `VK_ERROR_INITIALIZATION_FAILED`, and
explicitly false for `VK_ERROR_DEVICE_LOST`, `VK_ERROR_OUT_OF_HOST_MEMORY`,
`VK_ERROR_OUT_OF_DEVICE_MEMORY`, `VK_ERROR_OUT_OF_DATE_KHR` — confirmed by
direct test run (`surface_loss_test.cpp`, 3/3 on lavapipe) and by
inspection: a real device-lost condition on `vkGetPhysicalDeviceSurface
CapabilitiesKHR` is a different, unrelated call path from where this
classifier is even consulted, so `DeviceLost` can't be misrouted into a
graceful "surface lost" exit anywhere in this diff. One residual risk
flagged as Finding 1 below (the `VK_ERROR_INITIALIZATION_FAILED` half of
the classifier is empirically, not spec-, justified).

**`abandonNativeHandle()` leak safety.** Traced every call site in all 9
samples (`grep` + direct read of `01_triangle`, `08_gltf_viewer`,
`09_scene`): in every case, the call is immediately followed by setting
the loop's exit flag (`quit`/`running = false`) and either an immediate
`continue`/fallthrough-to-loop-bottom with no further `window->` calls in
that same iteration, or (09_scene) an explicit `if (!running) break;` gate
placed before the next `window->` access (`lastPixelSizeEvent()`,
harmless anyway — a pure accessor into cached state, not a native-handle
call). No path was found where the object is used again in a way that
would touch the dead native handle, and no path skips `SDL_DestroyWindow()`
for a window that is NOT actually abandoned (the flag defaults false and
is only ever set from the one call site per file, itself only reached
after a real `isSurfaceLost() == true` observation).

**Terminal-state semantics.** `surfaceLost_` never clears itself
(unlike `suspended_`); `acquireNextImage()`/`present()` short-circuit on it
before even checking `suspended_`; `recreateSwapchain()` checks it before
the zero-extent guard. Consistent, and matches the doc comments exactly.

**The 01-08 sweep.** Read all 8 files' full diffs. All three loop shapes
the report describes are real and distinct (01/02/04 vs. 05/06's added
`compileForExtent()` gating vs. 03's `rebuildDepthAndViews()` vs. 07/08's
`running`/`executor->realize()` shape), and every `||`-chain the report
says it split was in fact split correctly: `recreateSwapchain()` is always
checked alone first, `isSurfaceLost()` second, and the original chain only
evaluated in the surviving `else`/`else if` branch — no file lets a
surface-lost `recreateSwapchain()` (which returns `true`) fall through into
`onSwapchainRecreated(0)`/`createSwapchainViews()` against an empty image
list. `DeviceLost` branches are untouched in all 8. Independently
re-verified 2 of the 5 "inspection-only" files (02, 06) end-to-end on real
NVIDIA hardware (table above) — both correct. 03, 04, 07 remain
inspection-only from this review too (see below).

## Findings

1. **Minor — driver-labeled (NVIDIA/Xcb empirical, narrow scope).**
   `isSurfaceLossResult()` (`src/rx_rhi_vk/src/device.cpp`) treats
   `VK_ERROR_INITIALIZATION_FAILED` as surface-loss for
   `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`, but that code is not a
   spec-documented return value for this specific function (the documented
   set is `VK_SUCCESS`/OOM-host/OOM-device/`VK_ERROR_SURFACE_LOST_KHR`
   only). The choice is honestly labeled as empirical/non-spec in both the
   code comment and the report, and the blast radius is narrow (this one
   call site only, tested, and the two OOM codes plus `DEVICE_LOST` are
   correctly excluded). Residual risk: a different driver/vendor/version
   that returns this same out-of-spec code from this call for an unrelated
   reason would be silently treated as a graceful shutdown instead of a
   hard failure. Not a blocker given the current verified hardware matrix;
   worth re-checking if/when AMD or Intel present-mode samples are added
   to the verification matrix.
2. **Minor — no driver dependency (design nit, not a defect).** The vsync
   checkbox's deferred-application path in `recreateSwapchainAndDependents()`
   always re-runs `RenderGraph::compile()` + `Executor::realize()` even
   when only the present mode changed and the extent/format are identical
   — technically unnecessary work for that specific trigger. Not
   per-frame (fires once per checkbox click), so it doesn't violate the
   "no wasteful per-frame recompiles" concern from the attention lens;
   flagged purely as a missed micro-optimization, not worth blocking on.

No Major or Critical findings.

## Not independently verifiable this round

- Live human drag-resize and minimize/restore via the WM taskbar (the
  report's own MANUAL_VERIFICATION checklist rows) — not re-attempted;
  `xdotool windowsize`/`windowclose` were used instead, which is the same
  substitution the report itself used and documented as equivalent.
- Samples `03_bindless_mesh`, `04_streaming`, `07_stress` real-hardware
  `--present` close behavior — read and inspected in full (diff-level),
  structurally consistent with the spot-checked files, but not run
  end-to-end on hardware by either the implementer or this review. Same
  weaker-tier caveat the report itself states honestly.
- Windows-cross/Wine build and test results — relied on the report's own
  numbers (`rx_platform_tests.exe` 32/32, `sample_09_scene_tests.exe`
  43/43, `isSurfaceLossResult()` 3/3 standalone); not independently
  re-run this round.
- Wayland-specific behavior (the codebase's own documented gap, unrelated
  to this diff) — out of scope, unchanged by this round.

## Commit hygiene

7 commits, each pathspec-scoped to exactly the files its own message
claims (verified via `git show --stat` per commit). Author/committer on
every commit is the local git config identity (`Yousef Wadi
<ywadi85@gmail.com>`) — no override. No AI attribution anywhere in any
commit message or in the reviewed diff content (`git diff 4cddc26..952b868`
grepped clean for Claude/Anthropic/Copilot/ChatGPT/co-authored/"generated
by"). `git status` confirms `main` is 7 commits ahead of `origin/main`,
nothing pushed. Working tree at the end of this review is identical to the
start: the temporary revert-discrimination edit to
`samples/09_scene/main.cpp` was restored byte-identically (`git diff`
empty) and rebuilt; the only outstanding working-tree change is the
pre-existing, out-of-scope `progress.md` modification, left untouched as
instructed.
