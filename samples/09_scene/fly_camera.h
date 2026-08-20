#pragma once
// samples/09_scene/fly_camera.h -- rx::samples9::FlyCamera, the sample's own
// mouse+keyboard+gamepad fly-through camera rig, and the pure axis-to-
// local-move-delta mapping updateFlyCamera() (main.cpp) drives it with.
//
// WHY THIS EXISTS (extracted from main.cpp): the W/S-inverted-movement
// defect (fly-camera flies BACKWARD on W) lived entirely in a one-line sign
// flip at the update() call site, with no test seam able to reach it --
// FlyCamera itself has no rx::platform::Window/GPU dependency (plain glm
// data + rx::scene::Camera, same "device-free" shape draw_recording.h's own
// top comment already establishes as this sample's precedent for pulling
// pure logic out of main.cpp into a separately-testable header). Moving the
// struct AND the axis->local-delta mapping here lets
// samples/09_scene/tests/test_fly_camera.cpp assert the actual world-space
// movement direction for W/S/A/D/Space/Ctrl against camera.forward()/
// right()/up(), without an SDL window or a VkDevice.
#include <rx_scene/camera.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace rx::samples9 {

struct FlyCamera {
    rx::scene::Camera camera;
    float yawRadians = 0.0F;    // rotation around world +Y.
    float pitchRadians = 0.0F;  // rotation around the camera's own local +X, clamped.

    static constexpr float kMinPitch = glm::radians(-85.0F);
    static constexpr float kMaxPitch = glm::radians(85.0F);
    static constexpr float kMouseSensitivity = 0.0025F;
    static constexpr float kGamepadLookSensitivity = 2.5F;   // radians/sec at full stick deflection.
    static constexpr float kBaseMoveSpeed = 3.0F;            // world units/sec: overridden per scene by frameToScene().
    static constexpr float kFastMoveMultiplier = 4.0F;       // held Shift / gamepad right trigger.

    void syncOrientation() { camera.orientation = glm::angleAxis(yawRadians, glm::vec3(0, 1, 0)) *
                                                    glm::angleAxis(pitchRadians, glm::vec3(1, 0, 0)); }

    void applyLookDelta(float dxRadians, float dyRadians) {
        yawRadians -= dxRadians;
        pitchRadians = std::clamp(pitchRadians - dyRadians, kMinPitch, kMaxPitch);
        syncOrientation();
    }

    void moveLocal(const glm::vec3& localDelta) {
        camera.position += camera.right() * localDelta.x + camera.up() * localDelta.y + camera.forward() * localDelta.z;
    }
};

// The W/S/A/D/Space/Ctrl (and gamepad left-stick) axis-accumulator ->
// FlyCamera::moveLocal() input mapping updateFlyCamera() (main.cpp) uses,
// pulled out as a pure function so it is unit-testable directly.
//
// `forward`/`strafe`/`vertical` are the SAME signed accumulator values
// updateFlyCamera() computes before calling this: W alone yields
// forward=+1, S alone yields forward=-1 (gamepad: `-pad.leftStick.y`, SDL's
// own Y-down stick convention negated so a forward stick push also yields
// forward=+1) -- D alone yields strafe=+1, A alone yields strafe=-1; Space
// alone yields vertical=+1, LCtrl alone yields vertical=-1.
//
// FlyCamera::moveLocal(localDelta) already does
// `position += right()*x + up()*y + forward()*z` -- camera.forward()
// ITSELF already encodes the "local -Z is forward" convention
// (rx_scene/camera.h's own `forward()` accessor), so this mapping must NOT
// re-negate `forward` a second time: `localDelta.z` is `forward` directly,
// unnegated. (The historical bug here was exactly that redundant second
// negation -- `glm::vec3(strafe, vertical, -forward)` -- which silently
// cancelled forward travel into backward travel for every input device
// that ever set the `forward` accumulator: keyboard AND gamepad alike,
// since both feed this one shared function.)
[[nodiscard]] inline glm::vec3 flyCameraLocalMoveDelta(float forward, float strafe, float vertical) {
    return glm::vec3(strafe, vertical, forward);
}

// The keyboard-movement decision updateFlyCamera() (main.cpp) gates its
// W/S/A/D/Space/LCtrl/LShift `window.isKeyDown()` reads with [Issue #33
// review, latent-sibling finding 2]: WantCaptureKeyboard must gate ONLY
// keyboard-SOURCED movement, not the whole function -- the pre-existing
// code here used a single blanket `if (WantCaptureKeyboard) return;` at
// the top of updateFlyCamera(), which also skipped `window.poll()` and
// every gamepad-sourced contribution to `forward`/`strafe`/look/speed
// below it as an unintended side effect (gamepad reads neither
// `app.mouseCapture` nor `ImGui::GetIO()` by this sample's own contract --
// see mouse_capture.h's own top comment -- so a future HUD text field
// stealing keyboard focus must not freeze gamepad flight too). This
// decision is deliberately narrow (keyboard reads only) so it composes
// with gamepad/mouse contributions that must keep flowing regardless.
[[nodiscard]] inline bool keyboardDrivesCamera(bool imguiWantsKeyboard) {
    return !imguiWantsKeyboard;
}

}  // namespace rx::samples9
