#pragma once

// rx_scene/camera.h -- rx::scene::Camera, the single source of projection
// truth for the whole engine [spec D13, Phase 4 Stage 2 Task 18; gate
// matrix-issue05-scene-proxies.md's Camera rows, as amended by
// gate/rulings-2026-08-18.md's #5 section]. Every sample/pass that needs a
// view or projection matrix goes through this type -- "rx::scene::Camera
// owns the projection helpers so samples cannot get it inconsistently
// wrong" (D13's own text, quoted in the matrix's Conflict #1).
//
// Thread-affinity (D5): none. Camera is a plain, copyable value type with
// no shared/owned resource and no internal synchronization concern of its
// own -- every method below is a pure function of its own fields, safe to
// call from any thread, matching rx_asset/mesh_asset.h's AABB (also a
// plain value type with no thread-affinity comment).
//
// REVERSED-Z, INFINITE FAR [D13]: proj() maps world-space depth so that
// the near plane lands at NDC depth 1.0 and depth decreases monotonically
// toward 0.0 as distance grows without bound (far = infinity baked into
// the matrix itself, never a finite parameter) -- the standard
// "reversed-Z infinite far" projection (Reed, N., "Depth Precision
// Visualized", https://www.reedbeta.com/blog/depth-precision-visualized/;
// cross-checked against the community GLM formula at
// https://gist.github.com/pezcode/1609b61a1eedd207ec8c5acf6f94f53a per the
// matrix's own Sources section). This needs no external library -- GLM
// (already vendored) supplies every primitive the hand-written matrix
// below uses; GLM itself has no built-in reversed-Z/infinite-far helper to
// reuse instead.
//
// TWO PROJECTIONS, ON PURPOSE [gate ruling, matrix Conflict #1 resolved as
// option (a)]: proj()'s infinite far makes its own "far" row degenerate
// under Gribb-Hartmann plane extraction (see this header's own comment on
// extractFrustumPlanes() below for the exact algebra) -- Filament's
// answer, adopted here, is a SEPARATE finite-far projection
// (cullingProj(), Camera.h:159-162/292-309's `projectionForCulling`
// precedent) used ONLY to derive real, non-degenerate culling planes.
// DrawListBuilder (Task 19) is the intended consumer of cullingProj()/
// cullingFrustumPlanes(); proj()/viewProj() remain what the render pass
// itself binds.
//
// EXPOSURE [Phase 5 Task 4/#40, superseding D22 wholesale -- gate ruling
// rulings-2026-08-20.md's T4 section: "Filament-style PRE-EXPOSURE
// adopted... physical-units exposure API per the matrix's shape"; matrix
// .superpowers/sdd/2026-08-20-phase5-techniques/gate/matrix-p5t04-camera-
// exposure.md]: Camera now owns a Filament-precedented photographic
// aperture/shutterSpeed/sensitivity triple, an EV100/exposure() pair
// ported byte-for-byte from Filament's own `Exposure.cpp` (google/filament
// v1.75.0, commit 0e58877c09afb1aacd09ff640f74d2adcd2a7e80, Apache-2.0 --
// the `rx::scene::exposure` namespace below), and a direct EV100 override
// (`setExposure(float)`, no literal Filament Camera-level precedent -- see
// that overload's own comment). PRE-EXPOSURE, not a post-multiply on the
// final shaded color: `exposure()` is the multiplier every light/IBL-
// intensity producer this ruling binds (techniques-phase Stage 1 IBL,
// Stage 2 physical lights) applies to its OWN values before the lighting
// equation runs -- mirroring Filament's `View.cpp` (`prepareAmbientLight`/
// `prepareDirectionalLight`, both fed a single per-frame `exposure` value
// computed from `cameraInfo.ev100`), which this repo's own half-float HDR
// scene-color choice (T3/#39, B10G11R11) shares the identical
// precision-range motivation for. See exposureOverride's own comment for
// why a freshly-constructed Camera's exposure() is a neutral 1.0 despite
// its photographic triple defaulting to a real (non-neutral) daylight
// exposure -- the load-bearing byte-identical regression guard for every
// pre-Task-4 consumer.
//
// JITTER [preserve-later, TAA]: `jitter` is threaded through proj()'s
// translation terms (see proj()'s own comment) but is `{0,0}` (inert) in
// every Phase 4 call site -- reserved so the temporal cluster (techniques
// phase) does not need to change this type's call signature later
// (Filament places jitter on `View::TemporalAntiAliasingOptions`, not
// Camera -- RendererX has no separate View type in the Task 18 interface,
// so Camera is the correct analog by elimination, not a precedent match;
// see the matrix's own "Camera: jitter hook" row).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace rx::scene {

