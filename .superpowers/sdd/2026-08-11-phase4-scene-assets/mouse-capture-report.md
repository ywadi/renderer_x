# Issue #33 — relative mouse capture in sample 09's fly-through

Implementer report. Base commit at start: `4c481c7`; main advanced during
this session via a concurrent, unrelated docs-only commit (`5a96af0`,
layer-10 offline-tooling inventory) from another agent — no file overlap
with this work.

## Ticket text (`gh issue view 33`)

> Sample 09: engage relative mouse capture in fly-through (bug, phase-4)
>
> User-reported: the OS cursor keeps moving during mouse-look, making
> fly-through control impractical. Root cause: samples/09_scene never calls
> `Window::setRelativeMouseMode()` — the platform facility (hide+capture,
> delta accumulation, focus-loss auto-release) exists and is tested but
> unconsumed. Fix: capture on fly start (or click-to-capture), Esc/focus-loss
> release, ImGui `WantCaptureMouse` gating so the HUD remains usable while
> released; MANUAL_VERIFICATION rows updated. Queued behind the texture-path
> round.

## Files changed

- `samples/09_scene/mouse_capture.h` (new) — `rx::samples9::FlyThroughCaptureState`
  (pure capture/release toggle) + `mouseDeltaDrivesCamera()` (pure composed
  gate). Same device-free, header-only shape as `fly_camera.h`'s own W/S-fix
  precedent this task was told to follow.
- `samples/09_scene/tests/test_mouse_capture.cpp` (new) — 9 TEST_CASEs, fully
  SDL/Window-free, covering the toggle state machine and the
  `mouseDeltaDrivesCamera()` truth table.
- `samples/09_scene/tests/test_mouse_capture_focus_composition.cpp` (new) —
  3 TEST_CASEs linking a real (headless) `rx::platform::Window` to prove the
  requirement-1 composition claim (re-arm-on-focus-gain vs. the capture
  toggle) end to end, not just by citation.
- `samples/09_scene/tests/CMakeLists.txt` — registers both new test files;
  adds `rx_platform` to the test binary's link libraries (previously
  `rx_scene` + doctest only).
- `samples/09_scene/main.cpp` — wires the fix: include, `App::mouseCapture`
  field, `updateFlyCamera()`'s composed mouse gate, the initial
  `setRelativeMouseMode()` engagement call in `runPresent()`, the
  Esc/click-to-recapture handling inside the existing `pumpEvents`
  `preDispatch` lambda, the post-pump apply-on-change call, and a HUD status
  line (present-mode only).
- `MANUAL_VERIFICATION.md` — new capture/release/Esc/HUD-interaction
  checklist rows under `## 09_scene` (unchecked); reconciled the
  `## rx_platform input surface` section's "09_scene is now the first real
  consumer" claim (it consumed deltas but never actually engaged capture —
  exactly this ticket's gap); added a real-NVIDIA log-evidence addendum;
  cross-referenced the `## rx_debug_ui overlay` "camera stops moving" row.

Not touched: `.superpowers/sdd/.../progress.md` (ledger — another agent's
concurrent WIP, explicitly out of scope), any plan/spec/board content.

## Per-requirement proof

### 1. Engage relative mouse mode; capture default; Esc toggle; click-to-recapture; re-arm composition

`runPresent()` (main.cpp) now:
- Calls `app->window->setRelativeMouseMode(app->mouseCapture.captured())`
  unconditionally once, right after `Overlay::create()` succeeds (captured
  by default — recommended UX, usable from frame 1).
- Inside the existing `pumpEvents` `preDispatch` lambda: a non-repeat
  `SDL_EVENT_KEY_DOWN`/`SDL_SCANCODE_ESCAPE` calls
  `mouseCapture.toggleOnEscPressed()`; a `SDL_EVENT_MOUSE_BUTTON_DOWN`
  (left button) while released AND `!ImGui::GetIO().WantCaptureMouse` calls
  `mouseCapture.recaptureOnViewportClick()`.
- After the full event drain, applies exactly one real transition —
  `if (captured() changed) window->setRelativeMouseMode(captured())`.

Focus-loss/gain re-arm is platform-handled and was **not reimplemented** —
verified, not assumed: `src/rx_platform/src/window.cpp`'s
`SDL_EVENT_WINDOW_FOCUS_GAINED` case unconditionally re-issues
`SDL_SetWindowRelativeMouseMode(window_, true)` only `if
(relativeModeWanted_)`, and `setRelativeMouseMode()` is the only writer of
that field. Composition with our toggle is proven directly (not just cited)
by `test_mouse_capture_focus_composition.cpp`, which drives a real headless
`Window` through: initial capture → Esc release → alt-tab away/back →
assert `relativeMouseModeWanted() == false` (did NOT silently re-arm), and
the mirror case: release → recapture → alt-tab away/back → assert `== true`
(DID re-arm). Both pass. See "Test evidence" below for the mutation proof
that these assertions are load-bearing.

