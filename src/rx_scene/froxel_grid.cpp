#include <rx_scene/froxel_grid.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace rx::scene::froxel {
namespace {

// Froxelizer.cpp:679-680 (v1.75.0): "We use minimum cone angle of 0.5
// degrees because too small angles cause issues in the sphere/cone
// intersection test, due to floating-point precision." -- literal
// constants, not re-derived (matching the source's own comment: these are
// `1 / sin(0.5deg)` and `cos(0.5deg)^2` respectively).
constexpr float kMaxInvSin = 114.59301F;
constexpr float kMaxCosSquared = 0.99992385F;

uint32_t roundUpToMultiple(uint32_t v, uint32_t multiple) {
    return ((v + multiple - 1) / multiple) * multiple;
}

}  // namespace

// Port of Froxelizer::computeFroxelLayout() [Froxelizer.cpp:283-322,
// google/filament v1.75.0, 0e58877c09afb1aacd09ff640f74d2adcd2a7e80]. This
// engine has no "8-pixel-tile-for-shader-vectorization" concern Filament's
// own `roundTo8()` step targets (that rounding exists to keep Filament's
// OWN fragment-shader-side froxel index computation cheap on GPUs of that
// era) -- kept anyway, verbatim, since it is part of the SAME closed-form
// "square froxels tiling the viewport, sized from a total-entry budget"
// derivation the matrix's own acceptance criterion asks for, and changing
// it would make this port harder to cross-check against the source.
std::pair<uint32_t, uint32_t> computeFroxelGridXY(uint32_t viewportWidth, uint32_t viewportHeight,
                                                    uint32_t targetFroxelEntryCount, uint32_t sliceCountZ) {
    const uint32_t width = std::max(16U, viewportWidth);
    const uint32_t height = std::max(16U, viewportHeight);
    const uint32_t sliceCount = std::max(1U, sliceCountZ);

    const uint32_t froxelPlaneCount = std::max(1U, targetFroxelEntryCount / sliceCount);

    // solving: froxelCountX * froxelCountY == froxelPlaneCount
    //          froxelCountX / froxelCountY == width / height
    auto froxelCountX =
        static_cast<uint32_t>(std::sqrt(static_cast<double>(froxelPlaneCount) * width / height));
    auto froxelCountY =
        static_cast<uint32_t>(std::sqrt(static_cast<double>(froxelPlaneCount) * height / width));
    froxelCountX = std::max(1U, froxelCountX);
    froxelCountY = std::max(1U, froxelCountY);

    const uint32_t froxelSizeX = (width + froxelCountX - 1) / froxelCountX;
    const uint32_t froxelSizeY = (height + froxelCountY - 1) / froxelCountY;

    const uint32_t froxelDimension =
        roundUpToMultiple((roundUpToMultiple(froxelSizeX, 8) >= froxelSizeY) ? froxelSizeX : froxelSizeY, 8);

    froxelCountX = (width + froxelDimension - 1) / froxelDimension;
    froxelCountY = (height + froxelDimension - 1) / froxelDimension;

    return {froxelCountX, froxelCountY};
}

FroxelGridParams buildFroxelGrid(uint32_t viewportWidth, uint32_t viewportHeight, float verticalFovRadians,
                                  float aspectRatio, float zLightNear, float zLightFar,
                                  uint32_t targetFroxelEntryCount, uint32_t sliceCountZ) {
    FroxelGridParams grid;
    std::tie(grid.countX, grid.countY) =
        computeFroxelGridXY(viewportWidth, viewportHeight, targetFroxelEntryCount, sliceCountZ);
    grid.countZ = std::max(1U, sliceCountZ);

    grid.tanHalfFovY = std::tan(verticalFovRadians * 0.5F);
    grid.aspectRatio = aspectRatio;

    // Froxelizer.cpp:400-411's own sanitization (zLightFar < zLightNear ->
    // swap) simplified: this device-free helper trusts its caller (unlike
    // Froxelizer::update(), which also clamps against the camera's own
    // near/far -- this engine's Camera has no finite far at all, D13's
    // infinite-far projection, so there is no equivalent clamp target).
    grid.zLightNear = std::min(zLightNear, zLightFar);
    grid.zLightFar = std::max(zLightNear, zLightFar);
    if (grid.zLightNear <= 0.0F) {
        grid.zLightNear = std::min(0.01F, grid.zLightFar);
    }

    // Froxelizer.cpp:466: `linearizer = log2(zFar/zNear) / max(1, countZ-1)`.
    grid.linearizer = std::log2(grid.zLightFar / grid.zLightNear) / static_cast<float>(std::max(1U, grid.countZ - 1));
    grid.invLinearizer = grid.linearizer != 0.0F ? 1.0F / grid.linearizer : 0.0F;

    return grid;
}

uint32_t froxelIndex(uint32_t x, uint32_t y, uint32_t z, const FroxelGridParams& grid) {
    return x + (y * grid.countX) + (z * grid.countX * grid.countY);
}

glm::uvec3 froxelCoords(uint32_t index, const FroxelGridParams& grid) {
    const uint32_t plane = grid.countX * grid.countY;
    const uint32_t z = plane != 0 ? index / plane : 0;
    const uint32_t rem = plane != 0 ? index % plane : 0;
    const uint32_t y = grid.countX != 0 ? rem / grid.countX : 0;
    const uint32_t x = grid.countX != 0 ? rem % grid.countX : 0;
    return {x, y, z};
}

