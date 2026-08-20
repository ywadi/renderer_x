#pragma once
// samples/09_scene/mouse_capture.h -- rx::samples9::FlyThroughCaptureState,
// the sample's own relative-mouse-capture toggle for --present fly-through
// [Issue #33]. Device-free/SDL-free, no rx::platform::Window dependency --
// same "pull pure logic out of main.cpp into its own header" precedent
// fly_camera.h already established in this sample (that header's own top
// comment documents the W/S-inversion regression this one mirrors
// structurally) -- so samples/09_scene/tests/test_mouse_capture.cpp can
// exercise every toggle transition and the ImGui-composition decision
// directly, without an SDL window or a VkDevice.
//
// WHY THIS EXISTS: the reported defect (the OS cursor keeps moving and
// escapes the window/hits screen edges during --present fly-through mouse-
// look, making control impractical) traced to main.cpp never calling
// rx::platform::Window::setRelativeMouseMode() at all -- the Phase 4 Task
// 20 platform facility (hide+capture cursor, delta accumulation, focus-loss
// pause + unconditional re-arm on focus-gain -- see window.h's own
// setRelativeMouseMode()/consumeMouseDelta() comments) existed and was
// already tested, but had no consumer. This header is the small state
// machine that decides WHEN main.cpp's runPresent() calls
// setRelativeMouseMode(): CAPTURED by default on entering --present
// fly-through (recommended UX -- usable from frame 1, no extra click),
// Esc toggles release/recapture, and a left-click on the viewport (i.e. a
// click ImGui itself does not claim) while released recaptures.
// runPresent() applies every transition to the real Window at its own call
// site (main.cpp, "engages the platform capture facility" comment) --
// this header itself never touches SDL/Window, only tracks intent, exactly
// mirroring the split window.h's own relativeModeWanted_ already draws
// between "app-requested intent" and "what SDL/the OS actually granted".
namespace rx::samples9 {

// Fly-through mouse-capture intent. Captured: the platform cursor is
// hidden+locked (rx::platform::Window::setRelativeMouseMode(true)) and
// every mouse motion delta drives the camera. Released: the cursor is
// visible and free, and the HUD (rx_debug_ui::Overlay) is the normal
// ImGui-driven interactive surface.
class FlyThroughCaptureState {
public:
    // Captured by default -- see this header's own top comment for the
    // recommended-UX rationale (Issue #33, requirement 1).
    [[nodiscard]] bool captured() const { return captured_; }

    // Edge-triggered Esc handling: call exactly once per REAL
    // SDL_EVENT_KEY_DOWN whose `scancode == SDL_SCANCODE_ESCAPE` AND whose
    // `repeat` field is false -- filtering key-repeat is the caller's job
    // (this method takes no SDL type at all, keeping the "pure" contract
    // fly_camera.h's own precedent establishes). Calling this once per
    // held-key OS repeat event instead would thrash the toggle every
    // repeat interval rather than flipping it once per physical press.
    void toggleOnEscPressed() { captured_ = !captured_; }

    // Click-to-recapture: call on a real SDL_EVENT_MOUSE_BUTTON_DOWN
    // (left button) that the caller has ALREADY confirmed ImGui does not
    // claim (`!ImGui::GetIO().WantCaptureMouse` at the call site, gate
    // ruling #14/#16's own caller-level-gating convention) -- a click
    // landing on an open HUD panel must never recapture out from under it.
    // No-op while already captured (idempotent, matching
    // Window::setRelativeMouseMode(true)'s own idempotent-re-request
    // contract).
    void recaptureOnViewportClick() { captured_ = true; }

private:
    bool captured_ = true;
};

// The composed mouse-look decision updateFlyCamera() (main.cpp) drives the
// camera's own look-delta consumption with [Issue #33, requirement 2]:
//   - CAPTURED: mouse motion ALWAYS drives the camera. The cursor is
//     hidden, so there is no visible pointer for any HUD widget to be
//     hovered by -- the HUD ignores the (hidden) cursor by contract,
//     independent of whatever ImGui's own WantCaptureMouse happens to
//     read at this instant.
//   - RELEASED: mouse motion drives the camera ONLY when ImGui itself does
//     not claim it -- the pre-existing `!ImGui::GetIO().WantCaptureMouse`
//     gate (gate ruling #14/#16), now composed with capture state instead
//     of being the only condition, so the released/cursor-visible/
//     HUD-usable state keeps behaving exactly as it already did before
//     this ticket.
[[nodiscard]] inline bool mouseDeltaDrivesCamera(bool captured, bool imguiWantsMouse) {
    return captured || !imguiWantsMouse;
}

}  // namespace rx::samples9
