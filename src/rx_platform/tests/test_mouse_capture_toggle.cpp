// rx_platform/tests/test_mouse_capture_toggle.cpp -- [Phase 5 Task 5,
// ticket #41 row 3] moved from samples/09_scene/tests/test_mouse_capture.cpp
// on promotion of FlyThroughCaptureState -> rx::platform::MouseCaptureToggle
// (mouse_capture_toggle.h) -- proves Issue #33's original fix (fly-through
// mouse capture, Esc release/recapture, click-to-recapture) at its new,
// engine-owned home. Device-free/SDL-free, exactly like the sample-local
// file it replaces -- no rx::platform::Window/SDL/VkDevice anywhere here.
#include <rx_platform/mouse_capture_toggle.h>

#include <doctest/doctest.h>

using rx::platform::MouseCaptureToggle;
using rx::platform::mouseDeltaDrivesCamera;
using rx::platform::escTogglesCapture;

TEST_CASE("MouseCaptureToggle: captured by default [Issue #33, requirement 1 -- recommended UX: usable from "
          "frame 1 of an interactive fly-through session without an extra click]") {
    MouseCaptureToggle state;
    CHECK(state.captured());
}

TEST_CASE("MouseCaptureToggle: toggleOnEscPressed() flips captured -> released -> captured across repeated "
          "calls [simulating one edge-triggered key-down event per physical Esc press]") {
    MouseCaptureToggle state;
    REQUIRE(state.captured());

    state.toggleOnEscPressed();
    CHECK_FALSE(state.captured());

    state.toggleOnEscPressed();
    CHECK(state.captured());

    state.toggleOnEscPressed();
    CHECK_FALSE(state.captured());
}

TEST_CASE("MouseCaptureToggle: recaptureOnViewportClick() sets captured=true when released") {
    MouseCaptureToggle state;
    state.toggleOnEscPressed();
    REQUIRE_FALSE(state.captured());

    state.recaptureOnViewportClick();
    CHECK(state.captured());
}

TEST_CASE("MouseCaptureToggle: recaptureOnViewportClick() is a no-op (idempotent) while already captured "
          "[matches Window::setRelativeMouseMode(true)'s own idempotent-re-request contract]") {
    MouseCaptureToggle state;
    REQUIRE(state.captured());

    state.recaptureOnViewportClick();
    CHECK(state.captured());
}

TEST_CASE("MouseCaptureToggle: a click-then-Esc-then-click sequence round-trips correctly "
          "[revert-discrimination: a caller that dropped the edge/idempotency guards would still pass a naive "
          "single-toggle test but diverge here]") {
    MouseCaptureToggle state;
    state.recaptureOnViewportClick();  // already captured -- no-op.
    CHECK(state.captured());

    state.toggleOnEscPressed();  // release.
    CHECK_FALSE(state.captured());

    state.toggleOnEscPressed();  // recapture via Esc.
    CHECK(state.captured());

    state.toggleOnEscPressed();  // release again.
    CHECK_FALSE(state.captured());

    state.recaptureOnViewportClick();  // recapture via click.
    CHECK(state.captured());
}

TEST_CASE("mouseDeltaDrivesCamera(): CAPTURED always drives the camera, regardless of the UI's WantCaptureMouse "
          "[Issue #33, requirement 2 -- captured means the cursor is hidden, so the HUD ignores it by contract]") {
    CHECK(mouseDeltaDrivesCamera(/*captured=*/true, /*imguiWantsMouse=*/false));
    CHECK(mouseDeltaDrivesCamera(/*captured=*/true, /*imguiWantsMouse=*/true));
}

TEST_CASE("mouseDeltaDrivesCamera(): RELEASED preserves the pre-existing !WantCaptureMouse gate exactly "
          "[Issue #33, requirement 2 -- camera look must NOT consume input the UI claims while released]") {
    CHECK(mouseDeltaDrivesCamera(/*captured=*/false, /*imguiWantsMouse=*/false));
    CHECK_FALSE(mouseDeltaDrivesCamera(/*captured=*/false, /*imguiWantsMouse=*/true));
}

