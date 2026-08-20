# Matrix — P5 T27 (issue #63): LTC area lights

**Plan task:** Task 27 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:753-770`), Stage 3.
**Charter binding:** "Area lights via LTC (rect panels/screens/softboxes
— the 'suddenly looks AAA' feature)" (`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:414-421`,
clustered-lighting block); priority order item (8) (`:452-458`); reference
sources ("LTC area-light reference code from the original authors
(permissive redistribution)", `:388-389`); "area lights participate in
the froxel lists" — depends on Task 14 (Stage 2, froxel grid — already
delivered per the plan's sequencing).

**Sources consulted:**
- Ticket body: `gh issue view 63`.
- Plan Task 27 + Global Constraints (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:55-134, 753-770`).
- Charter block (cited above), including the "Ported-source additions"
  list's exact phrase "LTC reference code from the original authors
  (permissive)" (`:50`).
- `selfshadow/ltc_code` (Eric Heitz, Jonathan Dupuy, Stephen Hill, David
  Neubelt — the original authors of the LTC technique), `master` branch,
  commit `31e5e96b54f98f33098f8503003119ba2231a1c6` (2019-04-15, latest
  commit on the branch as of 2026-08-20, `gh api repos/selfshadow/ltc_code/commits/master`) —
  repo root listing, `LICENSE` (fetched verbatim), `README.md` (fetched),
  `fit/` directory listing (`fit/fitLTC.cpp`, `fit/LTC.h`,
  `fit/brdf_ggx.h`, `fit/brdf_beckmann.h`, `fit/brdf_disneyDiffuse.h`,
  `fit/nelder_mead.h`, etc.), `webgl/shaders/ltc/` directory listing
  (`ltc_quad.fs`, `ltc_disk.fs`, `ltc_line.fs`, `ltc.vs`, `ltc_blit.fs/vs`),
  `webgl/images/` listing (confirmed NOT to contain baked LUT texture
  assets — see row 3/Verification health), `webgl/shaders/ltc/ltc_quad.fs`
  (fetched, LUT UV-mapping formula + two-sided handling, quoted verbatim).
