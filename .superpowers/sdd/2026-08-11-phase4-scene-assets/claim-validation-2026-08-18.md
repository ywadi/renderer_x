# Capability-Gap Claim Validation — 2026-08-18

**Mandate:** user supplied 10 capability-gap claims (compute pipeline + a
9-row "critical missing pieces" table). Validation agent (Opus-model)
verified each on three axes — code reality (file:line), planning status
(doc citations), disposition — read-only at HEAD `bf5b853`. Coordinator
independently spot-checked the four most load-bearing code claims
(upload.cpp fence wait, features12 contents, kMaterialStageFlags,
compute-dispatch absence) before acting; all confirmed byte-for-byte.
Rulings and resulting spec/plan/registry/board changes are recorded in
`progress.md` (2026-08-18 entry). This file preserves the evidence.

## Verdict table

| # | Claim | Code reality | Planning status | Verdict | Disposition |
|---|---|---|---|---|---|
| 0 | No compute pipeline (esp. GPU particles) | Confirmed absent — zero `vkCreateComputePipelines`/`vkCmdDispatch`/`VK_PIPELINE_BIND_POINT_COMPUTE` in repo | Compute *passes* declared+tested (barriers only); compute *PSOs* nowhere; GPU particles unregistered | TRUE | Compute PSO: TRUE GAP, LATER PHASE (geometry, registered). GPU particles: TRUE GAP — registered (techniques) |
| 1 | Async GPU uploads ("can stall") | UNDERSTATED — `flush()` *always* blocks: `vkWaitForFences(…UINT64_MAX)` (upload.cpp:278); graphics queue only; sample 04 blocks mid-frame today | Task 15/#22 covers async CPU decode only; D5 pins uploads main-thread/single-queue (thread-safety, orthogonal to the fence wait) | PARTIALLY COVERED | Non-blocking-flush invariant → Phase 4 (D25, new T11/#28) before Stage 1 adds 4 call sites |
| 2 | Real scene culling/submission | Confirmed absent; 07_stress submits all 30k unconditionally | Fully specified: D14/D15/D19, plan tasks, #5/#6/#7 | ALREADY COVERED | Claimant wrong that unplanned. Sub-gap: seed-9c instancing/batching half missing from task text → D26.3 |
| 3 | GPU-driven / indirect draw | Confirmed absent; zero indirect anywhere; CPU ceiling measured in-tree (9.14→3.38 ms @30k) | Registered, committed, geometry phase (registry:235-243) | TRUE, deferral recorded+safe | Implementation LATER PHASE; three Phase-4 invariants were NOT captured → D26.1/.2 + addressing on materials |
| 4 | Geometry pooling | Confirmed absent; `MeshBuffers` = one VB/IB pair per mesh | Fully specified task w/ interface + GPU tests (#21); D1 sequences it before import deliberately | ALREADY COVERED | Claimant wrong (RED rating) — it is the next implementation task after the gate |
| 5 | GPU memory budget/residency | Confirmed absent; VMA flags never set; no `VK_EXT_memory_budget`; zero OOM handling anywhere | Elevated to Phase 4 Stage 1 on 2026-08-12 (#27, registry:221) — but absent from plan/spec text | ALREADY COVERED | Claim 6 days behind ledger. Bookkeeping fixed: D24 + plan T10 now carry it in source of truth |
| 6 | Multi-queue / async compute | Confirmed absent; only graphics+present queues acquired; `QueueClass` inert by recorded design | Explicit reasoned deferral (phase3 D4 → techniques phase); API-shape invariant prepaid in Phase 3 | TRUE, deferral recorded+safe | LATER PHASE — except optional transfer-queue *acquisition* prepaid now (D25/T11) |
| 7 | Render-graph hot-path allocation | CONFIRMED — 7 distinct per-frame heap-alloc sites in `execute()` (see detail); `std::function`/compile() NOT per-frame (honest counter-evidence) | Never named in any doc (0 hits) | TRUE GAP, PHASE-4 FIT | New T23/#29 executor cleanup; bigger item = draw-list return-by-value → D26 caller-owned storage on T19 |
| 8 | Buffer Device Address | Confirmed absent — not in features12 (device.cpp:216-227), no VMA flag (buffer.cpp:28-33), zero uses | One mention (registry:237) as ingredient of deferred GPU-driven item; never adopted | TRUE GAP, PHASE-4 FIT (enablement only) | Unpurchasable later (allocator-creation flag + buffer-creation usage bits) → D26.4 on T12; opportunistic, never required; verify lavapipe in-task |
| 9 | PSO compilation stutter | HALF-TRUE — persistent VkPipelineCache delivered+tested; Slang compiles at loadMaterial NOT draw time; but `vkCreateGraphicsPipelines` lazy on first use, main-thread, no warmup | FG audit classified pre-caching a near-miss (audit:73-75) | PARTIALLY COVERED | Warmup UX → SDK/tooling registry entry (supersedes near-miss ruling). Phase-4 correctness collision found → D27 |

## Key evidence (abridged; full citations verified at HEAD bf5b853)

- **Compute (0):** `resources.h:23-26` `QueueClass::AsyncCompute` read only
  by its own getter; `render_graph.cpp:610` compute-class = barrier-stage
  selection from attachment absence; `executor.cpp:1193-1199` bare pass
  gets `ctx.cmd` with no dispatch machinery; `compiler.cpp:61-62` Slang
  CAN emit compute, nothing consumes it; `material_system.cpp:118`
  vertex+fragment only; `:1706-1710` rejects attachment-free signatures;
  `:1815` only `vkCreateGraphicsPipelines`. "particle" appears exactly
  twice in the planning corpus, both inside FG3 (CPU-content contract).
- **Uploads (1):** `upload.cpp:278` unconditional fence wait ("synchronous
  by design"); `:31,40` graphics queue/family only; `:119` ring wrap →
  full blocking flush; `mesh_buffers.h:41-44` create() flushes per mesh;
  `samples/04_streaming/main.cpp:915-919` + `:1382-1383` in-frame-loop
  block; `device.h:72-74` no transfer queue exists. Task 15's overlap
  test criterion ("counters not wall-clock", plan) is exactly the metric
  that cannot detect a per-frame fence block — amended per D25.
- **Hot path (7):** per-frame in `Executor::execute()` —
  `executor.cpp:995-998` (2 vectors + 2 node-based unordered_maps),
  `:1061`, `:1077`, `:289,353` (barrier vectors), `:255` (debug-label
  std::string), `:1346,1463` (chunk buffer vectors), `:1486` (temporary
  std::string per name resolve). Counter-evidence honestly recorded:
  pass callbacks stored once at addPass; compile()/realize() are
  setup/resize-only in all samples.
- **BDA (8):** `device.cpp:216-227` ten descriptor-indexing bits, no
  `bufferDeviceAddress`; `buffer.cpp:28-33` `VmaAllocatorCreateInfo::flags`
  never assigned; VMA requires the flag at allocator creation and the
  usage bit at buffer creation — Stage-1 pool blocks created without it
  can never yield addresses (full pool reallocation+reupload later).
- **PSO (9) + collision:** cache load/save `material_system.cpp:1252-1272`
  / `:1177-1205`; lazy variant build `material_system.h:45-48`; NO Slang
  at draw time (`material_system.cpp:1718-1721`); `getPipeline` main-
  thread-guarded (`:1695`) while chunked passes run chunks ≥1 on workers
  (`pass.h:184-195`, `threading.md:110-128`) — the planned scene-path
  record helper could not legally bind materials off chunk 0. Sample 06
  migration hit this wall before (`threading.md:113-118`); sample 07
  sidestepped via raw pipelines (`07_stress:686`). Resolved by D27
  main-thread pre-resolution (sorted list enumerates all pairs pre-record;
  also the future warmup hook).

## Pushback recorded (claims vs. evidence)

- Claims 2/4/5 rated RED were already scheduled work (culling/submission,
  GeometryPool, memory budget) — #21 in particular is the single most
  completely specified task in the plan.
- Claims 3/6 are recorded, reasoned deferrals with prepaid API-shape
  invariants — capability statements fair, "planning failure" not.
- Claim 9's implicit "shaders compile at draw time" is wrong; only
  pipeline objects build lazily.
- Claim 1 was the only UNDERSTATED claim: it does not "can stall" — it
  always blocks, in-tree, today.
