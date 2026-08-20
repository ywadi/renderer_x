// samples/09_scene/tests/test_mouse_capture.cpp -- proves Issue #33's fix:
// samples/09_scene/main.cpp must engage rx::platform::Window::
// setRelativeMouseMode() for --present fly-through, driven by
// rx::samples9::FlyThroughCaptureState's own toggle transitions (Esc,
// click-to-recapture) and composed with ImGui's WantCaptureMouse the same
// way updateFlyCamera() (main.cpp) actually composes them
// (mouseDeltaDrivesCamera()). Device-free/SDL-free, matching
// test_fly_camera.cpp's own precedent for this sample -- no rx::platform::
// Window/SDL/VkDevice anywhere in this file.
#include "../mouse_capture.h"

#include <doctest/doctest.h>

using rx::samples9::FlyThroughCaptureState;
using rx::samples9::mouseDeltaDrivesCamera;

TEST_CASE("FlyThroughCaptureState: captured by default [Issue #33, requirement 1 -- recommended UX: usable from "
          "frame 1 of --present without an extra click]") {
    FlyThroughCaptureState state;
    CHECK(state.captured());
}

TEST_CASE("FlyThroughCaptureState: toggleOnEscPressed() flips captured -> released -> captured across repeated "
          "calls [simulating one edge-triggered SDL_EVENT_KEY_DOWN per physical Esc press]") {
    FlyThroughCaptureState state;
    REQUIRE(state.captured());

    state.toggleOnEscPressed();
    CHECK_FALSE(state.captured());

    state.toggleOnEscPressed();
    CHECK(state.captured());

    state.toggleOnEscPressed();
    CHECK_FALSE(state.captured());
}

TEST_CASE("FlyThroughCaptureState: recaptureOnViewportClick() sets captured=true when released") {
    FlyThroughCaptureState state;
    state.toggleOnEscPressed();
    REQUIRE_FALSE(state.captured());

    state.recaptureOnViewportClick();
    CHECK(state.captured());
}

TEST_CASE("FlyThroughCaptureState: recaptureOnViewportClick() is a no-op (idempotent) while already captured "
          "[matches Window::setRelativeMouseMode(true)'s own idempotent-re-request contract]") {
    FlyThroughCaptureState state;
    REQUIRE(state.captured());

    state.recaptureOnViewportClick();
    CHECK(state.captured());
}

TEST_CASE("FlyThroughCaptureState: a click-then-Esc-then-click sequence round-trips correctly "
          "[revert-discrimination: a caller that dropped the edge/idempotency guards would still pass a naive "
          "single-toggle test but diverge here]") {
    FlyThroughCaptureState state;
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

TEST_CASE("mouseDeltaDrivesCamera(): CAPTURED always drives the camera, regardless of ImGui's WantCaptureMouse "
          "[Issue #33, requirement 2 -- captured means the cursor is hidden, so the HUD ignores it by contract]") {
    CHECK(mouseDeltaDrivesCamera(/*captured=*/true, /*imguiWantsMouse=*/false));
    CHECK(mouseDeltaDrivesCamera(/*captured=*/true, /*imguiWantsMouse=*/true));
}

TEST_CASE("mouseDeltaDrivesCamera(): RELEASED preserves the pre-existing !WantCaptureMouse gate exactly "
          "[Issue #33, requirement 2 -- camera look must NOT consume input ImGui claims while released]") {
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

TEST_CASE("FlyThroughCaptureState + mouseDeltaDrivesCamera(): end-to-end composition across a full "
          "capture/HUD-interaction/release/recapture cycle, driving mouseDeltaDrivesCamera() with the state's own "
          "captured() accessor exactly as updateFlyCamera() (main.cpp) does") {
    FlyThroughCaptureState state;

    // Frame 1: fresh --present session, HUD not hovered -- camera drives.
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
