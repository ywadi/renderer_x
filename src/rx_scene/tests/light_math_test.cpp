// src/rx_scene/tests/light_math_test.cpp -- Phase 5 Stage 2 Task 13 [#49]:
// device-free unit tests for rx_scene/light_math.h's KHR_lights_punctual
// attenuation formulas. Every expected value below was computed
// independently (python3, double precision, see task-13-report.md for the
// exact script) directly from the KHR spec's own quoted formulas -- NOT
// reverse-derived from this function's own output -- matching this
// codebase's own established TDD discipline (camera_test.cpp's ev100
// table, task-4-report.md's own "expected values computed independently"
// precedent).
#include <doctest/doctest.h>
#include <rx_scene/light_math.h>

#include <glm/glm.hpp>

#include <cmath>

using rx::scene::lightmath::rangeAttenuation;
using rx::scene::lightmath::spotAngleAttenuation;
using rx::scene::lightmath::spotAngleScaleOffset;
using rx::scene::lightmath::SpotAngleScaleOffset;

TEST_CASE("lightmath::rangeAttenuation matches the KHR_lights_punctual spec's own literal formula "
          "[README.md#range-property: \"attenuation = max(min(1.0-(d/range)^4,1),0)/d^2\"] -- NOT "
          "Filament's own extra-squared shader variant (gate matrix Open Question, resolved: the spec "
          "text, its own reference implementation, AND the pinned glTF-Sample-Renderer conformance "
          "target all agree on the un-squared form; only Filament's shipped shader squares it again)") {
    // Near-field (d << range): both candidate formulas (literal vs.
    // Filament's squared variant) agree closely here -- this case alone
    // does NOT discriminate between them, by design (see the
    // near-`range` case below for the actual discriminator).
    CHECK(rangeAttenuation(2.0F, 10.0F) == doctest::Approx(0.2496F).epsilon(0.0001));

    // Close to `range` (d/range == 0.9): the literal KHR window term
    // (1-(d/range)^4) and Filament's squared (1-(d/range)^4)^2 DIVERGE
    // measurably here -- the discriminating case. Literal: window =
    // 1-0.9^4 = 0.3439, atten = 0.3439/81 = 0.0042456790...  Filament's
    // own squared alternative would instead give window' = 0.3439^2 =
    // 0.11826..., atten' = 0.11826/81 = 0.0014601... -- roughly 2.9x
    // smaller. This TEST_CASE certifies the LITERAL (un-squared) formula.
    const float measured = rangeAttenuation(9.0F, 10.0F);
    CHECK(measured == doctest::Approx(0.004245679F).epsilon(0.0001));
    const float filamentSquaredAlternative = (0.3439F * 0.3439F) / 81.0F;
    CHECK(measured > filamentSquaredAlternative * 2.0F);  // explicit discrimination, not just a close-enough check.

    // range <= 0.0 -- "no configured range" sentinel (LightRecord's own
    // 0.0-inert convention, doubling as glTF-Sample-Renderer's "negative
    // range means unlimited" reading) -- pure inverse-square, no window.
    CHECK(rangeAttenuation(5.0F, 0.0F) == doctest::Approx(0.04F).epsilon(0.0001));
    CHECK(rangeAttenuation(5.0F, -1.0F) == doctest::Approx(0.04F).epsilon(0.0001));

    // Beyond range: the spec's own "max(min(...,1),0)" clamp floors the
    // window term at exactly 0.0 -- fully dark, not negative/NaN.
    CHECK(rangeAttenuation(15.0F, 10.0F) == doctest::Approx(0.0F).epsilon(0.0001));
}

TEST_CASE("lightmath::rangeAttenuation: a directional-light regression invariant -- see "
          "standard_pbr.slang's own punctual term for where this matters: a light with lightType==0 "
          "(Directional) must NEVER go through this function at all (KHR spec: \"Because it is at an "
          "infinite distance, the light is not attenuated\") -- this device-free test only exercises "
          "the function's own math in isolation; the GPU-side regression proof (identical measured "
          "intensity at two different distances for a Directional light) lives in "
          "test_standard_pbr_punctual_gpu.cpp, since it requires a real render.") {
    // A trivial invariant check on the function itself: attenuation is a
    // strictly decreasing function of distance within [0, range) -- if a
    // future edit accidentally inverted the ratio (e.g. range/d instead
    // of d/range), this would catch it immediately.
    CHECK(rangeAttenuation(1.0F, 10.0F) > rangeAttenuation(5.0F, 10.0F));
    CHECK(rangeAttenuation(5.0F, 10.0F) > rangeAttenuation(9.0F, 10.0F));
}

