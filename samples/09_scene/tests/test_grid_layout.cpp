// samples/09_scene/tests/test_grid_layout.cpp -- proves gridInstanceTransform()
// (../grid_layout.h) actually composes the imported asset's own node
// rotation into a grid instance's transform, instead of silently dropping
// it [Issue #35: the default DamagedHelmet grid rendered every helmet
// lying on its back, visor toward the sky, because populateHelmetGrid()
// used to hand-build each instance's transform as pure grid-placement
// translation ALONE -- gridTransform(row, col, spacing), with no rotation
// at all -- while the --scene import path (populateImportedInstances(),
// InstanceRecord::worldTransform) and sample 08's own single-helmet view
// both apply DamagedHelmet.gltf's own root-node rotation].
//
// WHY A TRANSFORM-LEVEL TEST, NOT JUST A PIXEL-DIFF GATE: this sample's own
// D17 headless gate (main.cpp's runHeadless()) already pixel-diffs every
// frame against a COMMITTED reference PNG -- but that gate proved BLIND to
// exactly this defect, because the committed reference was itself
// regenerated FROM the buggy (rotation-dropped) render: a reference baked
// from the bug certifies the bug right along with it, and the gate cannot
// tell the difference between "correct" and "wrong, but self-consistent"
// without an independent source of truth. This test supplies that
// independent source of truth directly: DamagedHelmet.gltf's own root node
// (assets/fetched/DamagedHelmet/glTF/DamagedHelmet.gltf, node index 0) has
// exactly one property, "rotation": [0.7071068286895752, 0.0, -0.0,
// 0.7071068286895752] (glTF's own [x, y, z, w] quaternion order) -- a
// +90-degree rotation about world X. That value is hard-coded below,
// completely independent of gridInstanceTransform()'s own implementation,
// so a regression that silently drops or zeroes the composed rotation
// fails this test regardless of what any reference PNG says.
//
// Device-free (no VkDevice/Window/Registry/GeometryPool) -- grid_layout.h
// is plain glm data transformation, same precedent as
// test_fly_camera.cpp/test_mouse_capture.cpp's own top comments.
#include "../grid_layout.h"

#include <doctest/doctest.h>

#include <glm/gtc/quaternion.hpp>

using rx::samples9::gridInstanceTransform;
using rx::samples9::gridTransform;

namespace {
constexpr float kEps = 1e-5F;

bool mat3AlmostEqual(const glm::mat3& a, const glm::mat3& b, float eps) {
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            if (std::abs(a[col][row] - b[col][row]) > eps) {
                return false;
            }
        }
    }
    return true;
}

bool vec3AlmostEqual(const glm::vec3& a, const glm::vec3& b, float eps) { return glm::length(a - b) <= eps; }

// DamagedHelmet.gltf's own node 0 "rotation" quaternion, verbatim (glTF
// stores [x, y, z, w]; glm::quat's constructor takes (w, x, y, z)) -- see
// this file's own top comment. A +90-degree rotation about world X.
glm::mat4 damagedHelmetNodeTransform() {
    const glm::quat q(0.7071068286895752F, 0.7071068286895752F, 0.0F, -0.0F);
    return glm::mat4_cast(q);
}
}  // namespace

TEST_CASE("gridInstanceTransform composes DamagedHelmet.gltf's own node rotation into every grid cell "
          "[Issue #35 regression: the grid used to drop this rotation entirely]") {
    const glm::mat4 assetNodeTransform = damagedHelmetNodeTransform();
    constexpr float spacing = 2.0F;
    constexpr uint32_t gridCols = 4;

    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t col = 0; col < gridCols; ++col) {
            const glm::mat4 composed = gridInstanceTransform(row, col, spacing, gridCols, assetNodeTransform);

            // Rotational part: composing a pure translation (gridTransform)
            // with assetNodeTransform on the right must leave the asset's
            // own rotation untouched in the result -- exactly the
            // composition Issue #35's fix introduces, and exactly what the
            // pre-fix code (gridTransform(...) alone, no assetNodeTransform
            // at all) could never produce.
            CHECK(mat3AlmostEqual(glm::mat3(composed), glm::mat3(assetNodeTransform), kEps));

            // ... and it must NOT be identity -- the pre-fix bug's own
            // exact signature (a "dropped rotation" always manifests as an
            // identity rotational part, regardless of how it was dropped:
            // never composing assetNodeTransform at all, or composing an
            // identity in its place).
            CHECK_FALSE(mat3AlmostEqual(glm::mat3(composed), glm::mat3(1.0F), kEps));

            // Translation part: DamagedHelmet's own node carries NO
            // translation or scale (rotation only), so the composed
            // instance's translation must land exactly on the plain grid
            // placement -- a regression that also breaks placement (e.g.
            // composing in the wrong order, applying assetNodeTransform's
            // translation column) is caught here too.
            const glm::vec3 expectedTranslation = glm::vec3(gridTransform(row, col, spacing, gridCols)[3]);
            CHECK(vec3AlmostEqual(glm::vec3(composed[3]), expectedTranslation, kEps));
        }
    }
}

TEST_CASE("gridInstanceTransform with an identity asset node transform reduces to plain gridTransform() "
          "placement [documents the Issue #35 regression signature this test discriminates against]") {
    constexpr float spacing = 2.0F;
    constexpr uint32_t gridCols = 4;
    constexpr uint32_t row = 2;
    constexpr uint32_t col = 3;

    const glm::mat4 composed = gridInstanceTransform(row, col, spacing, gridCols, glm::mat4(1.0F));
    const glm::mat4 plain = gridTransform(row, col, spacing, gridCols);
    CHECK(mat3AlmostEqual(glm::mat3(composed), glm::mat3(plain), kEps));
    CHECK(mat3AlmostEqual(glm::mat3(composed), glm::mat3(1.0F), kEps));

    // This identity-rotation result must NOT match DamagedHelmet's real
    // node rotation -- the discriminating assertion the case above relies
    // on to fail loudly if a future change re-drops the rotation.
    const glm::mat4 assetNodeTransform = damagedHelmetNodeTransform();
    CHECK_FALSE(mat3AlmostEqual(glm::mat3(composed), glm::mat3(assetNodeTransform), kEps));
}
