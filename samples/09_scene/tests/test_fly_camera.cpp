// samples/09_scene/tests/test_fly_camera.cpp -- proves the W/S-inversion
// regression: FlyCamera's W input must displace the camera along its OWN
// camera.forward() direction, not the opposite. The reported defect (W
// flies backward) lived in updateFlyCamera()'s one-line construction of the
// world-space move delta -- `glm::vec3(strafe, vertical, -forward)` --
// which re-negated the `forward` accumulator a SECOND time even though
// FlyCamera::moveLocal() already multiplies it against camera.forward()
// itself (which already encodes the "-Z is local forward" convention via
// rx_scene/camera.h's own forward() accessor). Extracted to
// flyCameraLocalMoveDelta() (fly_camera.h) so this exact call-site logic is
// reachable without an rx::platform::Window/SDL/VkDevice -- see that
// header's own top comment for the full derivation.
//
// Every TEST_CASE below drives the REAL production function
// (flyCameraLocalMoveDelta()) and the REAL FlyCamera::moveLocal()/
// applyLookDelta(), not a re-implementation -- a bug reintroduced at the
// call site (main.cpp) would need to also break these assertions to pass
// CI silently.
#include "../fly_camera.h"

#include <doctest/doctest.h>

using rx::samples9::FlyCamera;
using rx::samples9::flyCameraLocalMoveDelta;
using rx::samples9::keyboardDrivesCamera;

namespace {
constexpr float kEps = 1e-4F;

// Displacement observed from moveLocal() for one frame's worth of input,
// isolated from the camera's starting position (moveLocal() is a `+=`).
glm::vec3 observedDisplacement(FlyCamera cam, float forward, float strafe, float vertical, float distance = 1.0F) {
    const glm::vec3 before = cam.camera.position;
    const glm::vec3 localDelta = flyCameraLocalMoveDelta(forward, strafe, vertical);
    REQUIRE(glm::length(localDelta) > kEps);
    cam.moveLocal(glm::normalize(localDelta) * distance);
    return cam.camera.position - before;
}
}  // namespace

TEST_CASE("FlyCamera: W input (forward=+1) displaces the camera along its OWN camera.forward() direction "
          "[regression -- the reported bug flew backward on W]") {
    FlyCamera cam;  // identity orientation: forward() == (0,0,-1).
    const glm::vec3 disp = observedDisplacement(cam, /*forward=*/1.0F, /*strafe=*/0.0F, /*vertical=*/0.0F, 5.0F);

    // Must move WITH forward(), not against it.
    CHECK(glm::dot(glm::normalize(disp), cam.camera.forward()) > 0.99F);
    CHECK(disp.x == doctest::Approx(0.0F).epsilon(kEps));
    CHECK(disp.y == doctest::Approx(0.0F).epsilon(kEps));
    CHECK(disp.z == doctest::Approx(-5.0F).epsilon(kEps));  // forward() == (0,0,-1) at identity.
}

TEST_CASE("FlyCamera: S input (forward=-1) displaces the camera opposite camera.forward()") {
    FlyCamera cam;
    const glm::vec3 disp = observedDisplacement(cam, /*forward=*/-1.0F, /*strafe=*/0.0F, /*vertical=*/0.0F, 5.0F);

    CHECK(glm::dot(glm::normalize(disp), cam.camera.forward()) < -0.99F);
    CHECK(disp.z == doctest::Approx(5.0F).epsilon(kEps));
}

TEST_CASE("FlyCamera: W displacement matches camera.forward() after a non-trivial yaw+pitch rotation too "
          "[proves the invariant generally, not only at identity orientation]") {
    FlyCamera cam;
    cam.yawRadians = glm::radians(37.0F);
    cam.pitchRadians = glm::radians(-15.0F);
    cam.syncOrientation();
    REQUIRE(glm::length(cam.camera.forward() - glm::vec3(0, 0, -1)) > 0.01F);  // sanity: really rotated.

    const glm::vec3 disp = observedDisplacement(cam, /*forward=*/1.0F, /*strafe=*/0.0F, /*vertical=*/0.0F, 3.0F);
    CHECK(glm::dot(glm::normalize(disp), cam.camera.forward()) > 0.999F);
    CHECK(glm::length(disp - cam.camera.forward() * 3.0F) < kEps);
}

