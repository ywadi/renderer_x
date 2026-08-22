// src/rx_cluster/tests/test_cluster_capacity_determinism_gpu.cpp -- Phase 5
// Stage 2 Task 14 [#50]. Two of the ticket's own named acceptance
// criteria:
//  (1) DETERMINISM [matrix row 2's own acceptance text: "dispatch the SAME
//      light set + camera through the froxel-assignment compute pass
//      TWICE... assert the per-froxel light-list buffer readback is
//      BYTE-IDENTICAL both times"]. This design uses ZERO atomics anywhere
//      (see shaders/cluster/froxel_light_scatter.slang's own header
//      comment) -- every write index is a pure function of (that froxel's
//      own thread, iterating a FIXED light-index order) + (that froxel's
//      own prefix-sum offset, itself a deterministic serial scan) -- so
//      this test is expected to pass BY CONSTRUCTION, not by luck; it
//      exists to PROVE that design claim empirically, matching CLAUDE.md's
//      "every criterion empirically proven" standing rule.
//  (2) CAPACITY+1, both axes [plan:504-505: "max lights per froxel /
//      total... counters exact and CI-gateable"; CLAUDE.md's own standing
//      capacity-past-declared-limit rule]. Both TEST_CASEs below construct
//      a scenario deliberately PAST the declared capacity and assert the
//      overflow counter is EXACT (never a boolean, never silently
//      dropped/corrupted) and that unrelated froxels are unaffected.
#include "cluster_gpu_fixture.h"

#include <doctest/doctest.h>
#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_scene/camera.h>

#include <algorithm>
#include <cstring>

using namespace rx::cluster;
using namespace rx::cluster::test;

namespace {

ClusterLightGpu pointLight(glm::vec3 viewPos, float radius) {
    ClusterLightGpu l;
    l.viewPositionRadius = glm::vec4(viewPos, radius);
    l.viewAxisSinInverse = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);
    l.cosSquaredFlags = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);
    return l;
}

}  // namespace

