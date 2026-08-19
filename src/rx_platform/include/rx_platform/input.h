#pragma once

// input.h -- value types + deadzone math for rx_platform's input surface
// [Phase 4 Task 20, gate ruling #14]. Split out of window.h (brief's own
// "+input.h if cleaner" option) because the deadzone math below is
// deliberately dependency-free (no SDL type anywhere in this file) so it
// is unit-testable with ZERO SDL initialization at all -- gate
// matrix-issue14-input.md row 5's own "pure math unit tests... if the
// math is factored out testably" request. rx::platform::Window
// (window.h) owns translating real SDL_GetGamepadAxis()/
// SDL_GetGamepadButton() reads into these types; this file owns only the
// shapes and the deadzone formulas themselves.

namespace rx::platform {

// A dependency-free 2D vector -- used both for the deadzone math's output
// and (via MouseDelta below) for accumulated mouse motion. Deliberately
// NOT glm::vec2/SDL type: this header has zero includes on purpose.
struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
};

// Per-frame accumulated mouse motion, returned (and reset to zero) by
// Window::consumeMouseDelta() [gate matrix-issue14 row 2's consume-and-
// reset algorithm]. A distinct type from Vec2f purely for call-site
// clarity -- a delta is not a direction or a position.
struct MouseDelta {
    float x = 0.0f;
    float y = 0.0f;
};

// Full discrete gamepad button surface [gate ruling #14 (rulings-2026-08-
// 18.md #14) + matrix row 7: SDL3's real SDL_GamepadButton enum
// (SDL_gamepad.h:154-182, this project's pinned release-3.4.14) has NO
// `SDL_GAMEPAD_BUTTON_A`/`_B` member at all -- grepped, zero hits, see
// the matrix's own Conflict #1]. This struct exposes the D-pad (x4), the
// four face buttons under their REAL SDL3 identifiers (SOUTH/EAST/WEST/
// NORTH -- the header's own doc comments gloss SOUTH as "e.g. Xbox A
// button" etc., but the enum member itself is never named that way),
// both shoulders, and START -- a bool-struct surface, per the ruling,
// not "two buttons."
struct GamepadButtons {
    bool dpadUp = false;
    bool dpadDown = false;
    bool dpadLeft = false;
    bool dpadRight = false;
    bool south = false;   // e.g. Xbox A / PlayStation Cross
    bool east = false;    // e.g. Xbox B / PlayStation Circle
    bool west = false;    // e.g. Xbox X / PlayStation Square
    bool north = false;   // e.g. Xbox Y / PlayStation Triangle
    bool leftShoulder = false;
    bool rightShoulder = false;
    bool start = false;
};

// Snapshot returned by Window::poll() [gate matrix-issue14 row 9]: the
// single "active" gamepad, selected as the lowest currently-connected
// SDL_JoystickID among every hot-plugged device Window::pumpEvents() is
// tracking (deterministic; re-selected only when that pad disconnects).
// `connected == false` (every other field left zero-initialized) when no
// gamepad is attached at all. Stick/trigger fields have ALREADY had the
// scaled-radial / 1D deadzone applied (applyStickDeadzone()/
// applyTriggerDeadzone() below) -- Window::poll() never hands back raw
// SDL axis values.
struct GamepadState {
    bool connected = false;
    Vec2f leftStick;
    Vec2f rightStick;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
    GamepadButtons buttons;
};

// SDL's own thumbstick-centering precedent (~8000 of a 32768-magnitude
// range -- SDL_gamepad.h's own SDL_GamepadAxis doc comment) expressed as
// a ratio of that range. The SAME ratio Microsoft's XInput reference
// deadzone constants (7849/8689 of 32768) approximate, and what gate
// ruling #14 (matrix row 5) retains as "the radius." Tunable per-call;
// this is only the default.
inline constexpr float kDefaultStickDeadzone = 8000.0f / 32768.0f;

// Small analog floor for resting trigger noise -- a DIFFERENT constant
// and formula from the stick deadzone (this is 1D, not the 2D-radial
// check below; see applyTriggerDeadzone()). Matches gate ruling #14's
// proposed default (~1000/32767, tunable).
inline constexpr float kDefaultTriggerDeadzone = 1000.0f / 32767.0f;

// Scaled-radial deadzone [gate ruling #14, matrix row 5 -- RESOLVED in
// favor of scaled-radial over two independent per-axis clamps, which
// snap diagonal stick motion to the cardinal axes ("Doing Thumbstick
// Dead Zones Right", Josh Sutphin; Microsoft's own XInput reference
// sample -- both fetched and cited in the matrix]. `rawX`/`rawY` are raw
// SDL_GetGamepadAxis() values (each in [-32768, 32767]); `deadzone` is
// expressed as a fraction of the 32768 axis-magnitude scale.
//
// Below the deadzone radius: {0,0} exactly. At or above it: the input's
// own direction, scaled to a magnitude that sweeps [0,1] linearly as the
// raw magnitude sweeps [deadzone*32768, max] -- never a bare pass-
// through, which would produce a discontinuous "jump" right at the
// deadzone boundary. This is a genuinely 2D check: two points with the
// SAME per-axis values can cross the deadzone differently depending on
// their combined magnitude (see window_test.cpp's mandatory (8500,8500)
// discrimination case, gate hardening block).
Vec2f applyStickDeadzone(float rawX, float rawY, float deadzone = kDefaultStickDeadzone);

// 1D analog deadzone for a single trigger axis [gate ruling #14, matrix
// row 6]. `raw` is SDL_GetGamepadAxis()'s trigger range, [0, 32767] (SDL3
// guarantees triggers never report negative -- SDL_gamepad.h's own doc,
// distinct from the signed stick range). Returns a rescaled [0,1] value,
// never a boolean "pressed" threshold -- this ticket's triggers stay
// analog (e.g. feeding sample 09's fly-through speed multiplier).
float applyTriggerDeadzone(float raw, float deadzone = kDefaultTriggerDeadzone);

}  // namespace rx::platform
