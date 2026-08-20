# Review: Issue #33 — relative mouse capture in sample 09's fly-through

Commits reviewed: `09fff66` (fix + tests), `6c9e45b` (docs/report), on top of
`5a96af0`. Independent review — no code written by the reviewer.

## Verdicts

**Spec compliance: PASS.** Every requirement in issue #33's ticket text and
`window.h`'s documented consumer contract is met: capture engaged by default
on entering `--present` (`runPresent()`'s unconditional startup call), Esc
toggles release/recapture (edge-triggered, non-repeat), click-to-recapture
gated on `!WantCaptureMouse` so a HUD click never steals capture, and
focus-loss/gain re-arm composes correctly with the toggle in both directions
(proven against a real headless `Window`, not just cited). Gamepad is
structurally unaffected. `MANUAL_VERIFICATION.md` rows are honestly
reconciled, not glossed over.

**Code quality: APPROVED**, with one minor (non-blocking) finding below.

## Findings

1. **Minor — Esc handler does not gate on `WantCaptureKeyboard`.**
   `main.cpp`'s `preDispatch` lambda toggles `mouseCapture` on
   `SDL_EVENT_KEY_DOWN`/Escape unconditionally, without checking
   `ImGui::GetIO().WantCaptureKeyboard` first. If a future HUD widget in this
   sample ever gains keyboard focus (e.g., a text field, which Dear ImGui
   conventionally uses Esc to cancel/unfocus rather than let bubble to the
   app), this handler would still unconditionally toggle mouse capture
   underneath it. Currently inert: `drawHud()` has zero `InputText`/keyboard-
   focusable widgets (checkboxes and text-only rows only), so the scenario
   cannot occur with the code as it stands today — verified by grep, no
   `ImGui::Input*` call anywhere in `main.cpp`. Not a live bug; flag for the
   next HUD-input round if a text-entry widget is ever added to this sample.

2. **Note, not a finding — pre-existing `WantCaptureKeyboard` early-return in
   `updateFlyCamera()` also gates gamepad polling**, not just WASD. This
   line (`if (ImGui::GetIO().WantCaptureKeyboard) { return; }`) predates this
   ticket and is untouched by the diff (confirmed: it's a diff context line,
   not a `+`/`-`), so it's out of scope here. It means the report's "gamepad
   is structurally unaffected by mouse-capture state" claim is accurate as
   scoped (gamepad reads neither `app.mouseCapture` nor `ImGui::GetIO()`
   directly) — but gamepad *can* still be blocked by this unrelated
   pre-existing keyboard-capture gate, which is a separate, older behavior
   this round correctly did not touch or need to touch.

3. **Note — neither this review's run nor the implementer's real-NVIDIA log
   explicitly names the selected physical device.** Both rely on
   `vulkaninfo`'s device listing + `vkb::PhysicalDeviceSelector`'s documented
   discrete-GPU-preference default to infer NVIDIA was selected over the
   also-installed lavapipe ICD, rather than a direct "selected: NVIDIA
   GeForce RTX 2080" log line. Pre-existing app behavior, unrelated to this
   diff — not a finding against this round, just recorded as a residual gap
   in what the log evidence alone proves.

No other findings. The `mouseDeltaDrivesCamera()` OR-composition, the
`FlyThroughCaptureState` toggle edge-triggering/idempotency, and the
`applyCaptureTransition()` guard are all correct, minimal, and covered by
tests that actually discriminate the bug classes they claim to (verified
directly, see below).

## Attention-lens verification detail

- **State machine coverage (`FlyThroughCaptureState`).** Captured by
  default; `toggleOnEscPressed()` flips unconditionally (correct — the
  caller is contractually required to call it only on a non-repeat
  `KEY_DOWN`, and `main.cpp` does: `!event.key.repeat` is checked at the
  call site); `recaptureOnViewportClick()` is idempotent while captured.
  Click-on-HUD-while-released correctly cannot recapture: the call site gates
  on `!app->mouseCapture.captured() && !ImGui::GetIO().WantCaptureMouse`
  before invoking `recaptureOnViewportClick()` — confirmed by direct read of
  `main.cpp:3230`.
- **Esc during HUD text input.** See finding 1 above — the gating order
  question is real in principle (Esc is checked before/independent of
  `WantCaptureKeyboard`), but inert today because this sample's HUD has no
  keyboard-focusable/text-input widgets. Not a defect in the delivered code.
- **WASD gating.** `updateFlyCamera()`'s pre-existing
  `WantCaptureKeyboard` early-return (unchanged by this diff — confirmed via
  the diff context, not a `+` line) already gates all keyboard movement
  (WASD/Space/LCtrl) and, as a side effect, gamepad polling too. This was
  correctly left untouched — it's pre-existing scope, not part of issue #33.
- **`mouseDeltaDrivesCamera(captured, WantCaptureMouse)` composition.**
  Captured always returns true (drives camera) regardless of
  `WantCaptureMouse`; released reduces to exactly the pre-existing
  `!WantCaptureMouse` gate. Truth table matches the 2×2 test case exactly.
- **Focus-loss/gain composition, both directions.** Read
  `test_mouse_capture_focus_composition.cpp` directly: test case 1 drives
  captured→Esc-release→alt-tab-away→alt-tab-back and asserts
  `relativeMouseModeWanted() == false` (did not silently re-arm); test case
  2 drives released→recapture→alt-tab-away→alt-tab-back and asserts
  `== true` (did re-arm); test case 3 proves a same-state no-op frame
  doesn't disturb either direction. This genuinely exercises
  `window.cpp`'s real `SDL_EVENT_WINDOW_FOCUS_LOST`/`_GAINED` handling
  (confirmed by reading `window.cpp:224-244`: `FOCUS_GAINED` unconditionally
  re-issues `SDL_SetWindowRelativeMouseMode(window_, true)` only `if
  (relativeModeWanted_)`, and `setRelativeMouseMode()` is the sole writer of
  that field) against a real headless `Window`, not a mock. Both directions
  proven, as claimed.
- **Gamepad unaffected.** Confirmed by code read:
  `updateFlyCamera()`'s gamepad block (`window.poll()`, left-stick
  move/right-stick look/trigger speed) sits entirely outside the
  `mouseDeltaDrivesCamera()` branch this ticket touched, and touches neither
  `app.mouseCapture` nor `ImGui::GetIO()`. Zero gamepad-path lines changed
  in the diff.
