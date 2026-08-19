### Task 20: Input expansion (seed 6) — Haiku

**Files:** Modify `src/rx_platform/{include/rx_platform/window.h,window.cpp}` (+input.h if cleaner per existing layout): relative mouse mode (SDL_SetWindowRelativeMouseMode), per-frame accumulated mouse deltas from SDL_EVENT_MOUSE_MOTION xrel/yrel, cursor show/hide, gamepad: hot-plug via SDL_EVENT_GAMEPAD_ADDED/REMOVED, `GamepadState poll()` (left/right stick float2 with 8000/32768 deadzone [R:present], triggers, A/B buttons); tests where device-free (deadzone math), manual rows for the rest.
**Steps:** per existing rx_platform test conventions → implement → both presets → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue14-input.md` as amended by
`gate/rulings-2026-08-18.md` §#14. Key deltas: SDL3 has NO
`SDL_GAMEPAD_BUTTON_A/_B` — the surface exposes the full discrete set
(D-pad ×4, face ×4 SOUTH/EAST/WEST/NORTH, both shoulders, START) as a
bitmask/bool struct; deadzone is **scaled-radial per stick** (mag =
|stick|/32768; below 8000/32768 → zero; else normalize × rescale —
never two per-axis clamps; the (8500,8500) discrimination case is
mandatory) + a 1D trigger deadzone (~1000/32767, tunable); mouse
deltas = xrel/yrel event accumulation with consume-and-reset;
focus-loss pause + unconditional re-arm of relative mode on
focus-gain; cursor confine via `SDL_SetWindowMouseGrab` in scope;
hot-plug map keyed by SDL_JoystickID (close-then-erase synchronously);
single-active rule = lowest connected ID, virtual-joystick-tested;
TEST-COVERAGE CORRECTION: `SDL_AttachVirtualJoystick` makes hot-plug/
axis/button paths CI-automatable (the "manual rows for the rest"
phrasing above is superseded; a smoke run confirms virtual joysticks
work under CI's driver first); **SCOPE GROWS: minimal keyboard
surface** — `bool isKeyDown(SDL_Scancode)` over `SDL_GetKeyboardState`
(sample 09's WASD fly-through requires it; its absence was an
oversight); rumble/touchpad deferred (registry, SDK/platform); gyro =
log-don't-drop with HasSensor + device-name logging (SDL #9148: Deck
gyro/paddles undetectable at our pin); thread-affinity one-liners +
RX_ASSERT_MAIN_THREAD guards per the in-repo convention; input
accumulators gate on ImGui's WantCapture flags (Task 21 coordination
— single event-dispatch owner in pumpEvents()).

