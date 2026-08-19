#include <doctest/doctest.h>
#include <rx_scene/draw_list.h>

#include <rx_core/debug_checks.h>
#include <rx_task/scheduler.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// [D26.2] volk.h (project convention: "volk.h, not vulkan/vulkan.h") pulled
// in ONLY here, in the TEST binary -- see draw_list.h's own top comment for
// why the PRODUCTION library (draw_list.{h,cpp}) never names a Vulkan type
// itself. Reachable transitively (rx_scene PUBLIC-links rx_asset, which
// PUBLIC-links rx_rhi_vk, whose own CMakeLists.txt exposes volk's include
// dir PUBLIC) with no new link dependency added for this test binary.
#if __has_include(<volk.h>)
#include <volk.h>
#define RX_SCENE_TEST_HAS_VOLK 1
#else
#define RX_SCENE_TEST_HAS_VOLK 0
#endif

// draw_list_test.cpp -- device-free coverage for rx::scene::DrawListBuilder
// [spec D14/D15/D24/D26/D27; Phase 4 Stage 2 Task 19; gate
// matrix-issue06-drawlists-culling.md + matrix-issue07-layer-masks.md as
// amended by gate/rulings-2026-08-18.md's #6/#7 sections + RC3/RC5]. Every
// case here uses trivial, in-test MeshSubmeshesFn/MaterialResolveFn
// providers (a `FakeAssetSource` below) -- never a real asset::Registry,
// same rationale as scene_test.cpp's own device-free convention.

using rx::asset::AABB;
using rx::asset::AlphaMode;
using rx::asset::MaterialAsset;
using rx::asset::MaterialDisposition;
using rx::asset::MaterialHandle;
using rx::asset::MeshHandle;
using rx::asset::MeshRange;
using rx::asset::Submesh;
using rx::scene::BlockRange;
using rx::scene::Camera;
using rx::scene::CullCounters;
using rx::scene::DirectionalLightDesc;
using rx::scene::DrawCommand;
using rx::scene::DrawListBuilder;
using rx::scene::DrawPayload;
using rx::scene::LightHandle;
using rx::scene::RenderableDesc;
using rx::scene::RenderableHandle;
using rx::scene::ResolvedMaterial;
using rx::scene::Scene;
using rx::scene::ShadowLists;
using rx::scene::ViewLists;

namespace {

// ---------------------------------------------------------------------
// Fixtures.
// ---------------------------------------------------------------------

AABB boxAt(glm::vec3 center, glm::vec3 halfExtent) {
    AABB box;
    box.expand(center - halfExtent);
    box.expand(center + halfExtent);
    return box;
}

AABB unitCubeAt(glm::vec3 center) { return boxAt(center, glm::vec3(0.5F)); }

Submesh makeSubmesh(uint32_t blockId, uint32_t firstIndex, uint32_t indexCount, int32_t vertexOffset, MaterialHandle material) {
    Submesh s;
    s.range.blockId = blockId;
    s.range.firstIndex = firstIndex;
    s.range.indexCount = indexCount;
    s.range.vertexOffset = vertexOffset;
    s.material = material;
    return s;
}

// A tiny in-test asset source -- mirrors asset::Registry's own D24
// fallback-substitution SHAPE (evict -> fallback content, never a crash)
// without needing a real GPU-backed Registry. Bound to Scene's
// MeshBoundsFn and DrawListBuilder's MeshSubmeshesFn/MaterialResolveFn.
class FakeAssetSource {
public:
    static constexpr uint32_t kFallbackMaterialIndex = 999'999U;

    MeshHandle addMesh(std::vector<Submesh> submeshes, AABB localBounds = unitCubeAt(glm::vec3(0.0F))) {
        const uint32_t idx = static_cast<uint32_t>(meshes_.size());
        meshes_.push_back(std::move(submeshes));
        bounds_.push_back(localBounds);
        return MeshHandle(idx, 1);
    }

    MaterialHandle addMaterial(MaterialAsset mat) {
        const uint32_t idx = static_cast<uint32_t>(materials_.size());
        materials_.push_back(mat);
        return MaterialHandle(idx, 1);
    }

    void evictMesh(MeshHandle h) { evictedMeshes_.insert(h.index()); }
    void evictMaterial(MaterialHandle h) { evictedMaterials_.insert(h.index()); }

    rx::scene::MeshBoundsFn meshBoundsFn() const {
        return [this](MeshHandle h) -> AABB {
            if (evictedMeshes_.count(h.index()) != 0 || h.index() >= bounds_.size()) {
                return AABB{};  // fallback shape: invalid/empty, mirrors makeFallbackMeshAsset().
            }
            return bounds_[h.index()];
        };
    }

    rx::scene::MeshSubmeshesFn meshSubmeshesFn() const {
        return [this](MeshHandle h) -> std::span<const Submesh> {
            static const std::vector<Submesh> kEmpty;
            if (evictedMeshes_.count(h.index()) != 0 || h.index() >= meshes_.size()) {
                return kEmpty;  // fallback shape: empty submesh list.
            }
            return meshes_[h.index()];
        };
    }

    rx::scene::MaterialResolveFn materialResolveFn() const {
        return [this](MaterialHandle h) -> ResolvedMaterial {
            if (evictedMaterials_.count(h.index()) != 0 || h.index() >= materials_.size()) {
                return ResolvedMaterial{kFallbackMaterialIndex, AlphaMode::Opaque, false, MaterialDisposition::Unlit};
            }
            const MaterialAsset& mat = materials_[h.index()];
            return ResolvedMaterial{h.index(), mat.alphaMode, mat.doubleSided, mat.disposition};
        };
    }

private:
    std::vector<std::vector<Submesh>> meshes_;
    std::vector<AABB> bounds_;
    std::vector<MaterialAsset> materials_;
    std::unordered_set<uint32_t> evictedMeshes_;
    std::unordered_set<uint32_t> evictedMaterials_;
};

MaterialAsset opaqueMaterial() {
    MaterialAsset mat;
    mat.alphaMode = AlphaMode::Opaque;
    return mat;
}
MaterialAsset maskMaterial() {
    MaterialAsset mat;
    mat.alphaMode = AlphaMode::Mask;
    return mat;
}
MaterialAsset blendMaterial() {
    MaterialAsset mat;
    mat.alphaMode = AlphaMode::Blend;
    return mat;
}

// Camera at the world origin, identity orientation (forward = -Z, up = +Y,
// right = +X), 90-degree vertical FOV, square aspect -- chosen so the
// frustum's half-extent at depth `d` (world Z = -d) is exactly `d`
// (tan(45 deg) == 1), making every test AABB placement below reason about
// with plain arithmetic instead of trigonometry.
Camera testCamera(float nearPlane = 1.0F, float farPlane = 100.0F) {
    Camera cam;
    cam.verticalFovRadians = glm::radians(90.0F);
    cam.aspectRatio = 1.0F;
    cam.nearPlane = nearPlane;
    cam.cullingFarPlane = farPlane;
    return cam;
}

RenderableHandle addRenderable(Scene& scene, MeshHandle mesh, uint32_t layers = ~0U, uint8_t channels = 0xFF,
                                bool castsShadows = true, uint8_t priority = 4) {
    RenderableDesc desc;
    desc.mesh = mesh;
    desc.layers = layers;
    desc.channels = channels;
    desc.castsShadows = castsShadows;
    desc.priority = priority;
    return scene.createRenderable(desc);
}

}  // namespace

// =======================================================================
// Frustum culling: known in/out AABB battery [Task 19's own "known in/out
// AABB sets (exact counters)" step, concretized into the p-vertex test's
// classic case battery per matrix-issue06's own acceptance criterion].
// =======================================================================

