# Task 11 brief — Uploader completion tickets: non-blocking flush invariant (card #28)

You are the implementer for Phase 4 Stage 1 Task 11 of RendererX
(Vulkan 1.3 renderer middleware, C++20, repo `/media/ywadi/second/renderer_x`,
work on the main checkout — base commit `2e1b55d`, tree clean).

## Requirements — read IN THIS ORDER; they are your spec

1. Plan task body: `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md`
   — section `### Task 11:` INCLUDING its "Gate hardening" block (BINDING).
2. Spec decision D25 AS AMENDED (gate ruling RC4 — **the ticket
   primitive is a TIMELINE SEMAPHORE**, not a fence) + D5:
   `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`.
3. Completeness matrix (your acceptance criteria, row by row):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue28-upload-tickets.md`.
4. Coordinator rulings (they amend the matrix and win on conflict):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/rulings-2026-08-18.md`
   — sections `RC4` and `**#28 upload tickets (Task 11).**`.
5. Ticket body: `gh issue view 28` (the GATE HARDENED block at the end).

Order of authority on conflict: rulings > spec > matrix > ticket.
Note: Task 10 just landed (commits 9bd5be2/f210e73) — `Allocator` now
carries category accounting and `FrameSync::advanceFrame(Allocator*)`
exists; build on current HEAD state, don't fight it. Staging-ring
bytes belong in the "staging" accounting category if you touch their
allocation path.

## Scope summary (details live in the sources above)

- `UploadTicket = {uint64 value, uint32 ringGeneration}` backed by ONE
  Uploader-owned timeline semaphore; every work-submitting `flush()`
  signals the next monotonic value; `isComplete(ticket)` = one
  `vkGetSemaphoreCounterValue` compare (pure poll, error → loud log,
  never silently treated as a state); `wait(ticket)` =
  `vkWaitSemaphores`; wait-on-completed returns immediately.
- **Highest-priority correctness gate:** two overlapping `flush()`
  calls without waiting on the first — the first ticket still reports
  its own true status, zero validation errors (this is the
  reused-fence hazard the redesign exists to kill).
- Direct-path-only batches (ReBAR/UMA, no GPU work recorded) return an
  ALREADY-COMPLETE ticket on the same call stack (no semaphore
  machinery touched) — matches today's no-op flush; test both this and
  the image/staged path (a texture-only batch's ticket must be
  incomplete until the GPU copy really finishes). Mixed batches: the
  ticket covers recorded work only (readback test on both
  destinations).
- Ring reclamation: (ticket, ring-offset) queue per the D3D12 pattern
  — on wrap, poll-reclaim all completed entries first; block ONLY on
  the oldest in-flight ticket covering the needed range; fixed-size
  ring (never silently grown); stress test proves partial reclaim
  engages (blocking-wait counter < flush count) and data correctness
  under heavy wrap (readback, not just no-crash).
- `MeshBuffers::create` keeps blocking semantics via `wait(ticket)` —
  existing tests pass UNMODIFIED.
- Sample 04 call sites migrated (lines ~919, 1281, 1580 cited; audit
  the file for others — the cited list is not claimed exhaustive); the
  eviction-cycle's descriptor rewrite becomes an explicit
  poll-or-wait choice, not an accidental block.
- `Device::create` acquires an OPTIONAL dedicated transfer queue via
  vk-bootstrap `get_dedicated_queue(QueueType::transfer)` (NEVER
  `require_dedicated_transfer_queue`) — graphics fallback, one logged
  degrade, `hasDedicatedTransferQueue()`/`transferQueue()` accessors;
  NOTHING submits to it this phase (code-review criterion).
- Wall-clock discrimination test: timer around every
  `flush()`/`isComplete()` call across N overlapped frames, asserting
  no call blocks past threshold — the test MUST FAIL against the old
  blocking `flush()` (prove it: scratch-worktree revert evidence in
  the report, same standard Task 10's fix round set).
- API stays main-thread-only (D5 unchanged — guards + header
  one-liners per the existing convention, incl. on any header gaining
  new public API).

## Global constraints (binding, from the plan)

- **NO AI attribution of any kind** in commits; author stays local git
  config. Conventional factual commit messages. Commit locally; do NOT
  push; do NOT touch board/issues/plan/spec/ledger.
- Production grade; TDD; suite green on BOTH presets (`linux-native`,
  `windows-cross-zig` — CMakePresets.json / ci.yml for commands); zero
  unfiltered validation errors with sync validation; match
  per-directory style.

## Report contract

Full report →
`.superpowers/sdd/2026-08-11-phase4-scene-assets/task-11-report.md`
(per-criterion proof, command output tails, the mandatory
revert-discrimination evidence, deviations, self-review). FINAL
MESSAGE: ONLY status (DONE / DONE_WITH_CONCERNS / NEEDS_CONTEXT /
BLOCKED), commit SHAs, one-line test summary, concerns.
