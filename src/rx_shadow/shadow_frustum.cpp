#include <rx_shadow/shadow_frustum.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace rx::shadow {

glm::mat4 lightSpaceView(const glm::vec3& lightDir) {
    const glm::vec3 forward = glm::normalize(lightDir);
    glm::vec3 upHint(0.0F, 1.0F, 0.0F);
    if (std::abs(glm::dot(forward, upHint)) > 0.99F) {
        upHint = glm::vec3(0.0F, 0.0F, 1.0F);
    }
    return glm::lookAt(glm::vec3(0.0F), forward, upHint);
}

ShadowFrustumFit fitShadowFrustum(const glm::vec3& visibleBoundsWorldMin, const glm::vec3& visibleBoundsWorldMax,
                                    const glm::mat4& lightView, uint32_t shadowMapResolution,
                                    float depthPaddingWorldUnits) {
    // Transform all 8 world-space AABB corners into light-eye-space and
    // take their own AABB -- the standard "project frustum/box corners
    // into light space" technique (GPU Gems 3 ch.10, cited by this
    // library's own header comment), correct regardless of the box's
    // orientation relative to the light (an axis-aligned WORLD box is not
    // generally axis-aligned in light-eye-space).
    const std::array<glm::vec3, 8> corners{{
        {visibleBoundsWorldMin.x, visibleBoundsWorldMin.y, visibleBoundsWorldMin.z},
        {visibleBoundsWorldMax.x, visibleBoundsWorldMin.y, visibleBoundsWorldMin.z},
        {visibleBoundsWorldMin.x, visibleBoundsWorldMax.y, visibleBoundsWorldMin.z},
        {visibleBoundsWorldMax.x, visibleBoundsWorldMax.y, visibleBoundsWorldMin.z},
        {visibleBoundsWorldMin.x, visibleBoundsWorldMin.y, visibleBoundsWorldMax.z},
        {visibleBoundsWorldMax.x, visibleBoundsWorldMin.y, visibleBoundsWorldMax.z},
        {visibleBoundsWorldMin.x, visibleBoundsWorldMax.y, visibleBoundsWorldMax.z},
        {visibleBoundsWorldMax.x, visibleBoundsWorldMax.y, visibleBoundsWorldMax.z},
    }};

    glm::vec3 lsMin(std::numeric_limits<float>::max());
    glm::vec3 lsMax(std::numeric_limits<float>::lowest());
    for (const glm::vec3& corner : corners) {
        const glm::vec3 ls = glm::vec3(lightView * glm::vec4(corner, 1.0F));
        lsMin = glm::min(lsMin, ls);
        lsMax = glm::max(lsMax, ls);
    }

    // SQUARE FOOTPRINT [this header's own comment]: the larger of the two
    // half-extents drives BOTH axes, centered on the box's own true X/Y
    // center (not re-centered to 0) so a non-square input AABB still gets
    // a symmetric, centered square footprint.
    const glm::vec2 center2(0.5F * (lsMin.x + lsMax.x), 0.5F * (lsMin.y + lsMax.y));
    const float halfExtent = 0.5F * std::max(lsMax.x - lsMin.x, lsMax.y - lsMin.y);
    const float worldTexelSize =
        shadowMapResolution > 0 ? (2.0F * halfExtent) / static_cast<float>(shadowMapResolution) : 0.0F;

    // TEXEL SNAPPING: round the center to the nearest whole multiple of
    // worldTexelSize, per axis -- see this header's own comment for why
    // std::round() (symmetric) rather than floor/ceil.
    glm::vec2 snappedCenter = center2;
    if (worldTexelSize > 0.0F) {
        snappedCenter.x = std::round(center2.x / worldTexelSize) * worldTexelSize;
        snappedCenter.y = std::round(center2.y / worldTexelSize) * worldTexelSize;
    }

    const float left = snappedCenter.x - halfExtent;
    const float right = snappedCenter.x + halfExtent;
    const float bottom = snappedCenter.y - halfExtent;
    const float top = snappedCenter.y + halfExtent;

    // DEPTH RANGE PADDING [this header's own comment]: pad both ends of
    // the light-space Z range by `depthPaddingWorldUnits` before
    // converting to glm::orthoZO()'s own "positive distance in front of
    // the eye" near/far convention (a right-handed view space looking
    // down -Z: distance-in-front = -viewZ, so the LARGER (least negative)
    // viewZ -- lsMax.z -- is the NEAR distance, and the SMALLER (most
    // negative) viewZ -- lsMin.z -- is the FAR distance).
    const float nearDistance = -lsMax.z - depthPaddingWorldUnits;
    const float farDistance = -lsMin.z + depthPaddingWorldUnits;

    ShadowFrustumFit fit;
    fit.lightView = lightView;
    fit.lightProj = glm::orthoZO(left, right, bottom, top, nearDistance, farDistance);
    // Vulkan's clip-space Y axis points down; GLM's ortho helpers assume
    // OpenGL's Y-up convention -- the same row-1 flip every projection
    // matrix in this codebase applies (rx::scene::Camera::proj() bakes
    // the equivalent in directly; samples apply it as a separate step).
    fit.lightProj[1][1] *= -1.0F;
    fit.lightViewProj = fit.lightProj * fit.lightView;
    fit.worldTexelSize = worldTexelSize;
    return fit;
}

}  // namespace rx::shadow
