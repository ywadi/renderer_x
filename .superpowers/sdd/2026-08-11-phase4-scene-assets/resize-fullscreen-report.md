# Issue #36: Sample 09 resizable window + runtime fullscreen — report

Base: `main` (started at `0549954`; `main` advanced to `4cddc26` mid-task via
unrelated Phase 5 planning docs/ledger commits — no conflict with the files
touched here). Real-NVIDIA verification: RTX 2080, driver 580.82.07, default
ICD (confirmed via `nvidia-smi --query-compute-apps` while the sample ran).
Lavapipe verification: system Mesa llvmpipe ICD
(`/usr/share/vulkan/icd.d/lvp_icd.json`).

## Status: complete

## What was there already (per the coordinator's pre-scout)

- `--fullscreen` CLI flag applied once at startup (main.cpp, then
  ~3048-3056) via `Window::setFullscreen()` + `Device::recreateSwapchain()`.
- No resizable-window flag anywhere in `rx_platform::Window`.
- No runtime fullscreen toggle.
- The present loop already handled driver-signaled `SwapchainStatus::
  NeedsRecreate` from both `acquireNextImage()` and `present()`, and the
  `Suspended` (zero-extent) retry loop — but every one of those call sites
  had never been exercised with a genuinely different extent, because the
  window was never resizable.

## What this round built

**`rx_platform::Window::create()`** gained a defaulted `resizable` parameter
that adds `SDL_WINDOW_RESIZABLE` at creation. Every existing caller (every
sample, every test in the repo) keeps its prior non-resizable behavior
unchanged — verified by grepping every call site before editing.

**Sample 09's `--present` window** now passes `resizable=true`. Two new
pure decision functions live in the new `samples/09_scene/window_resize.h`
(device-free, mirroring `fly_camera.h`/`mouse_capture.h`'s established
"pull pure logic out of main.cpp" precedent):

- `f11TogglesFullscreen(imguiWantsKeyboard)` — the same
  `!imguiWantsKeyboard` gate shape `escTogglesCapture()` already
  establishes for Esc, kept as its own named function (not reused) for the
  same "these gate different call sites, free to diverge" reason
  `mouse_capture.h`'s own comment gives.
- `pixelSizeRequiresRecreate(lastHandled, observed)` — decides whether a
  live `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` observation
  (`Window::lastPixelSizeEvent()`) should proactively trigger a swapchain
  recreation this frame. Requires both dimensions nonzero, so it never
  fires on the `{0,0}` pre-first-event sentinel and never itself decides
  suspended/resume — that stays exclusively `Device::recreateSwapchain()`'s
  own live-queried guard, matching `window.h`'s explicit "optimization/
  logging signal only" invariant for `lastPixelSizeEvent()`.

**One shared `recreateSwapchainAndDependents()` lambda** in `runPresent()`
is now the sole place that calls `Device::recreateSwapchain()` from inside
the frame loop. Every trigger — acquire/present `NeedsRecreate`, the
suspended-present retry, the new F11 toggle, the new live-resize
detection, and the HUD vsync toggle — funnels through it. This satisfies
Task 17's own explicit design rule (cited verbatim in
`window_state_test.cpp`'s fullscreen double-toggle test: "no bespoke
recreation function exists to call instead").

## Two real bugs found and fixed in this round (not deferred)

Both were latent, pre-existing gaps that had simply never been exercised
before this task made the window resizable and forced every recreation
path to actually run against a changing extent.

