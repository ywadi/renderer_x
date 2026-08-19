#include <rx_scene/camera.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace rx::scene {

glm::mat4 Camera::view() const {
    // inverse(T(position) * R(orientation)) = R(orientation)^-1 * T(-position)
    // -- conjugate(orientation) is the inverse of a unit quaternion's
    // rotation, and translate(-position) is the inverse of T(position);
    // composing the two known inverses is exact and cheaper than a
    // general glm::inverse(mat4).
    const glm::mat4 invRotation = glm::mat4_cast(glm::conjugate(orientation));
    const glm::mat4 invTranslation = glm::translate(glm::mat4(1.0F), -position);
    return invRotation * invTranslation;
}

namespace {

// Shared reversed-Z perspective builder. `far` is std::nullopt for the
// infinite-far case (D13's proj()); a real value for the finite-far
// culling case (cullingProj()) -- see this file's own header comment
// (camera.h) for the full A/B-coefficient derivation both branches below
// implement. `jitterX`/`jitterY` are added to the z-coefficient terms of
// the x/y rows (camera.h's own proj() comment); pass {0,0} for
// cullingProj(), which is never jittered.
glm::mat4 reversedZPerspective(float verticalFovRadians, float aspectRatio, float nearZ, const float* far,
                                float jitterX, float jitterY) {
    const float fY = 1.0F / std::tan(verticalFovRadians * 0.5F);
    const float fX = fY / aspectRatio;

    glm::mat4 m(0.0F);
    m[0][0] = fX;
    // Vulkan's clip-space Y axis points down the screen; GLM's own
    // projection helpers (and this hand-written matrix, which follows the
    // same OpenGL-style Y-up convention GLM assumes) need the same
    // `-f` row-1 flip every sample in this repo already applies to
    // glm::perspective()'s own output (see e.g. samples/03_bindless_mesh/
    // main.cpp's makeProjection()) -- baked in directly here rather than
    // left for a caller to apply.
    m[1][1] = -fY;

    if (far == nullptr) {
        // Infinite far [D13]: A = 0, B = nearZ (camera.h's own derivation).
        m[2][2] = 0.0F;
        m[3][2] = nearZ;
    } else {
        // Finite far (cullingProj()): A = near / (far - near),
        // B = near * far / (far - near) -- the standard reversed-Z
        // finite-far matrix (Reed, "Depth Precision Visualized").
        const float farZ = *far;
        const float a = nearZ / (farZ - nearZ);
        m[2][2] = a;
        m[3][2] = a * farZ;
    }
    m[2][3] = -1.0F;

    // Jitter [preserve-later, TAA]: added to the z-coefficients of the
    // x/y rows -- see camera.h's own proj() comment. {0,0} (every Phase 4
    // call) leaves this a no-op.
    m[2][0] += jitterX;
    m[2][1] += jitterY;

    return m;
}

}  // namespace

glm::mat4 Camera::proj() const {
    return reversedZPerspective(verticalFovRadians, aspectRatio, nearPlane, /*far=*/nullptr, jitter.x, jitter.y);
}

glm::mat4 Camera::viewProj() const { return proj() * view(); }

glm::mat4 Camera::cullingProj() const {
    return reversedZPerspective(verticalFovRadians, aspectRatio, nearPlane, &cullingFarPlane, 0.0F, 0.0F);
}

glm::mat4 Camera::cullingViewProj() const { return cullingProj() * view(); }

FrustumPlanes Camera::cullingFrustumPlanes() const { return extractFrustumPlanes(cullingViewProj()); }

namespace {

// Row i (0-indexed) of `m` as a math row-vector: (m[0][i], m[1][i],
// m[2][i], m[3][i]) -- GLM stores mat4 column-major (`m[col][row]`), so
// this is the one place that layout is spelled out explicitly, per
// extractFrustumPlanes()'s own algorithm (Gribb-Hartmann operates on the
// combined matrix's rows).
glm::vec4 matrixRow(const glm::mat4& m, int row) { return glm::vec4(m[0][row], m[1][row], m[2][row], m[3][row]); }

// Normalizes a plane (a,b,c,d) by dividing through by length(a,b,c) so `d`
// becomes a true signed distance offset. Leaves a near-zero-normal plane
// (the documented degenerate case for extractFrustumPlanes() called on an
// infinite-far viewProj() -- see that function's own header comment)
// UNCHANGED rather than dividing by (near-)zero: an unnormalized-but-finite
// plane with a ~0 xyz normal still reads as "always inside" for any
// `dot(plane.xyz, p) + plane.w >= 0` test, which is the correct behavior
// for a degenerate far plane under infinite far (there is no real far
// plane to cull against), just not a numerically "normalized" one.
glm::vec4 normalizePlane(const glm::vec4& plane) {
    const float lengthSq = plane.x * plane.x + plane.y * plane.y + plane.z * plane.z;
    if (lengthSq < 1e-12F) {
        return plane;
    }
    return plane / std::sqrt(lengthSq);
}

}  // namespace

FrustumPlanes extractFrustumPlanes(const glm::mat4& viewProj) {
    const glm::vec4 row0 = matrixRow(viewProj, 0);
    const glm::vec4 row1 = matrixRow(viewProj, 1);
    const glm::vec4 row2 = matrixRow(viewProj, 2);
    const glm::vec4 row3 = matrixRow(viewProj, 3);

    FrustumPlanes planes{};
    planes[static_cast<size_t>(FrustumPlaneIndex::Left)] = normalizePlane(row3 + row0);
    planes[static_cast<size_t>(FrustumPlaneIndex::Right)] = normalizePlane(row3 - row0);
    // Top/Bottom are `row3 +/- row1`, the OPPOSITE pairing from Left/Right's
    // `row3 +/- row0` -- not a copy-paste inconsistency. proj()/cullingProj()
    // bake in Vulkan's Y-flip (`m[1][1] = -fY`, reversedZPerspective() in
    // this file), so row1's sign is already inverted relative to row0's: a
    // view-space point ABOVE the camera (+Y) produces a NEGATIVE clip.y, not
    // positive. `row3 + row1` is therefore the one that goes negative for a
    // point too far ABOVE (the intuitive "Top" boundary: a point beyond it
    // is rejected), and `row3 - row1` is the one that goes negative for a
    // point too far BELOW ("Bottom") -- verified directly in
    // camera_test.cpp's own classification test against concrete
    // above/below points, not assumed from the X-axis pairing.
    planes[static_cast<size_t>(FrustumPlaneIndex::Top)] = normalizePlane(row3 + row1);
    planes[static_cast<size_t>(FrustumPlaneIndex::Bottom)] = normalizePlane(row3 - row1);
    // Vulkan clip-space depth range is [0, w] (z >= 0 test, "near" boundary
    // of the depth range -- NOT the camera's own near plane; see this
    // header's own top comment for why this specific row is the one that
    // degenerates under an infinite-far projection) and (z <= w, "far"
    // boundary of the depth range). D13's reversed-Z convention maps the
    // camera's geometric NEAR plane to depth=1 (the z<=w / row3-row2
    // test) and the geometric FAR plane to depth=0 (the z>=0 / row2 test)
    // -- so FrustumPlaneIndex::Far below is genuinely the row2 test, and
    // FrustumPlaneIndex::Near is genuinely the row3-row2 test, matching
    // Filament's own six-plane ordering by NAME as well as by position.
    planes[static_cast<size_t>(FrustumPlaneIndex::Far)] = normalizePlane(row2);
    planes[static_cast<size_t>(FrustumPlaneIndex::Near)] = normalizePlane(row3 - row2);
    return planes;
}

}  // namespace rx::scene
