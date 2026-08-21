#pragma once
// rx_platform/mouse_capture_toggle.h -- rx::platform::MouseCaptureToggle,
// the relative-mouse-capture INTENT state machine + its composition with an
// ImGui-shaped "does the UI want this input" flag [Phase 5 Task 5, ticket
// #41 row 3; promoted from samples/09_scene/mouse_capture.h's own
// `FlyThroughCaptureState`/`mouseDeltaDrivesCamera()`/`escTogglesCapture()`,
// renamed on promotion per the gate matrix's own naming note -- "FlyThrough"
// was the only sample-flavored artifact in an otherwise fully device-free/
// SDL-free header].
//
// WHY THIS LIVES HERE, AT THE PLATFORM LAYER, NOT AS A DUPLICATE PER SAMPLE:
// `Window::setRelativeMouseMode()`/`Window::consumeMouseDelta()` (window.h,
// this same directory) are the low-level MECHANISM -- hide+capture the OS
// cursor, accumulate motion deltas, pause on focus-loss and unconditionally
// re-arm on focus-gain. What was missing, and is exactly what THIS class
// is, is the POLICY layer deciding WHEN a caller should invoke that
// mechanism: captured by default (usable from frame 1 of an interactive
// fly-through run, no extra click needed), Esc toggles release/recapture,
// and a left-click on the (non-HUD) viewport while released recaptures.
// This class itself never touches `Window`/SDL/a `VkDevice` at all -- a
// caller (an interactive present loop) applies the transitions this class
// decides onto a real `Window` at its own call site, exactly mirroring the
// split `Window`'s own `relativeModeWanted_` already draws between
// "app-requested intent" and "what SDL/the OS actually granted" (see that
// field's own comment, window.cpp).
#include <cstdint>

namespace rx::platform {

// Relative-mouse-capture intent for an interactive fly-through-style
// session. Captured: the platform cursor is hidden+locked
// (`Window::setRelativeMouseMode(true)`) and every mouse motion delta
// drives the camera/view. Released: the cursor is visible and free, and a
// Dear ImGui-style HUD is the normal interactive surface.
class MouseCaptureToggle {
public:
    // Captured by default -- the recommended UX for a fly-through session:
    // usable immediately, with no "click to start looking around" step.
    [[nodiscard]] bool captured() const { return captured_; }

    // Edge-triggered Esc handling: call exactly once per REAL "key down"
    // event whose key is Escape AND whose "is this a held-key OS repeat"
    // flag is false -- filtering key-repeat is the caller's job (this
    // method takes no platform event type at all, keeping this class fully
    // device-free). Calling this once per held-key OS repeat event instead
    // would thrash the toggle every repeat interval rather than flipping it
    // once per physical press.
    //
    // The caller must ALSO gate this call on
    // `escTogglesCapture(<ImGui WantCaptureKeyboard this frame>)` below
    // (i.e. only call this when the UI does not claim the keyboard) --
    // otherwise an Esc a keyboard-focused UI widget wants for itself (e.g.
    // "cancel editing" on a text field) would still unconditionally toggle
    // mouse capture out from under it.
    void toggleOnEscPressed() { captured_ = !captured_; }

    // Click-to-recapture: call on a real left-mouse-button-down event that
    // the caller has ALREADY confirmed the UI does not claim (its own
    // "WantCaptureMouse this frame" check) -- a click landing on an open
    // HUD panel must never recapture out from under it. No-op while
    // already captured (idempotent, matching
    // `Window::setRelativeMouseMode(true)`'s own idempotent-re-request
    // contract).
    void recaptureOnViewportClick() { captured_ = true; }

private:
    bool captured_ = true;
};

// The composed mouse-look decision a caller's own camera-update function
// drives its look-delta consumption with:
//   - CAPTURED: mouse motion ALWAYS drives the camera. The cursor is
//     hidden, so there is no visible pointer for any HUD widget to be
//     hovered by -- the HUD ignores the (hidden) cursor by contract,
//     independent of whatever the UI's own "wants mouse" flag happens to
//     read at this instant.
//   - RELEASED: mouse motion drives the camera ONLY when the UI itself does
//     not claim it -- composed with capture state instead of being the
//     only condition, so the released/cursor-visible/HUD-usable state
//     keeps behaving exactly like a plain `!wantCaptureMouse` gate.
[[nodiscard]] inline bool mouseDeltaDrivesCamera(bool captured, bool imguiWantsMouse) {
    return captured || !imguiWantsMouse;
}

// The Esc-toggle gate: a caller's own event-pump handling must call
// `MouseCaptureToggle::toggleOnEscPressed()` ONLY when this returns true,
// i.e. only when the UI does not claim the keyboard -- matching
// `mouseDeltaDrivesCamera()`'s own established "compose with the UI's
// WantCapture* flag at the call site" shape. Deliberately its own named
// decision, separate from any keyboard-movement gate a caller might also
// have (even though both may reduce to `!imguiWantsKeyboard` today) --
// they gate two different call sites that are free to diverge
// independently later.
[[nodiscard]] inline bool escTogglesCapture(bool imguiWantsKeyboard) { return !imguiWantsKeyboard; }

}  // namespace rx::platform
