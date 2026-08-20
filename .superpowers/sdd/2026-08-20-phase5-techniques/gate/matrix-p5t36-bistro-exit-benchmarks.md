# Completeness matrix — P5 T36 (issue #72): Sample 12_bistro + phase benchmarks + release v0.5.0-phase5 (EXIT)

**Plan task:** Task 36, Stage 4 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:947-976`).
**Charter/CLAUDE.md binding:** *"Performance is an exit criterion"*
(CLAUDE.md) — *"From Phase 4 onward, every phase exits with published
benchmark numbers (desktop AND Steam Deck — the hardware floor)... and CI
carries performance regression gates on those numbers alongside the
correctness gates. A performance regression blocks a phase exit the same
way a failing test does."* Plan's own exit ladder text, `plan:1000-1004`.

**Sources consulted (in-repo, 2026-08-20) — auditing what Phase 4 ACTUALLY
delivered against this mandate, since Phase 5 is the phase this mandate
names as binding "from Phase 4 onward":**
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/phase4-exit-review.md`
  (402 lines, EXIT-READY section and its surrounding context read in
  full) — **no mention anywhere of published CI performance-regression
  gates or Deck/desktop `--bench` numbers as a phase-exit deliverable.**
  The exit review's own content is entirely correctness-focused (C1/I1-I4/
  M1-M5 findings, all correctness/threading/lifecycle bugs) — zero
  performance-regression-gate or benchmark-CSV content anywhere in the
  document.
- `.github/workflows/*.yml` (16 workflow files enumerated) — none named
  or shaped around a performance/benchmark job; `ci.yml` is the
  correctness suite (lavapipe + real-driver builds/tests) only.
- `MANUAL_VERIFICATION.md` — the existing Steam Deck posture, throughout:
  *"Last run: not yet performed on real Steam Deck hardware — this claim
  is honest until a human runs it"* (the recurring pattern at every Deck
  subsection, e.g. lines 141,642). **No Phase 4 sample carries a published
  Deck benchmark number today** — the file's own convention is an HONEST,
  UNFILLED checklist, not a filled-in number.
- Repo-wide grep: zero hits for `--bench` in any `samples/` directory.
  `samples/07_stress`'s A/B single-thread-vs-default-worker-count
  comparison and `samples/09_scene`'s `--stress` flag (both named in
  `README.md:86,88`) are the closest EXISTING precedent for a
  deterministic, scriptable, CSV-producing stress/benchmark mode — neither
  is wired to CI as a regression GATE (a pass/fail threshold check),
  confirmed by the same `ci.yml` audit above.