TEST_CASE("build() frustum-culls a battery of known in/out AABB placements with EXACT counters "
          "[p-vertex test case battery, gate matrix-issue06]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());

    // At depth d=10 (world Z=-10), the 90deg/aspect-1 frustum's half-extent
    // is exactly 10 in X and Y.
    struct Case {
        const char* name;
        glm::vec3 center;
        glm::vec3 halfExtent;
        bool expectedVisible;
    };
    const std::vector<Case> cases = {
        {"fully inside", {0.0F, 0.0F, -10.0F}, {1.0F, 1.0F, 1.0F}, true},
        {"fully outside RIGHT", {30.0F, 0.0F, -10.0F}, {1.0F, 1.0F, 1.0F}, false},
        {"fully outside LEFT", {-30.0F, 0.0F, -10.0F}, {1.0F, 1.0F, 1.0F}, false},
        {"fully outside TOP", {0.0F, 30.0F, -10.0F}, {1.0F, 1.0F, 1.0F}, false},
        {"fully outside BOTTOM", {0.0F, -30.0F, -10.0F}, {1.0F, 1.0F, 1.0F}, false},
        {"fully outside NEAR (closer than near=1)", {0.0F, 0.0F, -0.5F}, {0.1F, 0.1F, 0.1F}, false},
        {"fully outside FAR (beyond cullingFarPlane=100)", {0.0F, 0.0F, -150.0F}, {1.0F, 1.0F, 1.0F}, false},
        {"straddling ONE plane (right)", {10.0F, 0.0F, -10.0F}, {15.0F, 0.5F, 0.5F}, true},
        {"straddling MULTIPLE planes (huge box around camera)", {0.0F, 0.0F, 0.0F}, {1000.0F, 1000.0F, 1000.0F}, true},
    };

    Scene scene(assets.meshBoundsFn());
    std::vector<bool> expected;
    for (const Case& c : cases) {
        MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, boxAt(c.center, c.halfExtent));
        static_cast<void>(addRenderable(scene, mesh));
        expected.push_back(c.expectedVisible);
    }

    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    builder.build(scene, testCamera(), *scheduler, lists);

    uint32_t expectedVisibleCount = 0;
    for (bool v : expected) {
        if (v) {
            ++expectedVisibleCount;
        }
    }
    CHECK(lists.counters.totalCandidates == cases.size());
    CHECK(lists.counters.culledByLayerMask == 0);
    CHECK(lists.counters.visible == expectedVisibleCount);
    CHECK(lists.counters.culledByFrustum == cases.size() - expectedVisibleCount);

    // Cross-check WHICH renderables survived, not just the count: every
    // surviving draw's payload carries `instanceDataIndex` == the
    // renderable's own Scene slot index (created in the SAME order as
    // `cases`, so index i corresponds to cases[i]).
    std::unordered_set<uint32_t> visibleIndices;
    for (const DrawPayload& p : lists.payloads) {
        visibleIndices.insert(p.instanceDataIndex);
    }
    for (uint32_t i = 0; i < cases.size(); ++i) {
        INFO("case: ", cases[i].name);
        CHECK(visibleIndices.count(i) == (expected[i] ? 1U : 0U));
    }
}

TEST_CASE("build() never culls a degenerate (single-point, min==max) AABB fully inside the frustum, and correctly "
          "culls one fully outside -- no NaN/UB [gate 'Degenerate / zero-extent AABB' row]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    Scene scene(assets.meshBoundsFn());

    AABB insidePoint;
    insidePoint.expand(glm::vec3(0.0F, 0.0F, -10.0F));  // min == max
    AABB outsidePoint;
    outsidePoint.expand(glm::vec3(1000.0F, 0.0F, -10.0F));

    MeshHandle meshIn = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, insidePoint);
    MeshHandle meshOut = assets.addMesh({makeSubmesh(0, 3, 3, 0, mat)}, outsidePoint);
    static_cast<void>(addRenderable(scene, meshIn));
    static_cast<void>(addRenderable(scene, meshOut));

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    builder.build(scene, testCamera(), *scheduler, lists);

    CHECK(lists.counters.visible == 1);
    REQUIRE(lists.commands.size() == 1);
    CHECK(lists.commands[0].firstIndex == 0);  // the inside point's submesh
}

TEST_CASE("build() never culls a ground-slab AABB whose horizontal extent vastly exceeds the frustum's footprint "
          "[gate 'Very large objects (ground planes)' row]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    Scene scene(assets.meshBoundsFn());

    // A gigantic, thin horizontal slab straddling every plane at once.
    AABB slab = boxAt(glm::vec3(0.0F, 0.0F, -10.0F), glm::vec3(100000.0F, 0.1F, 100000.0F));
    MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, slab);
    static_cast<void>(addRenderable(scene, mesh));

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    builder.build(scene, testCamera(), *scheduler, lists);

    CHECK(lists.counters.visible == 1);
    CHECK(lists.counters.culledByFrustum == 0);
}

TEST_CASE("An object at extreme depth (100,000 units), still inside the side/top/bottom cone and within a "
          "sufficiently large cullingFarPlane, is NEVER culled -- proves the FAR-plane test is correct at large "
          "distances, not merely a degenerate always-true no-op or an accidental always-reject "
          "[gate 'Objects spanning far=infinity' row, adopted per RC/#5 as the finite-cullingProj() version]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    Scene scene(assets.meshBoundsFn());

    MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -100000.0F)));
    static_cast<void>(addRenderable(scene, mesh));

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    // cullingFarPlane wide enough to legitimately contain the object.
    builder.build(scene, testCamera(/*nearPlane=*/1.0F, /*farPlane=*/200000.0F), *scheduler, lists);

    CHECK(lists.counters.visible == 1);
    CHECK(lists.counters.culledByFrustum == 0);

    // And the SAME object, at the SAME depth, with a far plane that
    // legitimately excludes it -- proves the far test is not simply
    // "never rejects anything" (a sign-flipped or always-true plane would
    // pass BOTH halves of this test; only a correct one passes both).
    ViewLists lists2;
    builder.build(scene, testCamera(/*nearPlane=*/1.0F, /*farPlane=*/1000.0F), *scheduler, lists2);
    CHECK(lists2.counters.visible == 0);
    CHECK(lists2.counters.culledByFrustum == 1);
}

// =======================================================================
// Layer-mask filtering [issue #7's own five-case CI matrix, gate ruling
// #7].
// =======================================================================

