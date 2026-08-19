# Task 17 brief — Window edge-state hardening (FG7, card #25)

You are the implementer for Phase 4 Stage 1 Task 17 of RendererX
(Vulkan 1.3 renderer middleware, C++20, repo `/media/ywadi/second/renderer_x`,
main checkout — base commit recorded at dispatch, tree clean except SDD
workspace files which are not yours). Last implementation task of
Stage 1.

## Requirements — read IN THIS ORDER; they are your spec

1. Plan task body: `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md`
   — section `### Task 17:` INCLUDING its "Gate hardening" block
   (BINDING — note the DESIGN CORRECTION: the suspended-present guard
   is EXTENT-QUERY-DRIVEN, not event-driven; events are an
   optimization/log layer only).
2. Completeness matrix (acceptance criteria row by row, incl. the
   Wayland findings and the two-tier test design):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue25-window-hardening.md`.
3. Coordinator rulings (+Errata):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/rulings-2026-08-18.md`
   — section `**#25 window hardening (Task 17).**`.
4. Ticket body: `gh issue view 25` (GATE HARDENED block).

Order of authority: rulings (+errata) > spec > matrix > ticket.

## Scope summary (details in the matrix — this is a map)

- Zero-extent/minimize guard in the swapchain path: extent-query-driven
  suspended-present state (skip-acquire-entirely while width==0 ||
  height==0 per the queried surface caps — the Khronos-tutorial
  pattern); dependency-injection seam so the guard's logic is testable
  without a display that reports zero; events
  (MINIMIZED/RESTORED/PIXEL_SIZE_CHANGED) surfaced as optimization +
  logging, never the sole gate; one-shot Wayland-limitation INFO log
  (minimize undetectable via SDL events/flags there — matrix row 3's
  verified finding).
- Borderless-fullscreen toggle: `SDL_SetWindowFullscreen(w, true)` with
  NULL fullscreen mode (borderless-desktop; verify
  `SDL_GetWindowFullscreenMode()==nullptr`), `SDL_SyncWindow()` before
  any readback (documented async), routed through the SAME
  `SwapchainStatus::NeedsRecreate` → `recreateSwapchain` path as
  resize (exactly one recreation call site — device-free test);
  double-toggle GPU test with cumulative zero validation errors;
  `--fullscreen` flag on samples that already carry present-mode flags.
- Present-skip while suspended asserted by CALL COUNTS (0 acquires/
  presents over N suspended frames — counter, never wall-clock);
  uploads NOT gated on suspension (D25 polling continues — one-line
  doc + a CHECK that an Uploader flush during suspension neither
  blocks nor errors).
- Two-tier tests: (1) device-free SDL_PushEvent state-machine tests
  (synthetic MINIMIZED/RESTORED drained via pumpEvents; suspended flag
  flips; size only ever read from PIXEL_SIZE_CHANGED data1/data2);
  (2) GPU test driving the seam-injected zero-extent guard
  (given {0,0} → skip vkb build entirely, distinguishable status, no
  swapchain object touched, resume on restore, zero validation
  errors). The true OS-minimize path is MANUAL_VERIFICATION-only —
  add the rows per the matrix's row-16 shapes (minimize + fullscreen
  toggle, unchecked, honest "not yet performed" blocks).
- Hidden-window CI empirical check (matrix row 8): one debug log line
  of the queried extent inside recreateSwapchain on your first CI-
  bound commit... you don't push; instead run the existing
  frame_sync/device tests locally and record the observed hidden-
  window extents in your report; add the row-8 regression CHECK
  (suspended flag false throughout a normal 64x64 hidden-window run).
- Existing resize-chain tests pass UNMODIFIED (+ the row-8 CHECK
  added).

## Landed context

- Task 16 just landed (check `git log` at dispatch): sample 08 exists
  with present-mode flags — it receives `--fullscreen` too. Harness
  gate: GPU binaries fail on teardown-time validation errors. Standing
  lesson: abandon/teardown paths get real-resource tests (a
  suspend-during-in-flight-upload case is worth one test).
- D5 conventions: thread-affinity one-liners on new public surface;
  RX_ASSERT_MAIN_THREAD guards per the established pattern.

## Global constraints (binding)

- **NO AI attribution** in commits; author = local git config;
  conventional messages; commit locally; do NOT push; do NOT touch
  board/issues/plan/spec/ledger; only your own files.
- Production grade; TDD; suite green BOTH presets (serial linux ctest,
  windows-cross build, Wine per the expanded convention); zero
  unfiltered validation errors incl. teardown-time; per-directory
  style.

## Report contract

Full report →
`.superpowers/sdd/2026-08-11-phase4-scene-assets/task-17-report.md`
(per-criterion proof, command output tails, revert evidence for the
load-bearing guard tests, deviations, self-review). FINAL MESSAGE:
ONLY status, commit SHAs, one-line test summary, concerns.
