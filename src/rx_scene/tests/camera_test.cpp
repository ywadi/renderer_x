#include <doctest/doctest.h>
#include <rx_scene/camera.h>

#include <glm/glm.hpp>

#include <cmath>

// camera_test.cpp -- device-free coverage for rx::scene::Camera [spec D13,
// Phase 4 Stage 2 Task 18; gate matrix-issue05-scene-proxies.md's Camera
// rows]. Every case here is pure GLM math with no GPU/window dependency,
// matching this binary's own doctest_main.cpp comment.

namespace {

// dot(plane.xyz, p) + plane.w -- ">= 0" means "p is on the inside side of
// this plane" (extractFrustumPlanes()'s own documented convention).
// Written out manually rather than via GLM's optional .xyz swizzle (not
// enabled in this project's GLM configuration -- grep confirms no
// GLM_FORCE_SWIZZLE anywhere in this codebase).
float planeDot(const glm::vec4& plane, const glm::vec3& p) {
    return plane.x * p.x + plane.y * p.y + plane.z * p.z + plane.w;
}

float xyzLength(const glm::vec4& plane) { return std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z); }

}  // namespace

TEST_CASE("Camera defaults match D13's documented values") {
    rx::scene::Camera cam;
    CHECK(cam.position == glm::vec3(0.0F, 0.0F, 0.0F));
    CHECK(cam.nearPlane == doctest::Approx(0.1F));
    CHECK(cam.cullMask == ~0U);
    CHECK(cam.jitter == glm::vec2(0.0F, 0.0F));
    CHECK(cam.forward() == glm::vec3(0.0F, 0.0F, -1.0F));
    CHECK(cam.up() == glm::vec3(0.0F, 1.0F, 0.0F));
    CHECK(cam.right() == glm::vec3(1.0F, 0.0F, 0.0F));
}

TEST_CASE("Camera::proj() maps the near plane to NDC depth 1.0 [D13]") {
    rx::scene::Camera cam;
    cam.nearPlane = 1.0F;
    // Identity position/orientation -> view() is identity -> world space
    // == view space for this test, so a world point at (0,0,-near) sits
    // exactly on the near plane along the view axis.
    const glm::mat4 vp = cam.viewProj();
    const glm::vec4 clip = vp * glm::vec4(0.0F, 0.0F, -cam.nearPlane, 1.0F);
    REQUIRE(clip.w != 0.0F);
    CHECK(clip.z / clip.w == doctest::Approx(1.0F).epsilon(0.0001));
}