TEST_CASE("Cluster light assignment: DISPATCH DETERMINISM -- the SAME light set + camera, dispatched "
          "twice through the SAME ClusterPipelines instance, produces BYTE-IDENTICAL offsets/"
          "writeCounts/lightIndices/trueCounts readback both times") {
    auto fixture = makeFixture("rx_cluster_determinism");
    if (!fixture.has_value()) {
        return;
    }

    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
    auto pipelineCache = rx::rhi::ComputePipelineCache::create(fixture->device.device(), pipelineCachePath("rx_cluster_determinism"));
    REQUIRE(pipelineCache.has_value());
    auto pipelines =
        ClusterPipelines::create(fixture->device.device(), *pipelineCache, *compiler, RX_CLUSTER_SHADER_DIR);
    REQUIRE(pipelines.has_value());

    // A moderately busy synthetic scene: 40 lights scattered across a grid
    // of positions, several with overlapping footprints (spatially
    // "realistic" clutter -- not just one trivial light).
    std::vector<ClusterLightGpu> lights;
    for (int i = 0; i < 40; ++i) {
        const float fx = static_cast<float>((i % 5) - 2) * 3.0F;
        const float fy = static_cast<float>((i / 5 % 4) - 2) * 3.0F;
        const float fz = -10.0F - static_cast<float>(i % 7) * 4.0F;
        lights.push_back(pointLight(glm::vec3(fx, fy, fz), 2.5F));
    }
    auto lightsBuffer = uploadLights(fixture->allocator, lights);
    REQUIRE(lightsBuffer.has_value());

    rx::scene::Camera camera;
    ClusterParams params;
    params.zLightNear = 5.0F;
    params.zLightFar = 100.0F;
    params.maxLightsPerFroxel = 64;
    params.maxTotalLightIndices = 16384;

    auto run1 = runCluster(*fixture, *pipelines, lightsBuffer->handle(), static_cast<uint32_t>(lights.size()), camera,
                            1920, 1080, params);
    REQUIRE(run1.has_value());
    auto run2 = runCluster(*fixture, *pipelines, lightsBuffer->handle(), static_cast<uint32_t>(lights.size()), camera,
                            1920, 1080, params);
    REQUIRE(run2.has_value());

    REQUIRE(run1->grid.froxelCount() == run2->grid.froxelCount());
    CHECK(run1->totalUsed == run2->totalUsed);
    REQUIRE(run1->totalUsed > 0);  // a non-trivial scene actually assigned something.

    CHECK(run1->trueCounts == run2->trueCounts);
    CHECK(run1->offsets == run2->offsets);
    CHECK(run1->writeCounts == run2->writeCounts);
    CHECK(run1->perFroxelOverflow == run2->perFroxelOverflow);
    CHECK(run1->globalOverflow == run2->globalOverflow);
    // Byte-identical over the MEANINGFUL prefix only (lightIndices[totalUsed..)
    // is untouched/undefined memory from this dispatch's own perspective --
    // comparing it would assert something neither kernel ever promises).
    const bool indicesIdentical =
        std::memcmp(run1->lightIndices.data(), run2->lightIndices.data(), static_cast<size_t>(run1->totalUsed) * sizeof(uint32_t)) == 0;
    CHECK(indicesIdentical);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Cluster light assignment: CAPACITY+1, PER-FROXEL axis -- (maxLightsPerFroxel+1) lights all "
          "overlapping the SAME froxel: truncated (never corrupted/wrapped), overflow counter EXACT, "
          "and a FAR, unrelated froxel is completely unaffected") {
    auto fixture = makeFixture("rx_cluster_capacity_per_froxel");
    if (!fixture.has_value()) {
        return;
    }

    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
    auto pipelineCache =
        rx::rhi::ComputePipelineCache::create(fixture->device.device(), pipelineCachePath("rx_cluster_capacity_per_froxel"));
    REQUIRE(pipelineCache.has_value());
    auto pipelines =
        ClusterPipelines::create(fixture->device.device(), *pipelineCache, *compiler, RX_CLUSTER_SHADER_DIR);
    REQUIRE(pipelines.has_value());

    constexpr uint32_t kMaxPerFroxel = 8;
    // python: froxel_bounds(0,0,1,g)[0] -- see test_cluster_membership_gpu.cpp's
    // own top comment for the same corner point and its already-verified
    // localized-cluster behavior (the SAME grid config is reused here).
    const glm::vec3 targetPoint(-5.488147944009125F, -2.992096042551129F, -5.55263825016892F);

    // kMaxPerFroxel + 1 lights, all at the EXACT same point (radius ~0) --
    // every one of them intersects the SAME froxel (and, per the design
    // note in test_cluster_membership_gpu.cpp, the same small localized
    // neighbor cluster -- irrelevant here, this test only asserts the
    // TARGET froxel's own counters plus a FAR froxel's isolation).
    std::vector<ClusterLightGpu> lights;
    for (uint32_t i = 0; i < kMaxPerFroxel + 1; ++i) {
        lights.push_back(pointLight(targetPoint, 1e-4F));
    }
    auto lightsBuffer = uploadLights(fixture->allocator, lights);
    REQUIRE(lightsBuffer.has_value());

    rx::scene::Camera camera;
    ClusterParams params;
    params.zLightNear = 5.0F;
    params.zLightFar = 100.0F;
    params.maxLightsPerFroxel = kMaxPerFroxel;
    params.maxTotalLightIndices = 8192;  // generous -- isolates the PER-FROXEL axis alone.

    auto result = runCluster(*fixture, *pipelines, lightsBuffer->handle(), static_cast<uint32_t>(lights.size()),
                              camera, 1920, 1080, params);
    REQUIRE(result.has_value());

    const uint32_t targetIdx = rx::scene::froxel::froxelIndex(0, 0, 1, result->grid);
    // TRUE count is UNCAPPED -- every one of the kMaxPerFroxel+1 lights
    // really does intersect this froxel (radius ~0 at its own bounding-
    // sphere center).
    CHECK(result->trueCounts[targetIdx] == kMaxPerFroxel + 1);
    // Truncated to EXACTLY the declared capacity -- never corrupted/wrapped.
    CHECK(result->writeCounts[targetIdx] == kMaxPerFroxel);
    // The overflow counter is EXACT (1), never a boolean/saturated flag.
    CHECK(result->perFroxelOverflow[targetIdx] == 1);
    CHECK(result->globalOverflow[targetIdx] == 0);  // the global axis was never touched.
    // The scattered indices are the FIRST kMaxPerFroxel light indices, in
    // ascending order (deterministic truncation -- see
    // froxel_light_scatter.slang's own header comment).
    const std::vector<uint32_t> targetLights = result->froxelLights(targetIdx);
    REQUIRE(targetLights.size() == kMaxPerFroxel);
    for (uint32_t i = 0; i < kMaxPerFroxel; ++i) {
        CHECK(targetLights[i] == i);
    }

    // A FAR, unrelated froxel (the grid's opposite corner) is completely
    // unaffected by this froxel's own saturation -- the plan's own
    // "contained, not corrupt" acceptance text.
    const uint32_t farIdx =
        rx::scene::froxel::froxelIndex(result->grid.countX - 1, result->grid.countY - 1, result->grid.countZ - 1, result->grid);
    CHECK(result->trueCounts[farIdx] == 0);
    CHECK(result->writeCounts[farIdx] == 0);
    CHECK(result->perFroxelOverflow[farIdx] == 0);
    CHECK(result->globalOverflow[farIdx] == 0);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Cluster light assignment: CAPACITY+1, GLOBAL axis -- the light-index buffer's own total "
          "capacity is exhausted partway through the froxel sweep: EVERY subsequent froxel's writeCount "
          "is truncated (down to zero once fully saturated), globalOverflow is EXACT per froxel, "
          "totalUsed reports the buffer's own real capacity, and froxels with zero true lights stay at "
          "zero overflow (the cap never touches what it does not need to)") {
    auto fixture = makeFixture("rx_cluster_capacity_global");
    if (!fixture.has_value()) {
        return;
    }

    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
    auto pipelineCache =
        rx::rhi::ComputePipelineCache::create(fixture->device.device(), pipelineCachePath("rx_cluster_capacity_global"));
    REQUIRE(pipelineCache.has_value());
    auto pipelines =
        ClusterPipelines::create(fixture->device.device(), *pipelineCache, *compiler, RX_CLUSTER_SHADER_DIR);
    REQUIRE(pipelines.has_value());

    // 30 lights, all at the SAME corner point used by the membership test
    // -- known (verified there) to hit an 8-froxel localized cluster
    // around froxel (0,0,1), all 30 of them, every time.
    constexpr uint32_t kLightCount = 30;
    const glm::vec3 targetPoint(-5.488147944009125F, -2.992096042551129F, -5.55263825016892F);
    std::vector<ClusterLightGpu> lights;
    for (uint32_t i = 0; i < kLightCount; ++i) {
        lights.push_back(pointLight(targetPoint, 1e-4F));
    }
    auto lightsBuffer = uploadLights(fixture->allocator, lights);
    REQUIRE(lightsBuffer.has_value());

    rx::scene::Camera camera;
    ClusterParams params;
    params.zLightNear = 5.0F;
    params.zLightFar = 100.0F;
    params.maxLightsPerFroxel = 1000;   // generous -- isolates the GLOBAL axis alone.
    constexpr uint32_t kGlobalCap = 50;
    params.maxTotalLightIndices = kGlobalCap;

    auto result = runCluster(*fixture, *pipelines, lightsBuffer->handle(), static_cast<uint32_t>(lights.size()),
                              camera, 1920, 1080, params);
    REQUIRE(result.has_value());

    // Discover the localized cluster from trueCounts directly (no
    // hardcoded froxel-index assumption beyond the grid shape itself) --
    // every froxel with trueCounts[i] > 0 is one of the hit froxels.
    std::vector<uint32_t> hitFroxels;
    for (uint32_t i = 0; i < result->grid.froxelCount(); ++i) {
        if (result->trueCounts[i] > 0) {
            hitFroxels.push_back(i);
        }
    }
    // hitFroxels is already ascending (the sweep above iterates i ascending)
    // -- EXACTLY the prefix-sum's own processing order.
    REQUIRE_FALSE(hitFroxels.empty());
    for (uint32_t idx : hitFroxels) {
        CHECK(result->trueCounts[idx] == kLightCount);  // every hit froxel truly sees all 30.
    }

    // Simulate the SAME serial capping logic the prefix-sum kernel performs
    // (shaders/cluster/froxel_prefix_sum.slang) as the CPU oracle for this
    // capacity axis specifically -- froxel_light_count_test.cpp already
    // proves the INTERSECTION test agrees with the GPU; this proves the
    // CAPPING/prefix-sum arithmetic does too.
    uint32_t running = 0;
    uint32_t totalGlobalOverflow = 0;
    for (uint32_t idx : hitFroxels) {
        const uint32_t capped = std::min(result->trueCounts[idx], params.maxLightsPerFroxel);
        const uint32_t remaining = kGlobalCap > running ? (kGlobalCap - running) : 0;
        const uint32_t used = std::min(capped, remaining);
        const uint32_t overflow = capped - used;
        CHECK(result->writeCounts[idx] == used);
        CHECK(result->globalOverflow[idx] == overflow);
        CHECK(result->perFroxelOverflow[idx] == 0);  // per-froxel cap never engaged (maxLightsPerFroxel=1000).
        running += used;
        totalGlobalOverflow += overflow;
    }
    CHECK(running == kGlobalCap);          // the buffer ends up FULLY saturated.
    CHECK(result->totalUsed == kGlobalCap);  // reports the real, exact used count.
    CHECK(totalGlobalOverflow > 0);          // some lights really were dropped -- a non-trivial proof.

    // Every froxel OUTSIDE the hit cluster has zero true lights and,
    // correctly, zero overflow of either kind -- the cap never touches
    // what it does not need to (the plan's own "contained" acceptance text,
    // now proven for the GLOBAL axis specifically, not just the per-froxel
    // one).
    uint32_t uncappedZeroOverflowCount = 0;
    for (uint32_t i = 0; i < result->grid.froxelCount(); ++i) {
        if (result->trueCounts[i] == 0) {
            CHECK(result->writeCounts[i] == 0);
            CHECK(result->perFroxelOverflow[i] == 0);
            CHECK(result->globalOverflow[i] == 0);
            ++uncappedZeroOverflowCount;
        }
    }
    CHECK(uncappedZeroOverflowCount == result->grid.froxelCount() - hitFroxels.size());

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
