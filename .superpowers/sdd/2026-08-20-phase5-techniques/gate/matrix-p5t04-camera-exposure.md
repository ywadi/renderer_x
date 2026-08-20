# Matrix — Ticket #40 [P5 T04]: Camera exposure + physical-units API

**Plan task:** Task 4 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:232-254`), Stage 0.
**Spec/charter decisions binding this ticket:** D22 (Phase 4,
`docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md:360-373`,
"Manual exposure parameter on the tonemap... auto-exposure is
techniques-phase") — this ticket is D22's named successor, explicitly
superseding it, not amending it. Registry line "Camera exposure API"
(`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:351-353`,
"Filament's camera-owned exposure model; Phase 4 keeps manual exposure on
the tonemap per D22"). Phase 4's own gate matrix
(`.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue05-scene-proxies.md`,
Conflict #2, lines ~99-107) already flagged this exact tension
("Exposure ownership — Camera (Filament precedent) vs tonemap (D22)...
Not resolved here; flagged for the coordinator") and D22 won for Phase 4;
this ticket is where that deferred resolution actually lands. Charter's
priority order (spec lines 452-458): this ticket's pre-exposure ruling
"binds Stages 1-2" (physical light units, Task 13; IBL, Tasks 9-10) per
the plan's own text — the highest-leverage decision in this matrix.

**Sources consulted (first-hand reads, HEAD `bf5b853`, this session):**
- `src/rx_scene/include/rx_scene/camera.h` (full read, 187 lines) — its
  own header comment ALREADY documents the D22-vs-Filament tension
  (lines 41-47, quoting the Phase-4 gate ruling verbatim) and cites the
  Phase-4 matrix directly.
- `src/rx_scene/tests/camera_test.cpp` (grepped for the established
  `doctest::Approx(...).epsilon(...)` tolerance idiom, confirmed the
  repo's own convention — reused in this matrix's proposed criteria).
- `shaders/material/forward_entry.slang` (lines 205-220 read in full
  context) — exact current exposure-application site:
  `color.rgb *= exp2(gMaterialGlobals.exposure);`, applied to the
  FINAL LIT COLOR (post-lighting, pre-tonemap), with an existing
  comment noting "exposure=0.0 (2^0 == 1) is a byte-identical no-op
  regression guard."
- `shaders/material/material.slang` (lines 318-344) — `exposure` field
  on the material-globals UBO (`gMaterialGlobals`), documented as "the
  sample 08 `--exposure` CLI flag's pre-tonemap `2^exposure` multiplier."
- `samples/08_gltf_viewer/main.cpp` (`Args` struct) —
  `float exposure = 0.0F;  // pre-tonemap 2^exposure multiplier; 0 == neutral (2^0 == 1).`
  confirming the exact current default and semantics.
- `shaders/multipass/tonemap.frag.slang`/`shaders/stress/tonemap.frag.slang`
  (both read in full, cross-referenced from the T3/#39 matrix session) —
  confirms exposure is NOT applied inside either tonemap shader; D22's
  "manual exposure parameter on the tonemap" is implemented one stage
  EARLIER than its own name suggests (material/lighting output, not the
  tonemap pass itself) — a real, citable discrepancy between D22's prose
  and delivered code (see Conflicts row 1).
- Google Filament, `filament/src/Exposure.cpp` (GitHub, `google/filament`,
  `main` branch, fetched via `gh api` 2026-08-20) — the complete,
  authoritative EV100/exposure/luminance/illuminance formula set, quoted
  verbatim in row 2 below.
- Google Filament, `filament/src/details/Camera.cpp` — `FCamera::setExposure()`
  (line 257-261, aperture/shutterSpeed/sensitivity clamped and stored) and
  `CameraInfo`'s constructor (line ~290-315, `ev100 = Exposure::ev100(camera)`
  computed once per frame and threaded downstream).
- Google Filament, `filament/src/details/Camera.h` — default field values
  (`mAperture = 16.0f`, `mShutterSpeed = 1.0f / 125.0f`, `mSensitivity = 100.0f`,
  lines 211-213).
- Google Filament, `filament/src/details/View.cpp` (lines 536-572) — the
  ACTUAL pre-exposure consumption site, quoted verbatim in row 3 below:
  `exposure` computed once from `cameraInfo.ev100`, then passed into
  `prepareAmbientLight`/`prepareDirectionalLight`/`prepareExtraDirectionalLights`
  — i.e. exposure multiplies LIGHT INTENSITIES before the lighting
  equation runs, not the final pixel color afterward.

