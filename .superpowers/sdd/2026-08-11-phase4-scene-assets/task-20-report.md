# Task 20 report — Input expansion (mouse capture + gamepad, card #14)

Base commit: `736ed84`. Implementer: this session. Requirements read in
order per the brief: `task-20-brief.md` (task text + BINDING gate block),
`gate/matrix-issue14-input.md`, `gate/rulings-2026-08-18.md` §#14, `gh issue
view 14`. Order of authority followed: rulings > spec > matrix > ticket.

## Files changed (mine only)

- `src/rx_platform/include/rx_platform/input.h` (new) — dependency-free
  value types (`Vec2f`, `MouseDelta`, `GamepadButtons`, `GamepadState`) +
  deadzone free functions (`applyStickDeadzone`/`applyTriggerDeadzone`).
- `src/rx_platform/src/input.cpp` (new) — deadzone math implementation.
- `src/rx_platform/include/rx_platform/window.h` — new `Window` public
  surface (relative mouse mode, mouse delta, cursor, gamepad, keyboard) +
  new private members.
- `src/rx_platform/src/window.cpp` — implementation, `pumpEvents()`
  additions (FOCUS_LOST/GAINED, MOUSE_MOTION, GAMEPAD_ADDED/REMOVED),
  `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` fix (see "Production bug
  found and fixed" below).
- `src/rx_platform/CMakeLists.txt` — added `src/input.cpp`.
- `src/rx_platform/tests/window_test.cpp` — full new test suite (deadzone
  math, mouse/cursor, gamepad, keyboard, thread-affinity guard).
- `docs/threading.md` — new `rx::platform::Window` entry in the
  Main-thread-only registry.
- `MANUAL_VERIFICATION.md` — new "rx_platform input surface" section
  (honest, unchecked rows for what genuinely cannot be automated).

`.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md` shows
modified in `git status` but is **not mine** (a concurrent coordinator
ledger entry) — excluded from my commit via explicit pathspec.

## Design summary

- **Mouse**: `setRelativeMouseMode(bool)` tracks the app's own intent
  (`relativeModeWanted_`) independently of SDL's query, per gate ruling
  #14/matrix row 1. `pumpEvents()` handles `SDL_EVENT_WINDOW_FOCUS_LOST`
  (pauses delta accumulation, no assumption about SDL's own capture state)
  and `SDL_EVENT_WINDOW_FOCUS_GAINED` (unconditionally re-issues
  `SDL_SetWindowRelativeMouseMode(window, true)` if still wanted).
  `consumeMouseDelta()` is consume-and-reset over `accumMouseDeltaX_/Y_`,
  summed from `SDL_EVENT_MOUSE_MOTION.xrel/yrel` inside `pumpEvents()`'s
  existing full-drain loop, filtered to this window's own ID.
- **Cursor**: `setCursorVisible`/`cursorVisible` wrap
  `SDL_ShowCursor`/`HideCursor`/`CursorVisible`; `setCursorConfined`/
  `cursorConfined` wrap `SDL_SetWindowMouseGrab`/`GetWindowMouseGrab` — a
  distinct primitive from relative mode, per matrix row 3.
- **Gamepad**: `std::unordered_map<SDL_JoystickID, SDL_Gamepad*> gamepads_`
  owned by `Window`; `GAMEPAD_ADDED` opens+inserts (+ gyro log-don't-drop:
  `SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO)` + device name logged at
  open time); `GAMEPAD_REMOVED` closes-then-erases synchronously in the
  same event-handling step. `poll()` selects the lowest connected
  `SDL_JoystickID` (matrix row 9's single-active rule) and returns a
  `GamepadState` with deadzone-applied sticks/triggers and the full
  discrete button surface (D-pad ×4, SOUTH/EAST/WEST/NORTH, both
  shoulders, START — no `_A`/`_B`, per the naming correction).
- **Deadzone math** (`input.cpp`, dependency-free): `applyStickDeadzone`
  is 2D scaled-radial (`mag = |stick|/32768`; below threshold → `{0,0}`;
  else `normalize(stick) * ((mag-deadzone)/(1-deadzone))`, magnitude
  clamped to 1) — never two independent per-axis clamps.
  `applyTriggerDeadzone` is the 1D analogue over `[0,32767]`.
- **Keyboard**: `isKeyDown(SDL_Scancode)` wraps `SDL_GetKeyboardState`,
  bounds-checked against SDL's own reported `numkeys` (not a hardcoded
  constant) — an out-of-range scancode returns `false`, not UB.
- **Thread-affinity**: `pumpEvents()` (pre-existing, now enforced) +
  every new public method (`setRelativeMouseMode`/
  `relativeMouseModeWanted`/`consumeMouseDelta`/`setCursorVisible`/
  `cursorVisible`/`setCursorConfined`/`cursorConfined`/`poll`/
  `isKeyDown`) carry `RX_ASSERT_MAIN_THREAD`, matching the whole-class
  posture `GeometryPool`/`Uploader` already established (ruling row 11
  named `pumpEvents()`/`consumeMouseDelta()`/`poll()`/the cursor and
  relative-mode setters explicitly; I extended the same guard to the
  getters and `isKeyDown()` too, for one predictable contract across the
  class — several of these are individually "safe from any thread" per
  SDL's own doc, but this project's own D5 policy scopes the whole class
  uniformly, same reasoning as `Uploader::isComplete()`/`wait()`).
  `docs/threading.md` updated with the new registry entry.

## Production bug found and fixed (not just a test artifact)

While building the virtual-joystick test suite I hit a real, 100%-
reproducible failure: `SDL_GetGamepadButton`/`SDL_GetGamepadAxis` (and
even the lower-level `SDL_GetJoystickButton`) never reflected
`SDL_SetJoystickVirtualButton`/`Axis`-driven state changes **whenever a
real `SDL_Window` existed in the process** — reproduced with a from-
scratch standalone repro isolating every other variable (video-subsystem-
only-no-window: correct; window without the Vulkan flag: still wrong;
20×20ms pump+delay retries: still wrong). Root cause, found via
`SDL_hints.h`: `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` **defaults to
`"0"`** — SDL disables joystick/gamepad input processing while the app is
"in the background" (no window holding OS input focus), and every window
this test suite (and every headless CI run) creates is `visible=false`,
i.e. permanently unfocused.

This is not a test-only problem: a real game window that ever loses OS
focus — trivially true under Gamescope/Steam's own overlay on the actual
Steam Deck, the ticket's stated acceptance bar — would go **silently
gamepad-dead** at SDL's own default. `Window::create()` now calls
`SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1")`
unconditionally before `SDL_INIT_GAMEPAD` comes up (`window.cpp`, with the
full repro narrative in the code comment). Verified fix: before, an
isolated repro with a real hidden window showed `south=0` after driving it
`true` and pumping (20 retries, 400ms); after, `south=1` on the very first
read.

