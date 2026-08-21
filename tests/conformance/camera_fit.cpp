#include "camera_fit.h"

#include <algorithm>
#include <cmath>

namespace rx::conformance {

// Port of user_camera.js's fitDistanceToExtents(min, max) [pinned commit,
// source/gltf/user_camera.js] verbatim:
//   const maxAxisLength = Math.max(max[0] - min[0], max[1] - min[1]);
//   const yfov = this.perspective.yfov;
//   const xfov = this.perspective.yfov * (this.perspective.aspectRatio ?? 1);
//   const yZoom = maxAxisLength / 2 / Math.tan(yfov / 2);
//   const xZoom = maxAxisLength / 2 / Math.tan(xfov / 2);
//   this.distance = Math.max(xZoom, yZoom);
// NOTE the original's own deliberate choice: only the X/Y extents feed
// maxAxisLength -- Z (depth, toward/away from the fixed front-on camera
// this algorithm always produces) is NEVER part of the fit. Reproduced
// exactly, not "fixed" -- the whole point of this port is bit-for-bit
// agreement with the actual reference renderer, including this quirk.
namespace {
float fitDistanceToExtents(const glm::vec3& min, const glm::vec3& max, float yfovRadians, float aspectRatio) {
    const float maxAxisLength = std::max(max.x - min.x, max.y - min.y);
    const float xfovRadians = yfovRadians * aspectRatio;
    const float yZoom = maxAxisLength * 0.5F / std::tan(yfovRadians * 0.5F);
    const float xZoom = maxAxisLength * 0.5F / std::tan(xfovRadians * 0.5F);
    return std::max(xZoom, yZoom);
}
}  // namespace

CameraFit fitCameraToScene(const rx::asset::AABB& worldBoundsAABB, float aspectRatio, float yfovRadians) {
    CameraFit fit;
    fit.yfovRadians = yfovRadians;

    if (!worldBoundsAABB.isValid()) {
        return fit;  // struct defaults -- see this function's own header comment.
    }

    const glm::vec3& min = worldBoundsAABB.min;
    const glm::vec3& max = worldBoundsAABB.max;

    const float distance = fitDistanceToExtents(min, max, yfovRadians, aspectRatio);

    // Port of fitCameraTargetToExtents(min, max) [pinned commit]: target is
    // the AABB's own center (ALL three axes, unlike the distance fit above,
    // which only reads X/Y) --
    //   let target = [0, 0, 0];
    //   for (const i of [0, 1, 2]) { target[i] = (max[i] + min[i]) / 2; }
    //   this.setRotation(this.rotAroundY, this.rotAroundX);  // (0, 0) -- identity, resetView()'s own reset.
    //   this.setDistanceFromTarget(this.distance, target);
    // setDistanceFromTarget(), for the IDENTITY transform resetView() always
    // starts from (this.transform = mat4.create() -- resetView()'s own first
    // statement), resolves to a fixed, closed-form position: getLookDirection()
    // reads column 2 of the identity matrix, i.e. (0, 0, 1), negated -> (0, 0,
    // -1); position = target - lookDirection * distance = target + (0, 0,
    // distance). No trigonometry needed here BECAUSE resetView() always resets
    // rotation to identity first (verified directly against user_camera.js's
    // own resetView() body) -- this is not an approximation of the general
    // orbit-camera case, it is the exact closed form for THIS specific,
    // always-un-rotated call path.
    const glm::vec3 target(0.5F * (max.x + min.x), 0.5F * (max.y + min.y), 0.5F * (max.z + min.z));
    const glm::vec3 position = target + glm::vec3(0.0F, 0.0F, distance);

    // Port of fitCameraPlanesToExtents(min, max) [pinned commit] verbatim:
    //   const longestDistance = 10 * vec3.distance(min, max);
    //   let zNear = this.distance - longestDistance * 0.6;
    //   let zFar = this.distance + longestDistance * 0.6;
    //   zNear = Math.max(zNear, zFar / MaxNearFarRatio);   // MaxNearFarRatio = 10000
    const float longestDistance = 10.0F * glm::length(max - min);
    float znear = distance - longestDistance * 0.6F;
    const float zfar = distance + longestDistance * 0.6F;
    constexpr float kMaxNearFarRatio = 10000.0F;
    znear = std::max(znear, zfar / kMaxNearFarRatio);

    fit.position = position;
    fit.target = target;
    fit.znear = znear;
    fit.zfar = zfar;
    return fit;
}

}  // namespace rx::conformance
