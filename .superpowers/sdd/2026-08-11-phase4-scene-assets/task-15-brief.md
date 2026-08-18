# Task 15 brief — Async import pipeline (card #22)

You are the implementer for Phase 4 Stage 1 Task 15 of RendererX
(Vulkan 1.3 renderer middleware, C++20, repo `/media/ywadi/second/renderer_x`,
main checkout — base commit `55b4822`, tree clean except SDD workspace
files which are not yours).

## Requirements — read IN THIS ORDER; they are your spec

1. Plan task body: `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md`
   — section `### Task 15:` INCLUDING "Gate hardening" (BINDING).
2. Spec: D5 (threading contract — READ `docs/threading.md` in full),
   D18 AS AMENDED (RC6 stall-detector carve-out), D24, D25:
   `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`.
3. Completeness matrix (acceptance criteria row by row):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue22-async-import.md`.
4. Coordinator rulings (+Errata; win on conflict):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/rulings-2026-08-18.md`
   — sections `RC6` and `**#22 async import (Task 15).**`.
5. Ticket body: `gh issue view 22` (amendment + GATE HARDENED blocks).

Order of authority: rulings (+errata) > spec > matrix > ticket.

## Landed context you build on (don't fight it)

- Task 13: sync `Registry::importGltf(source, GeometryPool&,
  TextureCache*)` is complete (byte-source, full pipeline,
  parallelFor per-primitive CPU work ALREADY internal per plan);
  `import_gltf.cpp` documents two fastgltf v0.9.0 bug workarounds —
  respect them. Task 14: `TextureCache` complete. Task 11:
  `UploadTicket` (timeline semaphore) — poll `isComplete`, never
  `wait` on the async path. Task 10: accounting.
- `rx_task::Scheduler`: `parallelFor`, `runOnIoThread` (pinned IO
  thread), `postToMain`/`pumpMain` (main-thread-guarded). Exceptions
  escaping worker chunks are PROCESS-FATAL by design (threading.md).

## Scope summary (details in the matrix — this is a map)

- `Registry::importGltfAsync(source, GeometryPool&, TextureCache*,
  CompletionFn)` sharing ONE pipeline with the sync path (two
  completion styles, no forked logic; no parallel on/off flag in any
  signature — review-blocking).
- Worker/main split: byte-source reads on the IO thread
  (`runOnIoThread`; debug thread-id assert that decode never runs on
  the IO thread); parse/decode/transcode/MikkTSpace/meshopt on
  compute workers (`parallelFor`); ALL GPU-object mutation + registry
  mutation marshalled to the main thread (existing
  RX_ASSERT_MAIN_THREAD guards make violations loud — exercise the
  async path with guards on as the test).
- ORDERING RULE (the determinism criterion): pool uploads and
  registry insertions applied in FILE ORDER at the marshal point,
  regardless of worker completion order. Tests: async deep-equals
  sync (counts, ranges, AABBs, parameter sets, preserved data);
  run-to-run identical into fresh registries; identical results with
  1 vs N workers.
- Completion: `CompletionFn` on the main thread (thread-id assert),
  exactly once (counter), strictly after all of that import's
  registry mutation is visible; pipeline owns referenced resources
  until the callback returns (no teardown race).
- CANCELLATION (rulings N1, abandon semantics): `cancel(handle)` —
  atomic flag checked at stage boundaries; in-flight stage items run
  to completion; no registry mutation applied after observation; the
  completion callback does NOT fire (choose the one documented
  outcome: onCancelled path OR status-in-result — pick one, document
  it); all partial resources released (ASan-clean); latency bounded
  by one stage-item; cancel-after-completion is a documented no-op.
- PROGRESS (rulings N2, minimal): poll-able snapshot — stage enum
  (Reading/Parsing/Decoding/Optimizing/Uploading/Done/Failed) +
  items-completed/items-total; monotonic (polled every frame in the
  overlap test: no regression, terminal arrival); granularity at
  least per-primitive and per-texture. Internal C++ this phase.
- ERROR PROPAGATION: every worker-stage body exception-bounded
  (failures → error values marshalled to main; NEVER thrown across a
  chunk boundary — review-blocking rule + a deliberately-failing
  decode test); garbage file async == sync `ImportError` (paired
  assert); per-item degradation to D11 fallbacks without failing the
  whole import (same semantics as sync).
- WALL-CLOCK GATE (RC6 two-tier, THE load-bearing test): overlap
  test drives ≥300 rendered frames while a deliberately slow decode
  + large payload import is in flight; wraps EVERY `pumpMain()` and
  upload-poll call in steady_clock timing; asserts max per-call
  block < **2 ms** locally (published number) and < **10 ms** as the
  CI stall-detector tier; PLUS the original frame-counter assertion;
  PLUS wait-calls-from-async-path == 0 (debug counter); ring does
  not exhaust during the overlap (tickets polled and reclaimed).
  Time-sliced marshalling: no single posted closure exceeds the 2 ms
  budget on the dev baseline (split payloads; never block longer for
  a big asset — more frames instead).
- CONCURRENT IMPORTS: two overlapping async imports (different
  files) complete isolated and correct (deep-compare each vs its
  sync twin); completion order may differ from issue order
  (documented); no priorities (documented).
- TEARDOWN: destroying Registry/Scheduler with an import in flight
  is defined behavior — outstanding stages complete or abandon at
  boundaries, no callback after teardown begins (or fires with the
  documented Cancelled outcome — same single choice), process exits
  clean (ASan/LSan-clean test), byte-source lifetime contract
  documented.
- D24 interplay: handles resolvable only at completion-callback time
  (no placeholder handles mid-import — recorded scope bound);
  resolves against OTHER completed imports unaffected during an
  in-flight one (overlap test asserts).
- Tracy zones on every stage + bytes-uploaded/items plots; documented
  manual capture procedure (MANUAL_VERIFICATION pattern) — code
  presence + procedure, not CI-gated.
- Discrimination standard: scratch-worktree revert evidence for the
  wall-clock gate (against a reintroduced blocking wait — the Task 11
  evidence pattern), the ordering rule (perturb apply-order → deep-
  equal fails), and exactly-once completion.

## Global constraints (binding)

- **NO AI attribution of any kind** in commits; author stays local
  git config; conventional factual messages; commit locally; do NOT
  push; do NOT touch board/issues/plan/spec/ledger; commit only your
  own files. Do NOT touch src/rx_rhi_vk or src/rx_task internals
  unless a genuine blocking defect forces it — if so, document why,
  keep it additive, and flag it prominently.
- Production grade; TDD; suite green BOTH presets; zero unfiltered
  validation errors; per-directory style; D5 one-liners on new
  public surface.

## Report contract

Full report →
`.superpowers/sdd/2026-08-11-phase4-scene-assets/task-15-report.md`
(per-criterion proof, command output tails, revert evidence,
deviations, self-review). FINAL MESSAGE: ONLY status, commit SHAs,
one-line test summary, concerns.