## Gate-mandated virtual-joystick smoke check

Per the brief: confirm `SDL_AttachVirtualJoystick` works under this
project's CI-representative headless driver **before** relying on it for
load-bearing tests.

```
$ xvfb-run -a ./build/linux-native/src/rx_platform/rx_platform_tests -tc="*SMOKE*" -s
...
[doctest] test cases: 1 | 1 passed | 0 failed | 6 skipped
[doctest] assertions: 6 | 6 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Result: **works cleanly**, both under `xvfb-run -a` (native Linux) and
under `xvfb-run -a wine ...rx_platform_tests.exe` (Windows-cross target) —
confirmed separately, full test-case output in both cases. Matrix
Conflict #3's CI-automation claim holds; no fallback to manual-only rows
was needed. A second smoke check (real SDL calls, no `Window`) confirmed
the less-obvious fact that a virtual joystick's raw axis/button INDEX
equals the `SDL_GamepadAxis`/`SDL_GamepadButton` enum value directly for a
`SDL_JOYSTICK_TYPE_GAMEPAD`-typed device, **and** that
`SDL_SetJoystickVirtualAxis`/`Button` values are not visible until the
next `SDL_PumpEvents()`/`SDL_UpdateJoysticks()` (documented, but easy to
miss) — both load-bearing findings for the test design, both folded into
`VirtualGamepad`'s own comment in `window_test.cpp`.

## Environmental confound found and neutralized: a real gamepad

This dev machine has a **physically-connected USB Xbox-compatible
controller** (`/proc/bus/input/devices`: `"Generic X-Box pad"`,
`usb-0000:00:14.0-10`, VID `0x3537` PID `0x1012` — confirmed via sysfs,
not assumed). Since `poll()`'s single-active rule always selects the
lowest connected `SDL_JoystickID`, and SDL's instance-ID counter is
monotonic (a real device discovered at subsystem-init time always beats a
virtual pad attached later), this device wins selection over every test's
own virtual pad, starving the poll()-mediated assertions.

Tests are written to be **correct regardless of this**, not to assume a
clean environment: `foreignGamepadIds()` (top of `window_test.cpp`) checks
`SDL_GetGamepads()` for anything not owned by the current test and, when
present, logs an honest `MESSAGE` and skips only the specific assertions
that genuinely cannot be observed with a foreign device present — mirrors
this file's own pre-existing WM/driver-conditional convention (Task 17's
fullscreen tests). Confirmed this is not theoretical: a fix-round-1-style
re-verification with the real device temporarily neutralized via
`SDL_JOYSTICK_BLACKLIST_DEVICES=0x3537/0x1012` (an SDL hint, VID/PID
supplied as an env var for verification only — never hardcoded into the
shipped test file, since it's specific to this one machine) shows **every
assertion runs for real and passes**: 312/312 vs. 123/123 with the device
present (the smaller count reflects the same test cases correctly taking
their honest-skip branches). Both scenarios are 27/27 test cases, 0
failed.

## Per-criterion proof (matrix rows / gate ruling #14)

**Row 1 (relative mode + focus-loss).** `setRelativeMouseMode`/
`relativeMouseModeWanted` track app intent independent of SDL's query;
`pumpEvents()`'s FOCUS_LOST/GAINED handling per the design above.
Device-free test (`window_test.cpp`, "FOCUS_LOST pauses...") drives the
full pause→resume cycle via `SDL_PushEvent`; a separate test round-trips
`relativeMouseModeWanted()`/`SDL_GetWindowRelativeMouseMode()` with an
honest skip when the driver doesn't actually grant it (same pattern as
Task 17's fullscreen test). Confirmed load-bearing (revert evidence below).

**Row 2 (mouse-delta accumulation).** Exact algorithm from the matrix:
`accumX/Y += event.motion.xrel/yrel` inside the existing full-drain loop;
`consumeMouseDelta()` returns-and-resets. Device-free test pushes 3
synthetic `MOUSE_MOTION` events with known `xrel/yrel`, asserts the exact
float sum, then asserts a second call returns `{0,0}`; a second test
proves cross-window isolation. Confirmed load-bearing.

**Row 3 (cursor show/hide/confine).** `setCursorVisible`/`cursorVisible`
round-trip `SDL_CursorVisible()`; `setCursorConfined`/`cursorConfined`
round-trip `SDL_GetWindowMouseGrab()` with the same "request succeeded but
the driver didn't actually grant it" gating Task 17's fullscreen test
established (found live: `SDL_SetWindowMouseGrab(true)` reports success
under this dev machine's own `xvfb-run` session without actually granting
the grab). Visual confirmation is MANUAL_VERIFICATION-only (new section,
unchecked, honest).

**Row 4 (gamepad hot-plug lifecycle).** Owned
`unordered_map<SDL_JoystickID, SDL_Gamepad*>`; open-on-ADDED,
close-then-erase-on-REMOVED, synchronously. Automated via
`SDL_AttachVirtualJoystick` (hot-plug lifecycle test + rapid
attach-detach-attach test, both environment-robust per above). Confirmed
load-bearing (revert evidence below).

**Row 5 (scaled-radial deadzone, incl. mandatory (8500,8500) case).**
`applyStickDeadzone` per the matrix's exact formula. Pure-math tests:
below-threshold-zero, centered-stick-no-NaN, the diagonal-snap
discrimination case ((6000,6000): magnitude clears the radial deadzone
but a naive per-axis clamp would zero both axes), and the **mandatory**
(8500,8500) case asserting the exact scaled output value
(`(0.11480, 0.11480)`, magnitude `0.16235`) against a naive-unscaled-
per-axis-clamp comparator computed inline (`(0.25940, 0.25940)`) — proving
a bare zero/non-zero check cannot discriminate the two implementations at
this point, only the exact value can (per the gate hardening block's own
framing). Also exercised end-to-end via a real virtual joystick through
`Window::poll()` (environment-conditional). Confirmed load-bearing
(revert evidence below).

**Row 6 (1D trigger deadzone).** `applyTriggerDeadzone` per the matrix
formula, default ~1000/32767. Pure-math test: zero below threshold, 1.0
at max, exact mid-range rescale value. End-to-end virtual-joystick test
additionally found and documented a real SDL gotcha: a virtual trigger's
REST value at the raw joystick-axis level is `SDL_JOYSTICK_AXIS_MIN`
(-32768), not 0 — SDL's gamepad-mapping layer translates that to the
documented 0=released convention `poll()` consumes. Confirmed load-bearing.

**Row 7 (full button surface, no A/B).** `GamepadButtons` exposes D-pad
×4, SOUTH/EAST/WEST/NORTH, both shoulders, START. Test drives each of the
11 buttons individually via `SDL_SetJoystickVirtualButton`, asserting the
target field is true and every OTHER field stays false (proves no
stuck-true vacuous pass), then asserts release. Confirmed load-bearing
(revert evidence below, swapped SOUTH/EAST).

**Row 8/10 (gyro log-don't-drop, Deck quirk diagnostics).**
`SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO)` + `SDL_GetGamepadName`
logged at GAMEPAD_ADDED time. Test captures the real log line via the
same `LogForwardSink` mechanism the Wayland test uses (extended to search
the full accumulated capture, not just the last message, since a real
foreign gamepad's own connect-log can interleave with this test's own —
found live on this dev machine, fixed by searching for the specific pad's
own line rather than assuming ordering). Confirmed the log line reflects
a REAL query result (virtual pads declare zero sensors →
`hasGyroSensor=false` genuinely computed, not hardcoded).

**Row 9 (single-active = lowest ID).** `poll()` selects
`std::min_element` over `gamepads_` by `SDL_JoystickID`. Test attaches two
virtual pads with distinguishable button state, asserts the lower-ID one
is active, detaches it, asserts reselection to the higher-ID one with ITS
OWN state. Confirmed load-bearing (revert evidence below).

**Row 11 (thread-affinity).** All ten new/touched public methods carry
`RX_ASSERT_MAIN_THREAD`. Test mirrors `rx_asset/tests/thread_guard_test.cpp`'s
established "plain `std::thread` stands in for a chunk≥1 worker" pattern
(no `rx::task::Scheduler` integration exists for `rx_platform` this
phase): 4 guarded calls from 4 sequential worker threads all trip the
hook; the same 4 calls on the main thread trip nothing. Confirmed
load-bearing (revert evidence below).

**NEW SCOPE — keyboard.** `isKeyDown(SDL_Scancode)` wraps
`SDL_GetKeyboardState`, bounds-checked. Device-free test: every scancode
at rest reports `false`; an absurdly out-of-range value
(`static_cast<SDL_Scancode>(99999)`, and `-1`) returns `false` rather than
reading out of bounds. Confirmed load-bearing — reverting the bounds
check **crashes** (SIGSEGV), not just fails an assertion (revert evidence
below). Real key-press confirmation is MANUAL_VERIFICATION-only (SDL
provides no public API to inject synthetic keyboard STATE — only events,
which per `SDL_PushEvent`'s own doc and this file's own existing comment
on that point, don't move `SDL_GetKeyboardState()`'s array).

## Build & test evidence (both presets, CI-representative invocation)

**linux-native**, full suite:
```
$ xvfb-run -a ctest --preset linux-native --output-on-failure
...
100% tests passed, 0 tests failed out of 23
Total Test time (real) = 150.76 sec
```

**windows-cross-zig**, Wine, matching CI's own exclusion convention:
```
$ xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample' --output-on-failure
...
100% tests passed, 0 tests failed out of 11
Total Test time (real) = 131.92 sec
```
`rx_platform_tests` (containing every new test in this task) passed under
real Wine in 2.90s, all 27 test cases including the gamepad ones (verified
directly with `-s`; the environment-conditional skip logic correctly fired
there too — Wine surfaces the same real USB device through its own
joystick emulation).

`rx_platform_tests` standalone, both environment scenarios:
```
$ SDL_JOYSTICK_BLACKLIST_DEVICES="0x3537/0x1012" xvfb-run -a ./build/linux-native/src/rx_platform/rx_platform_tests
[doctest] test cases:  27 |  27 passed | 0 failed | 0 skipped
[doctest] assertions: 312 | 312 passed | 0 failed |