// Ported byte-for-byte from Filament's `namespace filament::Exposure`'s
// free `ev100`/`exposure` overloads [google/filament v1.75.0,
// filament/src/Exposure.cpp, commit 0e58877c09afb1aacd09ff640f74d2adcd2a7e80,
// Apache-2.0 -- fetched and diffed against this exact tag/commit while
// writing this file, not paraphrased from docs]. Pure functions (this
// codebase's own established test pattern -- see e.g. this header's own
// extractFrustumPlanes()) so camera_test.cpp can assert them directly
// against hand-derived reference values, independent of Camera's own
// exposureOverride mechanism (Camera::ev100()/exposure() below are thin
// wrappers over these).
namespace exposure {

// EV100 from a photographic aperture (f-stop, N)/shutterSpeed (seconds,
// t)/sensitivity (ISO, S) triple: `log2((N^2/t) * (100/S))` [Exposure.cpp
// `ev100(float, float, float)`].
[[nodiscard]] float ev100(float aperture, float shutterSpeed, float sensitivity);

// The pre-exposure multiplier from the SAME triple -- Filament's own
// merged, single-pow-call form (`1 / (1.2 * (N^2/t) * (100/S))`,
// mathematically identical to `exposure(ev100(aperture, shutterSpeed,
// sensitivity))` but without a redundant log2/pow round-trip) [Exposure.cpp
// `exposure(float, float, float)`]. The `1.2f` constant is exact, not a
// tunable: derived from the saturation-based-sensitivity method
// (`S_sat = 78/H_sat`, lens/vignetting attenuation `q = 0.65`,
// `78/(100*0.65) = 1.2`) -- Exposure.cpp's own `exposure(float ev100)`
// doc comment carries the full derivation.
[[nodiscard]] float exposure(float aperture, float shutterSpeed, float sensitivity);

// The pre-exposure multiplier from an ALREADY-KNOWN EV100 value:
// `1 / (1.2 * 2^ev100)` [Exposure.cpp `exposure(float ev100)`] -- the
// formula Camera::setExposure(float)'s direct override below uses.
[[nodiscard]] float exposure(float ev100);

}  // namespace exposure

// Six frustum planes, Filament's own `Frustum::getNormalizedPlane(s)`
// ordering (Frustum.h:78-89: "left, right, bottom, top, far, near") --
// matched deliberately (not RendererX's own invention) so anyone
// cross-referencing that precedent finds the same order here. Each
// glm::vec4 is a normalized plane (`dot(plane.xyz, worldPos) + plane.w >=
// 0` for "inside"), per extractFrustumPlanes()'s own comment.
enum class FrustumPlaneIndex : uint8_t { Left = 0, Right = 1, Bottom = 2, Top = 3, Far = 4, Near = 5 };
inline constexpr size_t kFrustumPlaneCount = 6;
using FrustumPlanes = std::array<glm::vec4, kFrustumPlaneCount>;

// Gribb-Hartmann plane extraction [Gribb, G. & Hartmann, K., "Fast
// Extraction of Viewing Frustum Planes from the World-View-Projection
// Matrix", http://graphics.cs.ucf.edu/cap4720/fall2008/plane_extraction.pdf]
// from an already-combined world-to-clip matrix (`viewProj`, i.e.
// `proj * view` -- see Camera::viewProj()/cullingViewProj() below): six
// planes read directly off `viewProj`'s own rows, no separate geometric
// computation. Assumes Vulkan's clip-space depth range (`0 <= z <= w`,
// `GLM_FORCE_DEPTH_ZERO_TO_ONE`'s convention) and `-w <= x,y <= w` -- both
// baked into Camera::proj()/cullingProj()'s own matrices below, so this
// function is correct for either one's output without a flag.
//
// FINITE FAR REQUIRED for a non-degenerate result [gate ruling, matrix
// Conflict #1]: `viewProj`'s row2 (the "z >= 0" test, FrustumPlaneIndex::Far
// below -- see the derivation in this header's own top comment) reduces to
// a WORLD-space-independent constant when built from an infinite-far
// projection (Camera::proj()), because that matrix's own z-row is `(0, 0,
// 0, near)` in view space and every affine view matrix's bottom row is
// `(0, 0, 0, 1)` -- multiplying the two never touches the view matrix's
// real (non-trivial) rows. The result is a plane with a zero xyz normal:
// this function does NOT special-case that (dividing a near-zero-length
// normal is left as-is, not corrected), so calling it on
// Camera::viewProj() yields 5 usable planes and one always-"true" one at
// FrustumPlaneIndex::Far. Call it on Camera::cullingProj()'s combined
// matrix (Camera::cullingFrustumPlanes(), the sanctioned entry point)
// for all 6 planes to be real.
[[nodiscard]] FrustumPlanes extractFrustumPlanes(const glm::mat4& viewProj);