**1. Render-graph transients were never resized.** `RenderGraph::compile()`
is the only place a `SwapchainRelative` `AttachmentDesc` (used by this
sample's `"hdr"` and `"depth"` passes) resolves into real pixels from
`CompileInfo::swapchainWidth/Height`. `Executor::realize()` only rebinds
pooled resources to whatever shape the *last* `compile()` produced — it
does not itself react to a new swapchain extent. The pre-existing
`NeedsRecreate` handling called `recreateSwapchain()` +
`rebuildSwapchainViews()` + `realize()` but never re-ran `compile()`, so a
real resize would have left the HDR/depth targets pinned at their startup
size while the backbuffer resized underneath them. `rx_graph`'s own
`src/rx_graph/tests/test_execute_gpu.cpp` already had a
"resize-rerealize" GPU test proving `realize()` is safe to call again
after a *recompiled* graph — confirming the fix is exactly "recompile before
realize", not a novel mechanism. `recreateSwapchainAndDependents()` now
re-derives `compileInfo.swapchainWidth/Height/Format` from the freshly
queried `Device::swapchainExtent()`/`swapchainFormat()` and calls
`graph.compile(compileInfo)` before `executor->realize(graph)`. Confirmed
fixed on real hardware: see the before/after resize screenshots below —
the scene scales correctly to the new window size instead of staying
cropped to the old one.

**2. The HUD's vsync checkbox recreated the swapchain mid-frame.** It used
to call `Device::recreateSwapchain()` directly from inside `drawHud()`,
which runs *after* that frame's `acquireNextImage()` had already fixed
`acquire.imageIndex` and after `vkBeginCommandBuffer()` had already begun
recording against the swapchain images valid at acquire time.
`recreateSwapchain()` destroys and rebuilds the whole `VkSwapchainKHR` for
a present-mode change, which would leave that frame's already-acquired
image index, swapchain views, and `FrameSync`'s per-image sync objects
referencing since-destroyed state for the rest of the frame. `drawHud()`
now only mutates `app.hud.vsyncOn`; the present loop detects the change
and applies it (via the shared helper) at the top of the *next* frame,
before that frame's own `acquireNextImage()` — a safe point, exactly like
every other trigger.

## Tests added

- `src/rx_platform/tests/window_test.cpp`: device-free — proves
  `resizable` threads through to `SDL_WINDOW_RESIZABLE` (default, explicit
  false, explicit true — three cases, revert-discrimination on all three).
- `samples/09_scene/tests/test_window_resize.cpp` (new file): device-free —
  full truth-table + revert-discrimination coverage for
  `f11TogglesFullscreen()` and `pixelSizeRequiresRecreate()` (the `{0,0}`
  sentinel guard, single-axis-only changes, idempotent no-op on an
  unchanged extent).
- `src/rx_rhi_vk/tests/window_state_test.cpp`: new GPU-path TEST_CASE — a
  real `SDL_SetWindowSize()` resize against a `resizable=true` fixture,
  followed by the exact production `Device::recreateSwapchain(surface)`
  call (no `extentOverride` seam), asserting the WM-grant-adaptive
  unconditional floor (recreation succeeds, not suspended, zero
  validation errors), matching the existing fullscreen double-toggle
  test's own fallback shape for environments that don't actually grant
  the resize. `makeFixture()` gained a `resizable` parameter (defaulted
  false).
- Live human drag-resize itself stays MANUAL_VERIFICATION (see checklist
  below) — not automatable headlessly.

## Verification

### Full serial lavapipe ctest — GREEN

```
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a ctest --output-on-failure
```
`100% tests passed, 0 tests failed out of 29` (linux-native preset, serial,
run twice — once before and once after removing temporary diagnostic
logging added during the shutdown-race investigation below; both green).
Confirmed the real NVIDIA GPU was NOT used for this run (forced
`VK_ICD_FILENAMES` to lavapipe only).

### Real-NVIDIA `--present --validate` run — GREEN

Ran on the real X11 desktop session (`DISPLAY=:1`, gnome-shell), default
Vulkan ICD (no `VK_ICD_FILENAMES` override). `nvidia-smi
--query-compute-apps` confirmed the sample process was resident on the
RTX 2080 (354 MiB) while running, not lavapipe.

In one session:
1. Started at 1280x720 windowed.
2. `xdotool windowsize <id> 1500 850` — a real, external, programmatic
   resize (the practical equivalent of `SDL_SetWindowSize`, which is what
   `xdotool windowsize` ultimately drives via the same WM resize path).
   Window genuinely resized to 1500x850; scene and HUD rendered correctly
   at the new size with zero validation errors.
2. `xdotool key --window <id> F11` — real fullscreen toggle. Window went
   fullscreen (3840x1080 in this dual-monitor-as-one-X-screen
   environment); scene rendered correctly, HUD read "Window: FULLSCREEN".
3. `xdotool key --window <id> F11` again — back to windowed, restored to
   1500x850 exactly.
4. Clean exit via `SIGINT` (see the shutdown-race note below for why not
   via the window's close button in this run) — exit code 0, log line
   `sample_09_scene: window closed cleanly`.

Full-run validation-error count: 0 unfiltered (every `Validation Error`/
`Validation Warning` line in the log carries this codebase's own
pre-existing `(known false positive: ...)` tag from `context.cpp`'s
established guards — none untagged).

Screenshots (saved alongside this report):
- `resize-fullscreen-nvidia-before-resize.png` — 1280x720 windowed.
- `resize-fullscreen-nvidia-after-resize.png` — 1600x900 after a live
  resize (separate capture from the exact sequence above, same behavior);
  scene fills the new extent correctly, proving the render-graph-transient
  fix.
- `resize-fullscreen-nvidia-fullscreen.png` — fullscreen via F11, HUD
  showing "Window: FULLSCREEN".

### Windows-cross build + Wine — GREEN (device-free tests)

`cmake --build --preset windows-cross-zig` — full project build, zero
warnings. Under `xvfb-run -a wine`:
- `rx_platform_tests.exe`: 30/30 passed (includes the new resizable-flag
  test).
- `samples/09_scene/tests/sample_09_scene_tests.exe`: 43/43 passed
  (includes the new `test_window_resize.cpp`).
- `ctest -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample'`
  (CI's own windows-cross exclusion pattern — no Vulkan under Wine):
  12/13 passed. The one failure, `rx_asset_gltf_gpu_tests`, is a
  wall-clock stall-detector calibration in `async_import_test.cpp` (a file
  this task never touched, in a module — `rx_asset` — this task never
  touched). Reproduced twice under Wine on this loaded machine; this is a
  known-shape flaky timing gate (the codebase's own G15 convention already
  treats wall-clock thresholds as non-gating elsewhere), unrelated to
  resize/fullscreen. Not something this round introduced or can
  meaningfully fix within scope.

## Concern: pre-existing window-close race (found, initially left out of scope)

**Superseded — see the "Issue #73" addendum at the end of this report.** The
coordinator escalated this exact finding to issue #73 and directed a
same-round fix; this section is kept verbatim as the original investigation
record (the addendum also corrects one part of this section's own premise —
this is NOT a "close event handled too late" ordering bug, as first assumed
below; there is no close event at all for this specific action).

While chasing "clean exit" for the real-NVIDIA verification run, closing
the `--present` window via the window manager's own close mechanism
(`xdotool windowclose`, i.e. the standard `_NET_CLOSE_WINDOW`/
`WM_DELETE_WINDOW` path every WM titlebar "X" button and Alt+F4 also use)
reliably exits with code 1, logging:

```
Device::recreateSwapchain: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: VkResult=-3
X Error of failed request:  BadWindow (invalid Window parameter)
```

Root-caused with temporary diagnostic logging (added, used, then fully
reverted — not in the shipped diff): the failure comes from the
**pre-existing** `presentStatus == NeedsRecreate` branch (i.e.
`vkQueuePresentKHR` itself returned `VK_ERROR_OUT_OF_DATE_KHR`/
`VK_SUBOPTIMAL_KHR`), and it fires *before* the app ever observes
`SDL_EVENT_WINDOW_CLOSE_REQUESTED` at all. Confirmed present on the
**unmodified pre-existing baseline** (`git stash`'d this task's own diff
and reproduced the identical failure with zero resize/fullscreen
interaction — just start, wait, close) — this is not caused or worsened by
this round's changes; it is a general "gnome-shell tears down the X11
window before/while the app is still mid-loop against a now-invalid
surface" race in the present loop's existing `NeedsRecreate` handling, and
would affect *any* interactive `--present` session on any of this
project's samples that shares this same acquire/present/NeedsRecreate
shape, whether or not it ever resizes. It plausibly went unnoticed until
now because CI's Xvfb runs with no window manager at all (no WM ever sends
a close request there), and prior interactive verification apparently
never hit this exact timing window.

This round's own real-NVIDIA "clean exit" verification instead used
`SIGINT` (`kill -INT <pid>`) — SDL3's default signal handler posts a
genuine `SDL_EVENT_QUIT`, which is processed through the normal, safe
`pumpEvents()` → `running = false` → graceful-shutdown path, sidestepping
the WM-specific race entirely and exercising the same clean-exit code path
a `SDL_EVENT_QUIT` from any other source would.

Recommend this be filed as its own follow-up issue (fixing it properly
means deciding how `Device::recreateSwapchain()`/the present loop should
distinguish "surface capabilities query failed because the window is
gone" from "surface capabilities query returned a transient zero extent"
— a real design question, not a one-line fix, and out of #36's own scope).

## Commits (pathspec-scoped, no AI attribution, author = local git config)

- `370b7a6` — `feat(rx_platform): add resizable-window flag to Window::create`
- `7f94dc4` — `test(rx_rhi_vk): prove a real SDL_SetWindowSize() resize drives Device::recreateSwapchain() cleanly`
- `204873b` — `feat(samples/09_scene): resizable present window + F11 fullscreen toggle`

No push. No board/issue/plan/spec/ledger edits (the one other modified
file in the tree at task start/end,
`.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md`, was left
untouched — not this task's).

## MANUAL_VERIFICATION checklist (live human drag-resize)

- [ ] Drag any window edge/corner of `sample_09_scene --present`: scene
  and HUD track the live size with no stretching/cropping, no crash, no
  validation errors (`--validate`).
- [ ] Drag-resize down to a sliver and back up: suspended-present engages
  (no crash, no spin) and resumes cleanly (covers the genuine OS-level
  minimize path `window_state_test.cpp`'s own matrix row 6 already notes
  as MANUAL_VERIFICATION-only, not headlessly reproducible).
- [ ] Minimize via the taskbar/WM and restore: same as above.
- [ ] F11 while a HUD text field would have keyboard focus (none exists
  today, but re-check after any future HUD text input is added) does NOT
  fire if `WantCaptureKeyboard` is true.
- [x] ~~Confirm the pre-existing window-close race above~~ -- fixed this
  same round as issue #73; see the addendum below. Still worth a real
  human clicking the window's own [X] button (not just `xdotool
  windowclose`) on the target release environment/WM combination this
  ships to, as a final sanity check, but the underlying mechanism is now
  handled regardless of exactly how the native window disappears.

---

# Addendum: Issue #73 — window-close race, root-caused and fixed

The coordinator escalated the "Concern" section above to issue #73 and
directed a same-round fix, with explicit guidance: fix the ordering so
`SDL_EVENT_WINDOW_CLOSE_REQUESTED` is handled before any further surface
query/acquire, route through an orderly teardown (stop rendering, drain
in-flight work, then destroy), assess whether the fix belongs in the
samples' shared loop pattern or lower (rx_platform/rx_rhi_vk), and verify
with an `xdotool`-driven WM close exiting 0 on real NVIDIA hardware.

## The premise needed correcting first

The directive's framing assumed a close EVENT arrives but is processed too
late. Investigated directly with temporary instrumentation (logging every
`SDL_EVENT_WINDOW_*` type observed, then fully reverted before committing):
for `xdotool windowclose` specifically, **zero** SDL window events of any
kind fire before the crash — not `SDL_EVENT_WINDOW_CLOSE_REQUESTED`, not
even `SDL_EVENT_WINDOW_DESTROYED`. `xdotool`'s own man page explains why:
`windowclose` "will destroy the window" directly (a raw third-party
`XDestroyWindow()`, not a `WM_DELETE_WINDOW` ClientMessage a client is
expected to cooperate with) — this is a genuinely different action from a
real WM's titlebar close button forwarding `WM_DELETE_WINDOW`, which this
sample already handled correctly (that path was never broken: `if
(!running) break;` already ran before any acquire/present, immediately
after `pumpEvents()`, before this round's work even started). There is no
advance-warning event for a third-party destroy to gate a check on — the
fix has to be REACTIVE, at the point a Vulkan call against the now-invalid
surface actually fails.

## Two-layer fix

**Layer 1 — the frame loop crash** (`Device::recreateSwapchain()`'s own
`vkGetPhysicalDeviceSurfaceCapabilitiesKHR` query hard-failing,
`VkResult=-3`/`VK_ERROR_INITIALIZATION_FAILED` on this project's verified
NVIDIA/Xcb stack): `rx_rhi_vk::Device` gained a new terminal state,
`isSurfaceLost()`/`SwapchainStatus::SurfaceLost`, a sibling of the existing
`isSuspended()`/`Suspended` — entered when `recreateSwapchain()`'s
capabilities query fails with a VkResult the new pure, device-free-tested
`isSurfaceLossResult()` classifies as "the native window is gone"
(`VK_ERROR_SURFACE_LOST_KHR`, the spec-correct code, plus the
empirically-observed `VK_ERROR_INITIALIZATION_FAILED`) rather than a
genuine, unrelated failure (OOM codes stay hard failures).
`acquireNextImage()`/`present()` short-circuit on it exactly like the
existing suspended-present guard. Unlike `Suspended`, this state is never
expected to clear on retry (no window is ever coming back), so the present
loop treats entering it as a GRACEFUL stop (`running = false`, not
`ok = false`) rather than a retry-forever loop.

**Layer 2 — the teardown crash** (found only after fixing layer 1: the
frame loop then exited its own logic successfully — "window closed
cleanly" logged — but the PROCESS still exited 1). Root-caused to
`~Window()`'s own `SDL_DestroyWindow()` call, made during normal teardown
(the local `App` unique_ptr's destructor, running as part of
`runPresent()`'s `return 0;` statement) against a native handle already
known gone — issuing further X11 protocol traffic against an invalid ID,
which Xlib's own DEFAULT error handler treats as FATAL (calls `exit()`),
overriding the process's own already-successful logical completion.
`rx_platform::Window` gained `abandonNativeHandle()`: once called, the
destructor/move-assignment skip `SDL_DestroyWindow()` entirely. Safe only
because the expected caller (present-loop teardown, immediately before the
whole process exits regardless) never uses that `Window` again — SDL's own
internal bookkeeping for the abandoned handle is reclaimed by the OS at
process exit either way, exactly like every other resource never
explicitly freed before exit.

Both layers were required — layer 1 alone still crashed (via layer 2)
during teardown; verified this directly (an intermediate build with only
layer 1 still exited 1, log showing the frame loop's own successful
"window closed cleanly" immediately followed by the same `X Error ...
BadWindow` and a nonzero exit).

## Architecture decision: where the fix lives

Confirmed by inspection that every `--present` sample in this codebase
(`01_triangle` through `08_gltf_viewer`) shares the IDENTICAL
`acquireNextImage()`/`present()` → `NeedsRecreate` → recreate-swapchain
loop shape sample 09 has — every one of them is equally exposed to this
same race today.

Given that, the REUSABLE half of the fix — `Device::isSurfaceLost()` and
`Window::abandonNativeHandle()` — was pushed down to the `rx_rhi_vk`/
`rx_platform` engine seam, per the samples-consume-engine principle: any
other sample can adopt the fix with a small, localized change (call
`isSurfaceLost()` after its own recreate call, call `abandonNativeHandle()`
on it, check `running` before touching the surface again), with zero need
to invent or retrofit a shared present-loop abstraction across 9
independently-maintained samples.

The LOOP-level consumption of that machinery (deciding exactly where to
check `running`, wiring `abandonNativeHandle()` into the teardown path) was
kept scoped to sample 09 only — the sample this issue was found in, traced
in, and verified in. Retrofitting the identical few lines into the other 7
samples is a real, valuable follow-up, but it is a separate, independently
reviewable change (each of those 8 files is its own maintained artifact,
and a silent drive-by edit across all of them inside a ticket scoped to
sample 09 risks an unreviewed behavior change landing somewhere nobody was
looking). Recommend a dedicated small follow-up sweep applying this same
pattern to samples 01–08.

## Tests added

- `src/rx_rhi_vk/tests/surface_loss_test.cpp` (new, device-free): full
  coverage of `isSurfaceLossResult()` — true for
  `VK_ERROR_SURFACE_LOST_KHR` and `VK_ERROR_INITIALIZATION_FAILED`, false
  for `VK_ERROR_OUT_OF_HOST_MEMORY`/`VK_ERROR_OUT_OF_DEVICE_MEMORY`/
  `VK_ERROR_DEVICE_LOST`/`VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUCCESS`
  (revert-discrimination against "classify any non-success as surface
  loss").
- `src/rx_platform/tests/window_test.cpp`: device-free — `Window::
  nativeHandleAbandoned()` defaults false, becomes true (idempotently)
  after `abandonNativeHandle()`, and the flag's own move-construction/
  move-assignment transfer (with the moved-from object resetting to
  false), mirroring this file's established per-member move-semantics
  test shape.
- The deep, end-to-end consequence (does skipping `SDL_DestroyWindow()`
  actually prevent the crash; does `isSurfaceLost()` actually engage
  against a real destroyed window) is MANUAL_VERIFICATION-only, exactly
  like the pre-existing zero-extent guard's own DI-seam split
  (`window_state_test.cpp`'s own "matrix row 6" precedent) — no CI
  driver/display backend this repo's fixtures use can genuinely destroy a
  live window out from under the process. Proven instead directly against
  real hardware, below.

## Verification (real NVIDIA hardware, driver-labeled)

Single `xdotool windowclose` close, repeated across independent runs (same
binary, same machine, real desktop WM present):

```
run 1: EXIT_CODE=0 unfiltered_val_errors=0
run 2: EXIT_CODE=0 unfiltered_val_errors=0
run 3: EXIT_CODE=0 unfiltered_val_errors=0
```

Combined session (matches this round's full feature surface in one run):
programmatic resize (`xdotool windowsize`, 1280x720 → 1500x850) → F11
fullscreen → F11 back to windowed (geometry confirmed restored to
1500x850) → `xdotool windowclose` → **exit code 0**, 0 unfiltered
validation errors, 0 X protocol errors, log ends with `Device::
recreateSwapchain: surface capabilities query failed ... entering the
surface-lost terminal state` → `the present window's native handle is
gone -- stopping without touching the surface further` → `window closed
cleanly`.

Full serial lavapipe ctest (linux-native preset, after this round's
changes): **29/29 passed**, including `rx_rhi_vk_tests` now at 79/79 test
cases (was 76 before this addendum's 3 new `isSurfaceLossResult()` cases),
`rx_platform_tests` at 32/32 (was 30, +2 new `abandonNativeHandle()`
cases).

Windows-cross build (`cmake --build --preset windows-cross-zig`): clean,
zero warnings, full project. Under `xvfb-run -a wine`: `rx_platform_tests.
exe` 32/32, `sample_09_scene_tests.exe` 43/43, and the new
`isSurfaceLossResult()` cases specifically re-run standalone (3/3) to
confirm the classifier is genuinely portable (no platform-specific
VkResult assumptions). `ctest -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|
rx_debug_ui_gpu|sample'` (CI's own exclusion pattern): 12/13 — the one
failure is the SAME pre-existing, unrelated `rx_asset_gltf_gpu_tests`
wall-clock stall-detector flake already noted in the base report above
(reproduced with an near-identical margin miss both times, in a module
this addendum never touches).

## Commits (pathspec-scoped, no AI attribution, author = local git config)

- `a739a40` — `fix(rx_rhi_vk): Device enters a surface-lost terminal state instead of hard-failing when the native window is gone`
- `9aa578c` — `fix(rx_platform): Window::abandonNativeHandle() skips SDL_DestroyWindow() on an already-gone handle`
- `7feced3` — `fix(samples/09_scene): stop touching a dying surface the moment Device::isSurfaceLost() fires`

No push. No board/issue/plan/spec/ledger edits.

---

# Addendum 2: samples 01–08 sweep (same-class defect, closed per policy)

The coordinator directed closing the same-class exposure in the other 8
`--present` samples in this round rather than deferring it, per the
"discovered pre-existing defects close when found" policy, with an
explicit warning against blind copy-paste — a couple of those loops
predate later conventions, so each file's own recreation call sites were
read and adapted individually rather than transplanting sample 09's own
shared-lambda refactor (which none of samples 01–08 have; each still
calls `Device::recreateSwapchain()` directly, inline, at each of its own
call sites — 2 or 3 per file: a suspended-present retry, `acquire`'s
`NeedsRecreate` branch, and `present`'s `NeedsRecreate` branch).

## What changed, and why it wasn't a mechanical find/replace

Every one of the 8 samples' own recreation call sites now checks
`Device::isSurfaceLost()` immediately after `recreateSwapchain()`
succeeds and, when set, calls `Window::abandonNativeHandle()` and stops
the loop gracefully (`quit = true;` in 01/02/03/04/05/06, `running =
false;` in 07/08 — each file's own existing flag name) — never treated as
a failure (`ok` stays `true`).

The one real adaptation needed across every file: several call sites
chain `recreateSwapchain()` together with the following
`onSwapchainRecreated()`/view-rebuild calls in a single `||` expression
(e.g. `if (!device->recreateSwapchain(surface) ||
!frameSync->onSwapchainRecreated(...) || !createSwapchainViews()) { ... }`).
Left as-is, short-circuit evaluation would let a surface-lost
`recreateSwapchain()` (which returns `true` — see the rx_rhi_vk commit)
fall through into evaluating `onSwapchainRecreated(0)` against an empty
image list, which could itself report failure and turn a graceful stop
back into `ok = false` — the exact wrong outcome this fix exists to
prevent. Every such chain was split: `recreateSwapchain()` checked alone
first, then `isSurfaceLost()`, then the rest of the original chain only
in the else branch — preserving each file's original control flow and
variable names (`rebuildDepthAndViews()` in `03_bindless_mesh`;
`compileForExtent()` — the render-graph recompile-before-realize call —
in `05_multipass`/`06_materials`; `executor->realize(graph)` in
`07_stress`/`08_gltf_viewer`).

`01_triangle`, `02_hotreload`, and `04_streaming` share one shape
(3-way `||` chains, `quit`/`ok`, `destroySwapchainViews()` +
`createSwapchainViews()`). `05_multipass`/`06_materials` share a second
shape (same 3-way chains, plus a `compileForExtent()` call gated on
`isSuspended()` after each rebuild — confirming these two samples
already independently discovered and implemented the same
"recompile-before-realize" pattern this round's own #36 work had to add
to `09_scene`). `03_bindless_mesh` has its own shape (depth-buffer
rebuild via `rebuildDepthAndViews()`, 2-way chains). `07_stress` and
`08_gltf_viewer` share a third shape (`running`/`ok`,
`executor->realize(graph)`, no `graph.compile()` re-run on resize at
all in either — a pre-existing, independent design choice in those two
samples, unrelated to and unchanged by this fix).

## Verification

Both presets build clean, zero warnings:
```
cmake --build --preset linux-native
cmake --build --preset windows-cross-zig
```

Full serial lavapipe ctest (linux-native, run once after the sweep):
**29/29 passed** — includes every affected sample's own headless gate
(`sample_01_triangle_headless` through `sample_08_gltf_viewer_headless`,
plus `sample_08_gltf_viewer_quit_during_load`), none of which exercise
`--present` but all of which prove the same binaries still build and run
their non-`--present` paths correctly.

Real-NVIDIA (RTX 2080, default ICD, driver-labeled) `xdotool windowclose`
spot-check, one representative per distinct loop shape, per the
coordinator's own selection:

| Sample | Shape represented | Exit code | Unfiltered validation errors | X errors |
|---|---|---|---|---|
| `01_triangle` | oldest/statically-linked, `quit`/`ok`, 3-way chains | **0** | 0 | 0 |
| `05_multipass` | `compileForExtent()` recompile-before-realize | **0** | 0 | 0 |
| `08_gltf_viewer` | `running`/`ok`, `executor->realize()`, closest sibling to 09 | **0** | 0 | 0 |

All three logs end with the identical sequence sample 09's own fix
produces: `Device::recreateSwapchain: surface capabilities query failed
... entering the surface-lost terminal state` → (sample-specific cleanup
logging, e.g. `08_gltf_viewer`'s own pipeline-cache save) → `window
closed cleanly` / `--present: window closed cleanly`.

**Stated honestly, per the verification bar given:** `02_hotreload`,
`03_bindless_mesh`, `04_streaming`, `06_materials`, and `07_stress` were
**not** independently run against real hardware this round — each was
verified by pattern inspection only (identical recreation-call-site
shape to whichever of the three spot-checked samples it structurally
matches, same fix mechanically applied and confirmed to compile and
link). This is a real, meaningfully weaker verification tier than the
three spot-checked samples for the ONE thing this fix specifically
guards — the exact runtime sequence of an external destroy racing the
frame loop — even though the code change itself is small, uniform, and
directly modeled on a pattern proven end-to-end three times over.

## Commit (pathspec-scoped, no AI attribution, author = local git config)

- `952b868` — `fix(samples): sweep Device::isSurfaceLost()/Window::abandonNativeHandle() consumption to samples 01-08`

No push. No board/issue/plan/spec/ledger edits.

---

# Addendum 3: round review, 2 Minors closed

Round review (`resize-fullscreen-review.md`): spec PASS on both #36 and
#73, Approved with 2 Minors, closed in-round.

## Minor 1: `isSurfaceLossResult()` hardening

`VK_ERROR_INITIALIZATION_FAILED` classifying as surface loss is an
out-of-spec, empirically-observed-only inference (verified correct on
this project's own NVIDIA/Xcb stack, unverified elsewhere). Hardened
without changing the working classification:

- `VK_ERROR_DEVICE_LOST` is now checked FIRST and unconditionally
  excluded inside `isSurfaceLossResult()` itself -- it already returned
  `false` before this (never in the match list), but the exclusion is now
  explicit and load-bearing rather than incidental, so it can't be
  silently lost as the function's own logic evolves. New dedicated
  `TEST_CASE`, separated from the general "unrelated errors" case, so
  this guarantee is its own visible assertion.
- `Device::recreateSwapchain()` now emits a clearly-labeled, one-shot
  (per process) `RX_LOG_WARN` -- naming the raw `VkResult` -- whenever the
  inference fires from anything other than `VK_ERROR_SURFACE_LOST_KHR`,
  mirroring `rx::platform::logWaylandMinimizeLimitationOnce()`'s own
  established "one-shot, diagnosable-not-silent" precedent (`window.h`).
  A future misclassification on a different driver now surfaces in the
  first bug report instead of silently eating a real, unrelated device
  error.
- Both `device.h`'s header comment and the implementation comment spell
  out the spec caveat explicitly (spec-documented `VK_ERROR_SURFACE_LOST_KHR`
  vs. the empirically-observed-only `VK_ERROR_INITIALIZATION_FAILED` half).

## Minor 2: unconditional graph recompile on vsync toggle

The deferred HUD vsync-toggle path (and every `recreateSwapchainAndDependents()`
caller) re-ran `RenderGraph::compile()` + `Executor::realize()`
unconditionally, even when a recreation only changed present mode (same
extent) -- pure redundant work for that case, since `compile()` is the
only place a `SwapchainRelative` resource's shape is resolved and a
present-mode-only recreation never changes it.

`recreateSwapchainAndDependents()` now computes "did the extent actually
change" exactly once, via the new pure `graphNeedsRecompileForExtent()`
(`window_resize.h`), and gates both `compile()` and the `realize()` that
must follow it on that single stored result -- computed once and reused,
never re-derived after `compileInfo`'s own width/height are updated (which
would trivially read "unchanged" the second time). `rebuildSwapchainViews()`/
`FrameSync::onSwapchainRecreated()` stay unconditional: every successful
`recreateSwapchain()` rebuilds the swapchain's own `VkImage`s regardless of
why it was called. `graphNeedsRecompileForExtent()` is deliberately its own
function rather than a reuse of `pixelSizeRequiresRecreate()` under a
confusing name -- that function's contract is built around
`Window::lastPixelSizeEvent()`'s own `{0,0}` pre-first-event sentinel, a
concern that does not apply at this call site (which only runs after
`Device::recreateSwapchain()` already succeeded with neither
`isSuspended()` nor `isSurfaceLost()` true).

Device-free coverage added to `test_window_resize.cpp`: unchanged extent
returns `false` (the exact vsync-toggle scenario), a genuinely different
extent on any single axis returns `true` (revert-discrimination against
silently reintroducing the stale-transient-extent bug the original `#36`
work fixed).

## Verification

- `rx_rhi_vk_tests` classifier cases: 4/4 (was 3/3 -- the new dedicated
  `VK_ERROR_DEVICE_LOST` case), run standalone
  (`--test-case="isSurfaceLossResult*"`) and as part of the full binary.
- `sample_09_scene_tests`: 45/45 (was 43/43 -- the 2 new
  `graphNeedsRecompileForExtent()` cases).
- Full rebuild (`ninja` at the `linux-native` preset) -- clean, zero
  warnings; picked up every sample automatically via the `rx_rhi_vk`
  header dependency, confirming the classifier hardening didn't disturb
  anything downstream.
- One full serial lavapipe ctest run: **29/29 passed**.

## Commits (pathspec-scoped, no AI attribution, author = local git config)

- `7a681af` — `harden(rx_rhi_vk): isSurfaceLossResult() round-review fixes -- explicit DEVICE_LOST exclusion, one-shot non-spec WARN`
- `07774a3` — `perf(samples/09_scene): skip RenderGraph::compile()/Executor::realize() when a recreation didn't change the extent`

No push. No board/issue/plan/spec/ledger edits.
