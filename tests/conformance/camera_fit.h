#pragma once
// tests/conformance/camera_fit.h -- Phase 5 Stage 1 Task 11 (#47), gate
// ruling T11: the "matched camera" half of the conformance harness's own
// methodology.
//
// PORTED, not re-derived, from glTF-Sample-Renderer's own default "fit
// camera to scene" algorithm (source/gltf/user_camera.js's
// UserCamera::resetView() -> fitDistanceToExtents() /
// fitCameraTargetToExtents() / fitCameraPlanesToExtents(), pinned commit
// 863b981fb755359063e370ff7b6e956bda0716e2, see tools/
// fetch_gltf_sample_renderer.sh's own header comment for the pin
// rationale) -- function-by-function, line-by-line, cited by name in this
// file's own implementation. tools/gltf_conformance/harness.html calls
// the ACTUAL JS resetView() (not a reimplementation) when generating each
// committed reference; THIS port is what RendererX's own conformance
// render tool (main.cpp) uses to frame the identical camera against its
// own imported scene. Both sides are therefore expected to converge on
// NUMERICALLY IDENTICAL camera parameters from the same deterministic
// closed-form formula over the same world-space scene AABB and the same
// square (aspectRatio=1.0) render resolution -- not merely "close enough"
// -- which is what makes "matched camera" a verifiable claim rather than
// an eyeballed one.
//
// SCOPE: this is a mechanical, closed-form geometric port (bounding-box
// fit distance/target/near-far-plane math) -- explicitly NOT a from-
// scratch reimplementation of anything BRDF/lighting-related; the
// "prefer ready-made or ported implementations" standing rule is
// satisfied by porting the reference renderer's own actual algorithm
// rather than inventing an independent "reasonable-looking" framing that
// would not actually match.
//
// Thread-affinity: none (pure host-side math, no GPU/Vulkan object
// touched).

#include <rx_asset/mesh_asset.h>

#include <glm/glm.hpp>

#include <span>

namespace rx::conformance {

// Port of getExtentsFromAccessor()'s own SECOND step (source/gltf/
// gltf_utils.js, pinned commit -- easy to miss on a first read, as this
// task's own development did: the function does NOT return the tight
// per-primitive AABB it just computed from the 8 transformed corners. It
// goes one step further and REPLACES that box with the smallest CUBE
// that inscribes the box's own bounding SPHERE:
//   const center = 0.5*(boxMin+boxMax);
//   const radius = length(boxMax - center);   // HALF THE BOX'S OWN DIAGONAL, not a per-axis half-extent.
//   outMin = center - radius; outMax = center + radius;   // per axis -- always a CUBE.
// getSceneExtents() then unions these per-PRIMITIVE cubes (not the tight
// boxes) across the whole scene -- so the two orders genuinely differ
// (union-of-cubes != cube-of-union) and only THIS order reproduces the
// reference renderer's real numbers (verified directly this task: an
// instrumented run of the actual reference renderer's own resetView()
// output was cross-checked against a hand derivation of both orders --
// only the per-primitive-then-union order matched, see task-11-report.md).
[[nodiscard]] inline rx::asset::AABB sphereInscribingCubeOf(const rx::asset::AABB& box) {
    if (!box.isValid()) {
        return box;
    }
    const glm::vec3 center = 0.5F * (box.min + box.max);
    const float radius = glm::length(box.max - center);
    rx::asset::AABB cube;
    cube.min = center - glm::vec3(radius);
    cube.max = center + glm::vec3(radius);
    return cube;
}

// Port of getSceneExtents()'s own accumulation loop: ONE
// sphereInscribingCubeOf() call PER PRIMITIVE (RendererX's own Submesh
// granularity -- verified load-bearing this task: MetalRoughSpheresNo
// Textures' own "Metal"/"Non-metal"/"Smooth"/"Rough" text-label meshes
// are each MULTIPLE primitives, one per glyph, so computing this at
// per-INSTANCE/whole-mesh granularity instead would silently diverge from
// the reference for exactly that model), unioned together via plain
// AABB::unionOf(). `perPrimitiveWorldBounds`: each submesh's own LOCAL
// `Submesh::bounds` transformed by its owning instance's world transform
// (`AABB::transformed()`) -- the caller's own responsibility (this
// function is a pure accumulator, no Registry/Scene dependency, matching
// this codebase's own established "small composable free function" shape
// for exactly this kind of derived-geometry helper).
[[nodiscard]] inline rx::asset::AABB accumulateReferenceStyleSceneExtents(
    std::span<const rx::asset::AABB> perPrimitiveWorldBounds) {
    rx::asset::AABB result;
    for (const rx::asset::AABB& box : perPrimitiveWorldBounds) {
        result = rx::asset::AABB::unionOf(result, sphereInscribingCubeOf(box));
    }
    return result;
}

struct CameraFit {
    glm::vec3 position{0.0F, 0.0F, 1.0F};
    glm::vec3 target{0.0F, 0.0F, 0.0F};
    float yfovRadians = glm::radians(45.0F);  // user_camera.js's own PerspectiveCamera default (source/gltf/camera.js).
    float znear = 0.01F;
    float zfar = 100.0F;
};

// `worldBoundsAABB`: MUST be `accumulateReferenceStyleSceneExtents()`'s
// own output (this header's own function above), NOT a plain tight union
// of instance/submesh world bounds (e.g. samples/08_gltf_viewer's own
// App::sceneBoundsWorld / rx::asset::AABB::unionOf over InstanceRecord::
// worldBounds) -- see that function's own header comment for why the
// distinction is load-bearing, not stylistic: the reference renderer's
// own resetView() ALWAYS fits against the sphere-inscribing-cube-unioned
// extent, never the tight box. `aspectRatio`/`yfovRadians` default to
// this harness's own fixed settings (512x512 square render, 45-degree
// default yfov, matching user_camera.js's own PerspectiveCamera default
// AND harness.html's own unmodified state.userCamera.perspective.yfov)
// -- overridable only for device-free unit testing at other aspect
// ratios.
// settings (512x512 square render, 45-degree default yfov, matching
// user_camera.js's own PerspectiveCamera default AND harness.html's own
// unmodified state.userCamera.perspective.yfov) -- overridable only for
// device-free unit testing at other aspect ratios.
//
// A degenerate (invalid, i.e. never-expanded) AABB returns CameraFit's
// own struct defaults (a fixed unit-distance framing looking at the
// origin) rather than dividing by zero or asserting -- mirrors this
// codebase's own established "never crash on a real content edge case"
// posture (e.g. rx::asset::AABB::isValid()'s own doc comment), even
// though no committed conformance model actually produces this case.
[[nodiscard]] CameraFit fitCameraToScene(const rx::asset::AABB& worldBoundsAABB, float aspectRatio = 1.0F,
                                          float yfovRadians = glm::radians(45.0F));

}  // namespace rx::conformance