- **Conclusion, stated plainly because it changes how T36 should be
  scoped:** CLAUDE.md's mandate reads as already-binding since Phase 4,
  but Phase 4's OWN exit did not actually deliver CI perf-regression gates
  or published Deck numbers. T36 is therefore not "extend an established
  Phase 4 precedent" — it is **the first phase-exit in this project's
  history that must actually build this mechanism end-to-end**, with no
  working prior-phase implementation to copy. This is not a defect in
  Phase 4 to relitigate (out of this gate's scope) — it is a scoping fact
  T36's own estimate and acceptance criteria should reflect honestly.
- `src/rx_core/include/rx_core/profile.h` + `third_party/CMakeLists.txt:
  302-331` — Tracy v0.14.0, confirmed genuinely vendored and wired
  (`RX_ZONE`/`RX_ZONE_NAMED`/`RX_FRAME_MARK`/`RX_PLOT` macros, real
  client library, not a stub) — the profiling PRIMITIVE T36's CSV
  benchmark mode and CI perf gate both build on.
- `phase4-exit-review.md:310-320` (`EXIT-READY` verdict + "the phase may
  tag v0.4.0-phase4" flip statement) — the exact PROCESS precedent (exit
  review → fix wave → re-verdict → tag) T36's own exit ladder should
  mirror; this part of Phase 4's precedent IS directly reusable, unlike
  the benchmark-gate part.

---

## The matrix

| # | Feature | First-tier precedent (named, cited) | Phase-5 disposition | Library/port-source support (verified, cited) | Proposed acceptance criterion |
|---|---------|--------------------------------------|----------------------|-------------------------------------|-------------------------------|
| 1 | Deterministic `--bench` camera path emitting per-frame CSV | `samples/07_stress`'s A/B benchmark + `samples/09_scene`'s `--stress` flag (existing, but neither is a scripted CAMERA PATH producing a CSV — both are static/procedural scene stress tests, not fly-through benchmarks). | consume-now (new work, honestly scoped — see Sources conclusion) | N/A — this is new sample-side work; the underlying Tracy plumbing it rides is real (confirmed). | A fixed, versioned camera keyframe path (position/orientation over time, committed as data, not regenerated per run) driving both the day and night rigs; per-frame CSV rows (frame time, GPU zone breakdown via Tracy's own timing, at minimum) written to a file path the CI/Deck procedure can consume — determinism verified by a byte-identical-CSV-on-rerun test (same discipline as the existing `--threads`-invariant determinism tests elsewhere in this codebase, e.g. DrawListBuilder's own). |
| 2 | CI perf regression gates wired on the benchmark numbers (a regression blocks phase exit like a failing test) | N/A in this repo (confirmed absent, Sources) — the general PATTERN (a numeric threshold check in CI, comparing a fresh run against a committed baseline with tolerance) is standard practice, not something requiring a specific external precedent to justify; CLAUDE.md's own text is the binding requirement, not a borrowed design. | consume-now (new CI infrastructure) | N/A. | A new (or extended) CI job runs `12_bistro --bench` on the runner's own lavapipe/available driver, compares the resulting CSV's aggregate frame-time metric against a committed baseline file with an explicit tolerance band, and FAILS the job (not just warns) outside that band — mirroring the existing `RC6`-precedent two-tier pattern this codebase already uses elsewhere for other budget/detector splits (`.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/rulings-2026-08-18.md`'s RC6: a trend number that never blocks CI vs. a detector threshold that does) — i.e., the CI-runner number is a coarse, noisy-runner-tolerant DETECTOR for gross regressions, while the real desktop/Deck numbers (rows 3-4) are the trustworthy, human-run, precisely-labeled numbers actually published in the release. |
| 3 | Published desktop benchmark numbers, driver-labeled | Phase 4's own convention for driver-labeled numbers elsewhere in the ledger (e.g. `progress.md`'s own "lavapipe + NVIDIA" labeling pattern throughout Task reports) — a REAL, reusable labeling convention even though Phase 4 never applied it to a PUBLISHED perf number specifically. | consume-now | N/A. | `--bench` CSV rows for both day and night rigs, on both the dev machine's real NVIDIA GPU (`--validate` clean) and lavapipe (for CI-comparability), each row driver-labeled per the standing convention; published in the release notes/README, not only buried in a CSV file. |
| 4 | Published Steam Deck benchmark numbers — **release-BLOCKING, not an unchecked box** | Ticket's own explicit text: *"the Deck run is an owner-executed scripted procedure (one command) whose published rows BLOCK the release — not an unchecked box."* This is a DELIBERATE escalation past Phase 4's own honest-but-permissive MANUAL_VERIFICATION convention ("not yet performed... this claim is honest until a human runs it" as an ACCEPTED state at Phase 4 exit) — T36 does not get to leave this row unfilled at exit the way every Phase 4 Deck row currently is. | consume-now, with a sequencing consequence (Open Question below) | N/A. | A single scripted command (e.g. `./tools/run_deck_bench.sh` or equivalent — packaged sample + fixed args) the owner runs ONCE on real Deck hardware; its output rows are committed into the release artifacts BEFORE the release is cut — this is a hard gate in the Steps ordering (plan's own Task 36 Steps text already lists "Deck numbers" as the second-to-last step, before "tag + release", plan:975-976 — confirmed correctly sequenced, not a gap). |
| 5 | Exit review: whole-phase, top-tier model, cross-stage seams named explicitly | Phase 4's own exit-review PROCESS precedent (`phase4-exit-review.md`, cited above) — directly reusable: independent top-tier review, empirical re-verification of every finding (gdb breakpoints, revert-and-restore spot-proofs, re-run test suites), a FINAL VERDICT section gating the tag. | consume-now — process precedent transfers cleanly even though the benchmark-gate precedent (rows 1-4) does not | Verified: the Phase 4 process (independent review → fix wave → re-verdict with the REVIEWER'S OWN re-proof, not the implementer's say-so → tag) is real and worth copying verbatim in shape. | Ticket's own named seams stand (cluster↔volumetric grid sharing — ties directly to this gate round's T14/T30 cross-reference already recorded in `matrix-p5t30-froxel-fog.md`; scene-color chain↔transmission↔bloom ordering — ties to `matrix-p5t31-bloom.md`'s reuse-vs-mirror question; TAA↔SSR↔volumetric history interplay — ties to `matrix-p5t33-taa.md` row 8; threading-contract adherence in all compute passes — a fresh audit, not inherited from any single Stage-4 ticket). |
| 6 | One fix wave, all findings in-round | Phase 4's own "no deferred fixes" precedent, now also a standing plan-wide rule (`plan:102-104`). | consume-now | N/A — already-established discipline. | Ticket's own text stands. |
| 7 | Sample consumes engine facilities only (headless gate: counters + pixels + discrimination floor) | Task 5's own already-specified rule (`plan:256-284`, Stage 0, landed before Stage 4) — this ticket is a CONSUMER/enforcer of that rule, not its author. | consume-now | N/A. | Same audit-row discipline every other Stage-4 exit sample criterion already uses (T20/T28's own text, plan:621-627,781-786) — `12_bistro` gets the same treatment, no weaker. |
| 8 | Registry layer-9 row annotated with delivered-vs-deferred precision | Existing registry precedent (`spec:520-535`'s layer-10 tooling inventory is the closest structural analog — a precise "what's delivered vs. what's deferred" ledger entry, not a vague "mostly done"). | consume-now | N/A. | Ticket's own text stands; recommend explicitly cross-referencing every Stage-4 ticket's own Open Questions in this gate round (T29's from-scratch god-rays call, T30's Godot port-source call, T34's per-extension Filament-absence findings) so the registry annotation is traceable back to WHY each deferred item was deferred, not just THAT it was. |