### 2. ImGui interplay (WantCaptureMouse/WantCaptureKeyboard gating)

`updateFlyCamera()`'s pre-existing `WantCaptureKeyboard` early-return is
unchanged (keyboard gating untouched by this ticket). The mouse-delta gate
is now `mouseDeltaDrivesCamera(mouseCapture.captured(),
ImGui::GetIO().WantCaptureMouse)`:
- CAPTURED → always `true` (cursor hidden; HUD ignores it by contract,
  requirement 2's own wording).
- RELEASED → `!WantCaptureMouse` exactly, i.e. the pre-existing gate,
  unchanged — HUD stays fully usable while released, camera never steals
  clicks/drags aimed at an open HUD panel.

No duplication of the overlay's own ImGui-first event routing: the fix adds
branches to the ALREADY-existing `preDispatch` lambda
(`app->overlay->processEvent(event)` still runs first, unconditionally,
before the new Esc/click branches) rather than introducing a second event
path.

### 3. Gamepad unaffected in both capture states

Verified by inspection, not merely "didn't break it": `updateFlyCamera()`'s
gamepad block (`window.poll()`, left-stick move, right-stick look, trigger
speed multiplier) sits entirely outside the mouse-delta `if` this ticket
touched — zero lines of the gamepad path were changed, and it reads neither
`app.mouseCapture` nor `ImGui::GetIO()`. Structurally cannot be affected by
capture state.

### 4. Tests

- **Pure state machine** (`test_mouse_capture.cpp`, 9 cases, 28
  assertions): default-captured, Esc toggle transitions (multi-press
  round-trip), click-to-recapture (including idempotent no-op while
  captured), the full `mouseDeltaDrivesCamera()` 2×2 truth table, and an
  end-to-end capture/HUD-interaction/release/recapture cycle scenario.
- **Real-Window composition** (`test_mouse_capture_focus_composition.cpp`,
  3 cases, 16 assertions): proves requirement 1's re-arm/toggle composition
  against a real (headless) `rx::platform::Window`, per the ticket's
  explicit ask to confirm this rather than assume it.
- **MANUAL_VERIFICATION.md**: new unchecked rows for capture-default,
  Esc release/recapture, click-to-recapture, HUD usability while released,
  and alt-tab-while-captured — the genuinely non-automatable half (actual
  OS cursor visibility/lock behavior, human feel).
- **Reconciliation**: the `## rx_platform input surface` section's
  "09_scene is now the first real consumer" claim is corrected — Task 24
  consumed deltas but never engaged capture; this ticket closes that gap.
  Cross-referenced from the `## rx_debug_ui overlay` section's pre-existing
  "camera stops moving while HUD has focus" row.

### 5. Verification (commands + tails below)

- Full serial lavapipe ctest: **29/29 green** (linux-native).
- Real-NVIDIA `--present --validate`, ~15s sustained: clean (see below).
- windows-cross-zig build + Wine: clean build, **28/28** device-free tests
  green under Wine; windows-cross-zig ctest (GPU/sample-excluded, CI's own
  convention): **13/13** green.
- Zero compiler warnings on every build performed (linux-native full
  rebuild, windows-cross-zig full rebuild) — nothing suppressed, plainly
  absent from `ninja` output.
- Zero unfiltered Vulkan validation errors on the real-NVIDIA run (see
  below).

## Test evidence: revert-discrimination (mutation testing)

Each claim below was verified by actually breaking the code, rebuilding,
observing the expected test failure, then restoring the original and
rebuilding green again — not asserted from reading the test source.

1. **`mouseDeltaDrivesCamera()`: OR → AND.** Changed
   `return captured || !imguiWantsMouse;` to `captured && !imguiWantsMouse;`.
   Result: 4 of 9 `test_mouse_capture.cpp` cases failed (6 assertions),
   including the explicit 2×2-truth-table case. Restored → 9/9 green again.

2. **`FlyThroughCaptureState::toggleOnEscPressed()`: toggle → always-release.**
   Changed `captured_ = !captured_;` to `captured_ = false;`. Result: 2
   cases failed (the toggle-round-trip case and the click/Esc/click
   sequence case) — exactly the two designed to catch this class of bug.
   Restored → 9/9 green again.

3. **`applyCaptureTransition()`'s guard: `!=` → `==`** (in
   `test_mouse_capture_focus_composition.cpp`, mirroring main.cpp's real
   `if (captured() != capturedBeforePump)` condition). Result: all 3
   composition cases failed at their first post-toggle
   `relativeMouseModeWanted()` assertion (both stayed wrongly stuck at the
   initial captured=true state). Restored → 28/28 green again (full
   `sample_09_scene_tests` binary).