- **Mutation re-proof (performed independently, not trusted from the
  report).** Edited `mouse_capture.h`'s `mouseDeltaDrivesCamera()` from
  `captured || !imguiWantsMouse` to `captured && !imguiWantsMouse`, rebuilt
  `sample_09_scene_tests`, and reran: **4 of 28 cases failed, 6 of 99
  assertions failed** — the exact truth-table case, the CAPTURED-always-true
  case, the RELEASED-preserves-gate case, and the end-to-end composition
  case — matching the report's claim exactly. Restored the file; `git diff`
  on `mouse_capture.h` is empty (byte-identical restore); rebuilt and reran:
  28/28, 99/99 green again.

## Empirical verification (all reproduced directly, not trusted from the report)

- **Full serial lavapipe ctest** (`VK_ICD_FILENAMES=.../lvp_icd.json
  xvfb-run -a ctest --preset linux-native --output-on-failure`): **29/29
  passed**, ~77s. Matches report.
- **`sample_09_scene_tests` standalone**: **28/28 cases, 99/99 assertions**
  passed under Xvfb. Matches report exactly.
- **Real-NVIDIA `--present --validate`, ~15s sustained**, run against a real
  (non-Xvfb) X display, default/unforced ICD loader. `vulkaninfo --summary`
  independently confirms `deviceName = NVIDIA GeForce RTX 2080`,
  `driverID = DRIVER_ID_NVIDIA_PROPRIETARY`, `driverInfo = 580.82.07`,
  alongside a second, also-installed `llvmpipe`/`DRIVER_ID_MESA_LLVMPIPE`
  device — the standing dual-ICD environment. Result: clean SIGTERM exit,
  `window closed cleanly` logged, **0 `[error]`-level lines**, **0**
  `Validation Error` lines lacking this codebase's documented
  `(known false positive: ...)` guard prefix (4839 of 4839 carried the
  guard), **0** `SDL_SetWindowRelativeMouseMode` failure warnings (positive
  evidence the real NVIDIA driver granted relative mode, per
  `setRelativeMouseMode()`'s log-on-failure-only convention). Line counts
  differ trivially from the report's own run (4864 vs. 4834 total lines) —
  expected run-to-run variance in frame count over a fixed wall-clock
  window, not a discrepancy.
- **windows-cross-zig build + Wine**: clean rebuild of `sample_09_scene` and
  `sample_09_scene_tests` (zero warnings in ninja output);
  `wine sample_09_scene_tests.exe` → **28/28 cases, 99/99 assertions**
  passed (Wine `fixme:` noise only, no test failures) — matches report
  exactly.
- **windows-cross-zig ctest**, same GPU/sample exclusion filter the report
  used (`-E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample'`):
  **13/13 passed**. Matches report exactly.

## D5 thread-affinity omission adjudication: SOUND

The implementer's rationale — that `mouse_capture.h` is a pure value type
with no OS resource/static/shared state, matching the precedent of every
other pure sample-local header in this codebase — was independently checked
rather than taken on faith:

- `samples/09_scene/fly_camera.h`: no `D5`/thread-affinity comment anywhere.
- `samples/09_scene/draw_recording.h`: no `D5`/thread-affinity comment
  anywhere.
- `src/rx_platform/include/rx_platform/input.h`'s
  `applyStickDeadzone()`/`applyTriggerDeadzone()`: no `D5`/thread-affinity
  comment on either free function.
- As a further data point not cited by the implementer: `main.cpp`'s own
  `HudState` struct (another sample-local, per-`App`-instance mutable value
  type) also carries no `D5` note at its definition.

`docs/threading.md`'s D5 note is applied, in every case checked, only to
headers exposing a main-thread-only/worker-allowed API over a
stateful/OS-resource-owning *class* (`Window` itself, `GeometryPool`, etc.),
never to plain sample-local value-type structs/free functions. The omission
is consistent with established codebase convention, not a gap. Adjudicated:
**sound**.

## Commit hygiene

- Exactly 2 commits under review: `09fff66` (fix + tests), `6c9e45b`
  (docs/report), both on `Yousef Wadi <ywadi85@gmail.com>` — matches local
  `git config user.*`.
- Pathspec scope is tight: `09fff66` touches only
  `samples/09_scene/{main.cpp,mouse_capture.h,tests/*}`; `6c9e45b` touches
  only the new report file and `MANUAL_VERIFICATION.md`. Neither commit
  touches `progress.md` or any plan/spec/board content, as the report
  claims.
- No AI attribution anywhere: `git show -s --format=%B` on both commits, and
  a content grep of the full `09fff66` diff, both come back clean of
  "Claude"/"Anthropic"/"Co-Authored-By"/"Generated with" or any equivalent.
- Nothing pushed: `git status --short --branch` shows
  `## main...origin/main [ahead 2]` — exactly the 2 commits under review,
  not yet on the remote.
- Working tree, post-review: clean except the pre-existing (out-of-scope,
  untouched by this round) local modification to
  `.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md`, left alone
  per instructions. The reviewer's own temporary mutation edit to
  `mouse_capture.h` was restored byte-identically (`git diff` on that file
  is empty) before this document was written.

## Not independently verifiable

- Real human-observed cursor behavior (visible hide/lock, Esc/click feel,
  Steam Deck gamepad drivability) — genuinely not observable headlessly or
  from a non-interactive `--present --validate` session, exactly as the
  implementer's report and the updated `MANUAL_VERIFICATION.md` rows both
  state. The code-path evidence (API call reached + granted, absence of SDL
  failure logs, real driver + real X display) is the strongest evidence
  obtainable without a human at the keyboard, and is present.
- Which physical device Vulkan validation-layer output was generated
  against is inferred (vulkaninfo + `vkb::PhysicalDeviceSelector` default
  discrete-GPU preference), not directly logged by the app itself — see
  finding 3. Pre-existing, out of scope for this diff.