---

## Conflicts

None against the plan/ticket text — the plan's own Task 36 Steps ordering
already correctly sequences Deck numbers as release-blocking before the
tag (row 4). The finding worth flagging prominently (not a text
contradiction, but a scoping-honesty issue) is the Sources section's own
conclusion: **CLAUDE.md's benchmark/CI-gate mandate has been binding
"from Phase 4 onward" in name since before this plan was written, but
Phase 4's own exit never actually built it.** T36 should not be estimated
or reviewed as "wire up the existing mechanism to a new sample" — there is
no existing mechanism. This is new infrastructure, on the phase's own
critical path, for the first time.

## New gaps

None beyond what rows 1-2 already name as new (in-scope) work.

## Open Questions (for the coordinator's binding ruling)

1. **Sequencing risk: T36 is both the FIRST phase to build the CI-perf-
   gate/Deck-benchmark mechanism AND the phase-exit ticket that mechanism
   gates.** Recommendation: **budget T36 as materially larger than a
   typical "new sample + package + tag" exit ticket** — it is that PLUS a
   novel CI job PLUS a novel Deck-side scripted procedure, none of which
   have a working prior-phase version to copy (Sources). If schedule
   pressure appears at Stage 4's close, the coordinator should treat
   "build the CI perf-gate mechanism correctly" as non-negotiable (it is
   the literal phase-exit criterion per CLAUDE.md, not a nice-to-have) and
   look for schedule slack elsewhere (e.g. Task 34's already-explicit
   stretch-tier deferrals) rather than shortcut rows 1-4 here.
2. **CI runner noise tolerance for the perf-regression gate (row 2).**
   Recommendation: follow the SAME two-tier shape this codebase already
   uses for its own wall-clock stall detectors (`.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/rulings-2026-08-18.md`'s
   RC6: a tight local/desktop number published as a trend, never CI-
   blocking on its own noisy variant; a coarser CI-runner threshold that
   only fires on genuinely large regressions, sized an order of magnitude
   above expected runner noise) — this precedent already exists in this
   codebase for exactly this class of problem (a shared-runner's timing
   noise vs. a real regression) and should be reused rather than
   re-derived from scratch at T36.