float sliceZDistance(uint32_t i, const FroxelGridParams& grid) {
    if (i == 0) {
        return 0.0F;
    }
    // Froxelizer.cpp:472-474: `mDistancesZ[i] = zFar * exp2(float(i-n) *
    // linearizer)` for i in [1,n], n=countZ. At i==1: exponent =
    // (1-n)*linearizer = -log2(zFar/zNear) (since linearizer =
    // log2(zFar/zNear)/(n-1)), so distance = zFar * (zNear/zFar) = zNear
    // exactly. At i==n: exponent = 0, distance = zFar exactly. Verified by
    // froxel_grid_test.cpp's own closed-form assertions at both endpoints.
    const auto n = static_cast<float>(grid.countZ);
    const auto fi = static_cast<float>(i);
    return grid.zLightFar * std::exp2((fi - n) * grid.linearizer);
}

uint32_t findSliceZ(float viewSpaceZ, const FroxelGridParams& grid) {
    // Froxelizer.cpp:580-598, bit-for-bit (fast::log2 -> std::log2; this
    // device-free helper has no vectorized-fast-math counterpart to match,
    // and correctness here is proven against exact closed-form values, not
    // against Filament's own approximate fast::log2 implementation).
    int s = static_cast<int>(std::log2(-viewSpaceZ / grid.zLightFar) * grid.invLinearizer +
                              static_cast<float>(grid.countZ));
    s = viewSpaceZ < 0.0F ? s : 0;
    s = std::clamp(s, 0, static_cast<int>(grid.countZ) - 1);
    return static_cast<uint32_t>(s);
}

FroxelBounds froxelViewSpaceBounds(uint32_t x, uint32_t y, uint32_t z, const FroxelGridParams& grid) {
    const float ndcX0 = -1.0F + 2.0F * static_cast<float>(x) / static_cast<float>(grid.countX);
    const float ndcX1 = -1.0F + 2.0F * static_cast<float>(x + 1) / static_cast<float>(grid.countX);
    const float ndcY0 = -1.0F + 2.0F * static_cast<float>(y) / static_cast<float>(grid.countY);
    const float ndcY1 = -1.0F + 2.0F * static_cast<float>(y + 1) / static_cast<float>(grid.countY);

    const float dNear = sliceZDistance(z, grid);
    const float dFar = sliceZDistance(z + 1, grid);

    const float tanHalfFovX = grid.tanHalfFovY * grid.aspectRatio;

    std::array<glm::vec3, 8> corners{};
    size_t idx = 0;
    for (float ndcX : {ndcX0, ndcX1}) {
        for (float ndcY : {ndcY0, ndcY1}) {
            for (float d : {dNear, dFar}) {
                corners[idx++] = glm::vec3{ndcX * d * tanHalfFovX, ndcY * d * grid.tanHalfFovY, -d};
            }
        }
    }

    glm::vec3 center{0.0F};
    for (const auto& c : corners) {
        center += c;
    }
    center *= (1.0F / 8.0F);

    float maxDist2 = 0.0F;
    for (const auto& c : corners) {
        const glm::vec3 delta = c - center;
        maxDist2 = std::max(maxDist2, glm::dot(delta, delta));
    }

    return FroxelBounds{center, std::sqrt(maxDist2)};
}

float pointLightCullRadius(glm::vec3 colorCandela, float range, float cutoffLux) {
    if (range > 0.0F) {
        return range;
    }
    const float maxIntensity = std::max({colorCandela.r, colorCandela.g, colorCandela.b, 0.0F});
    if (maxIntensity <= 0.0F || cutoffLux <= 0.0F) {
        return 0.0F;
    }
    return std::sqrt(maxIntensity / cutoffLux);
}

bool sphereIntersectsFroxel(glm::vec3 lightViewPos, float lightRadius, const FroxelBounds& froxel) {
    const glm::vec3 d = lightViewPos - froxel.center;
    const float rr = lightRadius + froxel.radius;
    return glm::dot(d, d) <= rr * rr;
}

SpotConeViewSpace spotConeViewSpace(glm::vec3 axisView, float outerConeAngle) {
    const float s = std::sin(outerConeAngle);
    const float c = std::cos(outerConeAngle);
    SpotConeViewSpace cone;
    cone.axis = glm::normalize(axisView);
    // Froxelizer.cpp:693-694: `light.invSin = min(maxInvSin, light.invSin)`
    // (guarding the divide against a near-zero sin for a near-0-degree cone).
    cone.sinInverse = std::min(kMaxInvSin, s > 0.0F ? 1.0F / s : kMaxInvSin);
    // Froxelizer.cpp:687: `light.cosSqr = min(maxCosSquared, params.x)`.
    cone.cosSquared = std::min(kMaxCosSquared, c * c);
    return cone;
}

bool spotIntersectsFroxel(glm::vec3 lightViewPos, float lightRadius, const SpotConeViewSpace& cone,
                           const FroxelBounds& froxel) {
    if (!sphereIntersectsFroxel(lightViewPos, lightRadius, froxel)) {
        return false;
    }
    // Intersections.h:50-62 `sphereConeIntersectionFast()`, ported
    // verbatim -- FROXEL is the tested "sphere" (froxel.center/radius),
    // LIGHT is the cone's own origin/axis, matching Froxelizer.cpp:985-986's
    // own call-site argument roles exactly.
    const glm::vec3 u = lightViewPos - (froxel.radius * cone.sinInverse) * cone.axis;
    const glm::vec3 d = froxel.center - u;
    const float e = glm::dot(cone.axis, d);
    const float dd = glm::dot(d, d);
    return (e * e >= dd * cone.cosSquared) && (e > 0.0F);
}

}  // namespace rx::scene::froxel
