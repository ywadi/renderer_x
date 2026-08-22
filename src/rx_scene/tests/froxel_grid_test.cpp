// src/rx_scene/tests/froxel_grid_test.cpp -- Phase 5 Stage 2 Task 14 [#50]:
// device-free unit tests for rx_scene/froxel_grid.h's camera-frustum froxel
// grid math. Every expected value below was computed INDEPENDENTLY
// (python3, double precision -- see task-14-report.md for the exact
// scripts) directly from Filament's own closed-form derivation
// (Froxelizer.cpp, google/filament v1.75.0, 0e58877c09afb1aacd09ff640f74d2adcd2a7e80)
// -- NOT reverse-derived from this function's own output -- matching this
// codebase's own established TDD discipline (light_math_test.cpp's own
// precedent, task-13-report.md).
#include <doctest/doctest.h>
#include <rx_scene/froxel_grid.h>

#include <glm/glm.hpp>

#include <cmath>

using namespace rx::scene::froxel;

TEST_CASE("computeFroxelGridXY matches Filament's own computeFroxelLayout() derivation "
          "[Froxelizer.cpp:283-322] for three independently hand/python-computed cases") {
    // python: compute_xy(512, 512, 8192, 16) -> (22, 22, 24) [countX, countY, tileDim]
    {
        auto [cx, cy] = computeFroxelGridXY(512, 512, 8192, 16);
        CHECK(cx == 22);
        CHECK(cy == 22);
    }
    // python: compute_xy(1920, 1080, 8192, 16) -> (27, 15, 72) -- the real
    // 1080p-viewport case this task's own GPU stress scenes use.
    {
        auto [cx, cy] = computeFroxelGridXY(1920, 1080, 8192, 16);
        CHECK(cx == 27);
        CHECK(cy == 15);
    }
    // python: compute_xy(64, 64, 64, 4) -> (4, 4, 16) -- the tiny grid this
    // file's own bounds/round-trip tests below use (small enough to
    // enumerate every froxel by hand).
    {
        auto [cx, cy] = computeFroxelGridXY(64, 64, 64, 4);
        CHECK(cx == 4);
        CHECK(cy == 4);
    }
}

TEST_CASE("froxelIndex<->froxelCoords round-trip for EVERY froxel in a real grid shape -- the "
          "plan's own named acceptance criterion (plan:499-500: \"index<->slice round-trips\")") {
    const FroxelGridParams grid = buildFroxelGrid(1920, 1080, glm::radians(60.0F), 16.0F / 9.0F, 5.0F, 100.0F);
    REQUIRE(grid.countX == 27);
    REQUIRE(grid.countY == 15);
    REQUIRE(grid.countZ == 16);

    uint32_t checked = 0;
    for (uint32_t z = 0; z < grid.countZ; ++z) {
        for (uint32_t y = 0; y < grid.countY; ++y) {
            for (uint32_t x = 0; x < grid.countX; ++x) {
                const uint32_t idx = froxelIndex(x, y, z, grid);
                const glm::uvec3 coords = froxelCoords(idx, grid);
                CHECK(coords.x == x);
                CHECK(coords.y == y);
                CHECK(coords.z == z);
                ++checked;
            }
        }
    }
    CHECK(checked == grid.froxelCount());
    CHECK(checked == 27U * 15U * 16U);

    // Corner/boundary indices, named explicitly (not just the sweep above):
    // the (0,0,0) origin froxel and the last froxel (countX-1,countY-1,countZ-1).
    CHECK(froxelIndex(0, 0, 0, grid) == 0);
    CHECK(froxelIndex(grid.countX - 1, grid.countY - 1, grid.countZ - 1, grid) == grid.froxelCount() - 1);
}

