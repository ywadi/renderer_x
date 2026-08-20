# Completeness matrix — P5 T19 (issue #55): Screen-space contact shadows

**Plan task:** Task 19, Stage 2 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:593-607`).
Depends on T16 (`T18, T19 after T16`, plan:987). Ticket body: *"the
bias-induced contact gap CSM/PCSS leave"* — this ticket closes a
correctness gap the OTHER shadow tickets structurally cannot close
themselves (any positive depth bias, however small, pulls a caster's own
base contact point out of its own shadow — a screen-space ray march
against the ACTUAL depth buffer has no bias term to leak).

**Sources consulted (in-repo, 2026-08-20):** cross-referenced against
matrix-p5t15's own "depth prepass policy" Open Question dependency — this
ticket's own screen-space ray march makes that dependency CONCRETE and
load-bearing (see matrix row below), not merely a frame-pipeline-ordering
nicety.

**Sources consulted (external, fetched 2026-08-20, pinned commit
`721ec800093de984cbee155e459298b6b2dbb855`, `google/filament`):**
`shaders/src/surface_shadowing.fs:420-461` (`screenSpaceContactShadow()`,
full function, read directly and quoted below); `filament/src/
ShadowMapManager.cpp:1317-1329` (`updateSpotShadowMaps()`'s
`screenSpaceContactShadows` per-light toggle — confirms this is a
PER-LIGHT opt-in flag in Filament's own architecture, not a single global
switch).

---

## The matrix

| Feature | First-tier precedent (cited) | Disposition | Library/source support (verified) | Acceptance criterion |
|---|---|---|---|---|
| Ray-march algorithm shape | Filament `screenSpaceContactShadow()` (surface_shadowing.fs:425-461, quoted structurally): fixed step count (`kStepCount`, a per-light-configurable field packed into `frameUniforms.directionalShadows`), march along the LIGHT direction reprojected into screen space (`ScreenSpaceRay`/`initScreenSpaceRay`), dithered start offset via `interleavedGradientNoise(gl_FragCoord.xy)` (avoids banding from a fixed step count), a per-step depth-buffer sample compared against the ray's own expected depth within a `tolerance` band (`abs(tolerance - dz) < tolerance`), early-exit on first hit. | consume-now | VERIFIED via direct source read, full function. This is a well-formed, directly-portable technique — no moment-texture/atlas dependency (unlike T17's EVSSM finding), reads only the scene's OWN depth buffer + the known light direction. | GPU test (the plan's own named discrimination pair, plan:601-603): a caster-receiver contact point at a KNOWN, tight (bias-hidden) gap renders SHADOWED with contact shadows ON, UNSHADOWED (visibly peter-panned, matching the CSM/PCSS bias artifact this ticket exists to close) with contact shadows OFF — the exact "value probe pair" the plan itself names, concretized against a specific, reproducible geometry (e.g. a thin object resting flush on a receiver plane, at a bias magnitude known from T16/T17's own delivered depth-bias constant). |
| Dithered march start offset (interleaved gradient noise) | Filament, quoted exactly (surface_shadowing.fs:444): `float dither = interleavedGradientNoise(gl_FragCoord.xy) - 0.5;` — a cheap, well-known (Jimenez 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare") per-pixel noise function that trades a FIXED step count's visible banding for TEMPORALLY-stable-but-spatially-dithered noise instead. | consume-now | VERIFIED present in the fetched function — this is the SAME noise primitive Filament's own EVSSM shadow filter optionally uses too (`SHADOW_SAMPLING_EVSSM_NOISE`, cited in matrix-p5t17's own research, surface_shadowing.fs:337-338) — a single, reusable `interleavedGradientNoise()` helper serves BOTH this ticket and (optionally) T17's own filter, worth sharing rather than reimplementing per-ticket. | Acceptance criterion: the ray march's step pattern uses a shared, single-definition `interleavedGradientNoise()` helper (not a per-shader-file duplicate) — a grep-gateable code-review criterion; visual/value test asserts marching the SAME ray twice (same pixel, same frame) produces the SAME dithered offset (deterministic given `gl_FragCoord`, not a true per-frame-random source — a discrimination test: two renders of a STATIC scene must be byte-identical, proving the "noise" is a deterministic per-pixel function, not a time-seeded RNG that would reintroduce non-determinism this project's own standing discipline forbids). |
| Screen-edge fade | Filament, quoted exactly (surface_shadowing.fs:456-459): *"we fade out the contribution of contact shadows towards the edge of the screen because we don't have depth data there"* — `fade = max(12.0*abs(ray.xy-0.5)-5.0, 0.0); occlusion *= saturate(1.0 - dot(fade,fade))`. | consume-now | VERIFIED present — a real, necessary correctness detail: a ray marching OFF-SCREEN has no depth-buffer data to compare against past the viewport edge, and without this fade the march would either falsely report a hit (garbage/wrapped sample) or falsely report a miss at the exact screen boundary, producing a visible seam at the frame edge. | GPU test: a caster-receiver contact point positioned NEAR the screen edge (but the actual contact point still on-screen) still shows the correct occlusion result, smoothly fading rather than hard-cutting at the boundary — a targeted regression for the exact failure mode this fade exists to prevent. |
| Depth-source dependency — REQUIRES a sampleable scene depth buffer BEFORE/DURING opaque lighting | Filament's own call: `textureLod(sampler0_structure, uvToRenderTargetUV(ray.xy), 0.0).r` (surface_shadowing.fs:449) — `sampler0_structure` is Filament's own name for a dedicated, pre-populated STRUCTURE (depth) buffer, implying Filament's OWN frame graph guarantees a depth buffer is already resolved and sampleable at the point contact shadows evaluate — i.e. Filament effectively assumes/uses a depth PREPASS architecture for this feature. | **cross-task dependency — makes matrix-p5t15's Open Question load-bearing, not optional** | Cross-referenced directly against matrix-p5t15's own "Depth prepass policy" row, which noted the Task 1 depth-prepass ruling was NOT hard-required by T14's froxel Z-slicing (Filament's `Froxelizer::prepare()` takes plain near/far scalars, no depth-buffer sample). Contact shadows are a GENUINELY DIFFERENT case: a screen-space ray march needs to sample OTHER pixels' depth values (not just its own fragment's), which is IMPOSSIBLE without a depth buffer that is already fully written and bindable as a texture — i.e. this ticket cannot function without SOME form of "depth already resolved and sampleable" architecture, whether that is a genuine prepass (Filament's own apparent choice) or a same-pass read of a depth attachment via a different mechanism (e.g. a resolved copy after a depth-only sub-pass). | Not this ticket's OWN acceptance criterion to unilaterally decide (the Task 1 ruling still owns the overall policy) — but flagged here with elevated urgency versus matrix-p5t15's own framing: if the Task 1 ruling picks "no depth prepass" for cost reasons, Task 19 needs an EXPLICIT alternative depth-sampling mechanism named before it can be scoped at all (e.g. rendering opaque geometry to depth FIRST within the same pass structure, then a second pass reads it) — this is a hard blocker, not a nice-to-have, for this specific ticket. |
| Default on/off + per-quality-tier toggle | Plan's own text (plan:605): *"default on/off per spec ruling; toggle exposed to samples."* Filament's own precedent: a PER-LIGHT boolean (`ShadowOptions::screenSpaceContactShadows`, ShadowMapManager.cpp:1324), not a single global on/off — i.e. contact shadows are opt-in PER LIGHT in Filament's own architecture, not scene-wide. | **decision point — the DEFAULT value, and per-light vs. global scope, are both open** | Filament's own choice (per-light opt-in) is directly verified — a real, corroborated precedent for HOW to scope the toggle, even though WHETHER it defaults on is still an explicit spec ruling this gate does not make. | Not this ticket's acceptance criterion to unilaterally set — flagged so the Task 1 spec ruling addresses BOTH sub-questions explicitly (default value AND per-light-vs-global scope), citing Filament's own per-light precedent as the recommended scope shape (matches this project's OWN existing per-light `castsShadows`/`channels` field precedent, scene.h:155-163 — a per-light bool is the path of least resistance given that existing convention). |
| Cost measured | Plan's own named criterion (plan:605). Standing CLAUDE.md rule. | consume-now | N/A — measurement requirement; a fixed-step-count screen-space ray march has a PREDICTABLE, step-count-proportional cost (unlike PCSS's variable-radius cost, which depends on scene content) — worth noting as a SIMPLER cost-publication case than T17's own. | Acceptance criterion: cost measured at the chosen `kStepCount`, published, driver-labeled, per the standing real-GPU-verification corrective. |

---

## Open Questions

*(None uniquely novel to this ticket beyond the two already-flagged
cross-task dependencies above — both are recorded as matrix rows rather
than a separate Open Questions section, since each already carries a
concrete recommendation inline: the depth-prepass dependency needs the
Task 1 ruling to CONFIRM a sampleable depth source exists before contact
shadows can be scoped at all, and the default-on/off + per-light-vs-global
toggle scope should follow Filament's own per-light precedent, matching
this project's own existing `castsShadows`/`channels` per-light field
convention.)*

## Verification health

**Verified first-hand this session:** `screenSpaceContactShadow()`
(surface_shadowing.fs:425-461) was fetched and read directly at the pinned
commit as part of the SAME full-file fetch used for matrix-p5t17 (no
separate/re-fetch, no commit-drift risk between the two matrices'
citations of this file). `ShadowMapManager.cpp`'s per-light
`screenSpaceContactShadows` toggle (:1317-1329) was read directly as part
of the same fetch used for matrix-p5t18.

**Not independently re-verified:** `interleavedGradientNoise()`'s own
definition (referenced by name, used but not itself defined in
`surface_shadowing.fs` — presumably a shared utility function elsewhere in
`shaders/src/`) was not independently located/fetched this session; the
Jimenez-2014 attribution is general, well-established knowledge for this
specific, widely-cited noise function, not drawn from an in-file comment
(surface_shadowing.fs does not itself cite the source).
