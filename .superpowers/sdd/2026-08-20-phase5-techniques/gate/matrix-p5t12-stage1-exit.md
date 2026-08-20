# Completeness matrix — ticket #48: [P5 T12] Stage 1 exit — viewer upgrade + checkpoint numbers

**Plan task:** Task 12, "Stage 1 exit — viewer upgrade + checkpoint
numbers" (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:444-459`),
Stage 1's closing task. Aggregates Tasks 7-11 (tickets #43-#47) into a
shippable checkpoint — this matrix is necessarily thinner on NEW research
than the others (its acceptance bar is mostly "the prior five tasks'
outputs, wired together and measured"), and instead focuses on what is
CURRENTLY MISSING that T12 must add, plus the cross-ticket assembly risk.

**Binding sources:** Task 5's sample-API-gap-audit rule (plan:256-284,
"samples are pure consumers of engine facilities... any facility a Phase
5 sample needs that the engine lacks is an API gap") binds this ticket
directly — the ticket's own acceptance line ("Viewer consumes only
engine APIs, audit row per the Task 5 rule") makes this explicit; the
Phase-4 stage-checkpoint precedent (`phase4-scene-assets.md:748`, "each
stage ends with a coordinator checkpoint: suite green both presets, stage
sample packaged and run standalone, numbers recorded in ledger, board
cards moved") is the direct template T12 must follow, not invent fresh.

**Ticket body (`gh issue view 48`):** `08_gltf_viewer` becomes the
Stage-1 demonstrator using engine facilities only — environment switching
(`--env`), Task-4 exposure controls, HUD environment/exposure readout;
packaging/CI updated; stage checkpoint (suite green both presets + real
driver, sample packaged + standalone-verified, numbers in ledger: bake
timings, frame times helmet/sponza/workshop, driver-labeled).

**Sources consulted (in-repo, read in full this session):**
`samples/08_gltf_viewer/main.cpp` (grepped for `--env`/`--exposure`/
`--scene` — confirms `--exposure` and `--scene` exist today, `--env`
does NOT), `tools/package_samples.sh` (:56-63, :277-351 — the FULL
per-sample manifest for `08_gltf_viewer`: `material_shaders/`
(material.slang/forward_entry.slang/standard_pbr.slang/unlit.slang),
tonemap shaders, `references/` (`loading_state.png`, `loaded_scene.png`),
pre-staged `assets/DamagedHelmet/glTF/` + its two license files — **no
HDR environment asset, no `--env`-related staging, no third reference
image anywhere in this manifest**), `MANUAL_VERIFICATION.md` (:117-150,
the Steam Deck section — *"Last run: not yet performed on real Steam
Deck hardware... Fill in the first time this is actually run on a Deck,
before the next release that claims Steam Deck support"* — the exact
"tracked honestly, never silently assumed" pattern the plan's own Global
Constraints require), `.github/workflows/ci.yml` (existence confirmed,
not read in full this session — see Verification health).

---

## The matrix

| Requirement | Current state | Disposition | Proposed acceptance criterion |
|---|---|---|---|
| `--env <path.hdr>` CLI flag | Absent (grep-confirmed against `main.cpp`'s own arg-parsing block, :175-183, which handles `--scene`/`--exposure` only). | consume-now, straightforward once Task 10's `Scene::setEnvironment()` API exists | Mirrors the existing `--scene`/`--exposure` arg-parsing pattern exactly (same file, same block) — no new parsing idiom needed, a pure extension of an established pattern. |
| Task-4 exposure controls (beyond the existing `--exposure` flag) | `--exposure` already exists as a flat `2^exposure` CLI override (Phase 4, unaffected by this ticket per se) — Task 4's OWN work (EV100/aperture-shutter-ISO physical model, Stage 0, outside this dispatch's scope) is what T12 must SURFACE here, not build. | consume-now, CROSS-TICKET (Task 4 → T12) | Whatever new exposure controls Task 4 adds (physical aperture/shutter/ISO inputs, or a direct `setExposure` override) get CLI/HUD surfacing in this sample — the acceptance bar is "the viewer exposes Task 4's real API," not "the viewer reinvents exposure logic," matching Task 5's own "samples are pure consumers" rule cited above. |
| HUD environment/exposure readout | `rx_debug_ui` exists (Phase 4, Stage 2 per the Phase-4 plan's own stage-exit text — not independently re-read this session, cited by name only) as the established HUD facility other samples (09_scene) already consume. | consume-now | New readout rows added to the EXISTING `rx_debug_ui` facility, not a sample-local HUD hand-roll — direct application of Task 5's "no sample hand-rolls what the engine provides" rule to this ticket's own new UI surface, the same rule the ticket's own acceptance line names explicitly. |
| Packaging (`tools/package_samples.sh`) | Sample 08's current manifest (cited above) has ZERO environment-asset staging and only 2 committed reference images. | **Real gap — the packaging script needs new lines, not automatic inheritance** | `package_samples.sh`'s `08_gltf_viewer` block gains: (a) a committed small HDR environment fixture (staged the SAME way `assets/DamagedHelmet/` is today — a real, license-recorded asset, per Task 6's own committed-tiny-HDR-fixture acceptance line, Stage 0), (b) a third (or more) reference image for the new `--env`-driven render state (matching the existing `loading_state.png`/`loaded_scene.png` two-reference pattern), (c) if Task 9's DFG LUT or a pre-baked SH/prefiltered-cubemap cache is EVER persisted to disk rather than purely regenerated at load time (the plan's own Task 9 text says runtime generation stands for Phase 5 — no persistent cache — so this sub-item is likely N/A, flagged only in case the coordinator rules otherwise). |
| CI update | `.github/workflows/ci.yml` exists (not read in full this session). | consume-now | New env-path GPU tests (Task 10's own skybox/mirror-metal/furnace gates) must run in CI exactly like every existing headless gate — no new CI mechanism implied unless Task 9's compute-bake timing publication (see row below) needs a NEW CI step (e.g. a perf-regression gate specifically for bake time, per CLAUDE.md's "CI carries performance regression gates... from Phase 4 onward" — Phase 5 IS "onward," so this applies here for the first time in this plan, not a Phase-4 carryover). |
| Bake timings published | Task 9's own acceptance line: "bake timings measured and published" (plan:393-395) — a Task 9 obligation, but the NUMBERS land in the Stage-1 ledger, which is T12's own deliverable per its acceptance sketch ("numbers in ledger: bake timings, frame times..."). | consume-now, CROSS-TICKET (Task 9 → T12) | T12 does not RE-MEASURE bake timings (Task 9 already did) — it aggregates Task 9's already-published numbers into the Stage-1 checkpoint ledger entry, driver-labeled per the plan's own standing "real-GPU verification" constraint (lavapipe AND real-driver rows, never lavapipe-only). |
| Frame times: helmet/sponza/workshop | DamagedHelmet is already the sample's default `--scene` asset (pre-staged in `package_samples.sh`, cited above). Sponza is fetchable via `tools/fetch_assets.sh --sponza` (Phase 4 precedent, confirmed present). **"Workshop" is not independently identified this session** — likely a Phase-4-era named test asset (not re-derived here; flagged in Verification health). | consume-now for helmet/sponza (existing assets); Workshop needs identification | Frame-time measurement under the FULL Stage-1 pipeline (IBL + skybox + Task-7/8 BRDF, not the Phase-4 flat-ambient path) for all three named scenes, both drivers, matching the plan's own "content-scale testing" constraint's spirit (real content beyond the committed synthetic fixture). |
| "Viewer consumes only engine APIs" audit | Task 5 (Stage 0, outside this dispatch's scope) is where the INHERITED backlog of sample hand-rolls gets closed — this ticket's own acceptance line applies that SAME discipline to whatever NEW facilities T7-T11 require the viewer to touch (environment binding, exposure controls, HUD rows — all three rows above). | consume-now | A short audit table (Task 5's own established output format — "audit table enumerates every sample-side hand-roll with promote/rule disposition," plan:277-278) scoped to JUST this ticket's new surface area (env/exposure/HUD), not a re-audit of the whole sample — zero undispositioned rows, matching Task 5's own zero-tolerance bar. |

## Open Questions

- **What is "Workshop"?** Not identified this session — no file/asset
  named "Workshop" was located in a quick pass over `tools/fetch_assets.sh`
  or the sample-08 manifest (both read in full for other reasons this
  session; a dedicated `grep -ri workshop` across `docs/`/`tools/` was
  NOT run as part of this matrix's own research budget). The plan text
  itself uses "Workshop" as a named benchmark scene in multiple places
  (e.g. Global Constraints: "Content-scale testing... Sponza, Workshop,
  Bistro exercises paths synthetic fixtures miss," plan:85-87) without
  defining it locally in Stage 1 — RECOMMEND the coordinator confirm
  whether "Workshop" is a Phase-4-era asset already fetchable (most
  likely, given the phrasing parallels Sponza's own already-fetchable
  status) or a new asset this ticket must additionally source; this
  matrix does not have enough evidence to rule either way and flags it
  rather than guessing.
- **Does T12 need a NEW CI perf-regression-gate mechanism, or does one
  already exist from Phase 4 to extend?** Not resolved this session
  (`.github/workflows/ci.yml` was not read in full) — CLAUDE.md's own
  performance-first policy states perf regression gates apply "from
  Phase 4 onward," meaning Phase 4 itself should already have SOME gate
  mechanism (sample 07_stress's own "counter gate + published parallel-
  vs-single numbers," cited in the Phase-4 spec's Stage-0-exit text) —
  RECOMMEND T12 extends that EXISTING mechanism to Stage 1's new
  bake-timing/frame-time numbers rather than building a second, parallel
  perf-gate mechanism; not independently confirmed this session whether
  the existing mechanism is generic enough to extend without its own
  small rework.

## New gaps

- None beyond what is already captured as matrix rows above — this
  ticket's own scope (aggregation + a small, well-precedented set of new
  CLI/HUD/packaging additions) does not surface new architectural gaps
  the way T7-T11 did; its risks are almost entirely CROSS-TICKET
  (depends on T7-T11 and Task 4 landing correctly first) rather than
  novel to itself.

## Verification health

- `main.cpp`'s CLI-flag grep and `package_samples.sh`'s full manifest
  for `08_gltf_viewer` are direct, first-hand reads of the current
  working tree this session.
- `MANUAL_VERIFICATION.md`'s Steam Deck section is a direct, full read
  of the relevant lines this session, not inherited from a prior gate
  matrix's citation of the same file.
- `.github/workflows/ci.yml` was confirmed to EXIST (a `find` hit) but
  NOT read in full this session — the CI-related acceptance-criterion
  rows above are therefore framed as "what must be true," not verified
  against the actual current CI configuration; a real follow-up before
  implementation, budget-permitting given this ticket's own light
  research weight relative to T7/T9/T11.
- `rx_debug_ui`'s existence and sample-09 consumption is cited BY NAME
  from general session context (the plan's own Stage-2 text, read
  earlier for broader Stage 0/1/2 orientation) — not independently
  re-verified against the actual `rx_debug_ui` header/sample-09 source
  this session; a lighter-weight citation than this matrix's other rows,
  flagged honestly rather than presented as freshly confirmed.
- "Workshop" identification is an explicit, named gap (Open Questions
  above), not silently assumed.