TEST_CASE("FlyCamera: D input (strafe=+1) displaces the camera along camera.right(); A (strafe=-1) opposite") {
    FlyCamera cam;
    const glm::vec3 dispD = observedDisplacement(cam, 0.0F, /*strafe=*/1.0F, 0.0F, 2.0F);
    CHECK(glm::dot(glm::normalize(dispD), cam.camera.right()) > 0.99F);

    const glm::vec3 dispA = observedDisplacement(cam, 0.0F, /*strafe=*/-1.0F, 0.0F, 2.0F);
    CHECK(glm::dot(glm::normalize(dispA), cam.camera.right()) < -0.99F);
}

TEST_CASE("FlyCamera: Space input (vertical=+1) displaces the camera along camera.up(); LCtrl (vertical=-1) "
          "opposite") {
    FlyCamera cam;
    const glm::vec3 dispUp = observedDisplacement(cam, 0.0F, 0.0F, /*vertical=*/1.0F, 2.0F);
    CHECK(glm::dot(glm::normalize(dispUp), cam.camera.up()) > 0.99F);

    const glm::vec3 dispDown = observedDisplacement(cam, 0.0F, 0.0F, /*vertical=*/-1.0F, 2.0F);
    CHECK(glm::dot(glm::normalize(dispDown), cam.camera.up()) < -0.99F);
}

TEST_CASE("FlyCamera: gamepad left-stick forward accumulator (forward = -stick.y, SDL's Y-down convention) "
          "resolves through the SAME shared mapping as keyboard W -- proves the fix covers gamepad move too, "
          "since updateFlyCamera() feeds both devices into one `forward` accumulator before this function ever "
          "runs") {
    FlyCamera cam;
    constexpr float kStickForwardPush = -1.0F;  // SDL: pushing the stick away from the player reports negative Y.
    const float forwardAccumulator = -kStickForwardPush;  // updateFlyCamera()'s own `forward += -pad.leftStick.y`.
    REQUIRE(forwardAccumulator == doctest::Approx(1.0F));

    const glm::vec3 disp = observedDisplacement(cam, forwardAccumulator, 0.0F, 0.0F, 4.0F);
    CHECK(glm::dot(glm::normalize(disp), cam.camera.forward()) > 0.99F);
}

TEST_CASE("FlyCamera: mouse-look sense -- moving the mouse right (dx>0) turns the camera toward its OWN "
          "pre-turn right() direction, not left") {
    FlyCamera cam;  // identity: forward=(0,0,-1), right=(1,0,0).
    const glm::vec3 rightBeforeTurn = cam.camera.right();

    cam.applyLookDelta(/*dxRadians=*/glm::radians(20.0F), /*dyRadians=*/0.0F);

    // Turning "right" means the new forward() should have swung toward
    // where right() used to point.
    CHECK(glm::dot(cam.camera.forward(), rightBeforeTurn) > 0.0F);
}

TEST_CASE("FlyCamera: mouse-look sense -- moving the mouse up (dy<0, SDL's own y-increases-downward relative "
          "motion) tilts the camera to look upward, toward +Y") {
    FlyCamera cam;
    cam.applyLookDelta(/*dxRadians=*/0.0F, /*dyRadians=*/-0.3F);  // mouse moved up.
    CHECK(cam.camera.forward().y > 0.0F);  // looking up now.
    CHECK(cam.pitchRadians > 0.0F);
}

TEST_CASE("FlyCamera: W/S-vs-A/D-vs-Space/Ctrl axis assignment [regression -- the fix must not have "
          "accidentally moved the sign flip onto a different axis]") {
    // A pure structural check on flyCameraLocalMoveDelta() itself: x is
    // strafe, y is vertical, z is forward, UNNEGATED.
    const glm::vec3 delta = flyCameraLocalMoveDelta(/*forward=*/1.0F, /*strafe=*/2.0F, /*vertical=*/3.0F);
    CHECK(delta.x == doctest::Approx(2.0F));
    CHECK(delta.y == doctest::Approx(3.0F));
    CHECK(delta.z == doctest::Approx(1.0F));
}

// --- keyboardDrivesCamera() [Issue #33 review, latent-sibling finding 2] --
TEST_CASE("keyboardDrivesCamera(): true (keyboard reads allowed) when ImGui does NOT claim the keyboard") {
    CHECK(keyboardDrivesCamera(/*imguiWantsKeyboard=*/false));
}

TEST_CASE("keyboardDrivesCamera(): false (keyboard reads suppressed) when ImGui DOES claim the keyboard "
          "[matches updateFlyCamera()'s pre-existing WASD/Space/LCtrl/LShift gating -- now narrowed to keyboard "
          "reads only, not the whole function]") {
    CHECK_FALSE(keyboardDrivesCamera(/*imguiWantsKeyboard=*/true));
}