## Command tails

### Full serial lavapipe ctest (linux-native)

```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a ctest --preset linux-native --output-on-failure
...
27/29 Test #27: sample_09_scene_headless .................   Passed    1.72 sec
28/29 Test #28: sample_09_scene_stress_headless ..........   Passed    1.03 sec
29/29 Test #29: sample_09_scene_tests ....................   Passed    0.40 sec

100% tests passed, 0 tests failed out of 29
Total Test time (real) =  79.96 sec
```

### `sample_09_scene_tests` full run (linux-native, under Xvfb — needs SDL video init now that it links rx_platform)

```
$ xvfb-run -a ./samples/09_scene/tests/sample_09_scene_tests
[doctest] doctest version is "2.5.3"
...
===============================================================================
[doctest] test cases: 28 | 28 passed | 0 failed | 0 skipped
[doctest] assertions: 99 | 99 passed | 0 failed |
[doctest] Status: SUCCESS!
```

### windows-cross-zig build + Wine

```
$ cmake --build --preset windows-cross-zig --target sample_09_scene sample_09_scene_tests
...
[7/7] Linking CXX executable samples/09_scene/sample_09_scene.exe   (also sample_09_scene_tests.exe)
(zero warnings emitted)

$ wine samples/09_scene/tests/sample_09_scene_tests.exe
...
[doctest] test cases: 28 | 28 passed | 0 failed | 0 skipped
[doctest] assertions: 99 | 99 passed | 0 failed |
[doctest] Status: SUCCESS!

$ xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample' --output-on-failure
...
100% tests passed, 0 tests failed out of 13
Total Test time (real) = 115.46 sec
```

### Real-NVIDIA `--present --validate`, ~15s sustained (default/unforced ICD)

Environment: `vulkaninfo --summary` confirms `GPU0: NVIDIA GeForce RTX 2080`,
`driverID = DRIVER_ID_NVIDIA_PROPRIETARY`, `driverInfo = 580.82.07`, alongside
an also-installed lavapipe ICD (`GPU1`) — `vkb::PhysicalDeviceSelector`'s own
default discrete-GPU preference (unchanged by this ticket) selects the
NVIDIA device. Run against a real X display (not Xvfb), default/unforced
`VK_ICD_FILENAMES`.

```
$ ./sample_09_scene --present --validate   # backgrounded, SIGTERM after ~15s
...
[info] Device::create: present mode in use: FIFO (explicit default, PresentMode::VsyncOn)
[info] sample_09_scene: --present: present mode in use: FIFO
...
[info] sample_09_scene: window closed cleanly
```

Log analysis (4834 lines total):
- `[error]`-level lines: **0**.
- `Validation Error` lines total: 4809, of which lines **NOT** carrying this
  codebase's own documented `(known false positive: ...)` guard prefix:
  **0**.
- `SDL_SetWindowRelativeMouseMode` failure warnings: **0** — `Window::
  setRelativeMouseMode()` only logs on failure (matches `setFullscreen()`'s
  convention), so this absence is positive evidence the real NVIDIA driver
  GRANTED relative mode for this session, not merely that the call was
  reached.

What this does **not** prove, and is intentionally left as unchecked
MANUAL_VERIFICATION rows: whether the OS cursor visibly stays hidden/locked,
whether Esc/click-to-recapture *feel* correct, and Steam Deck gamepad
drivability. None of that is observable from a log in a non-interactive
session — stated honestly per the task's own instruction, not glossed over.
The code-path proof for this run is exactly what the task asked for: the
platform API call being made (and granted) + the state-machine tests above.

## Concerns / open items

- Real human-observed cursor-capture behavior (visible hide/lock, Esc feel,
  click-to-recapture feel) remains unverified — genuinely not automatable
  headlessly, consistent with this file's own established posture for every
  other `09_scene` mouse/gamepad row. Flagged as new unchecked
  MANUAL_VERIFICATION rows, not silently left implicit.
- `mouse_capture.h` deliberately carries no D5 thread-affinity comment: it
  is a pure value type with no OS resource and no static/shared state,
  matching the established precedent of every other pure sample-local
  header in this codebase (`fly_camera.h`, `draw_recording.h`,
  `rx_platform/input.h`'s `applyStickDeadzone`/`applyTriggerDeadzone`) —
  none of which carry a D5 note either. D5 is scoped to headers exposing a
  main-thread-only or worker-allowed API over stateful/OS-resource-owning
  classes (`docs/threading.md`); applying it here would be boilerplate
  without content.
