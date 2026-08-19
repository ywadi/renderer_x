#include <rx_platform/input.h>

#include <algorithm>
#include <cmath>

namespace rx::platform {

Vec2f applyStickDeadzone(float rawX, float rawY, float deadzone) {
    const float rawMagnitude = std::sqrt(rawX * rawX + rawY * rawY);
    // Guards the division below regardless of `deadzone`'s value (a
    // caller-supplied 0.0f deadzone must still not divide by zero for a
    // genuinely centered stick) -- functionally a no-op for any positive
    // deadzone, since mag < deadzone already returns {0,0} first below.
    if (rawMagnitude <= 0.0f) {
        return {0.0f, 0.0f};
    }

    const float mag = rawMagnitude / 32768.0f;
    if (mag < deadzone) {
        return {0.0f, 0.0f};
    }

    // Rescale so the OUTPUT magnitude sweeps [0,1] linearly across
    // [deadzone, 1] of the input's own magnitude range -- never a bare
    // pass-through past the threshold (that would jump discontinuously
    // from 0 to `mag` right at the boundary). Clamped to 1: a diagonal
    // raw input can exceed the nominal 32768 unit-circle magnitude (e.g.
    // (32767,32767)'s magnitude is ~1.414), which must not produce an
    // output magnitude above 1.
    float scaledMag = (mag - deadzone) / (1.0f - deadzone);
    scaledMag = std::min(scaledMag, 1.0f);

    const float invRawMagnitude = 1.0f / rawMagnitude;
    return Vec2f{rawX * invRawMagnitude * scaledMag, rawY * invRawMagnitude * scaledMag};
}

float applyTriggerDeadzone(float raw, float deadzone) {
    const float t = raw / 32767.0f;
    const float scaled = std::max(t - deadzone, 0.0f) / (1.0f - deadzone);
    return std::clamp(scaled, 0.0f, 1.0f);
}

}  // namespace rx::platform