TEST_CASE("sliceZDistance matches Filament's own exponential Z-slicing formula "
          "[Froxelizer.cpp:463-477] at the near/far ANALYTIC endpoints plus two "
          "independently-python-computed midpoints") {
    const FroxelGridParams grid = buildFroxelGrid(1920, 1080, glm::radians(60.0F), 16.0F / 9.0F, 5.0F, 100.0F);
    REQUIRE(grid.countZ == 16);

    // i==0 is the near sentinel: EXACTLY 0.0 (Froxelizer.cpp:463, the "distance from the camera's own
    // view plane" boundary of slice 0, not part of the exponential distribution).
    CHECK(sliceZDistance(0, grid) == doctest::Approx(0.0F).epsilon(0.0001));
    // i==1 resolves to EXACTLY zLightNear by construction (see froxel_grid.cpp's own derivation
    // comment on sliceZDistance()) -- this is the "depth-slice distribution matches the ported
    // reference's formula" acceptance criterion's own near-boundary case.
    CHECK(sliceZDistance(1, grid) == doctest::Approx(5.0F).epsilon(0.0001));
    // i==countZ resolves to EXACTLY zLightFar.
    CHECK(sliceZDistance(grid.countZ, grid) == doctest::Approx(100.0F).epsilon(0.0001));

    // python: slice_dist(8, 16, 100.0, 5.0) -> 20.235658223516168
    CHECK(sliceZDistance(8, grid) == doctest::Approx(20.235658F).epsilon(0.0001));
    // python: slice_dist(15, 16, 100.0, 5.0) -> 81.89637274779153
    CHECK(sliceZDistance(15, grid) == doctest::Approx(81.896373F).epsilon(0.0001));

    // Monotonically increasing across every boundary -- a real invariant of
    // the exponential distribution, not merely "plausible".
    float prev = sliceZDistance(0, grid);
    for (uint32_t i = 1; i <= grid.countZ; ++i) {
        const float cur = sliceZDistance(i, grid);
        CHECK(cur > prev);
        prev = cur;
    }
}

TEST_CASE("findSliceZ is a bit-for-bit port of Froxelizer::findSliceZ() [Froxelizer.cpp:580-598] "
          "-- independently python-computed table spanning the near-plane boundary, mid-range, "
          "far-plane boundary/clamp, AND the behind-camera domain-boundary case") {
    const FroxelGridParams grid = buildFroxelGrid(1920, 1080, glm::radians(60.0F), 16.0F / 9.0F, 5.0F, 100.0F);
    REQUIRE(grid.countZ == 16);

    // python find_slice() table (viewZ -> expected slice):
    CHECK(findSliceZ(-0.01F, grid) == 0);
    CHECK(findSliceZ(-4.999F, grid) == 0);
    CHECK(findSliceZ(-5.0F, grid) == 1);
    CHECK(findSliceZ(-5.001F, grid) == 1);
    CHECK(findSliceZ(-50.0F, grid) == 12);
    CHECK(findSliceZ(-99.99F, grid) == 15);
    // Beyond zLightFar: clamped to the LAST slice, never out of range or wrapped.
    CHECK(findSliceZ(-100.0F, grid) == 15);
    CHECK(findSliceZ(-150.0F, grid) == 15);

    // Domain-boundary case [matrix row 1's own acceptance text: "the
    // near-plane and far-plane boundary cases" -- extended here to the
    // BEHIND-CAMERA case the matrix's GPU-membership row also names]:
    // viewSpaceZ >= 0 (at or behind the camera) resolves to slice 0 per
    // Froxelizer.cpp:594's own domain-boundary handling -- TRUE exclusion
    // of a behind-camera light is the sphere-test's job (see
    // test_cluster_membership_gpu.cpp), not this function's.
    CHECK(findSliceZ(0.0F, grid) == 0);
    CHECK(findSliceZ(5.0F, grid) == 0);
}

TEST_CASE("froxelViewSpaceBounds matches an INDEPENDENTLY hand/python-derived center+radius for a "
          "small, exactly-verifiable grid (countX=2, countY=1, countZ=1, tanHalfFovY=1.0, "
          "aspectRatio=2.0, zLightNear==zLightFar==1.0)") {
    // buildFroxelGrid() always applies its own XY-budget derivation, so this
    // test constructs FroxelGridParams DIRECTLY (bypassing buildFroxelGrid())
    // to pin the exact grid shape the hand computation below assumes.
    FroxelGridParams grid;
    grid.countX = 2;
    grid.countY = 1;
    grid.countZ = 1;
    grid.tanHalfFovY = 1.0F;
    grid.aspectRatio = 2.0F;
    grid.zLightNear = 1.0F;
    grid.zLightFar = 1.0F;
    grid.linearizer = 0.0F;  // countZ==1 -> log2(1/1)/max(1,0) == 0, matching buildFroxelGrid()'s own formula.
    grid.invLinearizer = 0.0F;

    // Sanity: sliceZDistance(0)==0 (sentinel), sliceZDistance(1)==zLightFar==1.0
    // exactly (i==countZ==1 -> exponent (1-1)*0==0).
    REQUIRE(sliceZDistance(0, grid) == doctest::Approx(0.0F));
    REQUIRE(sliceZDistance(1, grid) == doctest::Approx(1.0F));

    // python hand-derivation (this file's own header comment / task-14-report.md):
    // center=(-0.5, 0.0, -0.5), radius=1.8708286933869707.
    const FroxelBounds bounds = froxelViewSpaceBounds(0, 0, 0, grid);
    CHECK(bounds.center.x == doctest::Approx(-0.5F).epsilon(0.0001));
    CHECK(bounds.center.y == doctest::Approx(0.0F).epsilon(0.0001));
    CHECK(bounds.center.z == doctest::Approx(-0.5F).epsilon(0.0001));
    CHECK(bounds.radius == doctest::Approx(1.8708287F).epsilon(0.0001));
}