TEST_CASE("lightmath::spotAngleScaleOffset matches the KHR_lights_punctual reference code's closed form "
          "[README.md's own \"Inner and Outer Cone Angles\" section, quoted verbatim: \"float "
          "lightAngleScale = 1.0f / max(0.001f, cos(innerConeAngle) - cos(outerConeAngle)); float "
          "lightAngleOffset = -cos(outerConeAngle) * lightAngleScale;\"] -- using the SAME "
          "(innerConeAngle=0.2, outerConeAngle=0.6) pair assets/test/cube_lights_camera.gltf's own "
          "spot light fixture carries (see the import-consumption GPU test, import_gltf_gpu_test.cpp), "
          "so this device-free proof and that fixture-driven proof share one set of numbers.") {
    const SpotAngleScaleOffset result = spotAngleScaleOffset(0.2F, 0.6F);
    CHECK(result.scale == doctest::Approx(6.462830587F).epsilon(0.0001));
    CHECK(result.offset == doctest::Approx(-5.334004257F).epsilon(0.0001));
}

TEST_CASE("lightmath::spotAngleAttenuation matches the KHR reference code's own squared-saturate curve "
          "at four hand-computed angles -- dead-center (full), inside the window (partial), exactly at "
          "the inner/outer boundaries, and outside the outer cone (fully dark). `spotDirWorld` is fixed "
          "at (0,0,-1) (the spot's own facing direction); `travelToFragmentWorld` is rotated around the "
          "X axis by `theta` so `dot(spotDirWorld, travelToFragmentWorld) == cos(theta)` exactly -- "
          "verify: dot((0,0,-1),(0,sin(theta),-cos(theta))) == cos(theta).") {
    const glm::vec3 spotDir(0.0F, 0.0F, -1.0F);
    const SpotAngleScaleOffset scaleOffset = spotAngleScaleOffset(0.2F, 0.6F);

    auto travelAt = [](float theta) {
        return glm::vec3(0.0F, std::sin(theta), -std::cos(theta));
    };

    // Dead center (theta=0, cd=1) -- inside the inner cone: full brightness.
    CHECK(spotAngleAttenuation(spotDir, travelAt(0.0F), scaleOffset) == doctest::Approx(1.0F).epsilon(0.0001));

    // theta=0.4 (strictly between inner=0.2 and outer=0.6): partial,
    // matching the closed-form squared-saturate curve exactly.
    CHECK(spotAngleAttenuation(spotDir, travelAt(0.4F), scaleOffset) == doctest::Approx(0.3827363698F).epsilon(0.0005));

    // Exactly at innerConeAngle (theta=0.2): the window's own upper
    // boundary -- attenuation == 1.0 (saturate(scale*cos(inner)+offset) ==
    // saturate(1.0) == 1.0 by the scale/offset pair's own construction).
    CHECK(spotAngleAttenuation(spotDir, travelAt(0.2F), scaleOffset) == doctest::Approx(1.0F).epsilon(0.0005));

    // Exactly at outerConeAngle (theta=0.6): the window's own lower
    // boundary -- attenuation == 0.0 by construction (offset == -cos(outer)*scale).
    CHECK(spotAngleAttenuation(spotDir, travelAt(0.6F), scaleOffset) == doctest::Approx(0.0F).epsilon(0.0005));

    // theta=0.8 (beyond outerConeAngle): fully outside the cone -- the
    // saturate() clamp floors this at exactly 0.0, not a negative value.
    CHECK(spotAngleAttenuation(spotDir, travelAt(0.8F), scaleOffset) == doctest::Approx(0.0F).epsilon(0.0001));
}

TEST_CASE("lightmath::spotAngleAttenuation: DISCRIMINATION proof -- a light OUTSIDE the outer cone must "
          "read fully dark (0.0), not merely dim, distinguishing this from a naive linear/unclamped "
          "falloff that would still leak a small positive value there.") {
    const glm::vec3 spotDir(0.0F, 0.0F, -1.0F);
    const SpotAngleScaleOffset scaleOffset = spotAngleScaleOffset(0.1F, 0.3F);  // a narrow cone.
    // theta = PI/2 (light arriving from directly the side -- as far
    // outside the cone as this geometry can express): cd == 0, well
    // outside [cos(outer), cos(inner)].
    const glm::vec3 travelFromSide(0.0F, 1.0F, 0.0F);
    const float attenuation = spotAngleAttenuation(spotDir, travelFromSide, scaleOffset);
    CHECK(attenuation == 0.0F);  // EXACT zero, not approximate -- the saturate() clamp is exact arithmetic here.
}
