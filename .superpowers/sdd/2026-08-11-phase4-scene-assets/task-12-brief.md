# Task 12 brief — GeometryPool: pooled global vertex/index storage (card #21)

You are the implementer for Phase 4 Stage 1 Task 12 of RendererX
(Vulkan 1.3 renderer middleware, C++20, repo `/media/ywadi/second/renderer_x`,
work on the main checkout — base commit `0d9ca39`, tree clean).

## Requirements — read IN THIS ORDER; they are your spec

1. Plan task body: `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md`
   — section `### Task 12:` INCLUDING its "Added acceptance criteria"
   and "Gate hardening" blocks (BINDING).
2. Spec decisions D8, D9, D26.4 (+ D5, D24, D25):
   `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`.
3. Completeness matrix (acceptance criteria, row by row):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue21-geometry-pool.md`.
4. Coordinator rulings (amend the matrix, win on conflict):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/rulings-2026-08-18.md`
   — section `**#21 GeometryPool (Task 12).**`.
5. Ticket body: `gh issue view 21` (amendment + GATE HARDENED blocks).

Order of authority: rulings > spec > matrix > ticket.

## Landed context you build on (don't fight it)

- Task 10 (9bd5be2/f210e73): `Allocator` carries category accounting
  (`MemoryCategory`), `RxMemoryReport`, OOM classification. Pool chunk
  buffers MUST be attributed to the geometry-pool category via the
  existing choke point (`Allocator::createDeviceLocalBuffer`) — thread
  the category through, don't add a new VMA call site.
- Task 11 (37d9466..77a3d82): `Uploader::flush()` returns
  `UploadTicket` (timeline semaphore); `isComplete()`/`wait()` exist;
  `MeshBuffers::create` shows the blocking-convenience pattern.
  `GeometryPool::upload()` consumes tickets per D25 (sync convenience
  may wait once per upload batch at a documented point — never
  per-suballocation).

## Scope summary (details in the sources)

- NEW library `src/rx_asset` (CMakeLists wired like existing rx_*
  targets, both presets): `GeometryPool` — one big vertex buffer + one
  big index buffer per block (DEVICE_LOCAL), suballocated via
  **VmaVirtualBlock** (VMA already vendored); 64MB/32MB default chunks,
  new block on exhaustion, virtual-free, NO defrag; 48-byte
  `PoolVertex` static_assert-pinned (D8); `MeshRange {blockId,
  firstIndex, indexCount, vertexOffset}`; `bind(cmd, blockId)`;
  `stats()` incl. bytes used/capacity AND block count; main-thread
  affinity (D5 guard + header one-liner).
- Per-allocation VMA alignment: 48 (vertex) / 4 (index) — VMA does not
  know element strides; assert divisibility of every returned offset.
- BDA enablement (D26.4): `bufferDeviceAddress` set OPPORTUNISTICALLY
  in features12 (NEVER a device-selection requirement; logged degrade),
  `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` when on, usage bit
  on pool block buffers, `Device::supportsBufferDeviceAddress()`;
  verify lavapipe support empirically IN-TASK (vulkaninfo/feature
  query — record the result) before any CI dependency; test asserts a
  pool block yields nonzero `vkGetBufferDeviceAddress` on a supporting
  device. Enablement ONLY — nothing consumes addresses.
- GPU tests (device-free impossible): two uploads → distinct
  non-overlapping ranges; free + re-upload reuses space (stats/offset
  assert); exhaustion → second block, BOTH drawable (real indexed
  draws from two blocks, readback probe); checkerboard fragmentation
  test (alloc N same-size, free every other, request larger-than-any-
  hole-but-smaller-than-total-free → must take the exhaustion/new-
  block path, proving no hidden compaction); alignment divisibility
  asserts after every upload; zero validation errors (sync validation).
- Integration test: after a chunk allocation, the #27 accounting
  report's geometry-pool category grows by exactly the chunk size.
- Discrimination standard (established in Tasks 10/11): where a test
  guards a specific behavior (alignment, no-defrag, category
  attribution), be ready to show it FAILS when that behavior is broken
  — the reviewer will revert-test empirically; include scratch-worktree
  evidence in the report for the load-bearing ones you judge most at
  risk of vacuousness.

## Global constraints (binding)

- **NO AI attribution of any kind** in commits; author stays local git
  config; conventional factual messages; commit locally; do NOT push;
  do NOT touch board/issues/plan/spec/ledger.
- Production grade; TDD; suite green on BOTH presets; zero unfiltered
  validation errors; per-directory style; new public headers carry the
  D5 thread-affinity one-liner.

## Report contract

Full report →
`.superpowers/sdd/2026-08-11-phase4-scene-assets/task-12-report.md`
(per-criterion proof, command output tails, lavapipe BDA verification
result, revert evidence where applicable, deviations, self-review).
FINAL MESSAGE: ONLY status, commit SHAs, one-line test summary,
concerns.
