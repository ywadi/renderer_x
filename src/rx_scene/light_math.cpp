#include <rx_scene/light_math.h>

#include <algorithm>
#include <cmath>

namespace rx::scene::lightmath {

float rangeAttenuation(float distance, float range) {
    // Clamp away from an exact-zero distance -- see this function's own
    // header comment. Mirrors glTF-Sample-Renderer's own
    // `pow(distance, 2.0)` denominator (no epsilon there since its own
    // test content never places a light exactly at a shaded point), but a
    // GPU falloff probe that sweeps distance toward 0 would otherwise
    // divide by zero here; 1e-4 matches this codebase's own established
    // NdotV/NdotL-style grazing-angle epsilon (standard_pbr.slang).
    const float d = std::max(distance, 1e-4F);
    if (range <= 0.0F) {
        // "negative range means unlimited" (glTF-Sample-Renderer,
        // punctual.glsl) == KHR's own "range... undefined... assumed
        // infinite" (README.md#range-property) -- pure inverse-square,
        // no window term.
        return 1.0F / (d * d);
    }
    const float ratio = d / range;
    const float ratio4 = ratio * ratio * ratio * ratio;
    const float window = std::clamp(1.0F - ratio4, 0.0F, 1.0F);
    return window / (d * d);
}

SpotAngleScaleOffset spotAngleScaleOffset(float innerConeAngle, float outerConeAngle) {
    const float cosInner = std::cos(innerConeAngle);
    const float cosOuter = std::cos(outerConeAngle);
    SpotAngleScaleOffset result;
    result.scale = 1.0F / std::max(0.001F, cosInner - cosOuter);
    result.offset = -cosOuter * result.scale;
    return result;
}

float spotAngleAttenuation(glm::vec3 spotDirWorld, glm::vec3 travelToFragmentWorld, SpotAngleScaleOffset scaleOffset) {
    const float cd = glm::dot(spotDirWorld, travelToFragmentWorld);
    const float attenuation = std::clamp(cd * scaleOffset.scale + scaleOffset.offset, 0.0F, 1.0F);
    return attenuation * attenuation;
}

}  // namespace rx::scene::lightmath
