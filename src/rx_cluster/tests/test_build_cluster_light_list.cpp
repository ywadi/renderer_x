// src/rx_cluster/tests/test_build_cluster_light_list.cpp -- Phase 5 Stage 2
// Task 14 [#50]: device-free unit tests for rx::cluster::buildClusterLightList()
// -- the CPU-side step that filters a Scene's live lights down to
// Point/Spot only, transforms them into view space, and derives each
// light's cull radius. No VkDevice needed (this function touches no
// Vulkan type at all) -- the GPU membership tests (test_cluster_membership_gpu.cpp)
// instead directly construct `ClusterLightGpu` vectors for full scenario
// control; this file proves the SCENE -> GPU-LIGHT-LIST pipeline itself.
#include <doctest/doctest.h>
#include <rx_cluster/cluster_lighting.h>
#include <rx_scene/scene.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

using namespace rx::cluster;

namespace {
rx::asset::AABB fallbackShapedBounds(rx::asset::MeshHandle) { return rx::asset::AABB{}; }
}  // namespace

TEST_CASE("buildClusterLightList(): Directional lights never participate (never appear in the "
          "output), Point/Spot are transformed into VIEW SPACE via the supplied view matrix") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    (void)scene.createDirectionalLight(rx::scene::DirectionalLightDesc{});

    rx::scene::PointLightDesc pointDesc;
    pointDesc.position = glm::vec3(5.0F, 0.0F, 0.0F);
    pointDesc.colorCandela = glm::vec3(100.0F);
    pointDesc.range = 12.5F;
    (void)scene.createPointLight(pointDesc);

    // A view matrix that translates by (-5,0,0) -- the point light's world
    // position (5,0,0) must land at view-space origin.
    const glm::mat4 view = glm::translate(glm::mat4(1.0F), glm::vec3(-5.0F, 0.0F, 0.0F));

    auto lights = buildClusterLightList(scene, view);
    REQUIRE(lights.size() == 1);  // the Directional light is excluded.
    CHECK(lights[0].viewPositionRadius.x == doctest::Approx(0.0F));
    CHECK(lights[0].viewPositionRadius.y == doctest::Approx(0.0F));
    CHECK(lights[0].viewPositionRadius.z == doctest::Approx(0.0F));
    // Explicit range wins verbatim as the cull radius.
    CHECK(lights[0].viewPositionRadius.w == doctest::Approx(12.5F));
    CHECK(lights[0].cosSquaredFlags.y == doctest::Approx(0.0F));  // isSpot == 0 (point).
}

TEST_CASE("buildClusterLightList(): a dead (destroyLight()d) slot is excluded even though its stale "
          "LightRecord content is still physically present in Scene's own storage -- proves this "
          "function gates on lightAliveSpan(), not merely iterates lightRecordsSpan()") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::PointLightDesc desc;
    desc.position = glm::vec3(1.0F, 2.0F, 3.0F);
    rx::scene::LightHandle handle = scene.createPointLight(desc);
    scene.destroyLight(handle);

    auto lights = buildClusterLightList(scene, glm::mat4(1.0F));
    CHECK(lights.empty());
}

TEST_CASE("buildClusterLightList(): Spot light cone data (axis/sinInverse/cosSquared) is populated "
          "and rotated into view space; isSpot flag is exactly 1.0") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::SpotLightDesc spotDesc;
    spotDesc.position = glm::vec3(0.0F);
    spotDesc.dir = glm::vec3(1.0F, 0.0F, 0.0F);  // points down world +X.
    spotDesc.colorCandela = glm::vec3(500.0F);
    spotDesc.range = 20.0F;
    spotDesc.outerConeAngle = glm::radians(30.0F);
    (void)scene.createSpotLight(spotDesc);

    // A view matrix that rotates world +X onto view +Z (a -90-degree yaw
    // around Y, verified independently via a standalone glm program before
    // writing this assertion: glm::mat3(mat4_cast(angleAxis(-90deg, Y))) *
    // vec3(1,0,0) == (0,0,1)) -- pure rotation, so this also exercises that
    // ONLY the 3x3 upper-left is used for direction (never translation).
    const glm::mat4 view = glm::mat4_cast(glm::angleAxis(glm::radians(-90.0F), glm::vec3(0.0F, 1.0F, 0.0F)));

    auto lights = buildClusterLightList(scene, view);
    REQUIRE(lights.size() == 1);
    CHECK(lights[0].cosSquaredFlags.y == doctest::Approx(1.0F));  // isSpot == 1.
    // world +X rotated by this view -> view +Z (within float tolerance).
    CHECK(lights[0].viewAxisSinInverse.x == doctest::Approx(0.0F).epsilon(0.001));
    CHECK(lights[0].viewAxisSinInverse.y == doctest::Approx(0.0F).epsilon(0.001));
    CHECK(lights[0].viewAxisSinInverse.z == doctest::Approx(1.0F).epsilon(0.001));
    // sin(30deg)=0.5 -> invSin=2.0; cos(30deg)^2=0.75 (froxel_grid_test.cpp's
    // own spotConeViewSpace() hand-derivation, reused here).
    CHECK(lights[0].viewAxisSinInverse.w == doctest::Approx(2.0F).epsilon(0.0005));
    CHECK(lights[0].cosSquaredFlags.x == doctest::Approx(0.75F).epsilon(0.0005));
    // Explicit range wins verbatim as the cull radius.
    CHECK(lights[0].viewPositionRadius.w == doctest::Approx(20.0F));
}

TEST_CASE("buildClusterLightList(): unconfigured range (<=0) derives the cull radius from candela "
          "intensity at the default 1.0 lux threshold -- mirrors froxel_grid_test.cpp's own "
          "pointLightCullRadius() proof, exercised here through the full Scene pipeline") {
    rx::scene::Scene scene(&fallbackShapedBounds);
    rx::scene::PointLightDesc desc;
    desc.colorCandela = glm::vec3(100.0F, 50.0F, 25.0F);
    desc.range = 0.0F;
    (void)scene.createPointLight(desc);

    auto lights = buildClusterLightList(scene, glm::mat4(1.0F));
    REQUIRE(lights.size() == 1);
    // sqrt(maxComponent(100,50,25) / 1.0) == 10.0.
    CHECK(lights[0].viewPositionRadius.w == doctest::Approx(10.0F).epsilon(0.0001));
}