TEST_CASE("froxelViewSpaceBounds: every froxel's own 8 corners (recomputed independently in this "
          "test, not by calling the function under test) lie within its returned bounding sphere, "
          "swept across a REAL grid shape -- the structural invariant a per-froxel bounding VOLUME "
          "must satisfy for the sphere-intersection tests to be conservative (never a false negative)") {
    const FroxelGridParams grid = buildFroxelGrid(320, 180, glm::radians(70.0F), 16.0F / 9.0F, 2.0F, 40.0F, 64, 4);
    const float tanHalfFovX = grid.tanHalfFovY * grid.aspectRatio;

    for (uint32_t z = 0; z < grid.countZ; ++z) {
        for (uint32_t y = 0; y < grid.countY; ++y) {
            for (uint32_t x = 0; x < grid.countX; ++x) {
                const FroxelBounds bounds = froxelViewSpaceBounds(x, y, z, grid);

                const float ndcX0 = -1.0F + 2.0F * static_cast<float>(x) / static_cast<float>(grid.countX);
                const float ndcX1 = -1.0F + 2.0F * static_cast<float>(x + 1) / static_cast<float>(grid.countX);
                const float ndcY0 = -1.0F + 2.0F * static_cast<float>(y) / static_cast<float>(grid.countY);
                const float ndcY1 = -1.0F + 2.0F * static_cast<float>(y + 1) / static_cast<float>(grid.countY);
                const float dNear = sliceZDistance(z, grid);
                const float dFar = sliceZDistance(z + 1, grid);

                for (float ndcX : {ndcX0, ndcX1}) {
                    for (float ndcY : {ndcY0, ndcY1}) {
                        for (float d : {dNear, dFar}) {
                            const glm::vec3 corner{ndcX * d * tanHalfFovX, ndcY * d * grid.tanHalfFovY, -d};
                            const float dist = glm::length(corner - bounds.center);
                            // Small epsilon: bounds.radius is itself derived
                            // from the max corner distance, so this should
                            // hold to float precision, not merely approximately.
                            CHECK(dist <= bounds.radius + 0.001F);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("pointLightCullRadius: explicit range wins verbatim; unconfigured range (<=0) derives a "
          "finite cutoff from candela intensity at the default 1.0 lux threshold") {
    // Explicit range always wins, unscaled.
    CHECK(pointLightCullRadius(glm::vec3(1000.0F), 12.5F) == doctest::Approx(12.5F));
    CHECK(pointLightCullRadius(glm::vec3(1.0F), 0.001F) == doctest::Approx(0.001F));

    // range<=0: d = sqrt(maxComponent(colorCandela) / cutoffLux).
    // maxComponent(100,50,25)=100, cutoff=1.0 -> sqrt(100)=10.0.
    CHECK(pointLightCullRadius(glm::vec3(100.0F, 50.0F, 25.0F), 0.0F) == doctest::Approx(10.0F).epsilon(0.0001));
    // maxComponent=400, cutoff=4.0 -> sqrt(100)=10.0.
    CHECK(pointLightCullRadius(glm::vec3(400.0F, 0.0F, 0.0F), -1.0F, 4.0F) == doctest::Approx(10.0F).epsilon(0.0001));
    // Zero-intensity light: zero cull radius (never a divide producing inf/NaN).
    CHECK(pointLightCullRadius(glm::vec3(0.0F), 0.0F) == doctest::Approx(0.0F));
}

TEST_CASE("sphereIntersectsFroxel: exact geometric boundary cases -- touching (radius sum == "
          "distance) counts as intersecting; just beyond does not") {
    const FroxelBounds froxel{glm::vec3(0.0F, 0.0F, -10.0F), 2.0F};

    // Distance from froxel center to light == 5.0; lightRadius+froxelRadius == 5.0 exactly -> touching, included.
    CHECK(sphereIntersectsFroxel(glm::vec3(0.0F, 0.0F, -13.0F), 2.0F, froxel));  // distance 3, sum 4 -> included
    CHECK(sphereIntersectsFroxel(glm::vec3(3.0F, 0.0F, -10.0F), 1.0F, froxel));  // distance 3, sum 3 -> touching, included
    CHECK_FALSE(sphereIntersectsFroxel(glm::vec3(3.01F, 0.0F, -10.0F), 1.0F, froxel));  // distance 3.01 > sum 3 -> excluded

    // Light entirely on the wrong side, far away -- clearly excluded.
    CHECK_FALSE(sphereIntersectsFroxel(glm::vec3(100.0F, 100.0F, 100.0F), 1.0F, froxel));
}

TEST_CASE("spotConeViewSpace: sin/cos-squared derivation at a hand-computed 30-degree outer cone "
          "angle (sin(30deg)=0.5 -> invSin=2.0; cos(30deg)^2=0.75)") {
    const SpotConeViewSpace cone = spotConeViewSpace(glm::vec3(0.0F, 0.0F, -1.0F), glm::radians(30.0F));
    CHECK(cone.sinInverse == doctest::Approx(2.0F).epsilon(0.0005));
    CHECK(cone.cosSquared == doctest::Approx(0.75F).epsilon(0.0005));
    CHECK(cone.axis.z == doctest::Approx(-1.0F));

    // Near-zero cone angle: clamped exactly at Filament's own 0.5-degree
    // floor constants [Froxelizer.cpp:679-680] -- never a divide blow-up.
    const SpotConeViewSpace tiny = spotConeViewSpace(glm::vec3(0.0F, 0.0F, -1.0F), glm::radians(0.01F));
    CHECK(tiny.sinInverse == doctest::Approx(114.59301F).epsilon(0.001));
    CHECK(tiny.cosSquared == doctest::Approx(0.99992385F).epsilon(0.001));
}

TEST_CASE("spotIntersectsFroxel: a spot light's cone pointing AWAY from a froxel excludes it even "
          "though the coarse bounding-sphere test alone would include it -- the discriminating case "
          "the cone refinement exists for") {
    // Froxel directly in front of a spot light positioned at the origin,
    // but the spot points in +Z (away from the froxel, which sits at -Z) --
    // a bounding-SPHERE-only test would wrongly include it (within radius),
    // the cone test must exclude it.
    const FroxelBounds froxel{glm::vec3(0.0F, 0.0F, -5.0F), 1.0F};
    const SpotConeViewSpace coneAway = spotConeViewSpace(glm::vec3(0.0F, 0.0F, 1.0F), glm::radians(20.0F));
    const glm::vec3 lightPos(0.0F, 0.0F, 0.0F);
    const float lightRadius = 10.0F;

    // Sanity: the coarse sphere-only test WOULD include it (proves the cone test is load-bearing).
    REQUIRE(sphereIntersectsFroxel(lightPos, lightRadius, froxel));
    CHECK_FALSE(spotIntersectsFroxel(lightPos, lightRadius, coneAway, froxel));

    // Same light, cone now pointing TOWARD the froxel (-Z) -- included.
    const SpotConeViewSpace coneToward = spotConeViewSpace(glm::vec3(0.0F, 0.0F, -1.0F), glm::radians(20.0F));
    CHECK(spotIntersectsFroxel(lightPos, lightRadius, coneToward, froxel));

    // A narrow cone pointing toward the froxel but ROTATED off-axis (toward
    // +X) so the froxel falls outside the narrow cone's own angular window --
    // excluded despite pointing "generally" the right way.
    const SpotConeViewSpace coneOffAxis = spotConeViewSpace(glm::vec3(1.0F, 0.0F, 0.0F), glm::radians(5.0F));
    CHECK_FALSE(spotIntersectsFroxel(lightPos, lightRadius, coneOffAxis, froxel));
}
