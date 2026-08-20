# Matrix — P5 T28 (issue #64): Stage 3 exit — sample 11_surfaces + checkpoint numbers

**Plan task:** Task 28 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:772-786`), Stage 3 (closing).
**Charter binding:** Stage 3 demonstrates priorities 5-8 together (clearcoat/
anisotropy, real transmission, SSR, LTC) — `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:452-458`.
**Depends on:** T21, T22, T23, T24, T25, T26, T27 all landing first
(this ticket is the Stage-3 checkpoint, sequenced last per the plan's own
execution notes: `docs/superpowers/plans/2026-08-20-phase5-techniques.md:987-988`,
"T28 closes"). Also depends on Task 5's "samples are pure consumers of
engine facilities" rule (plan `:88-93, 256-284`) and Task 11's conformance
harness for any Khronos-fixture content the sample reuses.

**Sources consulted:**
- Ticket body: `gh issue view 64`.
- Plan Task 28 + Global Constraints + Task 5 + Task 11
  (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:55-134, 256-284,
  420-443, 772-786, 980-1004`).
- Charter block (cited above).
- This same gate round's T21-T27 matrices (cross-referenced for what
  real/synthetic content each landed feature will actually have to
  demonstrate with).
- Real-content asset audit (this session, direct glTF-JSON parse of
  `assets/fetched/Workshop/workshop_render_scene.glb`): confirms exactly
  which Stage-3 features Workshop can and cannot demonstrate (see matrix
  rows).
