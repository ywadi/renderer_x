# Completeness matrix — P5 T16 (issue #52): Cascaded shadow maps (sun)

**Plan task:** Task 16, Stage 2 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:530-551`).
**Ticket body (`gh issue view 52`):** *"Extend the Phase 4 shadow bridge
(`src/rx_shadow`, D21/D29 seams: dual depth conventions, texel snapping,
caster culling via `buildShadow`) to real CSM."*

**Sources consulted (in-repo, full-file reads, 2026-08-20):**
`src/rx_shadow/include/rx_shadow/shadow_frustum.h` + `shadow_frustum.cpp`
(full files — the delivered Phase-4 fit-and-snap algorithm this ticket
extends); `src/rx_shadow/include/rx_shadow/shadow_caster_pipeline.h` (full
file — the delivered standard-Z/comparison-sampler-PCF/dynamic-depth-bias/
depth-clamp pipeline); `docs/superpowers/specs/
2026-08-11-phase4-scene-assets-design.md:514-525` (D29, the dual
depth-convention ruling); `shaders/material/material.slang:127-330`
(delivered `rx_sampleShadowPCF`/`gShadowCompareSamplers` — the SINGLE-map
production shadow-sampling call site this ticket's cascade selection
inserts into); `shaders/shadow/shadow_caster.vert.slang` (full file — the
depth-only caster vertex shader, D26.1-addressed).

**Sources consulted (external, fetched 2026-08-20, pinned commit
`721ec800093de984cbee155e459298b6b2dbb855`, `google/filament`):**
`filament/src/ShadowMap.cpp:146,204,228-289,387,595-637,758,858-882,
1115-1150` (bounding-sphere fit, `snapLightFrustum`, texel-size Jacobian —
read directly); `filament/src/ShadowMapManager.cpp:1036-1060,1449-1453`
(cascade split-position plumbing, `CascadeSplits` struct); `filament/
include/filament/LightManager.h:219-248,478-518` (public
`ShadowOptions::cascadeSplitPositions` + `computeUniformSplits`/
`computeLogSplits`/`computePracticalSplits` — the Zhang-et-al.-class
"practical split scheme," read directly, quoted below); `shaders/src/
surface_getters.fs:127-154` (`getShadowCascade()`/
`getCascadeLightSpacePosition()` — the ACTUAL cascade-selection code, read
in full — see the CRITICAL FINDING below).

---

## CRITICAL FINDING — Filament's own cascade selection has NO seam blending

The plan's own acceptance sketch requires *"Cascade selection with seam
blending"* (plan:531-532) and names an explicit discrimination test:
*"Cascade-boundary continuity probe: shadow test values across a boundary
differ within tolerance (no visible seam), with a discrimination variant
(blending off → probe fails)."* (plan:544-546). Filament's ACTUAL current
shader source, however, does not blend at all:

```glsl
// shaders/src/surface_getters.fs:132-137
int getShadowCascade() {
    highp float z = mulMat4x4Float3(getViewFromWorldMatrix(), getWorldPosition()).z;
    ivec4 greaterZ = ivec4(greaterThan(frameUniforms.cascadeSplits, vec4(z)));
    int cascadeCount = frameUniforms.cascades & 0xF;
    return clamp(greaterZ.x + greaterZ.y + greaterZ.z + greaterZ.w, 0, cascadeCount - 1);
}
```

This is a **hard, single-cascade-per-fragment index selection** (four
comparisons against `frameUniforms.cascadeSplits`, clamped to a count) —
every fragment samples exactly ONE cascade's shadow map, with no
cross-cascade sampling, weighting, or dither blend anywhere in this
function or its caller (`getCascadeLightSpacePosition()`,
surface_getters.fs:139-152, which branches on `cascade==0` purely as a
vertex-interpolation performance shortcut, not a blend). **Seam blending
is therefore not something to "port from Filament" — it does not exist in
Filament's current source at this pinned commit.** See Open Questions #1.

---

## The matrix

| Feature | First-tier precedent (cited) | Disposition | Library/source support (verified) | Acceptance criterion |
|---|---|---|---|---|
| Per-cascade fit + texel snap (the AABB/translation half) | RendererX's OWN delivered `rx::shadow::fitShadowFrustum()` (shadow_frustum.cpp:21-93) — already does exactly this for a SINGLE shadow map: project a world AABB into light space, force a square footprint, snap the center to whole-texel increments via `std::round()`. | **preserve-later (extend, don't replace)** | VERIFIED: this function is already parameterized on `(visibleBoundsWorldMin/Max, lightView, shadowMapResolution, depthPaddingWorldUnits)` — it takes NO assumption that there is only one caller; CSM's per-cascade fit is a DIRECT, unmodified reuse of this exact function, called once per cascade with that cascade's own depth-sliced visible-bounds sub-frustum as input. | Acceptance criterion: `fitShadowFrustum()` itself is UNCHANGED by this ticket (a code-review/grep-gateable claim — zero edits to `shadow_frustum.cpp`'s existing function body) — CSM's own new code is the CALLER that slices the camera frustum into N depth ranges and invokes this function N times, not a rewrite of the fitting algorithm. |
| Rotation-invariant stability — the gap Phase 4's AABB fit does NOT close | Filament `ShadowMap::computeBoundingSphere()` (ShadowMap.cpp:758) + `getViewVolumeBoundingSphere()` (ShadowMap.cpp:619-637, quoted structurally: fits a SPHERE — not an AABB — to the per-cascade view-volume's 8 frustum-corner vertices, then derives the ortho scale `s = 1/radius` from that sphere's FIXED radius); `snapLightFrustum()` (ShadowMap.cpp:858-882) applies texel-snapping on top, matching RendererX's own already-delivered `std::round()`-based snap. | **new work, genuinely in-scope for this ticket** | VERIFIED via direct source read. This is a REAL, load-bearing gap the plan's own acceptance sketch does not explicitly name: `fitShadowFrustum()`'s existing AABB-fit (shadow_frustum.cpp:41-54) recomputes `halfExtent` from the INPUT AABB's own light-space min/max EVERY call — an AABB's projected extent in light-space CHANGES as the camera ROTATES (even with a perfectly fixed camera position and frustum shape), which changes `worldTexelSize` frame-to-frame, which reintroduces shimmer under pure rotation — a failure mode Phase 4's OWN two-position (translation-only) shimmer test cannot catch, because it never rotates the camera. A bounding-SPHERE fit avoids this: a sphere's radius is orientation-invariant for a FIXED frustum shape, so `worldTexelSize` (and therefore snap behavior) stays constant across camera rotation, not just translation. | GPU test (a genuinely NEW methodology beyond Phase 4's own "two-position" test, named explicitly here since the plan's own text does not distinguish translation-stability from rotation-stability): render the SAME static scene from two camera poses at the SAME position but DIFFERENT orientations (yaw rotation only); a cascade's fitted `worldTexelSize` and a static caster's rendered shadow edge must be pixel-identical across both poses — this DISCRIMINATES a literal reuse of `fitShadowFrustum()`'s tight-AABB approach (which would FAIL this specific test, since it is not what it was built for) from a genuine sphere-fit extension (which passes). |
| Cascade split-position scheme | Filament `LightManager::ShadowOptions::cascadeSplitPositions` (LightManager.h:248, default `{0.125, 0.25, 0.50}` for a 4-cascade default) is a CALLER-SUPPLIED percentage array — not auto-derived internally by default. Three named helper functions exist to COMPUTE it: `computeUniformSplits()`, `computeLogSplits()`, `computePracticalSplits(splitPositions, cascades, near, far, lambda)` — quoted doc comment (LightManager.h:501-514): *"uses a lambda value to interpolate between the logarithmic and uniform split schemes. Start with a lambda value of 0.5f and adjust for your scene."* — the standard, widely-cited "practical split scheme" (Zhang et al., commonly implemented this exact way across the industry). | consume-now | VERIFIED via direct header read (function signatures + doc comments, LightManager.h:478-518) — not search-digested. `ShadowMapManager::CascadeSplits` (ShadowMapManager.cpp:1449-1453) confirms the RUNTIME consumer just linearly interpolates `near + (far-near)*splitPositions[s]` — i.e. the SPLIT-COMPUTATION policy and the SPLIT-CONSUMPTION mechanism are cleanly separated in Filament's own architecture, a good structural precedent to mirror. | Acceptance criterion: RendererX ports the PRACTICAL split-scheme FORMULA (`lambda`-blended log/uniform, lambda=0.5 default per Filament's own doc comment) as the default cascade-boundary computation, exposed as an override-able percentage array (mirroring `cascadeSplitPositions`' own caller-facing shape) — a device-free unit test asserts the computed split boundaries match the closed-form log/uniform-blend formula at a table of (near, far, cascadeCount, lambda) inputs. |
| Standard-Z convention carries forward into CSM (D13/D29) | D13 (design doc, cited by shadow_caster_pipeline.h:75-85): *"Shadow maps keep standard-Z in Phase 4... the cascades work in the techniques phase revisits."* D29 (design doc:514-525): the dual `DepthConvention{Standard,Reversed}` mechanism landed WITH the Phase-4 shadow bridge specifically so a reversed-Z main camera and a standard-Z shadow pass could clear/compare correctly in the SAME frame. | **decision point — D13 explicitly flags this as revisited here, not settled** | D13's own text explicitly defers the standard-Z-vs-reversed-Z choice for shadow maps TO this exact phase ("techniques phase revisits") — it is not a silent carry-forward, it is a NAMED open decision the Phase 4 spec itself deferred to Task 16. See Open Questions #2. | Not this ticket's acceptance criterion to define unilaterally — a Task 1 spec ruling is needed (does CSM's precision profile — multiple, tighter-fitted cascades vs. one large single map — change the standard-Z-vs-reversed-Z calculus D13 originally made for a single coarse map?). Whichever is chosen, the EXISTING `DepthConvention` mechanism (D29) already has the plumbing to express either — no new render-graph work is needed regardless of the ruling, only the shadow pass's OWN pipeline-creation call site changes. |
| Per-cascade caster culling | RendererX's OWN delivered `DrawListBuilder::buildShadow()` (draw_list.h:458-479) — already culls against ONE light's extruded ortho box, producing a `ShadowLists`. | consume-now (called once per cascade) | VERIFIED: `buildShadow()`'s signature takes a `LightHandle`+`Camera`, not a fixed single-shadow-map assumption — nothing in its documented contract prevents calling it once per cascade with each cascade's own depth-sliced sub-frustum as the `camera` parameter (or an equivalent per-cascade `visibleBoundsWorldMin/Max`). | Acceptance criterion (plan's own named counter requirement, plan:549): per-cascade `CullCounters` are EXACT and independently reported (not summed across cascades into one opaque total) — a synthetic scene with casters visible to only SOME cascades asserts each cascade's own `shadowCastersVisible` count independently. |
| Shadow-map resolution/format policy tiers (desktop/Deck) | Registry note (design doc:357-359, quoted): *"the cascades-phase spec inherits Phase 4's explicit, parameterized 1024/D32_SFLOAT default rather than archaeology through sample 05."* Filament's own atlas architecture (ShadowMapManager.cpp, `mTextureAtlasRequirements`) supports MULTIPLE distinct texture sizes in one atlas ("the atlas has a depth of 4... 4 sizes of textures in the base level," ShadowMapManager.cpp:1416-1420) — precedent for a genuinely TIERED (not one-size-fits-all) resolution policy. | consume-now, tier VALUES are a Task 1 spec decision | VERIFIED registry text (already cited by matrix-issue23-shadow-bridge.md's own "New gaps" section, Phase 4) — this ticket is where that deferred decision is meant to land, per its own text. `ShadowCasterPipelineDesc::depthFormat` already defaults to the overridable `D32_SFLOAT` (shadow_caster_pipeline.h:46-51) — the MECHANISM to vary format/resolution per cascade/tier already exists; only the actual desktop-vs-Deck NUMBERS are undecided. | Acceptance criterion: desktop and Deck resolution tiers are explicit, named, spec'd constants (not re-hardcoded per-cascade literals) — e.g. `{desktop: 2048, deck: 1024}` per cascade, or a per-cascade-index table (near cascades often get a larger allocation than far ones in other engines) — whichever the Task 1 ruling picks, a test asserts the ACTIVE tier is read from one documented policy source, not scattered literals. |

---

## Open Questions

1. **Seam blending is not in Filament's own reference source (see CRITICAL
   FINDING above) — it must be independently designed, not ported.**
   **Recommendation: build a genuine cross-cascade blend, since the plan's
   OWN acceptance criteria explicitly require it and name a discrimination
   test for it** (this gate does not have authority to silently drop a
   plan-mandated, explicitly-tested feature just because the named "port
   from" source turns out not to have it — that is exactly the kind of
   silent gap CLAUDE.md's "no deferred fixes" standing rule forbids). The
   standard, well-established technique (independent of Filament,
   precedented broadly across the industry — Unreal, id Tech, and CryEngine
   all document variants): within a fixed-WIDTH blend band straddling each
   split boundary (e.g. the innermost 5-10% of a cascade's own depth
   range), sample shadows from BOTH the current cascade and the next one
   out, and linearly interpolate (or dither/stochastically select via
   `interleavedGradientNoise` — Filament's OWN existing noise helper,
   already cited in this gate's T17 research — a cheaper alternative to a
   true blend that avoids a double shadow-map fetch on most pixels). The
   plan's own "blending off → probe fails" acceptance criterion becomes
   directly testable this way: disabling the blend band collapses back to
   Filament's own hard-select `getShadowCascade()` behavior, which the
   probe should then detect as a visible discontinuity at the boundary.

2. **D13 explicitly defers the standard-Z-vs-reversed-Z choice for shadow
   maps TO this task ("the cascades work in the techniques phase
   revisits") — it is an open decision, not a default to silently
   inherit.** **Recommendation: keep standard-Z for CSM.** The D13
   rationale that motivated standard-Z originally ("ortho depth is less
   precision-critical [than a perspective main camera]; bias tuning is
   calibrated for it") applies EQUALLY, or more favorably, to CSM: each
   cascade is a TIGHTER-fitted ortho box than Phase 4's single coarse map,
   which only IMPROVES standard-Z's depth-precision distribution (ortho
   projections do not suffer perspective's severe near/far precision skew
   the way a perspective main camera does — reversed-Z's whole benefit is
   specifically countering PERSPECTIVE precision loss, which does not
   apply to an orthographic shadow projection in the first place).
   Changing convention would also force every already-delivered Phase-4
   shadow-bridge invariant (D29's clear-value/compare-op derivation, the
   existing depth-bias sign-convention code comment in
   `shadow_caster_pipeline.h:75-85` warning explicitly against exactly this
   "wrong fix") to be re-verified for no proven benefit. Recommend
   RE-AFFIRMING standard-Z explicitly in the Task 1 spec (closing D13's
   deferred question with "no change"), rather than leaving it ambiguous
   into Task 16's implementation.

## Verification health

**Verified first-hand this session:** every in-repo citation (`rx_shadow`'s
two headers + `shadow_frustum.cpp` in full, D29's design-doc text,
`material.slang`'s shadow-sampling section, `shadow_caster.vert.slang` in
full) was read directly from the working tree 2026-08-20. Filament's
`ShadowMap.cpp` (bounding-sphere fit + snap), `ShadowMapManager.cpp`
(cascade-split plumbing), `LightManager.h` (public split-scheme API +
doc comments), and `shaders/src/surface_getters.fs` (the actual cascade-
selection function — the CRITICAL FINDING's own direct evidence) were all
fetched and read directly at the pinned commit
`721ec800093de984cbee155e459298b6b2dbb855`, not search-digested.

**Not independently re-verified:** the EXACT closed-form formulas inside
`computeUniformSplits()`/`computeLogSplits()`/`computePracticalSplits()`'s
own `.cpp` bodies were not fetched (only the header's signatures + doc
comments) — the "practical split scheme" formula this matrix recommends
porting is cited from the doc comment's own description
("lambda-interpolates between logarithmic and uniform") plus this being a
well-known, independently-documented industry technique (Zhang et al.),
not from reading Filament's own `.cpp` implementation line-by-line. If the
Task 1 spec wants the EXACT Filament closed-form (vs. the standard textbook
one, which should be numerically equivalent), a follow-up fetch of
`LightManager.cpp`'s implementation would close that gap before
implementation.
