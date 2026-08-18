# Completeness Matrix — Issue #14: Input expansion (mouse capture + gamepad)

## Header

- **Ticket:** #14, "Input expansion: mouse capture + gamepad" (open, labels
  `phase-4`/`stage-2`, board column Todo). Full body (single paragraph, no
  amendment sections present): "**Plan Task 16** (docs/superpowers/plans/
  2026-08-11-phase4-scene-assets.md, Haiku). SDL3 input expansion in
  rx_platform: relative mouse mode + per-frame accumulated deltas, cursor
  show/hide, gamepad hot-plug + stick/trigger polling with deadzones
  (research-verified API names). Steam Deck pad drivability is the
  acceptance bar." Note: the issue's own "Plan Task 16" reference is stale —
  the 2026-08-18 plan renumbering note (plan file line 463) maps old T16
  (input) → current **Task 20**; this matrix targets Task 20 as instructed.
- **Plan task:** Task 20 (seed 6, Haiku), plan file lines 408-411, under
  Stage 2. Bound by the plan's "## Global Constraints" section (lines
  13-21): D5 threading contract (every new public header states thread
  affinity in one line), TDD, established per-directory test conventions,
  attribution ban.
- **Spec decisions checked for applicability:** D15 (culling/layer-masks —
  design spec lines 247-261, `layers: u32`/`cullMask: u32`,
  `channels: u8`) and D19 (scene proxies incl. plain `Camera` value type —
  lines 293-305) read as instructed for forward context on what a
  fly-through camera + HUD layer/channel toggles will eventually need from
  this ticket's button/axis surface (folded into row 7 below); neither
  D-number binds input directly. D24 (memory budget/eviction), D25
  (UploadTicket), D26 (GPU-driven readiness), D27 (main-thread
  pre-resolution) were checked per the brief's binding rule (design spec
  lines 344-439) — **none apply to this ticket**: input touches no VMA
  allocation, no upload path, no draw-list/pipeline resolution. No D24-D27
  rows are added for that reason, not by omission.
