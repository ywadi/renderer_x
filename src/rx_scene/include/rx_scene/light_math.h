#pragma once

#include <glm/glm.hpp>

// rx_scene/light_math.h -- device-free KHR_lights_punctual attenuation math
// [Phase 5 Stage 2 Task 13, #49; gate matrix-p5t13-physical-lights.md;
// gate ruling rulings-2026-08-20.md, "T13 (#49): KHR_lights_punctual unit
// mapping is spec-exact and value-asserted"]. Pure functions, no VkDevice,
// no Scene dependency -- the SAME formulas `shaders/material/standard_pbr.
// slang`'s punctual-light term ports into Slang, kept here ONCE so a C++
// unit test can assert the closed form directly (light_math_test.cpp)
// independently of any GPU render, and so the GPU falloff probes
// (test_standard_pbr_punctual_gpu.cpp) can compute their own expected
// values by calling these SAME functions rather than re-deriving the
// arithmetic inline at every call site.
//
// UNIT CONVENTION [KHR_lights_punctual README.md, Khronos glTF commit
// 2b29723d025a995971726f2989697cdc49b1222a, fetched verbatim this task]:
// "point and spot lights use luminous intensity in candela (lm/sr) while
// directional lights use illuminance in lux (lm/m^2)... range... Within
// the range of the light, attenuation should follow the inverse square
// law as closely as possible... A recommended implementation... attenuation
// = max(min(1.0-(current_distance/range)^4,1),0)/current_distance^2".
// `rangeAttenuation()` below is that formula, LITERALLY (the un-squared
// form) -- gate matrix's own Open Question resolution: Filament's shipped
// shader (`getSquareFalloffAttenuation()`, shaders/src/
// surface_light_punctual.fs:93-99, google/filament v1.75.0 pinned commit
// 0e58877c09afb1aacd09ff640f74d2adcd2a7e80) SQUARES the smooth-window term
// an extra time (`smoothFactor*smoothFactor`), which is NOT numerically
// identical to the KHR spec's own literal formula near `range` (both agree
// only in the near-field, d<<range) -- the matrix recommends the spec's
// own literal form as this project's conformance-path default, since a
// Khronos-Sample-Renderer-class reference (this project's own Task 11
// conformance harness target) is what any future light-bearing conformance
// case would be compared against. INDEPENDENTLY CONFIRMED this task: the
// pinned glTF-Sample-Renderer commit 863b981fb755359063e370ff7b6e956bda0716e2
// (source/Renderer/shaders/punctual.glsl, `getRangeAttenuation()`) uses the
// IDENTICAL un-squared formula -- so the spec text, its own reference code,
// AND the actual Khronos conformance renderer all agree; Filament's shader
// is the outlier here (a deliberate "cinematic" softening, not a spec
// reading difference).
namespace rx::scene::lightmath {

// KHR_lights_punctual range-property attenuation (README.md#range-property,
// quoted above). `range <= 0.0` means "no configured range" -- this
// project's own established sentinel (`LightRecord::range`'s "0.0 = inert"
// convention, scene.h) doubling as glTF-Sample-Renderer's own "negative
// range means unlimited" case (punctual.glsl's `getRangeAttenuation()`) --
// returns the un-windowed `1/distance^2` term in that case. `distance` is
// clamped away from 0 (a coincident light/shading-point is physically
// degenerate, not a real scene state) to avoid a division producing +inf.
[[nodiscard]] float rangeAttenuation(float distance, float range);

// KHR_lights_punctual spot-cone precomputation -- README.md's own "Inner
// and Outer Cone Angles" reference code, quoted verbatim: "float
// lightAngleScale = 1.0f / max(0.001f, cos(innerConeAngle) -
// cos(outerConeAngle)); float lightAngleOffset = -cos(outerConeAngle) *
// lightAngleScale;". Computed ONCE per light (CPU-side, like the spec's
// own comment says: "These two values can be calculated on the CPU and
// passed into the shader") -- `standard_pbr.slang`'s punctual term reads
// the two scalars directly rather than re-deriving them from the raw cone
// angles per-fragment.
struct SpotAngleScaleOffset {
    float scale = 0.0F;
    float offset = 0.0F;

    bool operator==(const SpotAngleScaleOffset&) const = default;
};
[[nodiscard]] SpotAngleScaleOffset spotAngleScaleOffset(float innerConeAngle, float outerConeAngle);

// The shader-side squared-saturate cone term (SAME reference code,
// continued): "float cd = dot(spotlightDir, normalizedLightVector); float
// angularAttenuation = saturate(cd * lightAngleScale + lightAngleOffset);
// angularAttenuation *= angularAttenuation;". `spotDirWorld` is the spot
// light's own FACING/travel direction (glTF's local -Z axis rotated by the
// node's world transform -- `LightRecord::direction`'s own existing
// convention, matching `DirectionalLightDesc::dir`'s "light travel
// direction" semantic). `travelToFragmentWorld` is the unit vector FROM
// the light's position TOWARD the shaded point (i.e. `normalize(worldPos -
// lightPositionWorld)`) -- NOT the "toward light" `L` vector NdotL uses;
// see this header's own top comment / the task report for the sign-
// convention derivation, cross-checked directly against the pinned
// glTF-Sample-Renderer's own `getSpotAttenuation()` (punctual.glsl:
// `actualCos = dot(normalize(spotDirection), normalize(-pointToLight))`
// where `pointToLight = light.position - worldPosition`, i.e. `-pointToLight
// == worldPosition - light.position` -- the SAME travel-direction vector
// this function takes). Both inputs are assumed pre-normalized by the
// caller (the GPU port normalizes once per fragment; this device-free
// twin trusts its own unit-test callers to pass unit vectors, matching
// this project's other device-free math helpers' own convention of not
// re-normalizing an already-unit input defensively).
[[nodiscard]] float spotAngleAttenuation(glm::vec3 spotDirWorld, glm::vec3 travelToFragmentWorld,
                                          SpotAngleScaleOffset scaleOffset);

}  // namespace rx::scene::lightmath