TEST_CASE("mouseDeltaDrivesCamera(): full 2x2 truth table [revert-discrimination -- an implementation that ANDed "
          "instead of OR'd captured with !imguiWantsMouse, or inverted either operand, fails at least one row "
          "here]") {
    CHECK(mouseDeltaDrivesCamera(true, true) == true);
    CHECK(mouseDeltaDrivesCamera(true, false) == true);
    CHECK(mouseDeltaDrivesCamera(false, true) == false);
    CHECK(mouseDeltaDrivesCamera(false, false) == true);
}

TEST_CASE("MouseCaptureToggle + mouseDeltaDrivesCamera(): end-to-end composition across a full "
          "capture/HUD-interaction/release/recapture cycle, driving mouseDeltaDrivesCamera() with the state's own "
          "captured() accessor exactly as an interactive present loop's camera update does") {
    MouseCaptureToggle state;

    // Frame 1: fresh interactive session, HUD not hovered -- camera drives.
    CHECK(mouseDeltaDrivesCamera(state.captured(), /*imguiWantsMouse=*/false));

    // Player presses Esc to interact with the HUD.
    state.toggleOnEscPressed();
    REQUIRE_FALSE(state.captured());

    // While released, hovering an open HUD panel: HUD wins, camera silent.
    CHECK_FALSE(mouseDeltaDrivesCamera(state.captured(), /*imguiWantsMouse=*/true));
    // While released, NOT over a HUD panel: pre-existing behavior preserved -- camera still drives.
    CHECK(mouseDeltaDrivesCamera(state.captured(), /*imguiWantsMouse=*/false));

    // Player left-clicks the (non-HUD) viewport to recapture.
    state.recaptureOnViewportClick();
    REQUIRE(state.captured());
    // Immediately after recapture, WantCaptureMouse might still read stale-true from last frame's HUD
    // hover -- captured must win regardless.
    CHECK(mouseDeltaDrivesCamera(state.captured(), /*imguiWantsMouse=*/true));
}

// --- escTogglesCapture() [Issue #33 review, Minor finding 1] --------------
TEST_CASE("escTogglesCapture(): true (Esc should toggle) when the UI does NOT claim the keyboard") {
    CHECK(escTogglesCapture(/*imguiWantsKeyboard=*/false));
}

TEST_CASE("escTogglesCapture(): false (Esc must NOT toggle) when the UI DOES claim the keyboard "
          "[the review's own scenario: a future keyboard-focused HUD widget, e.g. a text field mid-edit, must "
          "keep Esc's conventional cancel/unfocus meaning instead of it also releasing mouse capture]") {
    CHECK_FALSE(escTogglesCapture(/*imguiWantsKeyboard=*/true));
}

TEST_CASE("escTogglesCapture() composed with MouseCaptureToggle: an Esc press the UI claims leaves "
          "captured() completely unchanged, exactly matching a present loop's own "
          "`!event.key.repeat && escTogglesCapture(WantCaptureKeyboard)` call-site gate -- the caller simply does "
          "not call toggleOnEscPressed() at all in that case") {
    MouseCaptureToggle state;
    REQUIRE(state.captured());

    // Simulates the caller's own call-site condition: only invoke the
    // toggle when escTogglesCapture() says so.
    if (escTogglesCapture(/*imguiWantsKeyboard=*/true)) {
        state.toggleOnEscPressed();
    }
    CHECK(state.captured());  // untouched -- the UI claimed the keyboard, so Esc was never applied.

    if (escTogglesCapture(/*imguiWantsKeyboard=*/false)) {
        state.toggleOnEscPressed();
    }
    CHECK_FALSE(state.captured());  // NOW it toggles, once the UI releases the keyboard.
}