TEST_CASE("Camera::proj() depth approaches 0.0 as distance grows without bound [D13, infinite far]") {
    rx::scene::Camera cam;
    cam.nearPlane = 1.0F;
    const glm::mat4 vp = cam.viewProj();

    float previousDepth = 1.0F;
    for (float distance : {1.0F, 10.0F, 1'000.0F, 1'000'000.0F, 1e9F}) {
        const glm::vec4 clip = vp * glm::vec4(0.0F, 0.0F, -distance, 1.0F);
        REQUIRE(clip.w != 0.0F);
        const float depth = clip.z / clip.w;
        // Monotonically decreasing with distance (reversed-Z: near->1,
        // far-> 0).
        CHECK(depth <= previousDepth + 1e-6F);
        CHECK(depth >= 0.0F);
        previousDepth = depth;
    }
    // At a huge distance, depth is negligibly close to 0 -- proves the
    // matrix's far plane is genuinely unbounded, not merely "very large".
    CHECK(previousDepth < 1e-5F);
}

TEST_CASE("Camera::cullingProj() maps the FINITE culling far plane to depth 0.0 exactly [gate ruling]") {
    rx::scene::Camera cam;
    cam.nearPlane = 1.0F;
    cam.cullingFarPlane = 100.0F;
    const glm::mat4 cvp = cam.cullingViewProj();

    const glm::vec4 nearClip = cvp * glm::vec4(0.0F, 0.0F, -cam.nearPlane, 1.0F);
    CHECK(nearClip.z / nearClip.w == doctest::Approx(1.0F).epsilon(0.0001));

    const glm::vec4 farClip = cvp * glm::vec4(0.0F, 0.0F, -cam.cullingFarPlane, 1.0F);
    REQUIRE(farClip.w != 0.0F);
    CHECK(farClip.z / farClip.w == doctest::Approx(0.0F).epsilon(0.0001));
}

TEST_CASE("Camera::cullingFrustumPlanes() classifies points correctly [Gribb-Hartmann]") {
    rx::scene::Camera cam;
    cam.verticalFovRadians = glm::radians(90.0F);  // tan(45deg) == 1 -> half-extent == distance, simple arithmetic
    cam.aspectRatio = 1.0F;
    cam.nearPlane = 1.0F;
    cam.cullingFarPlane = 10.0F;
    // Identity position/orientation: view space == world space for every
    // point below.

    const rx::scene::FrustumPlanes planes = cam.cullingFrustumPlanes();

    // Every plane is non-degenerate (finite far -- see the header's own
    // comment on why this differs from viewProj()'s infinite-far case,
    // covered by a separate TEST_CASE below).
    for (const glm::vec4& plane : planes) {
        CHECK(xyzLength(plane) == doctest::Approx(1.0F).epsilon(0.001));
    }

    auto inside = [&](const glm::vec3& p) {
        for (const glm::vec4& plane : planes) {
            if (planeDot(plane, p) < -1e-4F) {
                return false;
            }
        }
        return true;
    };

    // Center of the frustum at half-depth: clearly inside every plane.
    CHECK(inside(glm::vec3(0.0F, 0.0F, -5.0F)));

    // At distance 5, the 90-degree-vfov/1:1-aspect half-extent is exactly
    // 5 in both X and Y (tan(45deg) == 1) -- 6 is just outside.
    const size_t leftIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Left);
    const size_t rightIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Right);
    const size_t topIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Top);
    const size_t bottomIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Bottom);
    const size_t nearIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Near);
    const size_t farIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Far);

    CHECK(planeDot(planes[rightIdx], glm::vec3(6.0F, 0.0F, -5.0F)) < 0.0F);
    CHECK(planeDot(planes[leftIdx], glm::vec3(-6.0F, 0.0F, -5.0F)) < 0.0F);
    CHECK(planeDot(planes[topIdx], glm::vec3(0.0F, 6.0F, -5.0F)) < 0.0F);
    CHECK(planeDot(planes[bottomIdx], glm::vec3(0.0F, -6.0F, -5.0F)) < 0.0F);

    // Closer than the near plane (0.5 < nearPlane 1.0).
    CHECK(planeDot(planes[nearIdx], glm::vec3(0.0F, 0.0F, -0.5F)) < 0.0F);
    // Beyond the culling far plane (11 > cullingFarPlane 10.0).
    CHECK(planeDot(planes[farIdx], glm::vec3(0.0F, 0.0F, -11.0F)) < 0.0F);

    // And the boundary-inside counterparts genuinely pass.
    CHECK(inside(glm::vec3(4.0F, 4.0F, -5.0F)));
    CHECK(planeDot(planes[nearIdx], glm::vec3(0.0F, 0.0F, -1.5F)) >= 0.0F);
    CHECK(planeDot(planes[farIdx], glm::vec3(0.0F, 0.0F, -9.0F)) >= 0.0F);
}