- **Sources consulted (version/date):**
  - `gh issue view 14` (2026-08-18 session).
  - Plan file `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md`:
    Task 20 (408-411), Global Constraints (13-21), Task 24 (452-455),
    execution notes/renumbering (459-464), and the gate-framing aside at
    line 177 ("input vs the full gamepad/keyboard/mouse surface real games
    need") — see Conflicts.
  - Design spec `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`:
    D15, D19, D24-D27.
  - `.superpowers/sdd/2026-08-11-phase4-scene-assets/research-p4-present.md`
    (2026-08-11) — read in full; section 2 (SDL3 Input APIs) treated as a
    lead only, every claim re-verified against the vendored header below.
  - `.superpowers/sdd/2026-08-11-phase4-scene-assets/feature-gap-audit.md`
    (2026-08-11) — read in full; no FG item covers input.
  - `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`
    (Approved, 2026-08-09) — read in full; master deferred registry has no
    input-surface entry beyond layer 1's one-line scope statement.
  - In-repo code: `src/rx_platform/include/rx_platform/window.h` (31
    lines), `src/rx_platform/src/window.cpp` (79 lines, `pumpEvents()` at
    52-58 is an empty drain loop), `src/rx_platform/tests/window_test.cpp`
    (29 lines, both tests), `docs/threading.md` (D5 contract),
    `src/rx_rhi_vk/include/rx_rhi_vk/bindless.h`:149-152 and
    `src/rx_rhi_vk/include/rx_rhi_vk/upload.h`:89-93 (thread-affinity
    one-liner convention), `src/rx_core/include/rx_core/debug_checks.h`:84,88
    (`RX_ASSERT_MAIN_THREAD` macro), `MANUAL_VERIFICATION.md` (201 lines).
  - SDL3 vendored headers, **release-3.4.14** (pinned
    `third_party/CMakeLists.txt` line ~99, `RX_SDL3_TAG`; confirmed
    installed version via `SDL_version.h`:47,56,65 → 3.4.14, and
    `SDL_revision.h` → `"SDL-3.4.14-release-3.4.14"`), tree
    `.deps-cache/SDL3-13a71fbe67f94153/include/SDL3/`: `SDL_mouse.h`,
    `SDL_video.h`, `SDL_events.h`, `SDL_gamepad.h`, `SDL_joystick.h`,
    `SDL_hints.h`, `SDL_keyboard.h` — every claim below cites a real
    file:line read in this session.
  - `gh api repos/libsdl-org/SDL/issues/9148` (fetched 2026-08-18) — real
    title/state/body, not a paraphrase.
  - Josh Sutphin, "Doing Thumbstick Dead Zones Right"
    (joshsutphin.com/blog, originally a Gamasutra/Game Developer feature) —
    fetched 2026-08-18, canonical scaled-radial-deadzone precedent.
  - Microsoft, "Getting Started with XInput"
    (github.com/MicrosoftDocs/win32, desktop-src/xinput) — fetched
    2026-08-18, confirms Microsoft's own reference deadzone sample is
    magnitude/radial, and the `XINPUT_GAMEPAD_*_THUMB_DEADZONE` constants
    (7849/8689) that the plan's round "8000" figure approximates.
  - Godot `Input.get_vector` radial-deadzone behavior — secondary
    corroboration via WebSearch snippets only (no direct fetch of Godot
    engine source in this session); flagged accordingly in Verification
    health.

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-4 disposition | Library support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------|-------------------------------|
| 1 | Relative mouse mode + focus-loss edge case | Godot/Unity/UE FPS-camera convention: relative-mode input suspends on focus loss and must be explicitly re-armed on focus regain, because OS input capture is revoked by the window manager regardless of the app's internal flag. | consume-now | **Verified, with a gap in SDL's own docs**: `SDL_SetWindowRelativeMouseMode(SDL_Window*, bool)` (`SDL_mouse.h:476`, doc 450-476) states "While the window has focus and relative mouse mode is enabled, the cursor is hidden, the mouse position is constrained..." — this is conditioned on focus but the doc **never states** the enabled flag auto-clears on focus loss (contrast `SDL_CaptureMouse`, whose doc at `SDL_mouse.h:513` explicitly says "If the window loses focus while capturing, the capture will be disabled automatically" — relative mode carries no equivalent sentence). `SDL_GetWindowRelativeMouseMode` (`SDL_mouse.h:490`) is the query. `SDL_EVENT_WINDOW_FOCUS_LOST`/`SDL_EVENT_WINDOW_FOCUS_GAINED` (`SDL_events.h:150-151`). | Track the app-requested relative-mode state independently of SDL's; on `SDL_EVENT_WINDOW_FOCUS_LOST` treat mouse-delta accumulation as paused (no assumption about what SDL/the OS did to cursor capture); on `SDL_EVENT_WINDOW_FOCUS_GAINED` unconditionally re-call `SDL_SetWindowRelativeMouseMode(window, true)` if the app still wants it enabled — idempotent and safe whether or not SDL had silently cleared it. Device-free test: toggling relative mode true/false under the dummy driver round-trips through `SDL_GetWindowRelativeMouseMode`. One-sentence interaction note: ticket #25 (window/minimize hardening) will hook the same `SDL_EVENT_WINDOW_MINIMIZED`/`FOCUS_LOST` events for swapchain-recreation guards — the two tickets should agree on a single event-dispatch owner rather than each independently re-deriving focus state; this matrix does not specify #25's rows. |
| 2 | Per-frame mouse-delta accumulation | id Software/Source-engine convention: raw per-event deltas summed within a frame boundary, never sampled once via absolute-position diffing (loses sub-poll-interval samples at high mouse report rates). | consume-now | **Verified**: `SDL_MouseMotionEvent` (`SDL_events.h:453-465`) carries `float xrel`/`float yrel` (lines 463-464) — "relative motion since last motion event." SDL3 deliberately widened these from SDL2's `Sint16` to `float` for sub-pixel precision at high poll rates (header's own `CategoryMouse` framing, `SDL_mouse.h:22-55`, corroborates the research doc's claim). `SDL_PollEvent` (`SDL_events.h:1304`) implicitly calls `SDL_PumpEvents` (`SDL_events.h:1100`), documented main-thread-only at line 1093. No SDL3 API coalesces multiple `MOUSE_MOTION` events into one; each physical sample is its own event. | **Concrete algorithm**: maintain `float accumX, accumY` (member state); on every `SDL_EVENT_MOUSE_MOTION` drained inside `pumpEvents()`'s existing full-drain `while (SDL_PollEvent(&event))` loop (`window.cpp:52-58`), do `accumX += event.motion.xrel; accumY += event.motion.yrel`; add a `MouseDelta consumeMouseDelta()` that returns the accumulated float pair and resets both to 0 in the same call (consume-and-reset, never leaks across frames). Correctness rests entirely on the queue being *fully* drained before the caller reads the accumulator — already true of `pumpEvents()`'s existing loop, so no new race is introduced. Device-free test: push N synthetic `SDL_Event`s via `SDL_PushEvent` (a dummy/offscreen driver services this without a real display) with known `xrel`/`yrel`, call `pumpEvents()`, assert `consumeMouseDelta()` equals the exact float sum, then assert a second call with no new events returns `{0,0}`. |
| 3 | Cursor show/hide/confine | Any windowed-game FPS/RTS convention: hide+lock during gameplay look-mode, show+free during menus, optionally confine-without-hiding for windowed-mode edge-scroll UIs. | consume-now | **Verified**: `SDL_ShowCursor()` (`SDL_mouse.h:775`), `SDL_HideCursor()` (`SDL_mouse.h:790`), `SDL_CursorVisible()` (`SDL_mouse.h:805`) — global, not per-window. Confine-without-hide/relative-mode is a **separate** primitive from relative mode: `SDL_SetWindowMouseGrab`/`SDL_GetWindowMouseGrab` (`SDL_video.h:2600`,`2631`) locks the cursor to the window without altering cursor visibility or motion semantics; `SDL_SetWindowMouseRect`/`SDL_GetWindowMouseRect` (`SDL_video.h:2667`,`2684`) confines to an arbitrary sub-rect; `SDL_SetWindowKeyboardGrab` (`SDL_video.h:2580`) is the sibling for keyboard, not required by this ticket. | Expose `setCursorVisible(bool)`, `setCursorConfined(bool)` wrapping the four functions above. Device-free test: toggling each under the dummy driver round-trips through its matching getter (`SDL_CursorVisible`, `SDL_GetWindowMouseGrab`). Visual confirmation (does the OS actually hide/confine the pointer) is not automatable — add a MANUAL_VERIFICATION row modeled on this file's existing per-sample structure (see row 8/verification-health note on sample 09 rows). |
| 4 | Gamepad hot-plug lifecycle | bgfx/SDL_GameControllerDB-era convention: an owned-handle table keyed by a stable device ID, opened on connect, closed on disconnect, never left dangling across ID reuse. | consume-now | **Verified**: `SDL_EVENT_GAMEPAD_ADDED`/`REMOVED`/`REMAPPED` (`SDL_events.h:205-207`) deliver `SDL_GamepadDeviceEvent` (`SDL_events.h:671-677`) whose `which` field is `SDL_JoystickID` (line 676). `SDL_OpenGamepad(SDL_JoystickID)` → `SDL_Gamepad*` (`SDL_gamepad.h:753`); `SDL_CloseGamepad(SDL_Gamepad*)` (`SDL_gamepad.h:1616`), documented safe from any thread. A stale handle after REMOVED is **not** unsafe to query: `SDL_GamepadConnected(SDL_Gamepad*)` (`SDL_gamepad.h:1067`) returns `false` rather than UB/crash — but SDL never guarantees `SDL_JoystickID` non-reuse across replug cycles, so an app that doesn't close-then-erase promptly risks its own map colliding a stale entry with a newly-reused ID. **CI-testable without hardware**: `SDL_AttachVirtualJoystick`/`SDL_DetachVirtualJoystick` (`SDL_joystick.h:539`,`555`) plus `SDL_SetJoystickVirtualAxis`/`SetJoystickVirtualButton` (`SDL_joystick.h:598`,`653`) create a fully driveable virtual device; `SDL_VirtualJoystickDesc.button_mask`/`axis_mask` (`SDL_joystick.h:479-482`) are explicitly documented as using the `SDL_GAMEPAD_BUTTON_*`/`SDL_GAMEPAD_AXIS_*` enum values directly, confirming a virtual joystick surfaces as a real `SDL_Gamepad` and fires `GAMEPAD_ADDED`. | Owned `std::unordered_map<SDL_JoystickID, SDL_Gamepad*>`: on `GAMEPAD_ADDED`, `SDL_OpenGamepad` then insert; on `GAMEPAD_REMOVED`, `SDL_CloseGamepad` **then** erase, synchronously in the same event-handling step (never deferred to next frame). Automated test (not manual-only, contra the plan's literal framing — see Conflicts): attach a virtual gamepad via `SDL_AttachVirtualJoystick`, assert the map gains an entry and the device is queryable; detach, assert the map loses the entry and no handle leak (e.g. via a close-call counter); attach-detach-attach in rapid succession asserts no stale-ID collision. Real-hardware plug/unplug still gets a MANUAL_VERIFICATION row for the physical experience. |
| 5 | Deadzone math: radial vs per-axis | **Resolved in favor of scaled-radial.** Microsoft's own XInput reference sample (`getting-started-with-xinput.md`, fetched) computes `magnitude = sqrt(LX*LX + LY*LY)` and only zeroes/rescales when `magnitude > deadzone` — a **radial**, not per-axis, check; `XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE`=7849, `_RIGHT_`=8689 (both close to the plan's round "8000"). Josh Sutphin, "Doing Thumbstick Dead Zones Right" (industry-canonical, originally Gamasutra/Game Developer) names three methods and recommends the third: **scaled radial** `stickInput.normalized * ((stickInput.magnitude - deadzone) / (1 - deadzone))`, explicitly because per-axis clamping produces a square dead zone that causes input to "snap" to cardinal directions during a sweeping stick motion. Godot's `Input.get_vector` follows the same scaled-radial shape (secondary corroboration, not directly fetched — see Verification health). | consume-now | **UNVERIFIED that SDL applies any deadzone itself** — confirmed by absence: `SDL_GetGamepadAxis(SDL_Gamepad*, SDL_GamepadAxis)` (`SDL_gamepad.h:1274`) takes no deadzone parameter and its doc (1249-1273) describes only the raw -32768..32767 range with "Zero is also a valid value in normal operation" — deadzone is entirely the caller's responsibility, matching the plan's own framing ("with deadzone"[R:present]). | **Explicit formula** (resolving the plan's `8000/32768` ambiguity): treat the stick as `Vector2(LEFTX, LEFTY)` (and separately `RIGHTX/RIGHTY`), compute `float mag = length(stick) / 32768.0f`; if `mag < deadzone` (deadzone = 8000/32768 ≈ 0.244) output `(0,0)`; else output `normalize(stick) * ((mag - deadzone) / (1 - deadzone))`, clamped to `[0,1]` magnitude. This is a **per-stick 2D radial check**, not two independent per-axis clamps. Device-free test: table-driven cases (below-threshold on-axis point → zero; above-threshold diagonal point that would fail a per-axis-only check at the same magnitude → non-zero; a point exactly on the per-axis boundary but inside the true radius, e.g. `(8500, 8500)` whose magnitude ≈ 12021 > deadzone but whose largest single axis, 8500, only barely clears 8000 — the radial formula must not zero this differently than a naive per-axis test would, proving the two are NOT equivalent) — exercised via `SDL_SetJoystickVirtualAxis` end-to-end, or as pure math unit tests matching `window_test.cpp`'s device-free convention if the math is factored out testably. |
| 6 | Trigger axes ranges | Standard console/PC convention (Xbox/PlayStation/Steam Input): triggers are unsigned 0..max, analog "half-pull" states matter for e.g. throttle, but resting noise near zero is common on worn/analog-drift hardware. | consume-now | **Verified**: `SDL_GetGamepadAxis` doc (`SDL_gamepad.h:1256-1258`): "Triggers range from 0 when released to 32767 when fully pressed, and never return a negative value... differs from... `SDL_GetJoystickAxis()`, which normally uses the full range." Confirmed no negative range exists for `LEFT_TRIGGER`/`RIGHT_TRIGGER` (`SDL_gamepad.h:229-230`). | A small **1D** deadzone (not the 2D radial formula from row 5 — triggers have no second axis to pair with) should apply: `float t = max(raw/32767.0f - deadzone, 0) / (1 - deadzone)`. Microsoft's own `XINPUT_GAMEPAD_TRIGGER_THRESHOLD`=30 (of 0..255, i.e. ≈3855/32767 scaled) corroborates that even the reference platform treats near-zero trigger noise as needing a floor, though its value is a binary "pressed" threshold rather than an analog rescale — this ticket's triggers stay analog (fed to e.g. a fly-through speed multiplier in sample 09), so a small analog deadzone (proposed default ≈1000/32767, tunable) is the correct shape, not a boolean threshold. Device-free test mirrors row 5's table-driven approach for the 1D case. |
| 7 | Button mapping surface | Sample 09 (plan Task 24, lines 452-455) needs: fly-through movement (stick-driven, rows 5/6), plus HUD toggles for vsync, layer-mask (hide/show instance groups), and light-channel demo toggle — none of which are movement, all of which need discrete buttons. | consume-now | **Verified — and the plan's literal text does not match the real enum.** Full `SDL_GamepadButton` enum (`SDL_gamepad.h:154-182`): `SOUTH, EAST, WEST, NORTH, BACK, GUIDE, START, LEFT_STICK, RIGHT_STICK, LEFT_SHOULDER, RIGHT_SHOULDER, DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT, MISC1, RIGHT_PADDLE1, LEFT_PADDLE1, RIGHT_PADDLE2, LEFT_PADDLE2, TOUCHPAD, MISC2..MISC6, COUNT`. **There is no `SDL_GAMEPAD_BUTTON_A`/`_B` in this SDL3 version** — grepped both `SDL_gamepad.h` and `SDL_oldnames.h`, zero hits. `SOUTH`/`EAST`/`NORTH`/`WEST` are Xbox-face-button-labeled only in doc comments (line 155-158: "e.g. Xbox A button" etc.), not in the identifier. See Conflicts. | `GamepadState` must expose at minimum the D-pad (`DPAD_UP/DOWN/LEFT/RIGHT`), the four face buttons (`SOUTH/EAST/WEST/NORTH`), both shoulders (`LEFT_SHOULDER/RIGHT_SHOULDER`), and `START` — a bitmask or `bool` struct, not "two buttons." Concrete sample-09 mapping proposal: `SOUTH` = confirm/no-op-in-demo, `START` = toggle HUD, `NORTH`/`EAST` = toggle vsync / cycle light-channel, `LEFT_SHOULDER`/`RIGHT_SHOULDER` = cycle layer-mask groups, `DPAD` = reserved. The exact mapping is Task 24's call; this ticket's acceptance criterion is that the **enum surface** (not just 2 buttons) is exposed so Task 24 is not blocked re-opening this ticket. Device-free test via `SDL_SetJoystickVirtualButton` per button, asserting `GamepadState` reflects each independently. |
| 8 | Rumble/haptics + touchpad/gyro (Deck-relevant) | DualShock/DualSense/Switch-Pro conventions treat rumble as baseline, touchpad-as-pointer and gyro-as-motion-control as enhancement layers with graceful absence. | **rumble: N/A-Phase-4** (retrofit-safe — see reasoning). **touchpad: N/A-Phase-4.** **gyro: log-don't-drop.** | **Verified APIs exist**: `SDL_RumbleGamepad`/`SDL_RumbleGamepadTriggers` (`SDL_gamepad.h:1535`,`1565`); `SDL_GetNumGamepadTouchpads`/`GetNumGamepadTouchpadFingers`/`GetGamepadTouchpadFinger` (`SDL_gamepad.h:1388`,`1405`,`1429`); `SDL_GamepadHasSensor`/`SetGamepadSensorEnabled`/`GamepadSensorEnabled`/`GetGamepadSensorData` (`SDL_gamepad.h:1446`,`1464`,`1479`,`1511`). **Deck gyro reliability is NOT verified working**: `gh issue #9148` (fetched, title "SDL3, Steam Deck is reported as Steam Virtual Gamepad", state **CLOSED**, created 2024-02-25, updated 2025-11-29, 19 comments — only the body was read, not the full 19-comment resolution thread, see Verification health) — body states "gyro and back paddles are not detected" even with `SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK` (`SDL_hints.h:1819`, doc 1805-1818), tested across multiple physical Decks by multiple users; body also states "**Rumble is working**." | **Retrofit-economics reasoning** (per brief's N/A justification requirement): all three are per-call query/action methods (`SDL_RumbleGamepad(handle, ...)`, `SDL_GamepadHasSensor(handle, type)`) that never require a field to live inside a `GamepadState` snapshot struct — adding them later is a new method on the existing owned-handle map (row 4), not a shape change at any existing call site, so deferring is genuinely retrofit-safe. **Rumble**: not required by Task 20's literal text or Task 24's HUD-toggle/fly-through needs; defer. **Touchpad**: the Deck's own trackpads are not confirmed to surface through `SDL_GetNumGamepadTouchpads` on the "Steam Virtual Gamepad" identity (UNVERIFIED — no test hardware, no header-level guarantee); no named consumer in the plan; defer. **Gyro**: given issue #9148's finding that it doesn't reliably attach on the actual floor hardware even with the documented hint, and given the issue's resolution status could not be fully confirmed from the body alone, consuming gyro now would build a feature that silently does nothing on the Deck — **log-don't-drop** is the correct disposition: query `SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO)` once per gamepad-open and log the boolean result, so a developer debugging "gyro doesn't work on my Deck" gets a diagnosable log line instead of silence, without shipping a consumption path that can't be proven to work on the hardware floor. |
| 9 | Multiple simultaneous gamepads policy | Local-multiplayer/co-op convention (any engine supporting 2+ pads) keys all gamepad state by a stable per-device ID, never assumes exactly one. | Task-20 scope: **consume-now** as single-active-by-design (documented, not an oversight). Multi-pad public surface: **preserve-later.** | N/A — this is a design-surface question, not a library-support question; the underlying `SDL_JoystickID`-keyed map (row 4) already supports N simultaneous devices at the SDL layer with zero additional library work. | The plan's literal `GamepadState poll()` (Task 20 line 410, no parameter) implies exactly one active gamepad — adequate for Task 24's single-player fly-through demo (plan Task 24, no split-screen/co-op mentioned). Acceptance: `poll()` is documented as selecting a single deterministic "active" gamepad from the internally-tracked `unordered_map<SDL_JoystickID, SDL_Gamepad*>` (row 4) — proposed rule: lowest currently-connected `SDL_JoystickID`, stable across a frame, re-selected only when the active pad disconnects — and this selection rule is unit-testable (attach two virtual gamepads, detach the lower-ID one, assert the higher-ID one becomes active). Because the map is already keyed by `JoystickID` regardless of the single-pad public surface, a later `poll(SDL_JoystickID)` or enumeration accessor is additive, not a retrofit — satisfying the brief's N/A-justification bar for calling this preserve-later rather than blocking on it now. |
| 10 | Steam Input/Deck gamepad quirks | Valve's Steam Input translation layer is the de-facto standard for Deck controller access; SDL3's own HIDAPI Deck driver (`hidapi_steamdeck.c`, named in issue #9148's own body) is the alternative path. | log-don't-drop | **Verified hint exists**: `SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK` (`SDL_hints.h:1819`, doc 1805-1818) — "controlling whether the HIDAPI driver for the Steam Deck builtin controller should be used," values `"0"`/`"1"`, defaults to `SDL_HINT_JOYSTICK_HIDAPI`'s value, must be set before joystick/gamepad init. Issue #9148's own **title** ("Steam Deck is reported as Steam Virtual Gamepad") confirms the device DOES enumerate as a working `SDL_Gamepad` (basic buttons/sticks/triggers functional — this is a name/identity quirk plus the gyro/paddle gap from row 8, not a total non-function report). | Log the effective device name/type (`SDL_GetGamepadName`-equivalent) and whether it identifies as "Steam Virtual Gamepad" at gamepad-open time, so a Deck-specific support issue (missing gyro, unexpected button mapping) is diagnosable from a log capture rather than requiring the reporter to already know about issue #9148. No consumption of Steam-Input-specific data is proposed — this is purely the log-don't-drop diagnostic floor. |
| 11 | Thread affinity of the new input API | D5 threading contract (this project's own established pattern — `BindlessTable`/`Uploader` headers each carry a one-line "Thread-affinity (D5, Phase 4): ... are main-thread-only -- see docs/threading.md." comment, `bindless.h:149-152`, `upload.h:89-93`, backed by a `RX_ASSERT_MAIN_THREAD` dev-time guard, `debug_checks.h:84,88`). | consume-now | **Verified from the SDL3 header itself**: `SDL_PumpEvents()` (`SDL_events.h:1100`) doc states "\threadsafety This function should only be called on the main thread." at line 1093; `SDL_PollEvent` (`SDL_events.h:1304`) implicitly calls it. `window.cpp:52-58`'s existing `pumpEvents()` already only makes sense called from the loop-owning thread (today undocumented and unguarded). | New public surface (`pumpEvents()` — already public, `consumeMouseDelta()`, `poll()`/`GamepadState`, cursor/relative-mode setters) gets the exact same one-line convention as `bindless.h`/`upload.h`: "Thread-affinity (D5, Phase 4): `pumpEvents()`/`consumeMouseDelta()`/`poll()` are main-thread-only -- see docs/threading.md." plus an `RX_ASSERT_MAIN_THREAD` guard on each, mirroring the existing pattern exactly. Device-free test: call one of the guarded methods from a `rx::task::Scheduler` worker (same pattern used to test the existing guards on `BindlessTable`/`Uploader`, if such a test exists there — this ticket's test should be symmetric with whatever that convention already is) and assert the loud-failure path fires rather than silently succeeding. |

---

## Conflicts

Per the brief, these are **not resolved here** — quoted for the coordinator.

1. **Button-name mismatch.** Task 20 (plan line 410) and issue #14's body
   both say the surface should expose "A/B buttons." The actual SDL3
   3.4.14 `SDL_GamepadButton` enum (`SDL_gamepad.h:154-182`) has no `_A`/`_B`
   member — the closest is `SDL_GAMEPAD_BUTTON_SOUTH`/`_EAST`, which the
   header's own doc comments gloss as "e.g. Xbox A button" / "e.g. Xbox B
   button" (lines 155-156) but does not name that way in code. A literal
   reading of the ticket text ("A/B buttons") does not compile against this
   pinned SDL3 version.

2. **Deadzone formula ambiguity.** Task 20 (plan line 410) reads "8000/32768
   deadzone" with no further qualification, which a literal implementation
   could read as two independent per-axis clamps (`if (abs(axisValue) <
   8000) axisValue = 0`, applied to X and Y separately). First-tier
   precedent (Microsoft's own reference XInput sample, fetched; Josh
   Sutphin's "Doing Thumbstick Dead Zones Right," fetched) is unambiguous
   that the correct treatment is a **radial** (2D-magnitude) check with
   scaled rescaling, and that a naive per-axis reading is a well-documented
   anti-pattern (causes diagonal-input "snapping" to cardinal directions).
   The plan text does not itself specify which; row 5 above proposes the
   resolution.

3. **Test-coverage assumption understates what's automatable.** Task 20
   (plan line 411) states: "tests where device-free (deadzone math), manual
   rows for the rest." This session verified that SDL3 3.4.14 ships
   `SDL_AttachVirtualJoystick`/`SDL_SetJoystickVirtualAxis`/
   `SetJoystickVirtualButton`/`SDL_DetachVirtualJoystick`
   (`SDL_joystick.h:539,555,598,653`), which drive a real `SDL_Gamepad`
   through the identical `SDL_EVENT_GAMEPAD_ADDED`/axis/button code paths a
   physical pad would — meaning hot-plug lifecycle (row 4), axis reads (row
   5/6), and button reads (row 7) are automatable in CI without hardware,
   not just deadzone math. "Manual rows for the rest" as literally written
   would leave automatable coverage on the table.

4. **Plan's own stated evaluation bar vs. Task 20's actual scope.** Plan
   line 177, describing how this very gate should measure the input
   ticket, says: "input vs the full gamepad/keyboard/mouse surface real
   games need" — explicitly naming keyboard. Task 20's actual file/interface
   list (plan lines 410-411) and Task 24's fly-through camera description
   (plan line 454, "fly-through camera (mouse capture + gamepad, D16
   input)") **both omit keyboard entirely** — no `SDL_GetKeyboardState`,
   no `SDL_EVENT_KEY_DOWN`/`UP` (confirmed present in SDL3:
   `SDL_keyboard.h:159`, `SDL_events.h:170-171`, respectively), no WASD
   anywhere in either task's text. A PC fly-through camera that is
   mouse-look + gamepad-only, with no keyboard movement, is an unusual
   control scheme for a desktop demo audience (WASD+mouse-look is the
   default PC expectation; gamepad is typically the secondary/console-style
   option). This session found no keyboard mention anywhere else in the
   Phase 4 planning universe (grep across specs/plans/SDD notes, one hit
   only at plan line 177 itself, and it is a values statement, not a task
   assignment).

---

## New gaps

Checked against the master registry deferred list
(`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`) and
`feature-gap-audit.md` (FG1-FG12) before claiming novelty; neither artifact
names input surfaces at all (the registry's only input mention is layer 1's
one-line scope statement "Window, input, events, threads"; the FG audit's
scope is lighting/host-ops/display-output and explicitly does not touch
input). The following are genuinely absent from the entire planning
universe, not just under-specified in Task 20:

- **Keyboard input surface.** Beyond the plan-line-177 aside quoted in
  Conflict #4, no artifact anywhere plans `SDL_GetKeyboardState`/key-event
  handling for RendererX samples or the eventual host-facing API. Given
  Task 24's fly-through camera is the one place a keyboard would matter in
  Phase 4, and it's currently scoped mouse+gamepad only, this is either a
  deliberate (but unrecorded) product choice or a genuine hole. Proposed
  phase fit: fold a minimal `KeyboardState` (a `bool[SDL_SCANCODE_COUNT]`
  wrapper over `SDL_GetKeyboardState`, mirroring the mouse/gamepad
  device-free test shape) into this same ticket if the coordinator confirms
  Task 24 needs WASD; otherwise record the mouse+gamepad-only scheme as an
  explicit, deliberate decision so it stops reading as an oversight.
- **Multi-gamepad / local-multiplayer public surface.** Addressed at
  Phase-4 scope by row 9's design (single-active `poll()` over a
  JoystickID-keyed internal map), but no SDK-phase artifact anywhere
  commits to eventually exposing `poll(JoystickID)` or an enumeration API
  for host engines wanting local co-op/split-input. Proposed phase fit: SDK
  phase, alongside the other C-ABI surface commitments already registered
  (multi-language bindings, scheduler sharing) — cheap given row 9's
  retrofit-safe internal design.

---

## Verification health

- **First-hand, directly read this session:** every SDL3 API claim in the
  matrix (file:line cited against the pinned `.deps-cache` tree, version
  cross-confirmed via `SDL_version.h`/`SDL_revision.h`); the full issue
  #14 body via `gh issue view`/`gh api`; the full Task 20/Task 24/Global
  Constraints plan text; D15/D19/D24-D27 design-spec sections;
  `feature-gap-audit.md` and the master registry doc in full;
  `window.h`/`window.cpp`/`window_test.cpp`/`MANUAL_VERIFICATION.md` in
  full; `bindless.h`/`upload.h` thread-affinity comment text;
  `debug_checks.h`'s `RX_ASSERT_MAIN_THREAD` macro; issue #9148's real
  title/state/body via `gh api` (not a paraphrase of the research doc).
- **Fetched this session (secondary but direct):** Josh Sutphin's
  deadzone-formula article (full fetch, formula quoted verbatim);
  Microsoft's XInput getting-started doc (full fetch, sample code quoted
  verbatim, confirming radial not per-axis).
- **Inferred / not independently confirmed:**
  - Whether `SDL_SetWindowRelativeMouseMode`'s enabled-flag genuinely
    survives or clears across a real focus-loss event on Linux/X11/Wayland
    or Windows — the header text was read closely for what it does and
    does NOT say (row 1), but no runtime experiment was performed in this
    session to observe actual behavior; row 1's proposed acceptance
    criterion (unconditional re-assert on focus-regain) is deliberately
    written to be correct regardless of the answer.
  - Whether a Steam Deck's own trackpads surface through
    `SDL_GetNumGamepadTouchpads` when the device identifies as "Steam
    Virtual Gamepad" — no test hardware available, no header-level
    guarantee found; flagged `UNVERIFIED` in row 8 rather than assumed.
  - Issue #9148's **current resolution status**: confirmed CLOSED
    (updated 2025-11-29) with 19 comments, but only the original body was
    read per the brief's scope ("confirm its real title/state/content");
    whether a fix landed between the 2024-02-25 report and the 2025-11-29
    close, or whether it was closed as stale/superseded, was not
    determined — this matters because it bears on whether gyro's
    log-don't-drop disposition (row 8/10) should eventually be revisited
    upward to consume-now.
  - Whether `SDL_AttachVirtualJoystick`-based tests actually run headless
    under this project's CI dummy/offscreen driver setup (SDL's joystick
    subsystem doesn't need a display, but this was not empirically
    exercised in this session — a build/run smoke test would confirm it
    before the coordinator relies on it as CI-automatable).
  - Godot's `Input.get_vector` radial-deadzone behavior (row 5) is
    secondary corroboration via WebSearch result snippets only — Godot's
    actual engine source was not fetched/read directly in this session,
    unlike the Sutphin article and the Microsoft XInput doc which were.
  - `docs/threading.md`'s D5 contract text was grepped for structure and
    line ranges rather than read end-to-end; the sections cited (10-119)
    were read for the specific main-thread-only/guard-macro pattern this
    ticket needs, not audited as a whole document.
- **No dead links encountered**: all fetched URLs (Sutphin article,
  Microsoft XInput doc, GitHub issue #9148 via `gh api`) resolved and
  returned real content on the first attempt.
