# Completeness matrix — P5 T20 (issue #56): Stage 2 exit — sample 10_lights + checkpoint numbers

**Plan task:** Task 20, Stage 2 exit (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:609-627`).
This ticket is a SYNTHESIS/exit-checkpoint ticket, not a new-technique
ticket — its own scope is almost entirely "prove T13-T19 actually landed
together, at scale, packaged, measured." Most of this matrix's rows are
therefore direct pointers into T13-T19's own matrices rather than new
independent research.

**Sources consulted (in-repo, 2026-08-20):** `samples/09_scene/main.cpp`
(the Stage-2-Phase-4 exit sample's own established patterns this ticket
must reuse, not reinvent, per the plan's own "samples are pure consumers"
standing rule): `--threads N` CLI-flag precedent (:229) → this ticket's
own `--lights N` flag follows the SAME shape; the HUD counter-display
convention (`app.viewLists.counters.*`, :2406-2409) → this ticket's own
light/froxel/shadow counter HUD rows follow the SAME convention;
`cullMaskFromToggles()`/layer-channel HUD toggle pattern (:562-605) →
this ticket's own "quality-tier toggles" (plan:614) reuse the SAME
toggle-widget shape. `tools/package_samples.sh:214,353-363` (the existing
per-sample packaging list, including sample 09's own shader-directory
enumeration — this ticket's own packaging entry is a direct structural
sibling, not a new pattern).

---

## The matrix

| Feature | First-tier precedent (cited) | Disposition | Library/source support (verified) | Acceptance criterion |
|---|---|---|---|---|
| Sample consumes engine facilities only (Task 5 rule) | Standing Phase 5 rule (plan:88-93): *"No sample hand-rolls what the engine provides... Any facility a Phase 5 sample needs that the engine lacks is an API gap: promote it into the engine in the same task."* Task 5 (Stage 0) is explicitly the backlog-CLOSURE task; this rule then binds every subsequent task, including this one. | consume-now | Cross-references sample 09's own established precedent directly (mouse-capture/fly-camera/grid-layout facilities already promoted into `rx_platform`/`rx_scene` per Task 5's own scope) — sample 10 is the FIRST Stage-2-of-Phase-5 sample built AFTER Task 5's closure, so it has NO excuse to reintroduce a hand-rolled facility Task 5 already promoted. | Acceptance criterion: an audit ROW (matching Task 5's own audit-table format, plan:268-269) for sample 10 specifically — enumerating every engine facility it consumes (light creation, froxel/cluster binding, CSM/PCSS/atlas/contact-shadow toggles, HUD counter reads) and confirming NONE are sample-local reimplementations. Zero undispositioned rows, per Task 5's own closure criterion applied here again. |
| Night-scene demonstrator content: authored punctual lights + `--lights N` synthetic stress | Plan's own text (plan:611): *"sponza with authored punctual lights + synthetic `--lights N` stress."* Sample 09's own `--threads N` flag (main.cpp:229) is the DIRECT CLI-flag-shape precedent. | consume-now | VERIFIED precedent shape in-repo (main.cpp:229's own `argv` parsing pattern). Cross-references matrix-p5t13's own "authored glTF punctual lights arrive... value-asserted" criterion — the AUTHORED half of this sample directly EXERCISES T13's own import-consumption path at real content scale (Sponza), not just T13's own synthetic fixtures. | Acceptance criterion: `--lights N` generates N synthetic point/spot lights (positions/colors/ranges varied, not degenerate/identical — a degenerate all-identical-light stress would not actually exercise T14's own per-froxel list-membership logic meaningfully) scaling PAST T14's own declared per-froxel/total capacity (the content-scale rule, applied here at REAL sample scale, not just T14's own unit-test scale) — this is the sample-level instance of T14's Open Question #2 (light-count capacity) being proven correct at the scale the charter actually cares about ("hundreds-to-thousands"), not merely at a synthetic unit-test's smaller scale. |
| HUD counters: light/froxel/shadow | Plan's own text (plan:612-613): *"HUD shows light/froxel/shadow counters + quality-tier toggles."* Sample 09's own `CullCounters` HUD-display convention (main.cpp:2406-2409, `ImGui::Text("  imported/candidates: %u", app.viewLists.counters.totalCandidates)` etc.) is the DIRECT structural precedent. | consume-now | VERIFIED precedent in-repo. Cross-references EVERY prior ticket's own "exact counters" acceptance criteria directly: T14's froxel-assignment counters (matrix-p5t14's "capacity+1" row), T16's per-cascade caster counters (matrix-p5t16's own row, echoing plan:549), T18's atlas-slot/exhaustion counters (matrix-p5t18's own row). | Acceptance criterion: the HUD displays counters SOURCED from each ticket's own already-specified exact-counter struct (no new, parallel, sample-local counter-tracking logic reimplementing what T14/T16/T18 already compute and expose) — a code-review/grep criterion (`app.hud` reads engine-owned counter fields, never increments its own). |
| Quality-tier toggles | Plan's own text (plan:613). Cross-references T16's resolution-tier ruling (matrix-p5t16's own "desktop/Deck tier" row) + T17's own "filter selectable per quality tier" criterion (matrix-p5t17, plan:568) + T18's spot/point on/off. | consume-now | N/A — an aggregation of already-specified per-ticket toggle surfaces; this ticket's own job is exposing them together in one HUD, not inventing new toggle semantics. | Acceptance criterion: EVERY quality-tier axis T16/T17/T18 individually specify (shadow resolution tier, PCF-vs-PCSS filter selection, contact-shadows on/off per T19's own Open Question resolution) is independently toggleable at runtime in this sample — a completeness check against the OTHER tickets' own delivered toggle surfaces, not a new design question of its own. |
| Exact counter gates + tolerance pixel gate with discrimination floor | Plan's own named criterion (plan:623-624): *"Exact counter gates (lights assigned/culled, froxel occupancy, shadow casters) + pixel gate with discrimination floor."* Standing project rule (plan:74-81): *"gates that bake a bug certify the bug... every new pixel gate ships with a discrimination proof."* | consume-now | N/A — standing rule, concretized here as the SAMPLE-LEVEL instance of every individual ticket's own already-specified GPU-value-probe criteria (T13's falloff probe, T14's membership tests, T16's cascade-boundary probe, T17's penumbra-monotonicity probe, T18's atlas-swap probe, T19's contact-gap probe) — this sample's own headless gate is not a NEW test design, it is the INTEGRATION-scale re-assertion that all of them still hold when composed together in one frame. | Acceptance criterion: the headless gate suite includes, at minimum, ONE discrimination-proof pixel test per major feature landed in T13-T19 (not necessarily identical to each ticket's own unit-level probe, but exercising the SAME feature at whole-scene scale) — a coverage checklist cross-referencing this matrix's own "sources consulted" list against the final gate suite's test names, closed before this ticket exits. |
| `--lights` scaling table + CI perf gate | Plan's own named criterion (plan:625-626). Cross-references T15's own "Scaling numbers published: 100/1k/5k synthetic lights" criterion (matrix-p5t15's own row, plan:521-522) — T15 takes the FIRST measurement; this ticket is where it becomes a PERMANENT, CI-gated regression check, per the standing "performance is an exit criterion... CI carries performance regression gates" rule (CLAUDE.md, echoed at plan:94-101). | consume-now | N/A — measurement + CI-wiring requirement; directly building on T15's own already-taken numbers (this ticket republishes/re-measures at the SAMPLE's own real content scale — Sponza + synthetic lights together — rather than T15's own more isolated synthetic-only measurement). | Acceptance criterion: a CI perf regression gate is wired on the `--lights` scaling numbers (a genuine regression — not merely "did it run" — fails CI, per CLAUDE.md's "a performance regression blocks a phase exit the same way a failing test does"), desktop numbers published in the ledger, Deck rows tracked honestly in MANUAL_VERIFICATION (never silently assumed, per the plan's own standing corrective, plan:65-73). |
| Packaging | `tools/package_samples.sh`'s existing per-sample structure (:214 sample-name list, :353-363 sample 09's own shader-directory copy list — the direct structural sibling this ticket's own entry follows). | consume-now | VERIFIED existing pattern in-repo — sample 10 needs its OWN entry in both the sample-name list and a shader-directory copy block, enumerating whatever NEW shader directories T14-T19 introduce (`shaders/cluster/`, growth to `shaders/shadow/`, any new `shaders/material/` lighting-module files) — a mechanical but real, easy-to-miss extension (a forgotten shader file in the packaging list produces a standalone-run failure that headless CI would not catch, since CI runs from the BUILD tree, not the packaged zip). | Acceptance criterion: `tools/package_samples.sh` gains a `10_lights` entry; the packaged zip is STANDALONE-verified (run from the packaged directory, not the build tree) per the standing Phase 4 precedent — this is the ONE class of defect headless CI structurally cannot catch on its own. |

---

## Open Questions

*(This ticket's own scope surfaces no NEW open questions beyond
aggregating T13-T19's own — see each linked matrix's Open Questions
section for the substantive decision points this exit checkpoint depends
on being resolved BEFORE it can close: T14's compute-shader-vs-CPU-
froxelization scope correction and light-count capacity design, T16's
seam-blending build decision and standard-Z re-affirmation, T17's
classic-PCSS-vs-EVSSM scope correction and lavapipe/real-driver gating
split, T18's cubemap-vs-dual-paraboloid ruling, T19's depth-prepass hard
dependency and contact-shadow default/scope ruling. This ticket cannot
meaningfully dispatch until those are settled, since its own HUD/counter/
toggle surface is a direct aggregation of what they deliver.)*

## Verification health

**Verified first-hand this session:** every in-repo citation
(`samples/09_scene/main.cpp`'s CLI-flag/HUD/counter/toggle patterns,
`tools/package_samples.sh`'s packaging structure) was read directly from
the working tree 2026-08-20. No new external fetches were needed for this
ticket — its research burden is almost entirely satisfied by the other
seven Stage-2 matrices' own already-cited primary sources, cross-referenced
here rather than re-fetched.
