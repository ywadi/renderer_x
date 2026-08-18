# Task 10 brief — Memory budget, accounting & eviction-invariant foundation (card #27)

You are the implementer for Phase 4 Stage 1 Task 10 of RendererX
(Vulkan 1.3 renderer middleware, C++20, repo `/media/ywadi/second/renderer_x`,
work on the main checkout — base commit `bc879c9`, tree clean).

## Requirements — read these IN THIS ORDER; they are your spec

1. Plan task body: `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md`
   — section `### Task 10:` INCLUDING its "Gate hardening" block (BINDING).
2. Spec decision D24 (+ D5 threading contract):
   `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`.
3. Completeness matrix (your acceptance criteria, row by row):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue27-memory-budget.md`.
4. Coordinator rulings that amend the matrix (they win on conflict):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/rulings-2026-08-18.md`
   — section `**#27 memory budget (Task 10).**` (+ the cross-cutting RC
   rulings it references).
5. Ticket body: `gh issue view 27` (the GATE HARDENED block at the end).

The matrix rows' "Proposed acceptance criterion" column, as amended by
the rulings, is the acceptance bar — implement ALL of it, no partial
work. If any two sources conflict: rulings > spec > matrix > ticket.

## Scope summary (details live in the sources above)

- Category-attributed GPU memory accounting (rx_rhi_vk-side ledger at
  the 5 existing allocation choke points — VMA has no category system).
- `VK_EXT_memory_budget` opportunistic enablement (device ext +
  `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT`, kept in LOCKSTEP — VMA
  asserts otherwise) + heap-size fallback path, both tested.
- `vmaSetCurrentFrameIndex` wired once per frame + staleness regression
  test (the budget query only refreshes on frame-index bump or every 30
  alloc/free ops — never called anywhere today).
- Host-facing POD `RxMemoryReport`: per-category bytes/counts, PER-HEAP
  usage/budget (never assume heap count — RADV APUs expose several),
  explicit budget-source flag.
- `VK_ERROR_OUT_OF_DEVICE_MEMORY` handling at all 5 sites (loud, named,
  report-attached; image-create vs image-view-create failure classes
  distinguishable).
- Eviction CONTRACT text (documented rules Tasks 13/19 implement) + the
  minimal synthetic evict→fallback→reclaim wiring test over the
  existing `DeletionQueue`.
- Teardown leak/balance report; Tracy plots per frame.

## Global constraints (binding, from the plan)

- **NO AI attribution of any kind** in commits (no Co-Authored-By, no
  generated-with, nothing) — commit author stays the local git config.
- Production grade; TDD (failing test first where testable); suite
  green on BOTH presets (`linux-native`, `windows-cross-zig` — see
  `CMakePresets.json` / `.github/workflows/ci.yml` for the canonical
  build+test commands); zero validation errors with sync validation
  active in GPU tests; match per-directory code style; every new
  public header carries a one-line thread-affinity note (D5 pattern —
  see `src/rx_rhi_vk/include/rx_rhi_vk/upload.h:89-93` for the shape).
- Commit message style: `feat:`/`chore:` conventional, factual. Commit
  locally; do NOT push; do NOT touch the board, issues, plan, spec, or
  ledger files.

## Report contract

Write your full report to
`.superpowers/sdd/2026-08-11-phase4-scene-assets/task-10-report.md`:
what you built (per matrix row: how satisfied + which test proves it),
test evidence (commands + pasted output tails), deviations with
rationale, self-review findings. Your FINAL MESSAGE back must contain
ONLY: status (DONE / DONE_WITH_CONCERNS / NEEDS_CONTEXT / BLOCKED),
commit SHAs, one-line test summary, and concerns if any. Nothing else —
the report file carries the detail.