TEST_CASE(
    "extractFrustumPlanes(viewProj()) yields a degenerate Far plane under D13's infinite far, documented not fixed "
    "[gate matrix Conflict #1]") {
    rx::scene::Camera cam;
    cam.verticalFovRadians = glm::radians(90.0F);
    cam.aspectRatio = 1.0F;
    cam.nearPlane = 1.0F;

    const rx::scene::FrustumPlanes planes = rx::scene::extractFrustumPlanes(cam.viewProj());

    const size_t farIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Far);
    // The degenerate plane: near-zero xyz normal (see camera.cpp's own
    // normalizePlane() comment -- left unnormalized/unchanged rather than
    // divided by ~0).
    CHECK(xyzLength(planes[farIdx]) < 1e-4F);
    // A near-zero-normal plane with a positive w reads as "always inside"
    // for any point -- exactly the documented "no real far plane under
    // infinite far" behavior.
    CHECK(planeDot(planes[farIdx], glm::vec3(0.0F, 0.0F, -1e12F)) > 0.0F);

    // Every OTHER plane stays real/non-degenerate -- only Far collapses.
    const size_t leftIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Left);
    const size_t rightIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Right);
    const size_t topIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Top);
    const size_t bottomIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Bottom);
    const size_t nearIdx = static_cast<size_t>(rx::scene::FrustumPlaneIndex::Near);
    CHECK(xyzLength(planes[leftIdx]) == doctest::Approx(1.0F).epsilon(0.001));
    CHECK(xyzLength(planes[rightIdx]) == doctest::Approx(1.0F).epsilon(0.001));
    CHECK(xyzLength(planes[topIdx]) == doctest::Approx(1.0F).epsilon(0.001));
    CHECK(xyzLength(planes[bottomIdx]) == doctest::Approx(1.0F).epsilon(0.001));
    CHECK(xyzLength(planes[nearIdx]) == doctest::Approx(1.0F).epsilon(0.001));
}

TEST_CASE("Camera::jitter is threaded through proj()'s translation terms and is inert by default [preserve-later]") {
    rx::scene::Camera cam;
    cam.nearPlane = 0.5F;
    const glm::mat4 unjittered = cam.proj();

    cam.jitter = glm::vec2(0.125F, -0.375F);
    const glm::mat4 jittered = cam.proj();

    // Only the two documented translation terms differ, by exactly the
    // jitter offset.
    CHECK(jittered[2][0] == doctest::Approx(unjittered[2][0] + 0.125F));
    CHECK(jittered[2][1] == doctest::Approx(unjittered[2][1] - 0.375F));
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            if (col == 2 && (row == 0 || row == 1)) {
                continue;  // the two jittered terms, checked above
            }
            CHECK(jittered[col][row] == doctest::Approx(unjittered[col][row]));
        }
    }

    // cullingProj() is never jittered (a jittered culling frustum would be
    // wrong -- see camera.h's own comment).
    rx::scene::Camera unjitteredCam = cam;
    unjitteredCam.jitter = glm::vec2(0.0F, 0.0F);
    CHECK(cam.cullingProj() == unjitteredCam.cullingProj());
}

TEST_CASE("Camera::view() places the camera at its own local origin looking down its own forward axis") {
    rx::scene::Camera cam;
    cam.position = glm::vec3(3.0F, 4.0F, 5.0F);
    const glm::mat4 v = cam.view();
    // The camera's own position transforms to the view-space origin.
    const glm::vec4 viewSpaceOrigin = v * glm::vec4(cam.position, 1.0F);
    CHECK(viewSpaceOrigin.x == doctest::Approx(0.0F).epsilon(0.0001));
    CHECK(viewSpaceOrigin.y == doctest::Approx(0.0F).epsilon(0.0001));
    CHECK(viewSpaceOrigin.z == doctest::Approx(0.0F).epsilon(0.0001));

    // A world point 10 units along the camera's own forward axis lands on
    // -Z in view space (this engine's view-space convention).
    const glm::vec3 aheadWorld = cam.position + cam.forward() * 10.0F;
    const glm::vec4 aheadView = v * glm::vec4(aheadWorld, 1.0F);
    CHECK(aheadView.x == doctest::Approx(0.0F).epsilon(0.0001));
    CHECK(aheadView.y == doctest::Approx(0.0F).epsilon(0.0001));
    CHECK(aheadView.z == doctest::Approx(-10.0F).epsilon(0.0001));
}