- Phase 4 exit-sample precedent: `docs/superpowers/plans/2026-08-20-phase5-techniques.md:444-459`
  (Task 12, Stage 1 exit — the SAME "viewer upgrade + checkpoint numbers"
  pattern this ticket repeats for Stage 3), read for pattern consistency,
  not independently re-verified against ITS delivered code (out of this
  ticket's own Stage-3 scope; Stage 1 is a different research agent's
  assignment in this gate round).

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/code support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------------|-------------------------------|
| 1 | Engine-facilities-only sample discipline | Task 5's own rule (plan `:88-93`): "No sample hand-rolls what the engine provides... Any facility a Phase 5 sample needs that the engine lacks is an API gap: promote it into the engine in the same task (or record an explicit ruling why sample-local stands)." | consume-now | This is a REPEATED, already-established Phase-5-wide rule (Task 5 closes the inherited Phase-4 backlog of this exact violation pattern) — not new for this ticket, but binding on it as the Stage-3-exit checkpoint. | Audit-row acceptance per Task 12's own precedent (row shape already used at the Stage 1 exit, `:456` "Viewer consumes only engine APIs (audit row per the Task 5 rule)"): sample 11's own hand-roll audit table, zero undispositioned rows, mirroring Task 5's own delivered audit format. |
| 2 | Real-content material for the glass-storefront vignette (thin + thick + frosted) | Ticket's own scope text: "glass storefront vignette (thin + thick + frosted)." | **Partially coverable by real content; genuinely mixed with synthetic** | **VERIFIED via direct glTF-JSON inspection** (this session, same method as the T23/T24 matrices): Workshop (`assets/fetched/Workshop/workshop_render_scene.glb`) carries 2 REAL `KHR_materials_transmission` materials (`glass`, `transmissionFactor≈0.973`; `Material.039`, `transmissionFactor=1.0`) — usable for the THIN-surface piece of this vignette. It carries **zero** `KHR_materials_volume` materials — **NOT usable for the thick-volume piece**. Frosted glass requires a non-zero transmission-roughness value on SOME material; whether either of Workshop's 2 transmission materials happens to carry meaningful roughness was not checked in this pass (a `roughnessFactor`/`metallicRoughnessTexture` read, not just extension presence — cheap to check but not done here; flagged as a concrete pre-implementation task). **Conclusion: the "thin" third of this vignette has a verified real-content option (Workshop); the "thick" and (unverified) "frosted" thirds do NOT** — the sample will need AT LEAST one purpose-authored or Khronos-conformance-fixture (`AttenuationTest`/`DragonAttenuation`, per the T24 matrix's row 8) asset to cover thick-volume, since no committed real-content asset in this repo has volume data today. | The sample's material-disposition table (mirroring the Bistro conversion task's own precedent, plan `:933-935`) explicitly names, per vignette element, whether its content is Workshop-real, a Khronos conformance fixture, or purpose-authored synthetic — not silently mixed without disclosure, matching the plan's own "own-content-blindness lesson" (`:86-87`, "'works on the committed fixture' is not 'works'"). |
| 3 | Real-content material for clearcoat/anisotropy spheres | Ticket's own scope: "clearcoat/anisotropy spheres." | Synthetic by the ticket's OWN description ("spheres" — a canonical synthetic BRDF-showcase primitive, not a real-content claim) | VERIFIED Workshop DOES carry 3 real `KHR_materials_clearcoat` materials (`extensionsUsed` includes `KHR_materials_clearcoat`, count=3, per this session's direct parse) — a real-content BONUS beyond the ticket's own stated synthetic-sphere plan, not required but available if the sample wants a real-content clearcoat callout alongside the synthetic spheres. Anisotropy has NO real-content option anywhere in this repo (Workshop uses none; no anisotropy fixture is committed) — purely synthetic per the ticket's own plan, consistent. | No conflict with the ticket's own text; flagged as an OPPORTUNITY (Workshop's real clearcoat materials could supplement, not replace, the synthetic spheres the ticket already plans) rather than a gap. |
| 4 | Real-content material for the LTC panel-lit set | Ticket's own scope: "LTC panel-lit set." | Synthetic (structural — LTC is a LIGHT technique, not a material; "real content" for a light-rig demonstration is inherently authored scene-lighting data, not an imported-asset property the way material extensions are) | N/A — this is a category distinction, not a gap: unlike rows 2-3 (material EXTENSIONS an imported asset either does or doesn't carry), LTC rect lights are SCENE-AUTHORING data (per the T27 matrix's own finding that `RectLightDesc` is new `rx_scene` API surface, not glTF-import data) — there is no glTF extension for rect-area-lights this ticket could "discover" in a real asset either way. | The panel-lit set is authored directly via the T27-delivered `RectLightDesc` API, illuminating either the synthetic clearcoat/anisotropy spheres (row 3) or a purpose-built showroom/softbox-style set — the ticket's own "panel-lit set" framing already implies this, no correction needed. |
| 5 | Counter gates on transmission/SSR paths | Ticket's own acceptance sketch: "counter gates on transmission/SSR paths." | consume-now | This mirrors the ALREADY-ESTABLISHED counter-gate pattern from Stage 2's own exit sample (Task 20, plan `:609-627`: "Exact counter gates (lights assigned/culled, froxel occupancy, shadow casters)") — Stage 3's counters are the natural analogues: transmission draw counts (per the T23 matrix's own open question about WHICH partition transmission draws live in — this ticket's counter gate is a direct consumer of whatever T23/T25 land, and should be written against the FINAL partition shape, not guessed at here), and SSR hit/miss/fallback counts (a natural instrumentation point given the T26 matrix's own finding that SSR's fallback path is a soft `fade`-weighted blend, not a binary flag — a counter here could report a HISTOGRAM of fade weights, not just a hit/miss binary, giving a richer regression signal). | Counter test: transmission-draw-count and SSR-hit/miss/fallback-count both exact and CI-gateable (same "counters exact and CI-gateable" bar Stage 2's Task 14 already set for froxel-list counters, plan `:504-505`), asserted against a scene with a KNOWN transmission-draw count and a KNOWN mix of SSR-hit/miss geometry. |
| 6 | Pixel gate + discrimination floor | Ticket's own acceptance sketch. | consume-now | Same D17 tolerance-gate discipline as every other Phase-4/5 pixel gate; the DISCRIMINATION requirement specifically (per the plan's own reference-vs-ground-truth global constraint, `:74-81`, "every new pixel gate ships with a discrimination proof: break the feature → gate fails, evidence pasted") means sample 11's committed reference must be PROVEN sensitive to each of the 4 showcased features independently (disabling clearcoat alone changes the gate; disabling LTC alone changes the gate; etc.) — a single "the whole scene looks right" gate that happens to pass is NOT sufficient evidence per this project's own standing discipline. | 4 independent discrimination proofs (one per showcased priority-5-8 feature), each demonstrating the committed reference FAILS when that one feature is disabled/reverted, alongside the nominal passing gate. |
| 7 | Real-driver sustained run, zero validation errors | Ticket's own acceptance sketch + the plan's standing corrective (`:65-73`). | consume-now | N/A — policy row, directly binding per the standing corrective already cited in the T26 matrix's row 6. | Sustained real-driver run (default ICD, `--validate`) with zero validation errors across the full showcase scene (all 4 features simultaneously active — the interaction case, not each feature in isolation, since Stage-3-checkpoint's whole POINT is proving they compose without conflict, e.g. a transmissive object seen through SSR, or an LTC-lit clearcoat sphere). |
| 8 | Numbers published (driver-labeled) | Ticket's own acceptance sketch. | consume-now | Per-feature cost numbers already committed at each landing ticket (T22's mip-chain cost, T23/T24's transmission cost — implied but not explicitly named in those tickets' own acceptance sketches as a PUBLISHED number, only T22/T26/T27 explicitly name "cost measured" in their own text) should be RE-MEASURED here in the COMPOSED scene, since per-feature isolated costs do not sum linearly on real hardware (shared bandwidth, cache pressure, descriptor-binding overhead) — the Stage-3 checkpoint number is the one that actually matters for the phase-exit performance-regression-gate CLAUDE.md requires, not the sum of each ticket's own isolated measurement. | Desktop AND — per CLAUDE.md's exit-criterion policy applying at minimum informally to STAGE checkpoints, even though the HARD Deck-blocking requirement is named at the phase-exit level (T36, plan `:966-969`) — Deck numbers tracked (MANUAL_VERIFICATION row per the plan's own established pattern, not silently skipped) for the COMPOSED Stage-3 scene, driver-labeled, published in the ledger per the Stage-checkpoint pattern (plan `:996-999`). |
| 9 | Packaging/CI | Ticket's own acceptance sketch. | consume-now | Same `tools/package_samples.sh` + CI pattern every prior sample checkpoint already establishes (Task 12, Task 20 precedents, both cited above). | Packaged zip standalone-verified, per the identical bar Task 12/20 already set. |

---

## Conflicts

None found that contradict the plan/charter/ticket text. This ticket is
correctly scoped as a checkpoint/aggregation task with no independent
technical risk of its own beyond what its seven prerequisite tickets
(T21-T27) already carry — its own risk is almost entirely SEQUENCING
(cannot productively start meaningful work until those land) and CONTENT
DISCLOSURE (row 2's real-vs-synthetic mixing must be honest, per the
plan's own "own-content-blindness lesson").

## New gaps

None beyond what T21-T27's own matrices already register (this ticket's
matrix intentionally does not re-register cross-referenced gaps — see
each prerequisite ticket's own "New gaps" section, particularly the
`MaterialTextureSlot` fixed-slot ceiling registered in T21/T23/T24 and
the transmission-partition open question registered in T23/T25, both of
which this sample's material-disposition table and counter gates
directly depend on being resolved before T28 can meaningfully test them).

## Verification health

- **Verified first-hand:** the Workshop-scene clearcoat/transmission/
  volume material-extension census (rows 2-3) via direct glTF-JSON
  parsing of the actual committed binary, same method and same session
  as the T23/T24 matrices' identical finding (not re-parsed independently
  — same underlying fact, cited consistently across all three matrices
  that touch this asset).
- **NOT verified in this pass, flagged as a concrete pre-implementation
  check:** whether Workshop's 2 transmission materials carry meaningful
  (non-zero, non-trivial) roughness values suitable for a "frosted" showcase
  element (row 2) — only extension PRESENCE was checked, not the
  roughness FACTOR's actual value.
- This ticket's own acceptance criteria are, by its nature as an
  aggregation/checkpoint task, mostly PROCESS/POLICY rows (packaging,
  counters, gates) rather than novel technical-precedent rows — this
  matrix's citation density is correspondingly lower than T21-T27's,
  which is an accurate reflection of the ticket's own shape, not a gap
  in this research pass.
- No dead links encountered.
