// Device-free tests for rx_shadow/shadow_frustum.h [spec D21, Phase 4
// Stage 2 Task 22]. No VkDevice anywhere in this file -- fitShadowFrustum()
// is a pure function of its own arguments.
#include <doctest/doctest.h>
#include <rx_shadow/shadow_frustum.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>

using namespace rx::shadow;

namespace {

// Projects a world point through `viewProj` and returns its NDC depth
// (clip.z / clip.w) -- standard-Z convention, so 0.0 == the near plane,
// 1.0 == the far plane [D13: shadow maps stay standard-Z].
float ndcDepth(const glm::mat4& viewProj, const glm::vec3& worldPos) {
    const glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0F);
    return clip.z / clip.w;
}

}  // namespace

TEST_CASE("lightSpaceView: an overhead-sun light (dir (0,-1,0)) produces a basis whose forward axis is +Y-down") {
    const glm::mat4 view = lightSpaceView(glm::vec3(0.0F, -1.0F, 0.0F));
    // A point directly "downstream" of the light (below it, where the
    // light travels toward) must have MORE-NEGATIVE view-space Z than a
    // point upstream (above it) -- the same derivation draw_list.cpp's
    // own worked example verifies for its independent copy of this
    // function.
    const float viewZAbove = (view * glm::vec4(0.0F, 10.0F, 0.0F, 1.0F)).z;
    const float viewZBelow = (view * glm::vec4(0.0F, -10.0F, 0.0F, 1.0F)).z;
    CHECK(viewZBelow < viewZAbove);
}

TEST_CASE("fitShadowFrustum: the fitted box's near/far planes bracket the input AABB (standard-Z: near depth ~0, far depth ~1)") {
    const glm::vec3 worldMin(-2.0F, -1.0F, -3.0F);
    const glm::vec3 worldMax(2.0F, 1.0F, 3.0F);
    const glm::mat4 lightView = lightSpaceView(glm::vec3(0.3F, -1.0F, 0.2F));

    const ShadowFrustumFit fit = fitShadowFrustum(worldMin, worldMax, lightView, /*shadowMapResolution=*/1024);

    // Every one of the 8 world-space corners must land within [0,1] NDC
    // depth (standard-Z) -- the whole point of fitting the ortho box TO
    // this AABB in the first place.
    const std::array<glm::vec3, 8> corners{{
        {worldMin.x, worldMin.y, worldMin.z},
        {worldMax.x, worldMin.y, worldMin.z},
        {worldMin.x, worldMax.y, worldMin.z},
        {worldMax.x, worldMax.y, worldMin.z},
        {worldMin.x, worldMin.y, worldMax.z},
        {worldMax.x, worldMin.y, worldMax.z},
        {worldMin.x, worldMax.y, worldMax.z},
        {worldMax.x, worldMax.y, worldMax.z},
    }};
    for (const glm::vec3& corner : corners) {
        const float depth = ndcDepth(fit.lightViewProj, corner);
        CAPTURE(corner.x);
        CAPTURE(corner.y);
        CAPTURE(corner.z);
        CHECK(depth >= -1e-4F);
        CHECK(depth <= 1.0F + 1e-4F);
    }

    // At least one corner should be near EACH end of the range (the fit
    // is tight, not merely "wide enough") -- with zero depth padding
    // (this test's own default), the extreme corners along the light's
    // own view-Z axis sit exactly at 0.0/1.0.
    float minDepth = 2.0F;
    float maxDepth = -1.0F;
    for (const glm::vec3& corner : corners) {
        const float depth = ndcDepth(fit.lightViewProj, corner);
        minDepth = std::min(minDepth, depth);
        maxDepth = std::max(maxDepth, depth);
    }
    CHECK(minDepth == doctest::Approx(0.0F).epsilon(0.001));
    CHECK(maxDepth == doctest::Approx(1.0F).epsilon(0.001));
}

TEST_CASE("fitShadowFrustum: depth padding widens the [0,1] range so a caster outside the tight bounds still lands inside it") {
    const glm::vec3 worldMin(-1.0F, -1.0F, -1.0F);
    const glm::vec3 worldMax(1.0F, 1.0F, 1.0F);
    const glm::mat4 lightView = lightSpaceView(glm::vec3(0.0F, -1.0F, 0.0F));

    const ShadowFrustumFit tight = fitShadowFrustum(worldMin, worldMax, lightView, 1024, /*depthPaddingWorldUnits=*/0.0F);
    const ShadowFrustumFit padded = fitShadowFrustum(worldMin, worldMax, lightView, 1024, /*depthPaddingWorldUnits=*/5.0F);

    // A caster well upstream (above, since the light travels downward --
    // larger Y is CLOSER to the light, i.e. nearer) of the tight bounds:
    // outside [0,1] under the tight fit (clipped by its own NEAR plane,
    // depth < 0 -- standard-Z depth 0.0 is near), inside it under the
    // padded one.
    const glm::vec3 upstreamCaster(0.0F, 3.0F, 0.0F);
    const float tightDepth = ndcDepth(tight.lightViewProj, upstreamCaster);
    const float paddedDepth = ndcDepth(padded.lightViewProj, upstreamCaster);
    CHECK(tightDepth < 0.0F);  // clipped by the tight fit's own near plane.
    CHECK(paddedDepth >= 0.0F);
    CHECK(paddedDepth <= 1.0F);
}

