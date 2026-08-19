# Task 20 review — Input expansion (mouse capture + gamepad + keyboard, card #14)

Reviewer: independent review session. Commit under review: `1a69c31`
(`feat(rx_platform): input expansion -- mouse capture, gamepad, keyboard
(#14)`), base `736ed84`. Authority order followed:
`gate/rulings-2026-08-18.md` §#14 > spec > `gate/matrix-issue14-input.md` >
ticket #14, per the brief.

## Verdict

**Spec compliance: PASS.** **Code quality: Approved** (no findings —
zero Low/Medium/High issues raised).

## Method

1. Read `task-20-brief.md`, `task-20-report.md`, the full review diff
   (`review-736ed84..1a69c31.diff`, 2315 lines), `gate/rulings-2026-08-18.md`
   (full file, cross-cutting + per-ticket #14 section), and
   `gate/matrix-issue14-input.md` (full file, all 11 rows + 4 conflicts +
   verification-health section) — in the order the brief specifies.
2. Rebuilt `rx_platform_tests` from a clean target (`--clean-first`),
   captured the build log, grepped for `warning`/`error` — zero hits.
3. Rebuilt the full `linux-native` tree and ran the complete `ctest` suite
   under `xvfb-run -a` — 23/23 passed, 144.77s (report claimed 150.76s;
   consistent run-to-run variance, same 23/23 outcome).
4. Confirmed the dev machine's real gamepad independently via
   `/proc/bus/input/devices`: `"Generic X-Box pad"`, VID `0x3537` / PID
   `0x1012` — exact match to the report's disclosed environmental confound.
5. Ran `rx_platform_tests` standalone in both the natural (real pad present)
   and neutralized (`SDL_JOYSTICK_BLACKLIST_DEVICES=0x3537/0x1012`)
   configurations — both reproduced the report's exact assertion counts:
   **27/27 cases, 123/123 assertions** natural; **27/27 cases, 312/312
   assertions** neutralized. The 189-assertion delta is entirely
   environment-conditional honest-skip branches (verified by reading every
   `foreignGamepadIds()` guard site — each skip emits a `MESSAGE` naming
   exactly why, never a silent pass).
6. Independently re-derived the MANDATORY (8500,8500) scaled-radial output
   by hand (`mag ≈ 0.36685`, `scaledMag ≈ 0.16235`, direction
   `(0.70711,0.70711)` → `(0.11481,0.11481)`) — matches the committed test's
   asserted values to 5 significant figures.
7. Performed three independent in-tree revert→rebuild→run→restore→rebuild
   cycles (git-diff-clean before and after each):
   - Scaled-radial deadzone → naive per-axis clamp (`input.cpp`): the
     MANDATORY (8500,8500) test failed on all 4 assertions exactly as
     claimed (`0.259399` vs. expected `≈0.1148`/`≈0.16235`, and the
     "NOT equivalent" assertion itself failed since the naive and buggy
     outputs now matched).
   - SOUTH/EAST swapped in `Window::poll()`: the full-button-surface test
     failed with exactly 4 assertion failures, as claimed.
   - `SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1")` commented
     out in `Window::create()` (not itself in the report's numbered revert
     list — added by this review to adjudicate the flagged production-bug
     fix): 5 test cases / 17 assertions failed, all gamepad-related,
     confirming every gamepad test in this suite is load-bearing on this
     fix given every test window here is `visible=false`.
   All three cycles were restored and reconfirmed green
   (`git diff --stat -- src/rx_platform/` empty after each restore; final
   state re-verified 27/27 cases, 312/312 assertions neutralized).
8. Checked commit hygiene: author/committer identity, message content, and
   full diff text for AI attribution; file-list scope; push status.

## Spec-compliance findings (attention-lens checklist)

All items below verified directly against the committed code and gate
ruling #14 / matrix rows, not merely against the implementer's report.

- **Deadzone is scaled-radial, never per-axis** (`input.cpp:8-35`):
  `mag = sqrt(x²+y²)/32768`; below threshold → `{0,0}`; else
  `normalize(stick) * ((mag-deadzone)/(1-deadzone))`, magnitude-clamped to
  1. Matches the matrix row 5 formula verbatim. The mandatory (8500,8500)
  case is present, asserts the exact scaled output (not a bare
  zero/non-zero check), and independently discriminates against a
  spelled-out naive-per-axis comparator in the same test body — confirmed
  both by hand-calculation and by the revert cycle above. **Discriminating
  power confirmed real, not decorative.**
- **Full discrete button set**: `GamepadButtons` (`input.h:648-660`) exposes
  D-pad ×4, SOUTH/EAST/WEST/NORTH (grepped SDL3 3.4.14's own
  `SDL_gamepad.h` — no `_A`/`_B` exists, confirmed independently), both
  shoulders, START. `poll()` wires all eleven fields to the correct
  `SDL_GamepadButton` enum values (`window.cpp:365-375`); the button-surface
  test drives each independently and asserts every OTHER field stays false
  (proves no stuck-true vacuous pass).
- **Trigger 1D deadzone** (`input.cpp:37-41`): matches matrix row 6's
  formula, default `1000/32767`. End-to-end test additionally documents a
  real SDL gotcha (raw rest value is `SDL_JOYSTICK_AXIS_MIN`, not 0) —
  verified present in the test file and consistent with SDL3's own
  documented virtual-axis behavior.
- **Mouse deltas = xrel/yrel accumulation, consume-and-reset**
  (`window.cpp:1199-1204`, `consumeMouseDelta()`): summed inside
  `pumpEvents()`'s existing full-drain loop, filtered to this window's own
  `SDL_WindowID`, reset on every `consumeMouseDelta()` call. Device-free
  test proves the exact float sum and the reset; a second test proves
  cross-window isolation.
- **Focus-loss pause + unconditional relative-mode re-arm**
  (`window.cpp:1172-1192`): `SDL_EVENT_WINDOW_FOCUS_LOST` sets `focusLost_`,
  which gates `MOUSE_MOTION` accumulation (`&& !focusLost_`);
  `SDL_EVENT_WINDOW_FOCUS_GAINED` clears it and unconditionally re-issues
  `SDL_SetWindowRelativeMouseMode(window_, true)` if the app still wants it
  — matches the gate ruling's exact wording ("track app intent
  independently... unconditional re-assert on focus-regain").
- **Cursor confine via `SDL_SetWindowMouseGrab`**
  (`setCursorConfined`/`cursorConfined`, `window.cpp:1290-1301`): correct
  API — not `SDL_SetWindowMouseRect`, which the matrix explicitly
  distinguishes as the wrong primitive for this row.
- **Hot-plug map keyed by `SDL_JoystickID`, synchronous close-then-erase**
  (`window.cpp:1233-1241`): `SDL_CloseGamepad(it->second)` THEN
  `gamepads_.erase(it)`, both inside the same `SDL_EVENT_GAMEPAD_REMOVED`
  case — never deferred. Hot-plug lifecycle and rapid attach-detach-attach
  tests both pass in the neutralized run.
- **Single-active rule = lowest connected ID** (`window.cpp:1316-1319`,
  `std::min_element` by `SDL_JoystickID`): virtual-joystick-tested
  (attaches two pads with distinguishable state, detaches the lower-ID one,
  asserts reselection with the higher pad's OWN state) — this test passed
  in the neutralized run (part of the 312/312 total).
- **Task 17's window state machine untouched**: `minimizedEventObserved_`,
  `lastPixelSizeEvent_`, `setFullscreen`/`isFullscreen`, and
  `logWaylandMinimizeLimitationOnce` are structurally unchanged in
  `window.h`/`window.cpp` (diff shows pure addition around them, no edits
  to their bodies); their own pre-existing tests all still pass (part of
  the 23/23 full-suite ctest run and the 27/27 `rx_platform_tests` run).
- **D5 one-liners + main-thread guards**: every one of the ten new/touched
  public methods carries both a "Thread-affinity (D5, Phase 4)..." doc
  comment and an `RX_ASSERT_MAIN_THREAD(...)` guard (verified by reading
  the full `window.h`/`window.cpp`); `docs/threading.md` gained a correctly
  formatted registry entry matching the file's existing convention exactly.
  The thread-guard test (worker-thread trip + main-thread no-trip) passed
  in every run observed.
- **MANUAL_VERIFICATION.md rows**: the new "rx_platform input surface"
  section's 6 rows are honestly unchecked (`- [ ]`), correctly note Task 24
  as first consumer, and correctly describe what genuinely cannot be
  automated (visual cursor confirmation, real key-press injection, real
  hardware hot-plug, Deck hardware). No row is checked prematurely.
- **Keyboard surface** (NEW SCOPE): `isKeyDown(SDL_Scancode)`
  (`window.cpp:1344-1352`) is bounds-checked against SDL's own reported
  `numKeys`, not a hardcoded constant — confirmed by reading the
  implementation directly.

No spec-compliance gaps found against the gate ruling #14 text, the matrix's
11 rows, or the attention-lens checklist.

## Code-quality findings

None. Zero findings at any severity (Low/Medium/High). Specific quality
observations, all positive:

- Deadzone math and value types are correctly factored into a
  dependency-free `input.h`/`.cpp` (zero SDL includes), making the core
  formula unit-testable without any SDL initialization — a clean design
  choice explicitly offered by the brief ("+input.h if cleaner") and
  exercised well by the test suite's pure-math test block.
- Move constructor/assignment/destructor correctly extended for all four
  new scalar members and the `gamepads_` map (moved, not copied), with
  `closeAllGamepads()` shared cleanly between the destructor and
  move-assignment's "destroy what I currently own" step.
- Test suite design is unusually disciplined about the real environmental
  confound (a physically-connected gamepad on the dev machine): every
  gamepad test checks `foreignGamepadIds()` and takes an honest,
  message-logged skip rather than either ignoring the confound or building
  a fragile test that silently no-ops. Verified this is not a fig leaf: the
  skip/no-skip assertion-count delta (123 vs. 312) is real and reproducible
  independently, and the skip logic was exercised on both branches during
  this review.
- Zero warnings on a clean rebuild; zero `TODO`/`FIXME`/`XXX`/leftover
  `REVERT-TEST` markers in the final tree (independently grepped).
- Commit hygiene: author/committer both `Yousef Wadi <ywadi85@gmail.com>`
  (matches the user's own git identity); commit message and full diff text
  grepped for AI-attribution strings (`co-authored`, `claude`, `anthropic`,
  `generated by`, `chatgpt`, `copilot`) — zero hits; file list matches the
  report's claimed 9 files exactly (no `progress.md` leakage — confirmed by
  `git status`/`git diff --stat` before this review began); local `main` is
  ahead of `origin/main` by exactly this one commit — nothing pushed.

## Background-events fix adjudication

**`SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1")` in
`Window::create()` is correctly scoped, is indirectly tested (and directly
testable), and carries no side-effect risk for the focus-loss pause
semantics.**

- **Scope**: the call sits inside `Window::create()`
  (`window.cpp:66`), set unconditionally before `SDL_InitSubSystem(
  SDL_INIT_GAMEPAD)` comes up two lines later, exactly matching the report's
  description. It touches only SDL's internal joystick/gamepad background-
  events policy — a global SDL hint, not a `Window` member — and has no
  other call site.
- **Tested**: not asserted by a dedicated unit test that reads the hint
  value back, but genuinely load-bearing and exercised on every test run:
  every window this test binary creates is `visible=false` (hidden,
  unfocused), so every one of the gamepad tests (hot-plug, single-active,
  axis, trigger, button, gyro-log) depends on this hint being set to
  observe any gamepad state change at all. Confirmed empirically in this
  review: commenting the hint out and rebuilding broke exactly 5 test
  cases / 17 assertions, all gamepad-related, with the failure mode being
  driven values never appearing (e.g. `south` staying `false` after
  `setButton(..., true)`) — the precise symptom the report's own repro
  narrative describes. This is real, reproducible, load-bearing coverage,
  even though no test literally asserts `SDL_GetHint(...) == "1"`.
- **No side-effect risk for focus-loss pause semantics**: the hint governs
  only SDL's joystick/gamepad subsystem's internal event-delivery policy —
  it has no relationship to `SDL_EVENT_WINDOW_FOCUS_LOST`/`FOCUS_GAINED` or
  to the `focusLost_` member, which exclusively gates `MOUSE_MOTION`
  accumulation (`window.cpp:1200`: `if (event.motion.windowID ==
  thisWindowId && !focusLost_)`). Reading through the full `pumpEvents()`
  body confirms `focusLost_` is never referenced in the
  `GAMEPAD_ADDED`/`GAMEPAD_REMOVED` cases, and `Window::poll()` performs a
  live `SDL_GetGamepadAxis`/`SDL_GetGamepadButton` read with no focus check
  at all — gamepad *consumption* was never gated on focus in the first
  place (correctly: neither the ticket, the matrix, nor the ruling asks for
  a gamepad focus-loss pause; only mouse relative-mode/delta accumulation
  carries that requirement, per matrix row 1 and ruling #14's "focus-loss
  re-arm criterion" language, which is mouse-scoped throughout). The two
  mechanisms are orthogonal by construction: one is an SDL-internal
  subsystem hint affecting whether gamepad state updates reach the process
  at all while unfocused; the other is this class's own app-level pause
  flag gating mouse-delta *accumulation* specifically. Making gamepad
  input keep working while a window is unfocused (e.g. under Gamescope's
  overlay) is in fact the entire point of the fix — gating gamepad
  *consumption* on `focusLost_` the way mouse deltas are gated would defeat
  it. No regression risk identified.
- This is a real, well-diagnosed production bug (SDL's documented default
  silently drops gamepad input for any unfocused window, which is Steam
  Deck's normal Gamescope-overlay operating condition — the ticket's own
  stated acceptance bar) and the fix is appropriately minimal, scoped
  exactly to this ticket's own `Window::create()` change surface, and
  disclosed prominently as an unbriefed deviation in the report. **Judged
  correctly in-scope, not deferred.**

## Not independently verifiable

- The report's "Gate-mandated virtual-joystick smoke check" section quotes
  a `-tc="*SMOKE*"` invocation ("1 | 1 passed | 0 failed | 6 skipped",
  "assertions: 6 | 6 passed") and a separate Windows/Wine smoke run. No
  `TEST_CASE` in the committed `window_test.cpp` matches a `"*SMOKE*"`
  wildcard (grepped every `TEST_CASE(...)` name in the file directly) —
  this specific invocation cannot be reproduced from the committed tree.
  This is not treated as a hygiene problem: the gate's own requirement
  ("a headless smoke run confirms the virtual-joystick path works under
  CI's driver **before** relying on it") does not require the smoke check
  itself to be a shipped, permanent test, and the substance it was meant to
  establish (virtual joysticks work under this project's `xvfb-run`
  driver) is independently and repeatedly confirmed by this review's own
  runs of the real, committed gamepad test suite (27/27 passing in both
  environment scenarios). Flagged here only because the exact transcript
  in the report cannot be independently reproduced as stated.
- The Windows-cross-zig/Wine build and test run (claimed "100% tests
  passed, 0 tests failed out of 11", `rx_platform_tests` "passed under
  real Wine in 2.90s, all 27 test cases") was not reproduced by this review
  — no Wine/Windows-cross toolchain was exercised in this session. The
  `linux-native` preset (both build and full `ctest`) was independently
  and completely verified instead.
- Whether the real gamepad's virtual-button-state repro narrative
  (`SDL_GetJoystickButton` returning stale state pre-fix, `south=0` vs.
  `south=1`) reproduces with the EXACT retry/timing numbers quoted
  (20 retries, 400ms) was not independently re-run as a standalone repro;
  this review instead confirmed the fix is load-bearing via the full
  committed test suite's pass/fail delta (see adjudication above), which is
  a stronger, more direct proof than reproducing the original throwaway
  repro would have been.
