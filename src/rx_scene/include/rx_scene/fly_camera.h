#pragma once
// rx_scene/fly_camera.h -- rx::scene::FlyCamera, a mouse+keyboard+gamepad
// fly-through camera rig built on top of rx::scene::Camera [Phase 5 Task 5,
// ticket #41 row 4; promoted from samples/09_scene/fly_camera.h].
// rx::scene::Camera (camera.h) owns projection/view helpers only --
// position/orientation plus forward()/right()/up() -- with no yaw/pitch
// state, no moveLocal(), no fly-rig at all; FlyCamera is the genuine
// extension every interactive sample with a free-look camera needs, not a
// reimplementation of anything Camera already provides.
//
// WHY THIS EXISTS (carried from the sample-local original): the
// W/S-inverted-movement defect (fly-camera flies BACKWARD on W) lived
// entirely in a one-line sign flip at the caller's own move-delta
// construction, with no test seam able to reach it. FlyCamera itself has no
// rx::platform::Window/GPU dependency (plain glm data + rx::scene::Camera)
// -- moving the struct AND the axis->local-delta mapping here keeps that
// regression covered by a device-free test, and makes the rig available to
// every future interactive sample (Stage 1's 10_lights/11_surfaces both
// need a free-look rig too), not just the one sample that first needed it.
#include <rx_scene/camera.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace rx::scene {

struct FlyCamera {
    rx::scene::Camera camera;
    float yawRadians = 0.0F;    // rotation around world +Y.
    float pitchRadians = 0.0F;  // rotation around the camera's own local +X, clamped.

    static constexpr float kMinPitch = glm::radians(-85.0F);
    static constexpr float kMaxPitch = glm::radians(85.0F);
    static constexpr float kMouseSensitivity = 0.0025F;
    static constexpr float kGamepadLookSensitivity = 2.5F;   // radians/sec at full stick deflection.
    static constexpr float kBaseMoveSpeed = 3.0F;            // world units/sec: a caller may scale this per scene.
    static constexpr float kFastMoveMultiplier = 4.0F;       // held Shift / gamepad right trigger.

    void syncOrientation() {
        camera.orientation =
            glm::angleAxis(yawRadians, glm::vec3(0, 1, 0)) * glm::angleAxis(pitchRadians, glm::vec3(1, 0, 0));
    }

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
// FlyCamera::moveLocal() input mapping -- a pure function so it is
// unit-testable directly, independent of whatever input device produced
// the accumulator values.
//
// `forward`/`strafe`/`vertical` are the SAME signed accumulator shape a
// caller's own per-frame input-polling code computes before calling this:
// W alone yields forward=+1, S alone yields forward=-1 (gamepad:
// `-pad.leftStick.y`, SDL's own Y-down stick convention negated so a
// forward stick push also yields forward=+1) -- D alone yields strafe=+1, A
// alone yields strafe=-1; Space alone yields vertical=+1, LCtrl alone
// yields vertical=-1.
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

// The keyboard-movement decision a caller's own per-frame update gates its
// W/S/A/D/Space/LCtrl/LShift key reads with: WantCaptureKeyboard-shaped
// input must gate ONLY keyboard-SOURCED movement, not the whole update --
// a blanket "if UI wants keyboard, skip everything" would also skip
// gamepad-sourced contributions to `forward`/`strafe`/look/speed as an
// unintended side effect (gamepad input does not read a keyboard-focus
// flag by contract, so a future HUD text field stealing keyboard focus
// must not freeze gamepad flight too). This decision is deliberately
// narrow (keyboard reads only) so it composes with gamepad/mouse
// contributions that must keep flowing regardless.
[[nodiscard]] inline bool keyboardDrivesCamera(bool imguiWantsKeyboard) { return !imguiWantsKeyboard; }

}  // namespace rx::scene