- `gh api repos/selfshadow/ltc_code/license` — GitHub's own SPDX
  auto-detection result (`NOASSERTION`), cross-checked against the
  fetched LICENSE text itself (see row 1).

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/code support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------------|-------------------------------|
| 1 | License terms — pin precisely, do not assume vanilla BSD/MIT | Charter text: "permissive redistribution" (`:389`). | consume-now (record exact terms, not a paraphrase) | **VERIFIED, and the charter's "permissive" characterization needs a precision qualifier.** Fetched `LICENSE` verbatim: copyright "Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt", a 3-clause-BSD-STRUCTURED license (retain copyright notice in source AND binary redistributions, standard disclaimer) **PLUS an added, non-standard clause**: "If you use (or adapt) the source code in your own work, please include a reference to the paper" (with a full citation supplied in the license text itself). This EXTRA citation-requirement clause is exactly why `gh api repos/selfshadow/ltc_code/license` returns `NOASSERTION` (GitHub's SPDX auto-detector does not recognize it as a clean, unmodified BSD-3-Clause) rather than a recognized SPDX identifier — a real, load-bearing distinction from "just BSD," not a detection artifact to ignore. | This ticket's port-source vendoring commit (per CLAUDE.md's "new dependencies/ports pinned with license + tag recorded in the vendoring commit" rule) must record the license as "custom permissive, BSD-3-Clause-structured, WITH a mandatory paper-citation clause" — not simply "BSD" or "permissive" — and the citation itself (Heitz et al., the paper title/venue as given in the LICENSE text) must actually appear somewhere in RendererX's own attribution surface (a THIRD_PARTY_NOTICES-style file or equivalent — the exact repo convention for this was not independently re-verified in this pass, out of scope; flagged as a New gap below). |
| 2 | Regenerable LUT data via the original fitting tool | Charter text: LTC code "from the original authors" — i.e. the AUTHORITATIVE source, not a third-party re-derivation. | consume-now (recommended path) | **VERIFIED the repo ships the fitting tool, not just precomputed tables to blindly copy.** `fit/` contains `fitLTC.cpp` (the fit driver), `LTC.h`, and — directly relevant since RendererX's BRDF is GGX (per Task 7's own charter-bound choice) — `brdf_ggx.h` (a GGX-SPECIFIC fit target, alongside `brdf_beckmann.h`/`brdf_disneyDiffuse.h` for other BRDFs RendererX does not use). This means the CORRECT, precedent-clean path is to COMPILE AND RUN the original authors' own fitting code against RendererX's own chosen table resolution/roughness sampling, rather than reverse-engineering the WebGL demo's runtime LUT-loading mechanism (which was NOT successfully traced to a concrete baked-data file format in this pass — see row 3/Verification health) — producing exact, first-party-sourced values instead of a re-derived approximation. | The vendoring commit builds/runs `fit/fitLTC.cpp` (with `brdf_ggx.h` as the fit target) at the pinned commit, and the resulting LUT is committed as RendererX's own asset (KTX2 or an embedded float table, per whatever format `shaders/material/ltc.slang` consumes) — NOT copied from any other engine's already-baked LTC textures, keeping the provenance chain to the original authors intact end-to-end. |
| 3 | LUT texture format/storage the WebGL demo actually uses | N/A — this repo's own internal data-format question, not a renderer-precedent row. | UNVERIFIED — flagged, not guessed | `webgl/images/` contains only `error_icon.png` (563 bytes) — NOT a baked LUT texture. The demo's actual LUT-loading mechanism (likely a JS/JSON data blob under the `webgl/js/` directory observed in the repo's top-level listing, or generated at page-load time from the `fit/` output) was NOT traced to a concrete file in this pass. This means: **do not assume the demo ships a ready-to-consume LUT texture file** — row 2's fitting-tool path is the verified-available option; treat any claim that a ready-made image/DDS LUT exists in this repo as unconfirmed until someone reads `webgl/js/*.js` directly. | An implementer's first step should be confirming row 3 empirically (read `webgl/js/` before assuming either "a ready LUT exists" or "must run the fitter from scratch") — this gate flags the ambiguity rather than resolving it, since resolving it requires reading files this pass did not reach. |
| 4 | Rect-light evaluation shader | Ticket's own scope: "rect area lights (panels, screens, softboxes)." | consume-now | VERIFIED: `webgl/shaders/ltc/ltc_quad.fs` is the EXACT precedent for RendererX's own "rect panels/screens/softboxes" scope (the repo also ships `ltc_disk.fs`/`ltc_line.fs` for disk/line lights, which the ticket does NOT request — those should be treated as available-but-out-of-scope reference material, not silently ported alongside the quad case). | GPU test per the ticket's own convergence-probe acceptance sketch (row 6) validates the quad-light path specifically; disk/line are N/A-Phase-5 (not requested by the charter's rect-light framing). |
| 5 | LUT sampling formula (roughness, view angle → UV) | `ltc_quad.fs`, fetched verbatim 2026-08-20: `vec2 uv = vec2(roughness, sqrt(1.0 - ndotv)); uv = uv*LUT_SCALE + LUT_BIAS;` where `ndotv = saturate(dot(N,V))`, `LUT_SCALE = (LUT_SIZE-1.0)/LUT_SIZE`, `LUT_BIAS = 0.5/LUT_SIZE`, `LUT_SIZE = 64.0`. | consume-now | VERIFIED, exact formula and the demo's own table resolution (64×64) — this is a concrete, testable UV-mapping relationship, and the `sqrt(1-ndotv)` term (not a linear `ndotv` mapping) is a load-bearing precision detail a naive re-derivation could get wrong (linear NdotV wastes LUT resolution at grazing angles where the visual response is most nonlinear). | Unit test: the sampling formula reproduces this EXACT `uv` value at a table of (roughness, NdotV) points — a direct port-parity check, same discipline as Task 7's BRDF port-parity rows. |
| 6 | Convergence behavior (low roughness → mirror; high roughness → cosine-weighted solid angle) | Ticket's own acceptance sketch names this directly; it is also the LTC technique's OWN defining mathematical property (a "linearly transformed cosine" is, by construction, an ellipsoidal transform of the clamped-cosine lobe that reduces to a delta/mirror-like distribution as the transform matrix approaches identity-scaled-by-roughness→0, and to the true clamped-cosine lobe at roughness→1 — general property of the technique itself, not independently re-derived from a specific code citation in this pass). | consume-now | The `ltc_quad.fs` LUT-sampling mechanism (row 5) is EXACTLY what encodes this convergence — the LUT's own fitted values (produced by `fit/fitLTC.cpp`, row 2) are what make roughness=0 converge to a mirror-like reflection and roughness=1 converge to Lambertian-like solid-angle-weighted response; this is not separate code to write, it falls out of correct LUT generation + correct UV sampling (rows 2+5) done right. | Tolerance-band convergence probes exactly as the ticket's own acceptance sketch states: at low roughness, the LTC result converges to the ANALYTIC mirror reflection of the rect light's pose (closed-form comparison); at high roughness, to the analytic cosine-weighted solid-angle expectation (closed-form comparison) — both are independently-computable ground truths, not fuzzy visual checks, satisfying the plan's own "reference-vs-ground-truth discipline" global constraint. |
| 7 | One-sided vs. two-sided semantics | Ticket's own acceptance sketch names this directly. | consume-now | VERIFIED exact mechanism, `ltc_quad.fs` fetched verbatim: back-face detection via `bool behind = (dot(dir, lightNormal) < 0.0);` (where `dir` points from the shading point to a light-quad vertex and `lightNormal = cross(points[1]-points[0], points[3]-points[0])`); when `behind && !twoSided`, the light contributes zero; the polygon-clipping integral's own sign handling: `sum = twoSided ? abs(sum) : max(0.0, sum)`. | Test per the ticket's own text: a shading point placed behind a one-sided rect light receives ZERO contribution (discrimination: the SAME point with `twoSided=true` on the SAME light receives a nonzero, sign-correct contribution) — proves both the `behind` gate and the `abs()`-vs-`max(0,·)` integral-sign handling are both live, not just one of the two mechanisms. |
| 8 | Numerical stabilization at extreme angles | `ltc_quad.fs`'s `IntegrateEdgeVec` function, fetched: `max(1.0 - x*x, 1e-7)` used as an edge-case guard (per the earlier fetch's own finding — no explicit roughness clamping elsewhere in the file). | consume-now | VERIFIED presence of this specific epsilon-guard; NOT independently verified whether `1e-7` is float32-precision-appropriate for RendererX's own numeric conventions (the codebase's established epsilon conventions elsewhere were not cross-checked in this pass — out of scope). | A test at a genuinely edge-case pose (light polygon edge passing very near the shading point's tangent plane) confirms no NaN/Inf propagates through the integral — a "does not blow up" test, distinct from the accuracy-focused convergence probes in row 6. |
| 9 | Scene-side rect-light proxy + clustered/froxel integration | Ticket's own file list: "`src/rx_scene` (RectLightDesc + clustered integration)"; charter text "area lights participate in the froxel lists" (`:417`). | consume-now | Depends on Task 14's froxel grid (Stage 2, already delivered per the plan's sequencing, `:986` "T13→T14→T15 sequential" — Stage 2 precedes Stage 3) — NOT independently re-verified in THIS pass whether the existing froxel/cluster light-list machinery (Task 14/15's delivered code) already generalizes to a NON-POINT light shape (a rect light has an EXTENT, not a single position+radius, which affects froxel-membership/culling tests differently than a point/spot light's sphere-of-influence) — flagged as a genuine open question for the implementer, out of this ticket's own Stage-3-scoped reading to verify against Stage-2 code (a cross-stage dependency check better suited to whoever holds Stage 2's own delivered-code context). | A rect light's froxel membership test uses a CONSERVATIVE bounding volume (e.g. a bounding sphere/AABB around the rect's extent) for culling, with the LTC evaluation itself (rows 4-8) still being exact regardless of which froxels the light was assigned to — this is a standard "conservative broad-phase, exact narrow-phase" split; the acceptance criterion is that broad-phase culling never FALSE-NEGATIVES (excludes a froxel the rect actually illuminates). |
| 10 | Per-light cost measurement | Ticket's own acceptance sketch: "per-light cost measured." | consume-now | N/A — policy row; same real-driver-labeled requirement as every other Stage-3 ticket's cost claim (CLAUDE.md, the plan's standing corrective). | Real-driver-labeled Tracy measurement of marginal per-rect-light cost (e.g. N rect lights vs. N-1, isolating the LTC evaluation's own cost from base clustered-shading overhead), published per-stage-checkpoint. |

---

## Conflicts

None found that contradict the plan/charter/ticket text. The charter's
one-word characterization of the license as "permissive redistribution"
is directionally correct but imprecise enough (row 1) that this gate
flags it as needing exact recorded terms rather than treating "permissive"
as license-clearance-complete — CLAUDE.md's own vendoring rule ("license
+ tag recorded in the vendoring commit") requires the precision this row
supplies.

## New gaps

- **RendererX's own third-party-attribution surface convention** (row 1)
  was not identified in this pass — is there an existing
  `THIRD_PARTY_NOTICES`/`LICENSES/` file this citation-requirement clause
  must be added to, or does CLAUDE.md's "license + tag recorded in the
  vendoring commit" mean the commit MESSAGE alone suffices? Not resolved
  here (out of this ticket's own code-research scope); flagged for the
  coordinator, since the LTC license's added citation clause is stricter
  than every OTHER port source this phase uses (Filament/Khronos-Sample-
  Viewer are plain Apache-2.0, no citation obligation) — this ticket may
  need a small piece of attribution-surface work no other Stage-3 ticket
  needs.
- **LUT data-file location within the demo repo** (row 3) is genuinely
  unresolved — not a registry-worthy "feature gap" in the renderer sense,
  but a concrete pre-implementation research task (read `webgl/js/`)
  that should happen before this ticket's own implementation starts, to
  avoid the implementer either wastefully re-deriving what already
  exists or wrongly assuming a ready file exists when it doesn't.

## Verification health

- **Verified first-hand:** LICENSE text, `fit/`/`webgl/shaders/ltc/`
  directory listings, and `ltc_quad.fs`'s sampling/two-sided/edge-case
  code were all fetched verbatim from the pinned commit via direct
  GitHub API/raw-content requests, not paraphrased from a README or
  search digest.
- **Verified first-hand:** the repo's actual commit history shows
  `31e5e96b...` (2019-04-15) as the LATEST commit on `master` as of this
  gate round (`gh api .../commits/master`, not assumed) — i.e. this is a
  genuinely stable, long-unchanged reference implementation (7 years
  with no further commits), which is favorable for pinning (no drift
  risk) but also means "current HEAD" for this repo specifically has been
  constant since 2019, unlike Filament/Khronos-Sample-Renderer's
  actively-moving HEADs cited elsewhere in this gate round.
- **Explicitly UNVERIFIED, flagged, not guessed at:** row 3's LUT-data
  storage location/format; row 9's froxel-grid generalization to
  non-point light shapes (a cross-stage code question outside this
  ticket's Stage-3-scoped reading).
- The LTC technique's own mathematical convergence property (row 6) is
  stated from general technical knowledge of the published LTC papers,
  not re-derived from the fetched code in this pass — the CODE citation
  (row 5's UV formula) is what's verified first-hand; the underlying
  MATH property is well-established public knowledge about the technique
  itself, appropriately distinguished here from a code-level claim.
- No dead links encountered.