// The single projection source of truth for the whole engine [D13]. A
// plain value type: default-constructed looks straight down -Z from the
// world origin (identity orientation, GLM/glTF's own right-handed
// convention -- forward = -Z, up = +Y, right = +X), a 60-degree vertical
// FOV, 16:9 aspect, and a 0.1-unit near plane.
struct Camera {
    glm::vec3 position{0.0F, 0.0F, 0.0F};
    glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};  // identity: forward -Z, up +Y, right +X

    float verticalFovRadians = glm::radians(60.0F);

    // [Deviation from the plan's illustrative interface sketch --
    // necessary, not incidental, matching this codebase's own established
    // convention for documenting such gaps (see e.g. GeometryPool::create's
    // Allocator& addition): a perspective matrix cannot be built without an
    // aspect ratio, and the plan's "vfov, near" text did not name a field
    // for it.] Width / height of the camera's own render target.
    float aspectRatio = 16.0F / 9.0F;

    float nearPlane = 0.1F;

    // [Gate ruling, matrix "Camera: separate finite culling-frustum
    // matrix" row] The FINITE far distance cullingProj() below uses --
    // proj()/viewProj() never read this field (their far is always
    // infinity, per D13). Has no effect on rendering; only on which
    // world-space distance cullingFrustumPlanes()'s Far plane sits at.
    float cullingFarPlane = 1000.0F;

    // [preserve-later, TAA] See this header's own top comment. Inert
    // ({0,0}) in every Phase 4 call site; threaded through proj()'s own
    // translation terms so a later temporal-AA consumer does not need to
    // change this type's call signature.
    glm::vec2 jitter{0.0F, 0.0F};

    // [D19/matrix-issue07] Camera-side visibility mask, ANDed against a
    // renderable's own RenderableDesc::layers by DrawListBuilder (Task 19)
    // -- all-ones default per Unity/Godot's own "opt-out, not opt-in"
    // convention (matrix-issue07's own "all-ones defaults CONFIRMED as
    // deliberate" ruling).
    uint32_t cullMask = ~0u;

    // --- Exposure [Phase 5 Task 4/#40; see this header's own top comment
    // for the full ruling/citation] --------------------------------------

    // Photographic exposure triple -- Filament's OWN real-camera defaults,
    // verbatim [details/Camera.h:211-213, v1.75.0]: aperture is an f-stop
    // (f/N), shutterSpeed is in seconds, sensitivity is ISO (100-relative).
    // A freshly-constructed Camera therefore has a sane, non-degenerate
    // real f/16-1/125s-ISO100 daylight exposure on hand (ev100() ~= 14.97)
    // if a caller reads it -- but see exposureOverride below for why
    // exposure() itself does NOT default to what this triple resolves to.
    float aperture = 16.0F;
    float shutterSpeed = 1.0F / 125.0F;
    float sensitivity = 100.0F;

    // Direct pre-exposure-multiplier override [RendererX addition -- no
    // literal Filament Camera-level precedent: only Filament's free
    // `Exposure::exposure(float ev100)` overload exists for "EV100 already
    // known", never wired onto FCamera itself; matrix-p5t04's row 6].
    // When engaged (`has_value()`), exposure() returns
    // `exposure::exposure(*exposureOverride)` verbatim, bypassing
    // aperture/shutterSpeed/sensitivity entirely; ev100() is UNAFFECTED
    // either way (it always reflects the photographic triple above -- an
    // override changes what multiplies onto light/IBL intensities, not
    // what EV100 the triple represents).
    //
    // DEFAULTS ENGAGED, AT EXACTLY 1.0 -- the neutral, byte-identical-to-
    // Phase-4 multiplier (matrix-p5t04 row 4's "Camera's default-
    // constructed exposure state must resolve to a multiplier of exactly
    // 1.0"), a DELIBERATELY different value from what the photographic
    // triple's own Filament defaults resolve to (~2.6e-5, a real daylight
    // exposure -- matrix Conflicts row 2: naively deriving the default
    // from Filament's real-camera triple would silently break every
    // pre-Task-4 consumer's byte-identical output). setExposure(aperture,
    // shutterSpeed, sensitivity) clears this override (falls back to the
    // triple); setExposure(ev100Override) re-engages it at a new, explicit
    // value; clearExposureOverride() re-engages it back to this same
    // neutral 1.0.
    std::optional<float> exposureOverride = 1.0F;

    // EV100 of the CURRENT photographic triple -- exposure::ev100() above,
    // applied to this Camera's own aperture/shutterSpeed/sensitivity.
    // Unaffected by exposureOverride (see that field's own comment).
    [[nodiscard]] float ev100() const;

    // The pre-exposure multiplier [Filament-style pre-exposure, gate
    // ruling rulings-2026-08-20.md's T4 section] every light/IBL-intensity
    // producer this ruling binds is required to multiply onto its OWN
    // values before the lighting equation runs (NOT a post-multiply on
    // the final shaded color -- see this header's own top comment).
    // `*exposureOverride` if engaged, else `exposure::exposure(ev100())`.
    [[nodiscard]] float exposure() const;

    // Sets the photographic triple and clears any direct override --
    // Filament `FCamera::setExposure(float, float, float)`, INCLUDING its
    // exact clamp ranges [details/Camera.cpp:45-50,258-260, v1.75.0]:
    // aperture clamped to [0.5, 64], shutterSpeed to [1/25000, 60]
    // (seconds), sensitivity to [10, 204800] (ISO). Unlike Filament's
    // FCamera (private fields, setter-only mutation), this type's fields
    // stay PLAIN AND PUBLIC like every other Camera field above (nearPlane,
    // aspectRatio, ...) -- a caller writing `camera.aperture` directly
    // bypasses this clamp, matching this type's existing "no invariant
    // enforcement on direct field writes" convention; only this named
    // setter clamps.
    void setExposure(float apertureIn, float shutterSpeedIn, float sensitivityIn);

    // Direct EV100 override -- bypasses the photographic triple entirely
    // (see exposureOverride's own comment above for why this has no
    // literal Filament Camera-level precedent). exposure() returns
    // `exposure::exposure(ev100Override)` verbatim from this call until
    // clearExposureOverride() or the triple-setting overload above runs.
    void setExposure(float ev100Override);

    // Re-engages the neutral (1.0) override -- equivalent to a freshly-
    // constructed Camera's own exposure() state, discarding any
    // setExposure(ev100Override) call or a prior setExposure(triple)
    // call's override-clearing effect.
    void clearExposureOverride();

    [[nodiscard]] glm::vec3 forward() const { return orientation * glm::vec3(0.0F, 0.0F, -1.0F); }
    [[nodiscard]] glm::vec3 up() const { return orientation * glm::vec3(0.0F, 1.0F, 0.0F); }
    [[nodiscard]] glm::vec3 right() const { return orientation * glm::vec3(1.0F, 0.0F, 0.0F); }

    // World-to-view matrix: inverse(translate(position) * mat4_cast(orientation)),
    // computed directly (conjugate rotation, then translate) rather than via
    // glm::inverse() -- both this camera's rotation and translation are
    // individually trivial to invert, so there is no reason to pay for a
    // general 4x4 inverse.
    [[nodiscard]] glm::mat4 view() const;

    // Reversed-Z, INFINITE far [D13] render projection -- see this
    // header's own top comment for the full derivation and the jitter/
    // exposure notes. `jitter` (in NDC-fraction units) is added to the
    // matrix's own x/y translation-of-z terms (`m[2][0]`/`m[2][1]` in
    // GLM's column-major storage -- the coefficients of view-space Z in
    // computing clip.x/clip.y), the standard depth-independent
    // screen-space-jitter technique multiple TAA implementations use;
    // `{0,0}` (Phase 4's only value) leaves the matrix byte-identical to
    // the unjittered form.
    [[nodiscard]] glm::mat4 proj() const;

    // proj() * view() -- what every Phase 4 render pass actually binds.
    [[nodiscard]] glm::mat4 viewProj() const;

    // Reversed-Z, FINITE far (cullingFarPlane) projection -- see this
    // header's own top comment ("TWO PROJECTIONS, ON PURPOSE"). Never
    // jittered (jitter is a rendering-only concern; the culling frustum
    // must reflect the camera's true, unjittered extent).
    [[nodiscard]] glm::mat4 cullingProj() const;

    // cullingProj() * view() -- the matrix extractFrustumPlanes() needs a
    // non-degenerate result from.
    [[nodiscard]] glm::mat4 cullingViewProj() const;

    // extractFrustumPlanes(cullingViewProj()) -- the sanctioned,
    // non-degenerate entry point (see extractFrustumPlanes()'s own
    // comment for why proj()/viewProj() are NOT interchangeable with this
    // for plane extraction).
    [[nodiscard]] FrustumPlanes cullingFrustumPlanes() const;
};

}  // namespace rx::scene