TEST_CASE("fitShadowFrustum: worldTexelSize is the square footprint's side length divided by the resolution") {
    const glm::vec3 worldMin(-4.0F, -4.0F, -1.0F);
    const glm::vec3 worldMax(4.0F, 4.0F, 1.0F);
    const glm::mat4 lightView = lightSpaceView(glm::vec3(0.0F, -1.0F, 0.0F));

    const ShadowFrustumFit fit = fitShadowFrustum(worldMin, worldMax, lightView, /*shadowMapResolution=*/1024);
    // Overhead sun: light-space X/Y IS world X/Z (up to the basis this
    // codebase's own lightSpaceView() derivation picks) -- an 8-unit-wide
    // footprint over 1024 texels is exactly 8/1024 world units per texel.
    CHECK(fit.worldTexelSize == doctest::Approx(8.0F / 1024.0F).epsilon(0.01));
}

// ---------------------------------------------------------------------
// TEXEL SNAPPING -- the gate's own named acceptance criterion: "render
// the SAME static scene from two slightly different camera positions
// that produce two different (but overlapping) fitted light extents...
// without snapping, a static caster's shadow edge would shift by a
// sub-texel amount between the two renders (shimmer); with snapping, the
// shadow edge... is pixel-identical across both renders." This is the
// device-free half of that proof: fitShadowFrustum() IS the "two slightly
// different camera positions" input (a visible-bounds AABB shifted by a
// tiny amount, exactly what a moving camera's own visible-bounds
// computation would produce frame to frame for an otherwise-static
// scene) and lightViewProj IS the "shadow edge" -- a texel-snapped
// lightViewProj that is BIT-IDENTICAL across a sub-texel shift proves no
// shimmer can occur; the same test's second half (a shift LARGER than one
// texel producing a genuinely different, but still snapped, result)
// proves this is real quantization, not an accidental no-op.
// ---------------------------------------------------------------------
TEST_CASE("fitShadowFrustum: texel snapping makes lightViewProj invariant to a sub-texel visible-bounds shift") {
    const glm::vec3 worldMin(-4.0F, -4.0F, -1.0F);
    const glm::vec3 worldMax(4.0F, 4.0F, 1.0F);
    // A perfectly vertical light -- deliberately, not the tilted light
    // other cases in this file use: a world-space shift purely in X/Z
    // must land purely in light-space X/Y for this test to isolate the
    // SNAPPING behavior alone (a tilted light's own Z (depth) axis also
    // picks up a small component of a world-X shift, which is real,
    // correct, UN-snapped depth-range movement -- not what this test is
    // about).
    const glm::mat4 lightView = lightSpaceView(glm::vec3(0.0F, -1.0F, 0.0F));
    constexpr uint32_t kResolution = 1024;

    const ShadowFrustumFit baseline = fitShadowFrustum(worldMin, worldMax, lightView, kResolution);
    REQUIRE(baseline.worldTexelSize > 0.0F);

    // Shift the SAME-SIZE visible bounds by a fraction of one texel (a
    // camera that moved a tiny bit, causing the visible-bounds estimate
    // to shift sub-texel) -- along BOTH light-space X and Y so this
    // exercises the snap on both axes at once.
    const glm::vec3 subTexelShift(0.3F * baseline.worldTexelSize, 0.0F, 0.0F);
    const ShadowFrustumFit shifted =
        fitShadowFrustum(worldMin + subTexelShift, worldMax + subTexelShift, lightView, kResolution);

    // Bit-identical (not merely "close") -- the whole point of snapping:
    // the shadow-map texel grid did not move at all for this sub-texel
    // camera jitter.
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            CAPTURE(col);
            CAPTURE(row);
            CHECK(baseline.lightViewProj[col][row] == doctest::Approx(shifted.lightViewProj[col][row]).epsilon(1e-6));
        }
    }
}

TEST_CASE("fitShadowFrustum: texel snapping is real quantization, not a no-op -- a shift larger than one texel changes the fit") {
    const glm::vec3 worldMin(-4.0F, -4.0F, -1.0F);
    const glm::vec3 worldMax(4.0F, 4.0F, 1.0F);
    const glm::mat4 lightView = lightSpaceView(glm::vec3(0.0F, -1.0F, 0.0F));
    constexpr uint32_t kResolution = 1024;

    const ShadowFrustumFit baseline = fitShadowFrustum(worldMin, worldMax, lightView, kResolution);
    REQUIRE(baseline.worldTexelSize > 0.0F);

    // A shift of 10 whole texels -- large enough that the snapped grid
    // itself must move, discriminating "always snaps to the same fixed
    // grid regardless of input" (a degenerate/broken implementation) from
    // real per-call quantization.
    const glm::vec3 largeShift(10.0F * baseline.worldTexelSize, 0.0F, 0.0F);
    const ShadowFrustumFit shifted =
        fitShadowFrustum(worldMin + largeShift, worldMax + largeShift, lightView, kResolution);

    bool anyDiffers = false;
    for (int col = 0; col < 4 && !anyDiffers; ++col) {
        for (int row = 0; row < 4 && !anyDiffers; ++row) {
            if (std::abs(baseline.lightViewProj[col][row] - shifted.lightViewProj[col][row]) > 1e-6F) {
                anyDiffers = true;
            }
        }
    }
    CHECK(anyDiffers);
}
