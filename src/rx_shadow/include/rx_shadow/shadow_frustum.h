#pragma once

// rx_shadow/shadow_frustum.h -- the device-free half of the shadow
// quality bridge [spec D21, Phase 4 Stage 2 Task 22; gate
// matrix-issue23-shadow-bridge.md as amended by
// gate/rulings-2026-08-18.md's #23 + RC2/RC3]: fits a directional light's
// STANDARD-Z (D13: shadow maps stay standard-Z in Phase 4) orthographic
// projection to a camera-visible world-space AABB, and snaps its center
// to whole-texel increments so a moving camera does not shimmer the
// shadow edges of otherwise-static geometry.
//
// Thread-affinity (D5): none -- every function here is a pure function of
// its own arguments, no shared/owned resource, matching rx_scene/camera.h's
// own "plain value type... safe to call from any thread" precedent.
//
// SCOPE, relative to DrawListBuilder::buildShadow() (Task 19): that method
// ALREADY computes an analogous light-space AABB internally, purely to
// drive its own shadow-CASTER CULLING test -- and its own header comment
// says so explicitly: "used only to fit/extrude the shadow ortho box
// (buildShadow()), never to bind a real GPU shadow-pass projection
// (Task 22's own scope)". This header is that promised, independent
// consumer: it takes the SAME kind of input (a camera-visible world AABB,
// D15's own "fitted to the camera frustum's world bounds" family) via the
// SAME public accessors any caller already has (Scene::worldBoundsSpan()
// filtered by a camera-frustum test, or simply the union of whatever
// geometry a caller already knows is visible) and derives the REAL GPU
// projection matrix -- deliberately NOT reaching into DrawListBuilder's
// own private Impl state, which is not part of anyone else's contract.
//
// TEXEL SNAPPING [D21, matrix's own "Texel snapping (shimmer prevention)"
// row]: quantizes the light-frustum's world-space size to shadowMapResolution
// texels and snaps its CENTER to a whole-multiple of that texel size, in
// the light's own X/Y basis -- the standard technique (commonly attributed
// to Microsoft's "Common Techniques to Improve Shadow Depth Maps" DirectX
// article) so the texel-to-world mapping stays constant frame to frame
// regardless of the camera's own sub-texel movement.

#include <glm/glm.hpp>

#include <cstdint>

namespace rx::shadow {

// The result fitShadowFrustum() below produces -- everything a caller
// needs to both build the shadow-caster pipeline's per-draw
// `lightViewProj` rows (rx::shadow::ShadowDrawData) and drive the
// forward-pass PCF tap loop's texel-offset math.
struct ShadowFrustumFit {
    // World -> light-eye-space (an eye-at-origin glm::lookAt() basis with
    // `forward = lightDir` -- see lightSpaceView() below). Never jittered,
    // never reused for anything but building lightProj/lightViewProj.
    glm::mat4 lightView{1.0F};

    // Light-eye-space -> clip, STANDARD-Z [D13: "Shadow maps keep
    // standard-Z in Phase 4... bias tuning is calibrated for it"] --
    // near maps to NDC depth 0.0, far to 1.0, ordinary VK_COMPARE_OP_LESS
    // convention. Fitted + texel-snapped to the caller's own visible
    // bounds (see fitShadowFrustum()'s own comment).
    glm::mat4 lightProj{1.0F};

    // lightProj * lightView -- what the shadow-caster pipeline's
    // per-instance `lightViewProj` rows are built from, and what a PCF
    // sampler projects a shaded fragment's world position through to
    // reach shadow-map UV + comparison depth.
    glm::mat4 lightViewProj{1.0F};

    // World-space size of one shadow-map texel at this fit's own extent
    // (the SQUARE X/Y footprint's side length divided by
    // shadowMapResolution) -- the PCF tap loop's own UV-space step size is
    // this value divided by that same footprint's side length (i.e.
    // `1.0 / shadowMapResolution`), but this world-space value is what a
    // caller needing to reason about "how many world units does one texel
    // cover" (e.g. a texel-snapping regression test) actually wants.
    float worldTexelSize = 0.0F;
};

// An arbitrary, consistent orthonormal basis with `lightDir` as its
// (view-space) forward axis, eye at the world origin (irrelevant for a
// DIRECTIONAL light -- only orientation matters) -- BYTE-FOR-BYTE the same
// derivation `rx::scene::draw_list.cpp`'s own (anonymous-namespace,
// unexported) `lightSpaceView()` uses, independently duplicated here per
// this header's own top comment (Task 22 does not reach into
// DrawListBuilder's private state) -- both must stay in sync if either
// changes; see draw_list.cpp's own copy for the up-hint degenerate-case
// derivation this mirrors.
[[nodiscard]] glm::mat4 lightSpaceView(const glm::vec3& lightDir);

// Fits + texel-snaps a STANDARD-Z orthographic projection to
// `visibleBoundsWorldMin`/`Max` (a camera-visible world-space AABB, D15's
// own "fitted to the camera frustum's world bounds" family) in `lightView`'s
// own basis (pass lightSpaceView(lightDir) -- kept as a separate parameter,
// not derived internally, so a caller that already has a light-space basis
// for other reasons this frame does not pay for a second derivation).
//
// SQUARE FOOTPRINT: the light-space X/Y extent is forced square (the
// LARGER of the two half-extents used for both axes) so `worldTexelSize`
// is uniform in X and Y -- a non-square footprint would make "one texel"
// mean a different world-space size per axis, which the PCF tap loop
// (a single, isotropic `texelSize` step) does not support.
//
// DEPTH RANGE PADDING: `visibleBoundsWorldMin`/`Max` describes what the
// CAMERA can see, not the full extent of what might CAST into it -- D15's
// own conservative caster extrusion means an off-screen caster upstream
// of the visible region can legitimately be far outside these bounds
// along `-lightDir`. This function pads BOTH ends of the light-space
// depth range by `depthPaddingWorldUnits` (a caller-supplied margin,
// typically sized to the scene's own expected caster extent -- e.g. a
// building's height) so such casters still land within [0,1] instead of
// being silently clipped by the ortho projection's own near/far planes.
// A value of 0.0F reproduces the visible bounds' own tight depth range
// exactly (no caster margin) -- the caller's choice, not a hidden default,
// since the right margin is scene-dependent and this library has no way
// to know it.
//
// TEXEL SNAPPING: the fitted box's CENTER (in lightView's X/Y) is snapped
// to the nearest whole multiple of the resulting `worldTexelSize` --
// std::round(), not floor/ceil, for a symmetric (no directional bias)
// quantization. Depth (Z) is never snapped -- shimmer is strictly an X/Y
// (screen-space-of-the-shadow-map) artifact.
[[nodiscard]] ShadowFrustumFit fitShadowFrustum(const glm::vec3& visibleBoundsWorldMin,
                                                  const glm::vec3& visibleBoundsWorldMax, const glm::mat4& lightView,
                                                  uint32_t shadowMapResolution, float depthPaddingWorldUnits = 0.0F);

}  // namespace rx::shadow