$ xvfb-run -a ./build/linux-native/src/rx_platform/rx_platform_tests
[doctest] test cases:  27 |  27 passed | 0 failed | 0 skipped
[doctest] assertions: 123 | 123 passed | 0 failed |
```

## Revert evidence (load-bearing, in-tree revert-and-restore — disable →
rebuild → run failing test → restore → rebuild → confirm green;
`grep -rn "REVERT-TEST" src/rx_platform/` confirms zero leftover markers
in the final tree)

1. **Scaled-radial deadzone → naive per-axis clamp** (`input.cpp`,
   `applyStickDeadzone` body replaced with the exact anti-pattern the
   ruling rejects): diagonal-snap test fails (`result.x/y` both 0 instead
   of non-zero); MANDATORY (8500,8500) test fails on all 4 assertions
   (`0.259399` vs. expected `≈0.1148`/`0.16235`, and the "NOT equivalent"
   assertion itself fails since the naive and buggy outputs now match).
2. **Mouse-delta accumulation short-circuited** (`if (false && ...)`):
   `consumeMouseDelta()` sums test fails (`0 == Approx(7.75)` etc.).
3. **Focus-pause condition dropped** (kept windowID filter, removed
   `&& !focusLost_`): FOCUS_LOST test fails — motion during focus-loss
   IS accumulated (`100 == 0` mismatch) when it must not be.
4. **GAMEPAD_REMOVED erase skipped**: hot-plug lifecycle test fails
   (device stays reported `connected` after detach).
5. **Single-active rule flipped to highest ID** (`std::max_element`):
   single-active test fails (south/east both wrong — the higher-ID pad's
   state leaks into what should be the lower-ID pad's turn).
6. **SOUTH/EAST swapped** in `poll()`'s button assignment: button-surface
   test fails (4 assertions — south field reads east's driven value and
   vice versa).
7. **Trigger deadzone bypassed** (`state.rightTrigger` = raw
   `SDL_GetGamepadAxis` value, no `applyTriggerDeadzone`): trigger test
   fails (`32767 == Approx(1.0)` mismatch).
8. **`RX_ASSERT_MAIN_THREAD` removed from `poll()`**: thread-guard test
   fails (`capture.callCount` 3 instead of 4; `lastContext` stale).
9. **`isKeyDown`'s bounds check removed**: test **CRASHES** — `SIGSEGV`
   reading `state[key]` for the out-of-range scancode, doctest reports
   `test case CRASHED`, not merely failed. This is the live UB the check
   exists to close.

All 9 disable→fail→restore→pass cycles independently rebuilt and re-ran
the specific affected test green again before moving to the next; the
final full suite (both presets, both real-device environment scenarios)
is green as shown above.

## Deviations (disclosed)

1. **`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` fix** — not requested by
   the brief/matrix/rulings text explicitly, but a genuine correctness
   requirement discovered while implementing (see "Production bug found
   and fixed" above); without it, the gamepad surface is unusable on any
   unfocused window, which is the Steam Deck's OWN normal operating
   condition under Gamescope. Judged in-scope, not deferred: it is a
   one-line fix inside this exact ticket's own `Window::create()` change
   surface, and this ticket's own acceptance bar names Steam Deck
   drivability explicitly.
2. **Guard extended beyond the ruling's literal list.** Ruling row 11
   names `pumpEvents()`/`consumeMouseDelta()`/`poll()`/the cursor and
   relative-mode SETTERS. I additionally guarded the GETTERS
   (`relativeMouseModeWanted`/`cursorVisible`/`cursorConfined`) and
   `isKeyDown()` for one predictable whole-class contract, matching
   `GeometryPool`'s established precedent (`docs/threading.md` already
   documents this reasoning for that class). Not scope creep on the
   ticket's own subject matter — same 10 methods this ticket adds, same
   file.
3. **`docs/threading.md` registry entry added** for `rx::platform::Window`
   — Task 17 documented its own thread-affinity by comment only, never
   added a registry entry despite the doc's own stated purpose ("what
   every new public header's one-line thread-affinity note... points back
   to"). This task's ruling explicitly requires the guard convention, so
   I closed that pre-existing gap for the methods this task touches
   (did NOT add an entry/guard for Task 17's `setFullscreen`/
   `isFullscreen`, out of this task's touched-surface scope).
4. **`+input.h` used** (brief's own "if cleaner" option) — kept the
   deadzone math and value types dependency-free from SDL entirely, so
   `applyStickDeadzone`/`applyTriggerDeadzone` are unit-testable with
   zero SDL initialization (matrix row 5's own suggestion, "if the math
   is factored out testably").
5. **Local-machine-only verification aid, never shipped**:
   `SDL_JOYSTICK_BLACKLIST_DEVICES=0x3537/0x1012` (this dev machine's
   real controller's VID/PID) was used as an env var during my own
   verification runs to get a foreign-device-free baseline for the
   revert-discrimination cycles; it appears nowhere in the committed test
   file (which is written to be correct with OR without a foreign device
   present, via `foreignGamepadIds()`) — disclosed here only so the
   coordinator/reviewer understands why some of my terminal transcripts
   show that env var and can reproduce the clean-environment runs
   locally if desired.

## Self-review

- Grepped the full diff for `REVERT-TEST`, `TEMP`, `TODO`, `FIXME`, `XXX`
  — clean (zero matches in the final tree).
- Verified `Window`'s move constructor/assignment/destructor correctly
  carry the 4 new scalar members (`relativeModeWanted_`/`focusLost_`/
  `accumMouseDeltaX_`/`Y_`) and move (not copy) `gamepads_`, with
  `closeAllGamepads()` shared between move-assignment's "destroy what I
  currently own" step and the destructor (mirrors the existing
  `SDL_DestroyWindow` pattern).
- Confirmed `pumpEvents()`'s cross-window isolation (existing
  `windowID`-filtering convention) extends correctly to
  `SDL_EVENT_MOUSE_MOTION`; gamepad device events are correctly left
  UNfiltered (gamepads are process-global, not per-window).
- Confirmed `SDL_GAMEPAD_BUTTON_SOUTH`/`EAST`/`WEST`/`NORTH` are the real
  SDL3 3.4.14 identifiers (grepped the vendored header directly, zero
  `_A`/`_B` hits) before writing a single line of `GamepadButtons`.
- `git diff` of every file I claim as mine reviewed before staging;
  confirmed `.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md`
  (a concurrent coordinator ledger entry, not mine) is excluded via
  explicit pathspec.

## Concerns for the coordinator

1. **The `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` fix is real and
   important but was NOT explicitly named anywhere in the brief/matrix/
   rulings** — flagging in case the coordinator wants to fold a one-line
   note into the ledger/registry (e.g. "gamepad input requires this hint;
   `Window::create()` sets it unconditionally") so a future task touching
   gamepad/focus code doesn't rediscover it the hard way.
2. **This dev machine's own physically-connected gamepad** is a real,
   disclosed environmental fact (not a fabricated test scenario) — every
   gamepad test is written to behave correctly with or without it, and
   both scenarios were independently verified green, but a reviewer
   running these tests on a THIRD machine with a DIFFERENT foreign
   gamepad will see the same honest-skip behavior (by design) rather than
   the full 312-assertion run, unless they similarly neutralize their own
   device via `SDL_JOYSTICK_BLACKLIST_DEVICES=<their VID>/<their PID>`
   (found via `cat /proc/bus/input/devices`).
3. **Real-hardware rows in `MANUAL_VERIFICATION.md`'s new section are
   honestly unchecked** — no sample exists yet to drive an end-to-end
   human-observed session (Task 24 is the first consumer); Steam Deck
   drivability specifically (the ticket's own stated acceptance bar)
   cannot be confirmed until that sample lands and runs on real Deck
   hardware.
