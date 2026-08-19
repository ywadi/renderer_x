### Task 24: Sample 09_scene + stress-v2 + release prep

**Files:** Create `samples/09_scene/` — loads DamagedHelmet field (headless: instanced helmets grid; present: `--scene sponza` when fetched) through Registry→Scene→DrawListBuilder→graph; fly-through camera (WASD + mouse capture + gamepad via Task 20's input surface — gate correction 2026-08-18: the earlier "D16 input" citation was a mislabel, D16 is test content); ImGui HUD: FPS/frame-ms, cull counters, vsync toggle, layer-mask toggles (hide/show instance groups), light-channel demo toggle, pool stats; stress-v2 mode `--stress` (same 30k-draw workload as sample 07 but through the full scene path — publishes A/B numbers vs 07 in the report + release notes); headless gate: counter assertions + tolerance pixels; MANUAL_VERIFICATION rows; packaging/CI; README/roadmap updates; `docs/superpowers/specs` layer table tick for layer 8.
**Steps:** TDD gate → implement → numbers (desktop; Deck rows added to MANUAL_VERIFICATION as unchecked) → packaging → commit(s).
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue15-sample09.md` (incl. the full
subsystem-exercise checklist, rows 3-17) as amended by
`gate/rulings-2026-08-18.md` §#15. Key deltas: **A/B comparability
contract** — held identical vs sample 07: 30k instances, the 4
pipeline/material variations, per-instance data, --threads/--vsync
semantics; expected to differ: draws SUBMITTED (07 submits all 30k
unconditionally; 09 culls+collapses) — published numbers report
wall-clock AND draws-submitted JOINTLY (wall-clock alone would
misrepresent the comparison; sample 07's own report format stays
untouched); collapse-ratio formula BLESSED: `1 −
drawsSubmitted/recordsIn` as a percentage, CI-asserted against the
hand-computed grid expectation; HUD ships TWO visibly distinct mask
controls (camera cullMask u32 ≠ light channels u8 — never conflated);
vsync toggle drives the same setPresentMode+recreate path as the CLI
flag; 60-frame rolling FPS average; headless gate asserts EXACT
counters (imported/visible/culled/recordsIn/drawsSubmitted) + a D17
tolerance reference via Task 16's regen script (no second mechanism);
packaged samples ship PRE-STAGED assets (fetched at package time —
must run standalone); `package_samples.sh` list + stale header count
fixed to include 08 AND 09, and the missing 07_stress
MANUAL_VERIFICATION section is added while touching that file;
registry layer-8 row annotated "(delivered: Phase 4 — scene
submission/culling; LOD remains deferred)" — the qualified form;
Task 23 lands before any stress-v2 number is published.

---

## Execution notes (coordinator)

- Models: Stage-0 Task 8 and Task 20 (input) Haiku (mechanical, fully specified); all others Sonnet; reviews all Sonnet; final whole-phase review at phase end.
- Sequencing: Stage 0: T1→T2→T3 sequential (T3 zones touch files broadly); T4/T5/T6 parallelizable in worktrees after T3 (disjoint); T7 after T2+T6; T8 anytime after T4 (Haiku, disjoint files). Stage 1: T9(gate)→T10→T11→T12→T13→T14→T15→T16, with T17 (window hardening) parallelizable any time after the gate (disjoint platform/present files; worktree). T10 before T11 (both touch allocator/device creation). Stage 2: T18→T19→{T20,T21 parallel}→T22→T24; T23 (executor cleanup) parallelizable with T20–T22 (rx_graph-only, worktree) but MUST land before T24's stress-v2 numbers.
- **Task renumbering (2026-08-18):** the primary-gate insertion left two "Task 9" headings and stale references; tasks are now uniquely numbered. Mapping for older ledger/report references — old T9 (GeometryPool)→T12, old T10 (import core/registry)→T13, old T11 (KTX2)→T14, old T12 (async import)→T15, old T13 (StandardPBR/viewer)→T16, old T14 (scene proxies)→T18, old T15 (draw lists)→T19, old T16 (input)→T20, old T17 (ImGui)→T21, old T18 (shadows)→T22, old T19 (sample 09)→T24. New tasks: T10 (memory budget, card #27), T11 (upload tickets, card #28), T17 (window hardening, card #25), T23 (executor allocation elimination, card #29).
- Each stage ends with a coordinator checkpoint: suite green both presets, stage sample packaged and run standalone, numbers recorded in ledger, board cards moved, then next stage dispatches.
- Phase exit: final whole-phase review (fresh, most scrutiny on cross-stage seams: registry↔scene handle lifetimes, parallel recording under real scene loads, threading contract adherence), one fix wave, push, CI green, tag v0.4.0-phase4, release with both packages + published numbers, board cards closed.