---

## The matrix

| # | Criterion | Verification method & evidence expectation | Current code state / Filament precedent (verified, cited) | Disposition | Proposed binding acceptance criterion |
|---|-----------|----------------------------------------------|------------------------------------------------------------|-------------|------------------------------------------|
| 1 | `Camera` gains aperture/shutter/ISO state + EV100/exposure helpers, replacing the tonemap-side manual `--exposure` | Device-free unit test (new — `camera_test.cpp` has zero exposure-related tests today, confirmed by full read). | `camera.h`'s current field set (lines 108-142) has NO aperture/shutter/sensitivity/exposure field — `EXPOSURE: deliberately absent from this type` is the header's own explicit comment (line 41), citing the D22 ruling by name. This is the exact field set Task 4 must add. | consume-now | `Camera` gains `aperture`/`shutterSpeed`/`sensitivity` fields (Filament's exact defaults as RendererX's own defaults: `16.0F`/`1.0F/125.0F`/`100.0F` — camera.cpp:211-213 — chosen so a freshly-constructed `Camera` has a sane, non-degenerate real-world exposure out of the box, matching this header's existing "default-constructed looks straight down -Z... 60-degree vertical FOV" precedent of shipping sane defaults, not zeros), an `ev100()` method, an `exposure()` method, and a direct `setExposure(float ev100Override)` override path (ticket's own "plus direct `setExposure` overrides" wording) that bypasses the aperture/shutter/ISO derivation entirely when a caller wants to set exposure directly (matching Filament's OWN `Exposure::exposure(float ev100)` overload existing alongside its three-parameter one — row 2's citation). |
| 2 | EV100/exposure math matches Filament reference values (unit tests against ported formulas, cited to source) | Device-free unit test using this repo's OWN established `doctest::Approx(...).epsilon(...)` tolerance idiom (already used throughout `camera_test.cpp` — rows 31/48/79/101/167-171 of that file, confirmed first-hand; reuse it verbatim, do not invent a new comparison style). | **Exact formulas verified first-hand against Filament's `Exposure.cpp`** (quoted in full in Sources; not paraphrased): `ev100(N, t, S) = log2((N² / t) * (100 / S))`; `exposure(N, t, S) = 1 / (1.2 * (N² / t) * (100 / S))` (the merged, single-pow-call form — NOT `1/(1.2 * 2^ev100)` computed via a separate `ev100()` call, though both are mathematically identical: Filament provides BOTH a merged 3-arg overload and a `exposure(float ev100)` overload for when EV100 is already known, e.g. from a direct override); the `1.2` constant is exact, derived from `78 / (100 * 0.65)` per the source's own derivation comment (saturation-based sensitivity method, `S_sat = 78/H_sat`, lens/vignetting attenuation `q = 0.65`) — NOT an arbitrary tuning constant, and must be ported byte-for-byte, not approximated. | consume-now | A device-free test table asserts RendererX's ported `ev100()`/`exposure()` against Filament's exact formulas at ≥5 (aperture, shutter, ISO) triples spanning bright daylight (Filament's own defaults: f/16, 1/125s, ISO 100 → `ev100 ≈ 14.97`) through low-light (e.g. f/1.4, 1/15s, ISO 3200), `doctest::Approx(...).epsilon(0.0001)` (matching this file's own existing epsilon convention for matrix-derived values). Additionally: the `1.2` calibration constant appears in the ported code with a comment citing its derivation (saturation-based sensitivity, `q=0.65`), not as a bare unexplained literal — this repo's own established citation discipline (every non-obvious constant in `camera.h` today is cited to a source, e.g. the reversed-Z derivation's Reed/Gribb-Hartmann citations) extends naturally to this one. |
| 3 | Pre-exposure vs full-float ruling — "Filament pre-exposes lights... the spec rules whether we adopt pre-exposure or full-float; the ruling binds Stages 1–2" | N/A — facts + recommendation for Task 1's spec; the ruling itself gets tested via row 4/5's regression + Stage 1/2's own light/IBL value gates once landed. | **Filament's actual mechanism, verified first-hand (`View.cpp:536-572`, quoted precisely)**: `const float exposure = Exposure::exposure(cameraInfo.ev100);` is computed ONCE per frame, then threaded into `prepareAmbientLight(engine, *ibl, intensity, exposure)`, `prepareDirectionalLight(engine, exposure, sceneSpaceDirection, directionalLight)`, and `prepareExtraDirectionalLights(engine, exposure, ...)` — i.e. Filament multiplies **light intensities** by exposure BEFORE the lighting equation runs (both direct lights AND IBL), keeping every intermediate lighting term (diffuse accumulation, specular, IBL contribution) inside a half-float-safe range throughout the ENTIRE shading pipeline, not just at the final output. **RendererX's current (Phase 4) mechanism is structurally different**: `forward_entry.slang:220`, `color.rgb *= exp2(gMaterialGlobals.exposure);`, applied to the ALREADY-COMPUTED final lit color — a single post-multiply, not a pre-multiply on light inputs. This means every Phase-4 lighting computation (currently just the interim flat-ambient term, per D22's own Phase-4 scope) runs in full, un-pre-exposed physical units before the one final scale-down. | **needs-coordinator-decision** — see Open Questions #1 | RECOMMEND adopting Filament's pre-exposure convention exactly (multiply light/IBL intensities by `exposure()` before the lighting equation, not the final color after). Rationale: (a) this repo's OWN chosen scene-color format (T3/#39's recommendation, RGBA16F — a half-float target) has EXACTLY the precision-range concern Filament's pre-exposure mechanism exists to solve; adopting the post-multiply-only Phase-4 shape into a phase that adds physical light units (Task 13: lux/lumens/candela, genuinely large dynamic range) and prefiltered IBL (Task 9-10) risks intermediate half-float overflow/precision loss during accumulation, before the single final exposure multiply ever gets a chance to bring values back into range; (b) the charter explicitly commits to "each lobe fed by both direct lighting and IBL" (spec line 394) — the SAME two consumer classes (`prepareAmbientLight`/`prepareDirectionalLight`) Filament's own pre-exposure call sites already cover, so porting the mechanism alongside the lobes it was designed for is more faithful to the reference source than reinventing a different application point; (c) it is the DIRECTLY verified, cited Filament source (row 3's quote), not an inference — satisfying CLAUDE.md's port-don't-reinvent rule at the mechanism level, not just the formula level. |
| 4 | Neutral-value regression guard: default exposure reproduces Phase 4 output byte-identically on existing gates | GPU pixel-gate regression (existing D17 `reference_gate.h` mechanism — see the T3/#39 matrix's row 5 for the exact tolerance/mechanism this reuses, ±4/255, <0.5% failing-pixel budget, lavapipe-only). | Current default: `samples/08_gltf_viewer/main.cpp`'s `Args::exposure = 0.0F`, applied as `exp2(0.0) == 1.0`, a byte-identical no-op (confirmed by the shader's own comment at `forward_entry.slang:217-218`: "exposure=0.0 (2^0 == 1) is a byte-identical no-op regression guard"). Task 4's ported `ev100()`/`exposure()` (row 1-2) produce a DIFFERENT numeric value at Filament's own defaults (`ev100 ≈ 14.97` → `exposure()` ≈ a very small positive number, NOT `1.0`) — so the "neutral" default this criterion needs is NOT "Filament's own default aperture/shutter/ISO," it is a DELIBERATELY chosen neutral EV100/exposure value that reproduces the CURRENT `1.0` multiplier, distinct from Filament's own real-world-camera defaults. | consume-now — but the ticket's phrasing needs one precision fix (see Conflicts row 2) | `Camera`'s default-constructed exposure state must resolve to a multiplier of exactly `1.0` (whatever aperture/shutter/ISO triple, or a direct `ev100`/`exposure` override, produces that — NOT necessarily Filament's own f/16-1/125-ISO100 defaults, which do NOT resolve to 1.0) — this is the genuinely binding "neutral" contract, tested as a device-free assertion (`defaultCamera.exposure() == doctest::Approx(1.0F)`) PLUS the existing GPU pixel gates (08/09) re-run unchanged and confirmed byte-identical against their current committed references with zero regeneration needed. |
| 5 | Exposure applied exactly once, pre-tonemap, at a documented pipeline point; `--exposure` flags migrate with behavior preserved | Code-truth verification + GPU test (a >1.0 and a <1.0 exposure override each measurably scale the final image, exactly once — a double-application bug would show as a squared, not linear, scale factor, checkable via a two-exposure-value readback ratio test). | Confirmed single current application site (`forward_entry.slang:220`); confirmed NOT duplicated anywhere else (grep for `exposure` across `shaders/` found no second multiply site). If row 3's pre-exposure ruling is adopted, this single post-multiply site is **removed entirely** (exposure moves to the light/IBL-preparation call sites instead — Stage 1/2 territory) — Task 4 itself has no direct lighting to pre-expose yet (Stage 1's IBL and Stage 2's punctual lights land later), so Task 4's own scope is: land the `Camera` API + the CONVENTION/documentation, and keep the Phase-4 interim flat-ambient term (D22's own still-active Phase-4 amendment, StandardPBR's "uniform color × occlusion" term) working via WHICHEVER application point the ruling picks, migrated correctly. | consume-now | If pre-exposure (row 3's recommendation) is ruled: the interim flat-ambient term's own intensity is pre-multiplied by `camera.exposure()` at its source (wherever that ambient term's uniform color is set/consumed) instead of the final `color.rgb *= exp2(...)` post-multiply, and `forward_entry.slang`'s post-multiply site is DELETED, not left dormant alongside the new one (a double-application bug is exactly a "left the old site in by accident" failure mode — the ticket's own two-exposure-value linearity test catches this directly). `samples/08_gltf_viewer`'s `--exposure` CLI flag is preserved AS A FLAG (same name, same user-facing semantics: "how much brighter/darker should the image be") but its VALUE now feeds `Camera::setExposure(ev100Override)` (or an equivalent direct override) instead of a raw shader-uniform scalar — documented as a one-line pipeline-point comment at the new application site, mirroring the removed site's own documentation style. |
| 6 | `setExposure` direct-override API shape | Device-free test: overriding exposure directly (bypassing aperture/shutter/ISO) produces the exact overridden value, independent of whatever aperture/shutter/ISO fields currently hold. | Filament provides exactly this shape: `Exposure::exposure(float ev100)` (the single-argument overload, `Exposure.cpp`, quoted in Sources) exists ALONGSIDE the 3-argument `exposure(aperture, shutterSpeed, sensitivity)` overload — a caller can set EV100/exposure directly without touching the photographic triple at all. `FCamera::setExposure()` itself (Camera.cpp:257-261) is aperture/shutter/ISO-only in Filament (no direct EV100 setter on `FCamera` was found in the files read) — the ticket's OWN wording ("plus direct `setExposure` overrides") asks for something Filament's `Camera` class itself does NOT expose as a first-class method (only the free `Exposure::exposure(float)` function does). | consume-now — minor extension beyond the literal Filament `Camera` API, using Filament's OWN `Exposure` free-function precedent as the justification | `rx::scene::Camera` gains BOTH the 3-parameter `setExposure(aperture, shutterSpeed, sensitivity)` (matching `FCamera::setExposure` exactly, including Filament's own clamp ranges if ported — verify `MIN_APERTURE`/`MAX_APERTURE`/etc. constants at implementation time, not yet independently verified by this matrix, see Verification health) AND a direct `setExposureDirect(float ev100)`-shaped override (name TBD by implementer) that stores an "override active" flag/value bypassing the photographic triple — precedented by Filament's own `Exposure::exposure(float ev100)` free function existing for exactly this "EV100 already known" case, even though Filament doesn't wire it directly onto `FCamera` itself. |

---

## Conflicts

1. **D22's own text ("manual exposure parameter on the TONEMAP") does not
   match delivered Phase-4 code (exposure is applied in the
   MATERIAL/LIGHTING shader's output, `forward_entry.slang:220`, one
   stage before either tonemap shader ever runs).** Not a contradiction
   this ticket needs to resolve (D22 is being superseded wholesale, not
   patched), but the coordinator should know the "tonemap-side" framing
   in both D22's text and the ticket's own body ("replacing Phase 4's
   manual tonemap-side `--exposure`") is imprecise — the actual site
   being replaced is the material shader's post-lighting multiply, not
   `tonemap.frag.slang`. This matters for row 5's "exactly once,
   pre-tonemap" criterion: it was ALREADY pre-tonemap in Phase 4 (just
   also pre-tonemap in a different, earlier sense than "the tonemap
   pass"), so this criterion is really about NOT accidentally applying
   exposure a second time inside a future tonemap-ladder task (Task 32),
   not about moving it further upstream than it already is.
2. **Ticket criterion "default exposure reproduces Phase 4 output
   byte-identically" is compatible with Filament's OWN neutral value
   only by coincidence, not by matching Filament's own real-world
   camera defaults.** Filament's own `mAperture=16/mShutterSpeed=1/125/
   mSensitivity=100` defaults do NOT produce an `exposure() == 1.0`
   neutral multiplier (they produce a small positive daylight-exposure
   value, per row 2's EV100≈14.97 computation) — a naive "port
   Filament's defaults verbatim" implementation would SILENTLY BREAK
   the byte-identical regression guard. Flagged prominently (row 4) so
   the implementer does not conflate "Filament's real-world defaults"
   with "this repo's neutral-regression default" — they must be two
   different concepts even though both live on the same `Camera` type
   (a real-camera default aperture/shutter/ISO triple, vs. Task 4's own
   OVERRIDE mechanism used to force exposure back to `1.0` for the
   regression-guard default, presumably via `setExposureDirect(0.0F ev100-equivalent)`
   or an engine-side "no override, assume neutral" initial state distinct
   from "override set to a photographic triple").

## New gaps

- **Filament's exact `MIN_APERTURE`/`MAX_APERTURE`/`MIN_SHUTTER_SPEED`/
  `MAX_SHUTTER_SPEED`/`MIN_SENSITIVITY`/`MAX_SENSITIVITY` clamp constants
  were not independently fetched/verified** in this session (only their
  USE inside `setExposure`'s clamp calls was seen, `Camera.cpp:258-260`)
  — the implementer should fetch `filament/src/details/Camera.h`'s full
  constant block before porting the clamp ranges, rather than inventing
  new ones; not blocking for this matrix's own recommendations, since
  none of them depend on the exact clamp bounds.
- **No existing test/mechanism in this repo asserts a "linear, not
  squared, exposure scale" discrimination proof** (row 5's double-
  application check) — this is a genuinely new test pattern for this
  codebase, flagged so the implementer budgets for writing it fresh
  rather than assuming a precedent exists.

## Open Questions

1. **Pre-exposure (Filament's actual mechanism) vs. full-float (keep
   Phase 4's single post-multiply, just moved onto `Camera`)?**
   RECOMMEND **pre-exposure**, matching Filament exactly (row 3's full
   rationale). This is the ticket's own headline decision ("the ruling
   binds Stages 1–2") and the single highest-leverage call in this
   matrix — load-bearing reasons: this repo's OWN half-float scene-color
   choice (T3/#39) has the identical range-safety concern Filament's
   mechanism was built to solve, and the charter's "every lobe fed by
   both direct lighting and IBL" design maps onto exactly the two
   call-site classes (`prepareAmbientLight`/`prepareDirectionalLight`)
   Filament's pre-exposure convention already threads through. Adopting
   the WRONG convention here would mean Stage 1 (Task 9-10, IBL) and
   Stage 2 (Task 13, physical light units — genuinely large dynamic
   range: candela/lumens) build their entire lighting integration on top
   of a decision this ticket is explicitly the one place chartered to
   make correctly the first time.
2. **`setExposureDirect`-shaped API naming/shape** — low-stakes,
   implementer's call; flagged only so it isn't silently invented without
   noting it has no direct 1:1 Filament `Camera`-class precedent (only a
   free-function one) — see row 6.

## Verification health

**Verified first-hand:** `camera.h` in full; `forward_entry.slang`'s
exposure line + full surrounding comment; `material.slang`'s exposure
field; `samples/08_gltf_viewer/main.cpp`'s `Args::exposure` default +
comment; both tonemap shaders (cross-referenced from the same session's
T3/#39 work); Filament's `Exposure.cpp` in FULL (fetched verbatim, quoted
without paraphrase); `Camera.cpp`'s `setExposure`/`CameraInfo` construction
region; `Camera.h`'s three default field values; `View.cpp`'s exact
pre-exposure call-site lines (536-572, grep-located then read in context).
Phase-4's own gate matrix (`matrix-issue05-scene-proxies.md`) Conflict #2
read in full for its prior framing of this exact tension.

**Not verified (flagged, not assumed):** Filament's exact aperture/
shutter/sensitivity CLAMP RANGE constants (New gaps, item 1) — seen used,
not fetched at their declaration site. The exact Filament commit/tag this
port should pin to is Task 1's job (per the plan's own text, "the exact
Filament commit to port from is pinned in the rulings") — this matrix
fetched from Filament's `main` branch on 2026-08-20 for verification
purposes only; the implementer must re-verify against whatever commit
Task 1 actually pins, in case `Exposure.cpp`'s formulas have changed
since this fetch (unlikely for a file this stable, but not assumed).
