#pragma once
// samples/09_scene/grid_layout.h -- rx::samples9::gridTransform()/
// gridInstanceTransform(), the pure grid-placement + asset-node-rotation
// composition the default DamagedHelmet grid (populateHelmetGrid(),
// main.cpp) uses to place each instance.
//
// WHY THIS EXISTS [Issue #35]: populateHelmetGrid() used to hand-build each
// instance's transform as gridTransform(row, col, spacing) ALONE -- a pure
// translation, no rotation at all. DamagedHelmet.gltf's own single root
// node carries a rotation (+90 degrees about world X; quaternion
// x=0.7071068, y=0, z=0, w=0.7071068 -- see
// assets/fetched/DamagedHelmet/glTF/DamagedHelmet.gltf's own "nodes" array)
// that the --scene import path (populateImportedInstances(), which reads
// InstanceRecord::worldTransform directly) and sample 08's own
// single-helmet view both apply -- but the grid path silently dropped it:
// the grid's helmets rendered lying on their backs (visor toward the sky)
// while every import-path helmet rendered upright. gridInstanceTransform()
// composes the two so the grid matches import-path orientation exactly:
// `gridTransform(...) * assetNodeTransform` -- grid placement in world
// space, applied to the mesh AFTER the asset's own node rotation, the same
// composition order populateImportedInstances() gets for free since its
// own `instance.worldTransform` already IS the node transform (there is
// nothing else to compose it with there).
//
// Extracted to a device-free header -- same "no VkDevice/Window seam"
// precedent as fly_camera.h/mouse_capture.h/draw_recording.h's own top
// comments -- so samples/09_scene/tests/test_grid_layout.cpp can assert the
// composed rotation against DamagedHelmet.gltf's own hard-coded node
// quaternion directly. A dropped rotation (reverting to gridTransform(...)
// alone, or passing an identity assetNodeTransform) fails that test
// immediately -- no GPU, no lavapipe, no rendered/regenerated reference PNG
// in the loop at all (see that test file's own top comment for why the
// pixel-only D17 gate alone proved insufficient: a reference regenerated
// FROM the buggy render certifies the bug right along with it).
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace rx::samples9 {

// Pure grid-cell placement -- world-space translation only, no rotation:
// row 0 nearest the camera, walking -Z per row; columns centered on X.
[[nodiscard]] inline glm::mat4 gridTransform(uint32_t row, uint32_t col, float spacing, uint32_t gridCols) {
    const float x = (static_cast<float>(col) - 0.5F * static_cast<float>(gridCols - 1)) * spacing;
    const float z = -(static_cast<float>(row) * spacing);
    return glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.0F, z));
}

// Composes a grid cell's placement with the imported asset's own node
// transform (InstanceRecord::worldTransform from the import path -- see
// this header's own top comment) so a grid instance renders in the SAME
// orientation the import path (populateImportedInstances(), sample 08)
// already does. `assetNodeTransform` pre-multiplies on the right: it acts
// on the local mesh FIRST, then the whole (now-upright) instance is placed
// in the grid -- pass glm::mat4(1.0F) for an asset with no meaningful node
// transform to recover plain gridTransform() behavior.
[[nodiscard]] inline glm::mat4 gridInstanceTransform(uint32_t row, uint32_t col, float spacing, uint32_t gridCols,
                                                       const glm::mat4& assetNodeTransform) {
    return gridTransform(row, col, spacing, gridCols) * assetNodeTransform;
}

}  // namespace rx::samples9