TEST_CASE("Camera cullMask / renderable layers: the five-case CI mask matrix [gate matrix-issue07]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    Scene scene(assets.meshBoundsFn());

    // Four renderables, each on a distinct layer bit, all comfortably
    // inside the test camera's frustum.
    std::vector<RenderableHandle> handles;
    for (uint32_t bit = 0; bit < 4; ++bit) {
        MeshHandle mesh = assets.addMesh({makeSubmesh(0, bit, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
        handles.push_back(addRenderable(scene, mesh, /*layers=*/1U << bit));
    }

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());

    // (a) single-bit cullMask: only the matching-layer object visible.
    {
        ViewLists lists;
        Camera cam = testCamera();
        cam.cullMask = 1U << 2;
        builder.build(scene, cam, *scheduler, lists);
        CHECK(lists.counters.visible == 1);
        REQUIRE(lists.commands.size() == 1);
        CHECK(lists.commands[0].firstIndex == 2);
    }
    // (b) cullMask == 0: zero visible, even for layers == ~0u objects
    // (there are none here, but the AND-test with 0 must reject everyone).
    {
        ViewLists lists;
        Camera cam = testCamera();
        cam.cullMask = 0;
        builder.build(scene, cam, *scheduler, lists);
        CHECK(lists.counters.visible == 0);
        CHECK(lists.counters.culledByLayerMask == 4);
    }
    // (c) cullMask == all-ones: all visible.
    {
        ViewLists lists;
        builder.build(scene, testCamera(), *scheduler, lists);  // default cullMask = ~0u
        CHECK(lists.counters.visible == 4);
    }
}

TEST_CASE("cullMask == 0 excludes a renderable with layers == ~0u too -- a non-empty intersection is required, "
          "never a subset test [gate matrix-issue07 row]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    Scene scene(assets.meshBoundsFn());
    MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    static_cast<void>(addRenderable(scene, mesh, /*layers=*/~0U));

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    Camera cam = testCamera();
    cam.cullMask = 0;
    builder.build(scene, cam, *scheduler, lists);
    CHECK(lists.counters.visible == 0);
}

TEST_CASE("A default-constructed renderable is visible to a default-constructed camera with zero explicit "
          "layer/mask configuration -- the all-ones-default regression guard [gate matrix-issue07 'Default "
          "values' row]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    Scene scene(assets.meshBoundsFn());
    MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    RenderableDesc desc;  // fully default: layers = ~0u
    desc.mesh = mesh;
    static_cast<void>(scene.createRenderable(desc));

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    builder.build(scene, Camera{}, *scheduler, lists);  // fully default camera: cullMask = ~0u
    // (default Camera looks down -Z with a 60deg fov/16:9 aspect -- the
    // object at z=-10 is comfortably inside it.)
    CHECK(lists.counters.visible == 1);
}

// (d)/(e): light-channel filtering + the "independent of layers" combined
// case are covered by the shadow-caster section below, where channels are
// actually consumed.

// =======================================================================
// Sort key: bit layout + decode() round trip [bgfx/Filament discipline,
// gate ruling #6].
// =======================================================================

TEST_CASE("sortkey::encodeOpaque/decodeOpaque round-trips every field exactly, and reservedBlendOrder in the "
          "blend shape decodes to zero when never written [gate ruling #6: named constants + decode() test]") {
    using namespace rx::scene::sortkey;

    const uint8_t priority = 5;
    const uint16_t pipelineKey = 0x3F;      // fits in 7 bits
    const uint16_t materialKey = 0x2AAA;    // fits in 15 bits
    const uint32_t tieBreak = 0xFFFF;       // fits in 16 bits
    const float ndcDepth = 0.75F;

    const uint64_t key = encodeOpaque(priority, pipelineKey, materialKey, ndcDepth, tieBreak);
    const DecodedOpaque d = decodeOpaque(key);
    CHECK(d.priority == priority);
    CHECK(d.pipelineKey == pipelineKey);
    CHECK(d.materialKey == materialKey);
    CHECK(d.tieBreak == tieBreak);
    // The depth field round-trips as STORED (already direction-inverted --
    // see encodeOpaque()'s own comment); re-inverting it recovers the raw
    // bucket, which must equal depthBucketBits() computed directly.
    const uint32_t mask = static_cast<uint32_t>((uint64_t{1} << kOpaqueDepthBits) - 1);
    const uint32_t recoveredRawBucket = (~d.invertedDepthBucket) & mask;
    CHECK(recoveredRawBucket == depthBucketBits(ndcDepth, kOpaqueDepthBits));
}

TEST_CASE("sortkey::encodeBlend/decodeBlend round-trips exactly; reservedBlendOrder is always zero [RC5/#5: "
          "'blendOrder bits reserved, unpopulated']") {
    using namespace rx::scene::sortkey;
    const uint8_t priority = 3;
    const uint32_t tieBreak = 12345;
    const float ndcDepth = 0.25F;

    const uint64_t key = encodeBlend(priority, ndcDepth, tieBreak);
    const DecodedBlend d = decodeBlend(key);
    CHECK(d.priority == priority);
    CHECK(d.tieBreak == tieBreak);
    CHECK(d.reservedBlendOrder == 0);  // never written -- proves nothing else leaks into these bits
    CHECK(d.depthBucket == depthBucketBits(ndcDepth, kBlendDepthBits));
}

TEST_CASE("sortkey::encodeShadow/decodeShadow round-trips exactly") {
    using namespace rx::scene::sortkey;
    const uint16_t pipelineKey = 0x3AA;
    const uint16_t meshKey = 0xABCD;
    const uint32_t tieBreak = 0xDEADBEEFU;

    const uint64_t key = encodeShadow(pipelineKey, meshKey, tieBreak);
    const DecodedShadow d = decodeShadow(key);
    CHECK(d.pipelineKey == (pipelineKey & ((1U << kShadowPipelineKeyBits) - 1)));
    CHECK(d.meshKey == meshKey);
    CHECK(d.tieBreak == tieBreak);
}

TEST_CASE("Priority occupies a documented tier ABOVE pipeline/material/depth: a lower-priority record sorts "
          "before a higher-priority one regardless of every other field [RC5/#5 ruling]") {
    using namespace rx::scene::sortkey;
    // priority=0 with the WORST depth/pipeline/material vs priority=7 with
    // the BEST -- priority must still dominate.
    const uint64_t low = encodeOpaque(/*priority=*/0, 0x7F, 0x7FFF, /*ndcDepth=*/0.0F, /*tieBreak=*/0xFFFF);
    const uint64_t high = encodeOpaque(/*priority=*/7, 0, 0, /*ndcDepth=*/1.0F, /*tieBreak=*/0);
    CHECK(low < high);
}

TEST_CASE("Depth bucket preserves ordering for NDC depths clustered near the reversed-Z near plane (1.0) -- "
          "truncated float32 bit-pattern, never a linear rescale [gate ruling #6]") {
    using namespace rx::scene::sortkey;

    // Real reversed-Z-infinite-far NDC depths (ndc = near/distance --
    // Camera::proj()'s own formula, D13) for a near=0.1 camera: two pairs
    // with the SAME 0.1-world-unit absolute gap, one pair close to the
    // near plane, one pair far away -- the exact property reversed-Z
    // exists to exploit (precision concentrates near depth=1.0, not
    // uniformly across distance).
    constexpr float kNear = 0.1F;
    auto ndc = [&](float distance) { return kNear / distance; };

    const uint32_t nearPairA = depthBucketBits(ndc(0.2F), kOpaqueDepthBits);
    const uint32_t nearPairB = depthBucketBits(ndc(0.3F), kOpaqueDepthBits);
    CHECK(nearPairA != nearPairB);  // resolvable near the near plane
    CHECK(nearPairA > nearPairB);   // distance 0.2 is nearer -> larger ndc depth -> larger raw bucket

    // Monotonicity across a realistic clustered range near the near plane.
    const std::vector<float> distances = {0.15F, 0.2F, 0.3F, 0.5F, 1.0F, 2.0F, 5.0F};
    std::vector<uint32_t> buckets;
    for (float d : distances) {
        buckets.push_back(depthBucketBits(ndc(d), kOpaqueDepthBits));
    }
    for (size_t i = 1; i < buckets.size(); ++i) {
        INFO("comparing distance=", distances[i - 1], " vs distance=", distances[i]);
        CHECK(buckets[i - 1] > buckets[i]);  // nearer (smaller distance) -> larger ndc depth -> larger raw bucket
    }
}

// =======================================================================
// Sort ORDER + DIRECTION [shared fixture, gate ruling #6: opaque
// decreasing / blend increasing under reversed-Z].
// =======================================================================

TEST_CASE("Opaque partition sorts front-to-back (DEcreasing depth) and blend partition sorts back-to-front "
          "(INcreasing depth) on the SAME depth values, via ONE shared fixture [gate ruling #6 direction test]") {
    FakeAssetSource assets;
    MaterialHandle opaqueMat = assets.addMaterial(opaqueMaterial());
    MaterialHandle blendMat = assets.addMaterial(blendMaterial());

    // Three known depths -- nearest (z=-5) to farthest (z=-30).
    const std::vector<float> depthsZ = {-5.0F, -30.0F, -15.0F};  // deliberately out-of-order creation

    // --- Opaque scene ---
    {
        Scene scene(assets.meshBoundsFn());
        for (uint32_t i = 0; i < depthsZ.size(); ++i) {
            MeshHandle mesh =
                assets.addMesh({makeSubmesh(0, /*firstIndex=*/100 + i, 3, 0, opaqueMat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, depthsZ[i])));
            static_cast<void>(addRenderable(scene, mesh));
        }
        auto scheduler = rx::task::Scheduler::create(2);
        REQUIRE(scheduler != nullptr);
        DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
        ViewLists lists;
        builder.build(scene, testCamera(0.1F, 1000.0F), *scheduler, lists);
        REQUIRE(lists.commands.size() == 3);
        // Nearest (index 0, z=-5) first; farthest (index 1, z=-30) last.
        CHECK(lists.commands[0].firstIndex == 100 + 0);
        CHECK(lists.commands[1].firstIndex == 100 + 2);
        CHECK(lists.commands[2].firstIndex == 100 + 1);
    }
    // --- Blend scene: SAME depths, opposite order expected ---
    {
        Scene scene(assets.meshBoundsFn());
        for (uint32_t i = 0; i < depthsZ.size(); ++i) {
            MeshHandle mesh =
                assets.addMesh({makeSubmesh(0, /*firstIndex=*/200 + i, 3, 0, blendMat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, depthsZ[i])));
            static_cast<void>(addRenderable(scene, mesh));
        }
        auto scheduler = rx::task::Scheduler::create(2);
        REQUIRE(scheduler != nullptr);
        DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
        ViewLists lists;
        builder.build(scene, testCamera(0.1F, 1000.0F), *scheduler, lists);
        REQUIRE(lists.commands.size() == 3);
        CHECK(lists.opaqueCommandCount == 0);  // pure blend scene -- entire list is the blend partition
        // Farthest (index 1, z=-30) first; nearest (index 0, z=-5) last.
        CHECK(lists.commands[0].firstIndex == 200 + 1);
        CHECK(lists.commands[1].firstIndex == 200 + 2);
        CHECK(lists.commands[2].firstIndex == 200 + 0);
    }
}

TEST_CASE("Alpha-MASK draws land in the opaque partition, front-to-back, never in the blend partition "
          "[D14, gate matrix-issue06 'Alpha-MASK draws in the opaque partition' row]") {
    FakeAssetSource assets;
    MaterialHandle opaqueMat = assets.addMaterial(opaqueMaterial());
    MaterialHandle maskMat = assets.addMaterial(maskMaterial());
    MaterialHandle blendMat = assets.addMaterial(blendMaterial());
    Scene scene(assets.meshBoundsFn());

    MeshHandle mo = assets.addMesh({makeSubmesh(0, 0, 3, 0, opaqueMat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    MeshHandle mm = assets.addMesh({makeSubmesh(0, 1, 3, 0, maskMat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -12.0F)));
    MeshHandle mb = assets.addMesh({makeSubmesh(0, 2, 3, 0, blendMat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -14.0F)));
    static_cast<void>(addRenderable(scene, mo));
    static_cast<void>(addRenderable(scene, mm));
    static_cast<void>(addRenderable(scene, mb));

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    builder.build(scene, testCamera(0.1F, 1000.0F), *scheduler, lists);

    REQUIRE(lists.commands.size() == 3);
    CHECK(lists.opaqueCommandCount == 2);  // opaque + MASK
    for (uint32_t i = 0; i < lists.opaqueCommandCount; ++i) {
        CHECK(lists.commands[i].firstIndex != 2);  // blend's firstIndex never appears in the opaque region
    }
    CHECK(lists.commands[lists.opaqueCommandCount].firstIndex == 2);
}

// =======================================================================
// D26.3 -- instancing collapse + the D26.3 lockstep criterion.
// =======================================================================

TEST_CASE("An identical-run scene (1000 identical renderables) collapses into ONE DrawCommand with "
          "instanceCount==1000; recordsIn==1000, drawsSubmitted==1 [D26.3, Task 19's own named step]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    Scene scene(assets.meshBoundsFn());

    constexpr uint32_t kCount = 1000;
    for (uint32_t i = 0; i < kCount; ++i) {
        static_cast<void>(addRenderable(scene, mesh));
    }

    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    builder.build(scene, testCamera(0.1F, 1000.0F), *scheduler, lists);

    CHECK(lists.counters.recordsIn == kCount);
    CHECK(lists.counters.drawsSubmitted == 1);
    REQUIRE(lists.commands.size() == 1);
    CHECK(lists.commands[0].instanceCount == kCount);
    CHECK(lists.payloads.size() == kCount);
}

TEST_CASE("D26.3 lockstep criterion: an interleaved-creation-order scene collapses into per-identity groups "
          "whose payload ranges [firstInstance, firstInstance+instanceCount) contain ONLY payload entries "
          "belonging to that exact source-renderable identity -- catches silent commands/payloads "
          "desynchronization [gate ruling #6, D26.3 lockstep criterion, mandatory]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    // Two distinct mesh identities (different firstIndex), same material/block.
    MeshHandle meshA = assets.addMesh({makeSubmesh(0, 100, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    MeshHandle meshB = assets.addMesh({makeSubmesh(0, 200, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    Scene scene(assets.meshBoundsFn());

    // Interleaved A,B,A,B,A,B creation order -- a naive independent sort of
    // commands vs. payloads (or a collapse pass that merges commands
    // without moving payloads to match) would desynchronize here.
    std::unordered_set<uint32_t> typeAIndices;
    std::unordered_set<uint32_t> typeBIndices;
    for (uint32_t i = 0; i < 6; ++i) {
        const bool isA = (i % 2 == 0);
        RenderableHandle h = addRenderable(scene, isA ? meshA : meshB);
        (isA ? typeAIndices : typeBIndices).insert(h.index());
    }

    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    builder.build(scene, testCamera(0.1F, 1000.0F), *scheduler, lists);

    CHECK(lists.counters.recordsIn == 6);
    REQUIRE(lists.commands.size() == 2);  // exactly two collapsed groups: A and B
    CHECK(lists.counters.drawsSubmitted == 2);

    for (const DrawCommand& cmd : lists.commands) {
        CHECK(cmd.instanceCount == 3);
        const bool isTypeACommand = (cmd.firstIndex == 100);
        const bool isTypeBCommand = (cmd.firstIndex == 200);
        REQUIRE((isTypeACommand || isTypeBCommand));
        const std::unordered_set<uint32_t>& expectedSet = isTypeACommand ? typeAIndices : typeBIndices;
        for (uint32_t k = cmd.firstInstance; k < cmd.firstInstance + cmd.instanceCount; ++k) {
            REQUIRE(k < lists.payloads.size());
            CHECK(expectedSet.count(lists.payloads[k].instanceDataIndex) == 1);
        }
    }
}

// =======================================================================
// Per-block contiguous BlockRange grouping [D26.2].
// =======================================================================

TEST_CASE("ViewLists.blocks groups commands contiguously by blockId -- each block occupies exactly ONE "
          "contiguous BlockRange for a synthetic multi-block scene [gate matrix-issue06 D26.2 row]") {
    FakeAssetSource assets;
    MaterialHandle matA = assets.addMaterial(opaqueMaterial());
    MaterialAsset matBAsset = opaqueMaterial();
    matBAsset.doubleSided = true;  // distinct pipelineKey from matA, so sorting interleaves pipeline groups
    MaterialHandle matB = assets.addMaterial(matBAsset);

    Scene scene(assets.meshBoundsFn());
    // Renderables alternate block 0/block 1 AND material A/B, so a
    // correct implementation must still produce one contiguous run per
    // block despite the pipeline/material tiers wanting to interleave them.
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t blockId = i % 2;
        MaterialHandle mat = (i % 4 < 2) ? matA : matB;
        MeshHandle mesh =
            assets.addMesh({makeSubmesh(blockId, i * 10, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F - static_cast<float>(i))));
        static_cast<void>(addRenderable(scene, mesh));
    }

    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    builder.build(scene, testCamera(0.1F, 1000.0F), *scheduler, lists);

    CHECK(lists.counters.visible == 8);
    // Exactly one BlockRange per distinct blockId (2 blocks -> 2 ranges).
    std::unordered_set<uint32_t> seenBlockIds;
    for (const BlockRange& br : lists.blocks) {
        CHECK(seenBlockIds.count(br.blockId) == 0);  // never split across two ranges
        seenBlockIds.insert(br.blockId);
        // Every command in [firstCommand, firstCommand+commandCount) must
        // actually belong to this block -- cross-checked via firstIndex's
        // own block-encoding scheme (i*10 -> block i%2) is indirect, so
        // instead assert contiguity + coverage below.
        CHECK(br.commandCount > 0);
    }
    CHECK(seenBlockIds.size() == 2);
    uint32_t totalCovered = 0;
    for (const BlockRange& br : lists.blocks) {
        totalCovered += br.commandCount;
    }
    CHECK(totalCovered == lists.commands.size());
}

// =======================================================================
// D26.2: VkDrawIndexedIndirectCommand-compatible layout.
// =======================================================================

TEST_CASE("DrawCommand is byte-for-byte VkDrawIndexedIndirectCommand-compatible: same size, and a "
          "std::vector<DrawCommand> reinterpret-casts directly into an indirect-draw-shaped upload with no "
          "copy/transform step [D26.2, gate matrix-issue06 acceptance criterion]") {
#if RX_SCENE_TEST_HAS_VOLK
    static_assert(sizeof(DrawCommand) == sizeof(VkDrawIndexedIndirectCommand),
                  "DrawCommand must stay byte-compatible with VkDrawIndexedIndirectCommand");
    static_assert(offsetof(DrawCommand, indexCount) == offsetof(VkDrawIndexedIndirectCommand, indexCount));
    static_assert(offsetof(DrawCommand, instanceCount) == offsetof(VkDrawIndexedIndirectCommand, instanceCount));
    static_assert(offsetof(DrawCommand, firstIndex) == offsetof(VkDrawIndexedIndirectCommand, firstIndex));
    static_assert(offsetof(DrawCommand, vertexOffset) == offsetof(VkDrawIndexedIndirectCommand, vertexOffset));
    static_assert(offsetof(DrawCommand, firstInstance) == offsetof(VkDrawIndexedIndirectCommand, firstInstance));

    std::vector<DrawCommand> commands(3);
    commands[0] = DrawCommand{10, 2, 20, 5, 30};
    commands[1] = DrawCommand{11, 3, 21, 6, 31};
    commands[2] = DrawCommand{12, 4, 22, 7, 32};

    // The reinterpret-cast upload path: no transform, no copy, exactly
    // what an MDI upload does with this array.
    const auto* asIndirect = reinterpret_cast<const VkDrawIndexedIndirectCommand*>(commands.data());
    for (size_t i = 0; i < commands.size(); ++i) {
        CHECK(asIndirect[i].indexCount == commands[i].indexCount);
        CHECK(asIndirect[i].instanceCount == commands[i].instanceCount);
        CHECK(asIndirect[i].firstIndex == commands[i].firstIndex);
        CHECK(asIndirect[i].vertexOffset == commands[i].vertexOffset);
        CHECK(asIndirect[i].firstInstance == commands[i].firstInstance);
    }
#else
    WARN("volk.h not on the include path -- VkDrawIndexedIndirectCommand compatibility not checked this build");
#endif
}

// =======================================================================
// D24 -- residency-tolerant handle resolution at list-build time.
// =======================================================================

TEST_CASE("An evicted MATERIAL handle resolves to the fallback material's content/index at build() time -- "
          "never a crash, never a stale index; the draw still records via its (valid) mesh [D24]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    Scene scene(assets.meshBoundsFn());
    static_cast<void>(addRenderable(scene, mesh));

    assets.evictMaterial(mat);  // simulate eviction BEFORE build()

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    CHECK_NOTHROW(builder.build(scene, testCamera(0.1F, 1000.0F), *scheduler, lists));

    REQUIRE(lists.commands.size() == 1);
    REQUIRE(lists.payloads.size() == 1);
    CHECK(lists.payloads[0].materialIndex == FakeAssetSource::kFallbackMaterialIndex);
}

TEST_CASE("An evicted MESH handle resolves to the (empty) fallback submesh list at build() time -- zero draw "
          "records for that renderable, never a crash [D24]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    MeshHandle liveMesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    MeshHandle evictedMesh = assets.addMesh({makeSubmesh(0, 1, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -12.0F)));
    Scene scene(assets.meshBoundsFn());
    static_cast<void>(addRenderable(scene, liveMesh));
    static_cast<void>(addRenderable(scene, evictedMesh));

    assets.evictMesh(evictedMesh);

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    CHECK_NOTHROW(builder.build(scene, testCamera(0.1F, 1000.0F), *scheduler, lists));

    // The evicted mesh's worldBounds ALSO collapses to the fallback's
    // invalid/empty shape (Scene's own D24-at-the-proxy-level mechanism,
    // Task 18), so it is culled upstream too -- either way, zero draw
    // records, never a crash.
    CHECK(lists.commands.size() == 1);
    CHECK(lists.commands[0].firstIndex == 0);
}

// =======================================================================
// Shadow-caster culling [D15]: off-screen-caster-still-casts, conservative
// exclusion, BLEND exclusion (RC3), channel coupling (RC5).
// =======================================================================

TEST_CASE("buildShadow(): a caster fully OUTSIDE the camera frustum, whose shadow (extruded along the light "
          "direction) still reaches the camera-visible ground, appears in ShadowLists; a second caster outside "
          "BOTH the camera frustum AND the light's extruded bounds is absent [D15, gate ruling #6, Task 19's "
          "own named 'off-screen-caster-still-casts' step]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    Scene scene(assets.meshBoundsFn());

    // Ground: a thin slab at depth -10, comfortably inside the camera
    // frustum (half-extent at depth 10 is 10).
    MeshHandle groundMesh =
        assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, boxAt(glm::vec3(0.0F, 0.0F, -10.0F), glm::vec3(5.0F, 0.5F, 5.0F)));
    RenderableHandle ground = addRenderable(scene, groundMesh);
    static_cast<void>(ground);

    // Caster A: off-camera (x=-50, well beyond the frustum's half-width of
    // 10 at depth 10), same Y/Z footprint as the ground -- upstream of a
    // mostly-+X-travelling light, so its shadow should still reach the
    // ground (see buildShadow()'s own worked-example derivation).
    MeshHandle casterAMesh = assets.addMesh({makeSubmesh(0, 1, 3, 0, mat)}, boxAt(glm::vec3(-50.0F, 0.0F, -10.0F), glm::vec3(0.5F)));
    RenderableHandle casterA = addRenderable(scene, casterAMesh);

    // Caster B: off-camera AND off the light's cross-section (huge Y
    // offset, far beyond the ground's tiny +-0.5 Y extent) -- should be
    // excluded by the extruded-box test (proving the extrusion is
    // conservative, not "cast everything").
    MeshHandle casterBMesh =
        assets.addMesh({makeSubmesh(0, 2, 3, 0, mat)}, boxAt(glm::vec3(-50.0F, 2000.0F, -10.0F), glm::vec3(0.5F)));
    RenderableHandle casterB = addRenderable(scene, casterBMesh);
    static_cast<void>(casterB);

    DirectionalLightDesc lightDesc;
    lightDesc.dir = glm::normalize(glm::vec3(1.0F, 0.0F, 0.0F));  // travels toward +X
    LightHandle light = scene.createDirectionalLight(lightDesc);

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ShadowLists shadowLists;
    builder.buildShadow(scene, light, testCamera(0.1F, 1000.0F), *scheduler, shadowLists);

    // Independently confirm both casters are genuinely off-screen (not a
    // vacuous test): a separate camera-only build() must exclude them.
    ViewLists cameraLists;
    builder.build(scene, testCamera(0.1F, 1000.0F), *scheduler, cameraLists);
    CHECK(cameraLists.counters.visible == 1);  // ground only

    std::unordered_set<uint32_t> shadowFirstIndices;
    for (const DrawCommand& cmd : shadowLists.commands) {
        shadowFirstIndices.insert(cmd.firstIndex);
    }
    CHECK(shadowFirstIndices.count(0) == 1);  // ground casts its own shadow (a valid caster too)
    CHECK(shadowFirstIndices.count(1) == 1);  // caster A: off-screen but still casts
    CHECK(shadowFirstIndices.count(2) == 0);  // caster B: excluded -- conservative, not "cast everything"
    static_cast<void>(casterA);
}

TEST_CASE("buildShadow(): BLEND-partition renderables are EXCLUDED from ShadowLists entirely [RC3]; "
          "opaque+MASK renderables are included; ShadowLists is a single partition") {
    FakeAssetSource assets;
    MaterialHandle opaqueMat = assets.addMaterial(opaqueMaterial());
    MaterialHandle maskMat = assets.addMaterial(maskMaterial());
    MaterialHandle blendMat = assets.addMaterial(blendMaterial());
    Scene scene(assets.meshBoundsFn());

    MeshHandle mo = assets.addMesh({makeSubmesh(0, 0, 3, 0, opaqueMat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    MeshHandle mm = assets.addMesh({makeSubmesh(0, 1, 3, 0, maskMat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    MeshHandle mb = assets.addMesh({makeSubmesh(0, 2, 3, 0, blendMat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    static_cast<void>(addRenderable(scene, mo));
    static_cast<void>(addRenderable(scene, mm));
    static_cast<void>(addRenderable(scene, mb));

    DirectionalLightDesc lightDesc;
    LightHandle light = scene.createDirectionalLight(lightDesc);

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ShadowLists shadowLists;
    builder.buildShadow(scene, light, testCamera(0.1F, 1000.0F), *scheduler, shadowLists);

    CHECK(shadowLists.opaqueCommandCount == shadowLists.commands.size());  // single partition
    std::unordered_set<uint32_t> firstIndices;
    for (const DrawCommand& cmd : shadowLists.commands) {
        firstIndices.insert(cmd.firstIndex);
    }
    CHECK(firstIndices.count(0) == 1);  // opaque included
    CHECK(firstIndices.count(1) == 1);  // MASK included
    CHECK(firstIndices.count(2) == 0);  // BLEND excluded
}

TEST_CASE("buildShadow(): channel coupling (RC5) -- a renderable whose channels don't overlap the light's own "
          "channels is excluded from the caster list; castsShadows=false independently excludes too") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    Scene scene(assets.meshBoundsFn());

    MeshHandle mA = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    MeshHandle mB = assets.addMesh({makeSubmesh(0, 1, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    MeshHandle mC = assets.addMesh({makeSubmesh(0, 2, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    static_cast<void>(addRenderable(scene, mA, ~0U, /*channels=*/0x1U));                              // matches light
    static_cast<void>(addRenderable(scene, mB, ~0U, /*channels=*/0x2U));                              // does NOT match light
    static_cast<void>(addRenderable(scene, mC, ~0U, /*channels=*/0x1U, /*castsShadows=*/false));      // opted out

    DirectionalLightDesc lightDesc;
    lightDesc.channels = 0x1U;
    LightHandle light = scene.createDirectionalLight(lightDesc);

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ShadowLists shadowLists;
    builder.buildShadow(scene, light, testCamera(0.1F, 1000.0F), *scheduler, shadowLists);

    std::unordered_set<uint32_t> firstIndices;
    for (const DrawCommand& cmd : shadowLists.commands) {
        firstIndices.insert(cmd.firstIndex);
    }
    CHECK(firstIndices.count(0) == 1);
    CHECK(firstIndices.count(1) == 0);
    CHECK(firstIndices.count(2) == 0);
    CHECK(shadowLists.counters.shadowCastersConsidered >= 1);
    CHECK(shadowLists.counters.shadowCastersVisible == 1);
}

TEST_CASE("buildShadow() on a dead or shadow-disabled light produces an empty, well-formed ShadowLists rather "
          "than a throw") {
    FakeAssetSource assets;
    Scene scene(assets.meshBoundsFn());
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());

    DirectionalLightDesc desc;
    desc.castsShadows = false;
    LightHandle disabledLight = scene.createDirectionalLight(desc);
    ShadowLists lists;
    CHECK_NOTHROW(builder.buildShadow(scene, disabledLight, testCamera(), *scheduler, lists));
    CHECK(lists.commands.empty());

    LightHandle deadLight = scene.createDirectionalLight(DirectionalLightDesc{});
    scene.destroyLight(deadLight);
    ShadowLists lists2;
    CHECK_NOTHROW(builder.buildShadow(scene, deadLight, testCamera(), *scheduler, lists2));
    CHECK(lists2.commands.empty());
}

// =======================================================================
// Exact CullCounters [D18, gate ruling #6].
// =======================================================================

TEST_CASE("CullCounters are EXACT across a mixed layer/frustum/visible scene") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    Scene scene(assets.meshBoundsFn());

    // 2 layer-culled, 3 frustum-culled, 5 visible.
    for (int i = 0; i < 2; ++i) {
        MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
        static_cast<void>(addRenderable(scene, mesh, /*layers=*/0x2U));  // won't match cullMask below
    }
    for (int i = 0; i < 3; ++i) {
        MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(500.0F, 0.0F, -10.0F)));
        static_cast<void>(addRenderable(scene, mesh));
    }
    for (int i = 0; i < 5; ++i) {
        MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
        static_cast<void>(addRenderable(scene, mesh));
    }

    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    Camera cam = testCamera();
    cam.cullMask = 0x1U;
    builder.build(scene, cam, *scheduler, lists);

    CHECK(lists.counters.totalCandidates == 10);
    CHECK(lists.counters.culledByLayerMask == 2);
    CHECK(lists.counters.culledByFrustum == 3);
    CHECK(lists.counters.visible == 5);
    CHECK(lists.counters.recordsIn == 5);
    CHECK(lists.counters.drawsSubmitted == 1);  // all 5 identical -> collapsed to one instanced draw
}

// =======================================================================
// Determinism across thread counts [gate ruling #6].
// =======================================================================

TEST_CASE("build() produces byte-identical ViewLists across --threads 1/2/8 for the same scene [gate ruling #6 "
          "'Determinism across thread counts' -- fixed index-range chunks + deterministic serial compaction]") {
    FakeAssetSource assets;
    MaterialHandle matA = assets.addMaterial(opaqueMaterial());
    MaterialAsset matBAsset = opaqueMaterial();
    matBAsset.doubleSided = true;
    MaterialHandle matB = assets.addMaterial(matBAsset);
    MaterialHandle blendMat = assets.addMaterial(blendMaterial());

    Scene scene(assets.meshBoundsFn());
    constexpr uint32_t kCount = 2000;
    for (uint32_t i = 0; i < kCount; ++i) {
        const uint32_t blockId = i % 3;
        MaterialHandle mat = (i % 5 == 0) ? blendMat : ((i % 2 == 0) ? matA : matB);
        const float z = -1.0F - static_cast<float>(i % 50);
        // A handful of shared mesh identities (i % 17) so collapse runs are
        // real and exercised too, not just the trivial "everyone unique"
        // case that would make this test weaker.
        MeshHandle mesh =
            assets.addMesh({makeSubmesh(blockId, (i % 17) * 4, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, z)));
        static_cast<void>(addRenderable(scene, mesh, /*layers=*/~0U, /*channels=*/0xFFU, /*castsShadows=*/true,
                                          static_cast<uint8_t>(i % 8)));
    }

    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    Camera cam = testCamera(0.1F, 1000.0F);

    std::optional<ViewLists> reference;
    for (uint32_t threads : {1U, 2U, 8U}) {
        auto scheduler = rx::task::Scheduler::create(threads);
        REQUIRE(scheduler != nullptr);
        ViewLists lists;
        builder.build(scene, cam, *scheduler, lists);
        if (!reference.has_value()) {
            reference = lists;
            continue;
        }
        INFO("threads=", threads);
        REQUIRE(lists.commands.size() == reference->commands.size());
        for (size_t i = 0; i < lists.commands.size(); ++i) {
            CHECK(lists.commands[i] == reference->commands[i]);
        }
        REQUIRE(lists.payloads.size() == reference->payloads.size());
        for (size_t i = 0; i < lists.payloads.size(); ++i) {
            CHECK(lists.payloads[i] == reference->payloads[i]);
        }
        REQUIRE(lists.blocks.size() == reference->blocks.size());
        for (size_t i = 0; i < lists.blocks.size(); ++i) {
            CHECK(lists.blocks[i] == reference->blocks[i]);
        }
        CHECK(lists.opaqueCommandCount == reference->opaqueCommandCount);
        CHECK(lists.counters == reference->counters);
    }
}

// =======================================================================
// Zero-alloc, caller-owned storage [D26 amendment].
//
// COUNTING MECHANISM: a scoped global operator new/delete override, active
// ONLY while `g_countingAllocations` is set. Counts TOTAL allocation
// CALLS (monotonic, never decremented on free) -- deliberately NOT a net
// (allocs-minus-frees) live count, a NET-count design this task tried
// FIRST and empirically found unsound (kept here as the documented reason,
// not a hypothetical): a buggy "discard and reallocate from empty every
// call" implementation still ends each call with exactly ONE live buffer
// for a given vector (same as a correctly-reused one), so a net-count
// delta across the call is ZERO either way -- the regrowth's own
// alloc/free PAIRS cancel out in a net metric even though real allocator
// traffic genuinely occurred. A MONOTONIC total-calls counter cannot be
// fooled the same way: every real `operator new` invocation during the
// window increments it permanently, so a call that reallocates ANYTHING
// shows a nonzero delta even though its net live-count change is zero.
// Verified directly, this task: a deliberate revert-probe (reassigning
// `out.commands = std::vector<DrawCommand>();` instead of `.clear()`)
// passed BOTH the capacity-equality and net-live-count checks, and only
// this monotonic counter caught it -- see the task report's
// revert-discrimination section for the full account.
// =======================================================================

namespace {
std::atomic<int64_t> g_totalAllocationCalls{0};
std::atomic<bool> g_countingAllocations{false};
}  // namespace

void* operator new(size_t size) {
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    if (g_countingAllocations.load(std::memory_order_relaxed)) {
        g_totalAllocationCalls.fetch_add(1, std::memory_order_relaxed);
    }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void* operator new[](size_t size) { return operator new(size); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

TEST_CASE("build() into reused ViewLists/DrawListBuilder storage performs zero net capacity growth across "
          "steady-state frames after a warm-up call [D26 zero-alloc invariant]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    MaterialHandle blendMat = assets.addMaterial(blendMaterial());
    Scene scene(assets.meshBoundsFn());
    constexpr uint32_t kCount = 500;
    for (uint32_t i = 0; i < kCount; ++i) {
        MaterialHandle m = (i % 4 == 0) ? blendMat : mat;
        MeshHandle mesh = assets.addMesh({makeSubmesh(i % 2, (i % 9) * 4, 3, 0, m)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
        static_cast<void>(addRenderable(scene, mesh));
    }

    auto scheduler = rx::task::Scheduler::create(4);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    Camera cam = testCamera(0.1F, 1000.0F);

    ViewLists lists;
    builder.build(scene, cam, *scheduler, lists);  // warm-up: establishes peak capacity

    const size_t commandsCap = lists.commands.capacity();
    const size_t payloadsCap = lists.payloads.capacity();
    const size_t blocksCap = lists.blocks.capacity();
    // Capacity equality and `.data()` pointer identity are BOTH checked
    // below too, but neither is sufficient ON ITS OWN (verified directly,
    // this task, via a deliberate revert-probe -- see the task report's
    // revert-discrimination section): a buggy "discard and reallocate from
    // empty every call" implementation can still coincidentally reach the
    // SAME final `.capacity()` (std::vector's geometric growth from empty
    // is a deterministic function of final size alone) AND the SAME
    // `.data()` address (glibc's allocator commonly hands back the
    // just-freed block immediately for a same-size request). The
    // g_totalAllocationCalls monotonic counter below (captured after this
    // point) is the mechanism that actually catches it; capacity/pointer
    // checks are kept as cheap, corroborating signals, not the primary
    // proof.
    const DrawCommand* commandsData = lists.commands.data();
    const DrawPayload* payloadsData = lists.payloads.data();
    const BlockRange* blocksData = lists.blocks.data();
    const rx::scene::detail::DrawListBuilderCapacitiesForTesting builderCaps1 =
        rx::scene::detail::capacitiesForTesting(builder);

    // Baseline: the FIRST post-warm-up call's own total-allocation-call
    // delta. Any incidental, legitimate per-call churn this class does not
    // own (e.g. rx_task's own std::function/task-node bookkeeping inside
    // parallelFor(), which this test makes no claim about) shows up here
    // too -- captured once and required to be small AND stable across
    // every subsequent steady-state call, rather than assumed to be
    // exactly zero.
    int64_t baselineDelta = -1;
    constexpr int kSteadyFrames = 10;
    for (int frame = 0; frame < kSteadyFrames; ++frame) {
        const int64_t before = g_totalAllocationCalls.load(std::memory_order_relaxed);
        g_countingAllocations.store(true, std::memory_order_relaxed);
        builder.build(scene, cam, *scheduler, lists);
        g_countingAllocations.store(false, std::memory_order_relaxed);
        const int64_t delta = g_totalAllocationCalls.load(std::memory_order_relaxed) - before;

        INFO("frame=", frame, " allocation-call delta=", delta);
        if (frame == 0) {
            baselineDelta = delta;
            MESSAGE("rx_scene zero-alloc: steady-state build() call made ", delta,
                    " operator-new call(s) (baseline; must stay small and IDENTICAL every subsequent frame)");
            // A ceiling, not a guess: the revert-probe this task ran (see
            // the task report) showed a buggy reallocate-from-empty
            // implementation needs double-digit operator-new calls just to
            // regrow ONE 500-entry vector back to its steady size via
            // geometric doubling -- far above this ceiling, which only
            // needs to be generous enough for whatever legitimate,
            // unrelated-to-this-class churn rx_task's own parallelFor()
            // bookkeeping performs.
            CHECK(baselineDelta <= 4);
        } else {
            CHECK(delta == baselineDelta);
        }
        CHECK(lists.commands.capacity() == commandsCap);
        CHECK(lists.payloads.capacity() == payloadsCap);
        CHECK(lists.blocks.capacity() == blocksCap);
        CHECK(lists.commands.data() == commandsData);
        CHECK(lists.payloads.data() == payloadsData);
        CHECK(lists.blocks.data() == blocksData);

        const auto builderCapsN = rx::scene::detail::capacitiesForTesting(builder);
        CHECK(builderCapsN.cullReasons == builderCaps1.cullReasons);
        CHECK(builderCapsN.visibleIndices == builderCaps1.visibleIndices);
        CHECK(builderCapsN.pending == builderCaps1.pending);
        CHECK(builderCapsN.opaqueRecords == builderCaps1.opaqueRecords);
        CHECK(builderCapsN.blendRecords == builderCaps1.blendRecords);
        CHECK(builderCapsN.opaqueGroups == builderCaps1.opaqueGroups);
    }
}

// =======================================================================
// D27 -- main-thread pre-resolution + worker-guard.
// =======================================================================

TEST_CASE("resolveDrawGroups(): a single linear scan calls the resolver exactly once per distinct materialIndex "
          "boundary in the sorted list, never a second grouping pass [D27]") {
    ViewLists lists;
    // 4 commands: material 1, 1, 2, 1 (non-adjacent repeat of material 1 --
    // a correct implementation still resolves material 1 TWICE, once per
    // contiguous run, never memoizing across non-adjacent runs, and never
    // re-scanning already-grouped commands).
    const std::vector<uint32_t> materialSequence = {1, 1, 2, 1};
    for (uint32_t mat : materialSequence) {
        DrawCommand cmd;
        cmd.firstInstance = static_cast<uint32_t>(lists.payloads.size());
        cmd.instanceCount = 1;
        lists.commands.push_back(cmd);
        lists.payloads.push_back(DrawPayload{mat, 0});
    }

    std::vector<uint32_t> resolveCalls;
    auto resolver = [&](uint32_t materialIndex) -> uint32_t {
        resolveCalls.push_back(materialIndex);
        return materialIndex * 100;
    };
    const auto groups = rx::scene::resolveDrawGroups(lists, resolver);

    REQUIRE(resolveCalls.size() == 3);  // {1},{2},{1} -- three contiguous runs, three calls
    CHECK(resolveCalls[0] == 1);
    CHECK(resolveCalls[1] == 2);
    CHECK(resolveCalls[2] == 1);

    REQUIRE(groups.size() == 3);
    CHECK(groups[0] == rx::scene::ResolvedDrawGroup{0, 2, 100});
    CHECK(groups[1] == rx::scene::ResolvedDrawGroup{2, 1, 200});
    CHECK(groups[2] == rx::scene::ResolvedDrawGroup{3, 1, 100});
}

#ifdef RX_DEBUG_CHECKS
namespace {

std::atomic<std::vector<std::string>*> g_recordDrawListCapture{nullptr};
std::mutex g_recordDrawListCaptureMutex;

void captureRecordDrawListViolation(const char* context) {
    auto* capture = g_recordDrawListCapture.load(std::memory_order_relaxed);
    if (capture == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_recordDrawListCaptureMutex);
    capture->emplace_back(context != nullptr ? context : "");
}

struct ViolationHookGuard {
    ~ViolationHookGuard() {
        g_recordDrawListCapture.store(nullptr, std::memory_order_relaxed);
        rx::core::debug::detail::setViolationHookForTests(nullptr);
    }
};

ViewLists makeSimpleLists(uint32_t commandCount) {
    ViewLists lists;
    for (uint32_t i = 0; i < commandCount; ++i) {
        DrawCommand cmd;
        cmd.firstInstance = static_cast<uint32_t>(lists.payloads.size());
        cmd.instanceCount = 1;
        lists.commands.push_back(cmd);
        lists.payloads.push_back(DrawPayload{i, 0});  // every command a distinct materialIndex -> N groups
    }
    return lists;
}

}  // namespace

TEST_CASE("recordDrawList()'s RecordChunkFn callback -- proven to run on genuine background worker threads via "
          "a deterministic rendezvous barrier -- NEVER triggers the main-thread guard the pre-resolution "
          "resolver carries: the chunk callback structurally never sees PipelineResolveFn at all [D27; gate "
          "ruling #6 'D27 worker-guard test reuses setViolationHookForTests + the rendezvous-barrier pattern "
          "from test_material_system.cpp:854-1042'; adapted here to rx::task::Scheduler::parallelFor's own "
          "documented n-way barrier precedent (rx_task/tests/scheduler_test.cpp), since this device-free "
          "library has no rx_graph::Executor/live VkDevice to bind the ORIGINAL test's own GPU scaffolding to]") {
    const ViewLists lists = makeSimpleLists(6);
    std::vector<std::string> capture;
    g_recordDrawListCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureRecordDrawListViolation);
    ViolationHookGuard hookGuard;

    auto resolver = [](uint32_t materialIndex) -> uint32_t {
        RX_ASSERT_MAIN_THREAD("test resolvePipeline (legal path)");
        return materialIndex;
    };

    constexpr uint32_t kWorkerCount = 6;
    auto scheduler = rx::task::Scheduler::create(kWorkerCount);
    REQUIRE(scheduler != nullptr);

    // Deterministic n-way barrier [rx_task/tests/scheduler_test.cpp's own
    // precedent, cited above]: `chunkCount` chunks, grainSize 1, each
    // increments `arrived` then busy-waits for ALL to arrive. This can
    // ONLY resolve if every chunk is alive and making progress
    // CONCURRENTLY -- if parallelFor() ever ran the whole range
    // sequentially on one thread, the first chunk's wait would spin to
    // its own timeout with nothing else able to bump the counter.
    constexpr uint32_t kChunkCount = kWorkerCount;
    std::atomic<uint32_t> arrived{0};
    std::atomic<uint32_t> timeouts{0};

    rx::scene::RecordChunkFn recordChunk = [&](uint32_t /*chunkIndex*/, uint32_t chunkCount,
                                                  std::span<const rx::scene::ResolvedDrawGroup> groups) {
        CHECK(chunkCount == kChunkCount);
        // Touch the pre-resolved plain data ONLY -- never the resolver
        // (structurally unreachable: RecordChunkFn's own signature never
        // receives PipelineResolveFn -- see draw_list.h's own comment).
        uint32_t total = 0;
        for (const auto& g : groups) {
            total += g.commandCount;
        }
        CHECK(total == 6);

        arrived.fetch_add(1, std::memory_order_acq_rel);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (arrived.load(std::memory_order_acquire) < kChunkCount) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timeouts.fetch_add(1, std::memory_order_acq_rel);
                return;
            }
            std::this_thread::yield();
        }
    };

    rx::scene::recordDrawList(lists, *scheduler, kChunkCount, resolver, recordChunk);

    REQUIRE(timeouts.load() == 0);
    CHECK(arrived.load() == kChunkCount);
    std::lock_guard<std::mutex> lock(g_recordDrawListCaptureMutex);
    CHECK(capture.empty());  // the (proven-concurrent) chunk callbacks never touched the guarded resolver.
}

TEST_CASE("Adversarial control: a chunk callback that DELIBERATELY calls the guarded resolver from a thread "
          "PROVEN (two-chunk rendezvous barrier, same construction as test_material_system.cpp:1042-1168) to "
          "be a genuine non-main worker DOES trip RX_ASSERT_MAIN_THREAD -- proves the guard mechanism itself "
          "would have caught the bug the test above proves absent, not merely that nothing was ever tried") {
    auto resolver = [](uint32_t materialIndex) -> uint32_t {
        RX_ASSERT_MAIN_THREAD("test resolvePipeline (deliberately illegal call site)");
        return materialIndex;
    };
    std::vector<std::string> capture;
    g_recordDrawListCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureRecordDrawListViolation);
    ViolationHookGuard hookGuard;

    constexpr uint32_t kWorkerCount = 6;
    auto scheduler = rx::task::Scheduler::create(kWorkerCount);
    REQUIRE(scheduler != nullptr);

    const std::thread::id mainThreadId = std::this_thread::get_id();
    std::atomic<uint32_t> barrierArrived{0};
    std::atomic<bool> barrierTimedOut{false};
    std::atomic<bool> claimed{false};
    std::atomic<uint32_t> illegalCalls{0};
    constexpr auto kBarrierTimeout = std::chrono::seconds(10);

    // Two designated participants (chunks 0 and 1, out of >=3 total) form
    // the mutual barrier; every other chunk is a no-op -- same "a single
    // thread cannot satisfy a mutual wait with itself" argument
    // test_material_system.cpp's own comment spells out in full: at most
    // one of {chunk 0, chunk 1} can be mainThreadId, so at least one
    // reaching the post-barrier code is unconditionally a genuine worker.
    scheduler->parallelFor(6, 1, [&](uint32_t begin, uint32_t /*end*/, uint32_t) {
        const uint32_t chunkIndex = begin;
        if (chunkIndex != 0 && chunkIndex != 1) {
            return;
        }
        barrierArrived.fetch_add(1, std::memory_order_acq_rel);
        const auto waitStart = std::chrono::steady_clock::now();
        while (barrierArrived.load(std::memory_order_acquire) < 2) {
            if (std::chrono::steady_clock::now() - waitStart > kBarrierTimeout) {
                barrierTimedOut.store(true, std::memory_order_relaxed);
                return;
            }
            std::this_thread::yield();
        }
        if (std::this_thread::get_id() == mainThreadId) {
            return;  // legal either way -- the other participant proves the worker case.
        }
        bool expected = false;
        if (!claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        illegalCalls.fetch_add(1, std::memory_order_relaxed);
        static_cast<void>(resolver(0));  // deliberately illegal: calling the guarded resolver off the main thread.
    });

    REQUIRE_FALSE(barrierTimedOut.load());
    REQUIRE(illegalCalls.load() == 1);
    std::lock_guard<std::mutex> lock(g_recordDrawListCaptureMutex);
    REQUIRE(capture.size() == 1);
    CHECK(capture[0] == "test resolvePipeline (deliberately illegal call site)");
}
#endif  // RX_DEBUG_CHECKS

// =======================================================================
// Scene::aliveSpan()/generationsSpan() [Task 19 addition -- see the task
// report for why these were necessary].
// =======================================================================

TEST_CASE("Scene::aliveSpan()/generationsSpan() correctly distinguish a live slot from a destroyRenderable()d "
          "hole still holding stale column values -- build() must never treat a freed slot as a candidate "
          "[Task 19 addition, necessary correctness fix]") {
    FakeAssetSource assets;
    MaterialHandle mat = assets.addMaterial(opaqueMaterial());
    MeshHandle mesh = assets.addMesh({makeSubmesh(0, 0, 3, 0, mat)}, unitCubeAt(glm::vec3(0.0F, 0.0F, -10.0F)));
    Scene scene(assets.meshBoundsFn());

    RenderableHandle h0 = addRenderable(scene, mesh);
    RenderableHandle h1 = addRenderable(scene, mesh);
    RenderableHandle h2 = addRenderable(scene, mesh);
    static_cast<void>(h0);
    static_cast<void>(h2);

    REQUIRE(scene.aliveSpan().size() == 3);
    REQUIRE(scene.generationsSpan().size() == 3);
    CHECK(scene.aliveSpan()[1] != 0);

    scene.destroyRenderable(h1);  // leaves layers_[1]/mesh_[1]/etc. at their stale last values (scene.cpp's own contract)
    CHECK(scene.aliveSpan()[1] == 0);

    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);
    DrawListBuilder builder(assets.meshSubmeshesFn(), assets.materialResolveFn());
    ViewLists lists;
    builder.build(scene, testCamera(0.1F, 1000.0F), *scheduler, lists);

    CHECK(lists.counters.totalCandidates == 2);  // the freed slot is NOT a candidate
    CHECK(lists.counters.visible == 2);
    CHECK(lists.counters.recordsIn == 2);
}
