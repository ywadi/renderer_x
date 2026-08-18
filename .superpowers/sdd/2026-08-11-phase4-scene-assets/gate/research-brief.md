# PRIMARY GATE research brief — shared method (read this first)

**Project:** RendererX — a Vulkan 1.3 renderer middleware DLL (C++20),
desktop + Steam Deck as the hardware floor. Repo:
`/media/ywadi/second/renderer_x`. Phase 4 (Scene & Assets) Stage 0 is
complete; Stages 1–2 are specified but not dispatched. This gate deepens
every open Phase 4 ticket into a production-grade specification BEFORE any
implementation dispatches.

**Your mission:** for each ticket assigned to you, produce a **completeness
matrix** measuring the ticket against what first-tier renderers actually
require — Filament, bgfx, Godot, the glTF 2.0 specification in full,
Unreal/Unity feature expectations, and the relevant Khronos/Vulkan
specs/best practices. The matrices become the coordinator's raw material
for rewriting ticket acceptance criteria; they must be exhaustive and
citation-grounded.

## Matrix format (one file per ticket)

Write `gate/matrix-issueNN-<slug>.md` (this directory). Structure:

1. **Header:** ticket number/title, plan task number, spec decisions that
   bind it (D-numbers), sources consulted with versions/dates.
2. **The matrix** — one row per required feature/behavior/edge case:
   | Feature | First-tier precedent (named, cited) | Phase-4 disposition | Library support (verified, cited) | Proposed acceptance criterion |
   - **Disposition** is exactly one of: `consume-now` (Phase 4 implements),
     `preserve-later` (import/store now, consume in a later phase),
     `log-don't-drop` (detected + logged, never silently ignored),
     `N/A-Phase-4` (genuinely out of scope — justify with retrofit
     economics: does deferring force expensive changes at existing call
     sites later? If yes it is NOT N/A).
   - **Library support** must be VERIFIED, not assumed: fetch the library's
     docs/repo at a named version/tag, or cite vendored source
     `file:line`. If you could not verify, say `UNVERIFIED` and why —
     never present an assumption as fact.
   - **Acceptance criterion** must be concrete and testable (what test
     proves it; exact behaviors, not "handles X properly").
3. **Conflicts:** any row where your findings contradict the current
   plan/spec/issue text — quote both sides. Do not resolve; the
   coordinator adjudicates.
4. **New gaps:** capabilities missing from the ENTIRE planning universe
   (check the master registry deferred list in
   `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`,
   the feature-gap audit FG1–FG12, and the phase spec before claiming
   novelty). Propose a phase fit. Do NOT edit the registry yourself.
5. **Verification health:** what you verified first-hand vs. inferred;
   dead links; version ambiguities.

## Required in-repo reading (scope to YOUR tickets only)

- Your ticket bodies: `gh issue view <N>` (amendment sections at the
  bottom are binding).
- Your plan task sections in
  `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md` (+ its
  Global Constraints section). Do not read other agents' task sections.
- The spec decisions your tasks cite:
  `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`.
- Fact sources: `.superpowers/sdd/2026-08-11-phase4-scene-assets/`
  `research-p4-{assets,threading,scene,present}.md`,
  `feature-gap-audit.md`, `claim-validation-2026-08-18.md` (already-ruled
  items — do not re-litigate rulings; DO deepen them into criteria).
- Delivered code where a claim needs grounding (cite `file:line`).

## Binding rules

- The recently added invariants **D24 (memory budget/eviction invariant),
  D25 (UploadTicket), D26 (GPU-driven readiness), D27 (main-thread
  pre-resolution)** must appear as acceptance-criterion rows on every
  ticket they touch — the gate enforces them.
- Project policy binds your recommendations: performance is an exit
  criterion (fast-path-as-default; measured claims only); prefer
  ready-made libraries over from-scratch; features above the Vulkan 1.3
  baseline are optional-with-fallback; Steam Deck is the floor.
- The glTF-import depth rule (worked example for all tickets): a renderer
  need not RENDER every feature in Phase 4, but must (a) decode whatever
  is needed to open real files at all, (b) preserve what later phases
  consume, (c) log — never silently drop — everything else.
- **Write access:** ONLY your matrix files in `gate/`. No git commits, no
  pushes, no edits to issues/plan/spec/registry/board, no other file
  writes in the repo. These files will be committed to a public
  repository: factual, professional tone; no AI attribution or
  self-reference of any kind.
- No TBD/placeholder cells. If something is genuinely unknowable from
  available sources, say exactly what is missing and what would resolve it.

## Final message contract (keep it compact — files carry the detail)

Per ticket: 3–6 bullets of load-bearing findings. Then: new gaps found
(one-liners), conflicts found (one-liners), verification-health note.
