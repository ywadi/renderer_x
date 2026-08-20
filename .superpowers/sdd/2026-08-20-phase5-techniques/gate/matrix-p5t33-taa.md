# Completeness matrix — P5 T33 (issue #69): TAA

**Plan task:** Task 33, Stage 4 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:872-895`).
**Ticket's own claim (verified below, one of three seams does NOT hold as
claimed):** *"Temporal anti-aliasing riding the seams Phase 4 pre-paid: the
Camera's inert jitter offset (`rx_scene/camera.h`), render-graph history
resources, and prev-frame transforms in the transform pools → per-pixel
velocity buffer."*

**Sources consulted (in-repo, 2026-08-20) — the three claimed seams,
verified one at a time:**

1. **Camera jitter offset — VERIFIED REAL.** `src/rx_scene/include/
   rx_scene/camera.h:49-56,131-135,155-164`: `Camera::jitter` (a
   `glm::vec2`, `{0,0}` default) is threaded through `proj()`'s own
   translation terms (the header's own comment: "added to the matrix's own
   x/y translation-of-z terms... the standard depth-independent
   screen-space-jitter technique"); `{0,0}` is byte-identical to the
   unjittered matrix today, so every Phase 4 call site is provably
   unaffected. This seam is genuinely delivered and TAA-ready as claimed.

2. **Render-graph history resources — VERIFIED REAL.**
   `src/rx_graph/include/rx_graph/pass.h:67-138`: `addHistoryInput`/
   `setHistoryOutput`, backed by TWO pinned physical images that ping-pong
   across frames (`transient_pool.h`), with a documented cross-frame (not
   intra-frame) synchronization contract the executor derives per-slot.
   Delivered in Phase 4 Task 1, exercised by `src/rx_graph/tests/
   test_execute_gpu.cpp`/`test_compile.cpp` (grep-confirmed test files
   reference `history`). This seam is genuinely delivered.

3. **"Prev-frame transforms in the transform pools" — NOT VERIFIED; NO
   SUCH PRODUCTION API EXISTS.** Repo-wide grep for `prev`/`Prev`/
   `previous` (case-insensitive, excluding `preview`/`prevent`) across
   `src/rx_scene/include/rx_scene/*.h` and `src/rx_scene/**/*.cpp`:
   **zero hits in production headers.** The ONLY hit anywhere in
   `rx_scene` is a **test-only design note**:
   `src/rx_scene/tests/scene_test.cpp:136-156`, TEST_CASE
   `"Transform SoA column accepts a same-shaped 'previous transforms'
   copy with zero reshaping [transform-pool prev-frame-slot design note,
   issue #5 2026-08-10]"`. Read in full — its own comment states plainly:
   *"A stub 'previous transforms' array, built ENTIRELY in this test — no
   new production API added for it. Proves `transformsSpan()`'s existing
   contiguous layout is already sufficient for a future double-buffered/
   copyable-row addition to bolt onto, with zero reshaping."* This is a
   COMPATIBILITY PROOF that `Scene::transformsSpan()`'s SoA layout WOULD
   accept a double-buffered previous-frame column later — it is not, and
   was never claimed by its own text to be, an actual previous-frame
   transform STORE, API, or double-buffering mechanism. `Scene` (`scene.h:
   245-330`) exposes exactly one `transformsSpan()` — the CURRENT frame's
   transforms, single-buffered.

**This is the single most consequential finding for T33's gate:** the
ticket/plan text presents all three as equally "pre-paid" seams, but only
two of three are. The third does not exist as production infrastructure
and must be BUILT by T33 itself (or by whichever task first needs it), not
merely "consumed."

**Sources consulted (external, fetched 2026-08-20, `google/filament`,
pinned tag `v1.75.0`, Apache-2.0):**
- `filament/src/materials/antiAliasing/taa/taa.mat` (533 lines, full file
  read). Confirmed real, working TAA implementation:
  - History buffer + reprojection: `materialParams.reprojection`,
    depth-based UV reprojection of the history sample (lines 333-341:
    "read the depth buffer center sample for reprojection... reproject
    history to current frame").
  - Neighborhood clamping: min/max box (`clipToBox`, line 220) with three
    selectable quality tiers named in-source at lines 129-140 ("accurate
    box clipping", "clamping instead of clipping", "no clipping (for
    debugging only)") — directly maps to the ticket's own "clamp provably
    live" ON/OFF discrimination requirement.
  - `useYCoCg` option (`RGB_YCoCg`/`YCoCg_RGB`, lines 86,202-217) —
    optional colorspace for the clamp/clip operation.
  - History sampling: Catmull-Rom (`sampleTextureCatmullRom`, line 346)
    vs. plain `textureLod` depending on `historyReprojection`/quality
    settings.
  - Jitter-aware reconstruction: `materialParams.jitter` consumed
    directly (lines 365-411, "jittered sample"/"closest jittered sample"/
    Lanczos "unjittering" reconstruction).
- Per-frame FXAA is a separate, simpler file
  (`filament/src/materials/antiAliasing/fxaa/fxaa.mat`) — not the port
  source for T33 (fundamentally different, non-temporal technique); named
  here only to record it was not confused with `taa.mat`.

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/port-source support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------|-------------------------------|
| 1 | Camera jitter activation (flip the inert `{0,0}` on) | N/A — this codebase's own prepaid seam. | consume-now | Verified real (Sources item 1). | Ticket's own "jitter-off path byte-identical to pre-task gates" criterion stands, PLUS a jitter-ON regression: turning jitter on with TAA's history/clamp also on reproduces the jitter-off image within tolerance over N accumulated frames (the actual point of TAA), value-asserted. |
| 2 | Render-graph history-resource consumption (TAA's own color history) | Filament `taa.mat`'s reprojected-history-sample pattern (Sources item 3, lines 333-348). | consume-now | Verified real, both the RendererX-side mechanism (`addHistoryInput`/`setHistoryOutput`) and the Filament-side algorithm shape. | Ticket's own text stands: history reprojection, neighborhood clamping per the ported reference. |
| 3 | **Per-pixel velocity buffer fed by "prev-frame transforms in the transform pools"** | Ticket's own claimed seam. | **preserve-later reclassified to consume-now-as-NEW-WORK** — this is not a consumption of existing infrastructure; it is new infrastructure T33 must build from the ground up (or a dedicated small task ahead of it — Open Question below). | **NOT present** — verified absent (Sources item 3). The `scene_test.cpp` design note only proves `Scene::transformsSpan()`'s SoA shape is compatible with a future addition; no double-buffering, no per-instance previous-transform storage, no per-frame update wiring exists in `Scene`/`DrawListBuilder`/anywhere else in `rx_scene` today. | The velocity-buffer pass needs a REAL previous-frame world-transform per rendered instance. Minimum viable shape: `Scene` grows a second `glm::mat4` SoA column (`previousTransformsSpan()`), updated by whatever currently writes `transformsSpan()` at the START of each frame (copy current → previous BEFORE this frame's transform updates apply) — exactly the "double-buffered/copyable-row addition" the design-note test already proved fits with zero reshaping. This is real, scoped, buildable work — but it is NOT free, and the acceptance criteria must say so rather than imply it is a pure "riding a seam" consumption like rows 1-2. |
| 4 | Velocity computation (current clip-space position vs. reprojected previous clip-space position, using row 3's data) | Standard technique (current_clip - previous_clip, both divided by their own `w`, encoded to a velocity/motion-vector texture) — this specific formulation is NOT named as a discrete Filament file in this pass's search (Filament's TAA reprojects via depth+current-camera-matrices rather than a discrete stored velocity buffer, per `taa.mat`'s own `materialParams.reprojection` approach, Sources item 3) — **worth flagging: Filament's OWN `taa.mat` does NOT use a stored per-object velocity buffer the way the ticket's text implies ("per-pixel velocity buffer... TAA is its first consumer")**; Filament reprojects using depth + camera view-projection deltas alone (sufficient for STATIC geometry; it does not claim correct reprojection for MOVING objects without their own velocity term — Filament's `taa.mat` header does not document a moving-object velocity path in the lines read). | Task-1-spec decision point (Open Question below) | Partially verified: depth-based static reprojection is real and ported (row 2); a discrete stored velocity buffer for MOVING-object correctness is the ticket's own broader ambition (explicitly serving future upscalers/motion-blur consumers too, per the registry text the ticket cites) and is NOT itself present in the cited Filament file. | If the spec commits to a genuine velocity-buffer resource (recommended — see Open Questions), its acceptance criterion is the ticket's own: "velocity VALUES asserted for a known-motion object (analytic pixel offset)" — this requires row 3's real per-instance previous-transform data, not a depth-only reprojection trick. |
| 5 | Halton jitter sequence | Standard technique; not Filament-file-specific (jitter SEQUENCE GENERATION is a small, well-known formula — Filament's own jitter comes from `View`-side C++ code, not the `.mat` shader, consistent with Camera owning `jitter` in RendererX per the header's own precedent note, `camera.h:49-56`). | consume-now | N/A — small, standard, low-risk math; not a "port" so much as a known formula (base-2/base-3 Halton sequence). | Device-free unit test: N-sample Halton sequence values match the standard formula at cited indices. |
| 6 | Neighborhood clamping (ghosting bounded, clamp provably live via ON/OFF) | Filament `taa.mat` `clipToBox`/min-max box variants (Sources item 3, lines 129-140, 220-235). | consume-now | Verified real, three quality tiers available to port. | Ticket's own text stands (moving-object test, clamp ON vs OFF discriminates). |
| 7 | Convergence measured (static-scene edge-aliasing energy reduced by a spec'd factor) | N/A — RendererX's own metric; Filament does not ship a numeric convergence-metric test in the `.mat` file itself (that would live in Filament's own test suite, not fetched this pass — out of scope, this is a RendererX-side test to author regardless of what Filament's own CI does). | consume-now | N/A. | Ticket's own text stands: value metric (e.g. Sobel-edge-energy delta), not eyeballs. |
| 8 | Interplay: SSR + volumetric reprojection stable under jitter | Cross-stage dependency — SSR is Task 26 (Stage 3, `#62`, out of THIS gate's ticket range) and volumetric fog is Task 30 (this same Stage 4 round, `matrix-p5t30-froxel-fog.md`, which ALSO consumes history resources for its own temporal reprojection). | preserve-later (cross-task re-verification, not a T33-only deliverable) | N/A — both dependencies verified absent/not-yet-landed at gate-research time (Task 26 not in this agent's scope to re-verify; Task 30 confirmed unbuilt in its own matrix). | Ticket's own text stands: re-run Task 26's and Task 30's own gates with TAA's jitter ON, asserting their existing tolerances still hold — this is naturally sequenced LAST within Stage 4 (T33 is scheduled strictly after T29-T32 anyway per plan:989-990, and T30 already lands before T33). |

---

## Conflicts

**The ticket's own scope text overstates seam 3 as delivered
infrastructure.** *"Prev-frame transforms in the transform pools"* reads
as an existing, consumable resource alongside the two genuinely-delivered
seams (camera jitter, history resources) — verified FALSE: only a
test-only, zero-production-code design NOTE exists (`scene_test.cpp:
136-156`), whose own comment explicitly disclaims adding any production
API. This is exactly the class of finding the primary gate exists to
surface before dispatch: the plan's "Phase 4 pre-paid" framing for T33
would otherwise lead an implementer to search for a `previousTransforms`
API that does not exist and either waste time looking for it or,
worse, silently build ad hoc per-sample previous-transform tracking
(the Task 5 "samples are pure consumers of engine facilities" violation
this same plan explicitly forbids elsewhere, plan:88-93).

**Secondary, smaller finding:** the ticket's "per-pixel velocity buffer"
framing implies Filament's own TAA needs and uses one — verified Filament's
`taa.mat` itself reprojects via depth + camera matrices only, with no
stored velocity texture read in the 533 lines examined. This does not mean
RendererX should skip a velocity buffer (a real stored velocity buffer is
the CORRECT, more general solution for moving-object reprojection AND is
explicitly wanted for future upscaler/motion-blur consumers per the
registry text the ticket itself cites) — it means the "riding a
[Filament-implied] seam" framing for the velocity buffer specifically is
softer than claimed: this is genuinely NEW infrastructure design, not a
straight port of an existing Filament mechanism.

## New gaps

None beyond what's already captured above (row 3/4 IS the gap, not a
separate item).

## Open Questions (for the coordinator's binding ruling)

1. **Who builds the previous-frame transform storage (row 3)?**
   Recommendation: **T33 builds it directly**, as a small, well-scoped
   `Scene`-side addition (a second `glm::mat4` SoA column,
   `previousTransformsSpan()`, populated by a "copy current → previous"
   step at a documented point in the frame — the natural home is
   alongside whatever already runs once per frame in `Scene`'s update
   path). It is small enough not to warrant a separate ticket, and
   deferring it further would just relocate the same gap into T33 anyway
   (nothing else in the Stage-4 plan needs it before T33). The spec
   should record explicitly that this is NEW work, not a consumed seam,
   so T33's own estimate/acceptance criteria are not silently short by
   this scope.
2. **Depth-only reprojection (Filament's own approach) vs. a genuine
   stored velocity buffer (the ticket's stated ambition).**
   Recommendation: **build the real velocity buffer**, not Filament's
   narrower depth-only trick — the ticket's own text explicitly wants this
   as shared infrastructure for future upscaler/motion-blur consumers
   (registry-cited), which depth-only reprojection cannot serve (it has no
   representation of a MOVING object's own screen-space delta, only the
   camera's). This is the more ambitious but correctly-scoped choice,
   consistent with row 3's recommendation above (the velocity buffer is
   exactly what row 3's previous-transform data feeds).
