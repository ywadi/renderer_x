# Completeness matrix — Issue #22: Async import pipeline

## 1. Header

- **Ticket:** #22 "Async import pipeline" — plan **Task 15** (issue body's
  first line still reads "Plan Task 12" from before the 2026-08-18
  renumbering; the amendment section carries the correction).
- **Binding decisions:** D5 (threading contract — GPU-object mutation
  main-thread-only, `postToMain()`/`pumpMain()` handoff), D24 (eviction
  invariant, as it bounds what an in-flight import may publish), D25
  (UploadTicket; amended wall-clock main-thread-block assertion), D6/D7
  (the sync pipeline being asynchronized), plan Task 15 text, issue #28
  (ticket API this pipeline consumes), `docs/threading.md` (the full
  worker-allowed / main-thread-only contract, including the
  exception-in-chunk fatality rule).
- **Sources consulted (fetched/verified 2026-08-18):**
  - Godot `ResourceLoader` threaded-loading class reference + background
    loading tutorial (docs.godotengine.org, stable) — primary.
  - Unreal Engine "Asynchronous Asset Loading" conceptual doc +
    `UEngine::HandleUnifiedStreaming` API doc (dev.epicgames.com) —
    primary; `FStreamableHandle` API-reference pages are dead links
    (404-redirect), so CancelHandle behavior is community-sourced
    (ikrima.dev UE guide; Epic staff forum posts) — medium confidence,
    flagged inline.
  - Unity "Understanding the Async Upload Pipeline" engineering blog
    (unity.com/blog) — primary; source of the 2 ms time-slice default.
  - bgfx docs (bkaradzic.github.io/bgfx/{tools,overview}.html) — primary.
  - Delivered code at HEAD `bf5b853`:
    `src/rx_task/include/rx_task/scheduler.h`, `src/rx_task/scheduler.cpp`
    (postToMain/pumpMain, IO pinned-task thread),
    `src/rx_rhi_vk/src/upload.cpp:278` (the blocking flush D25 replaces),
    `docs/threading.md` (guards, chunk-0 rule, exception fatality).

**Precedent-landscape note (honesty row):** bgfx has **no runtime async
import system** to imitate — its pipeline is offline (`texturec`/
`geometryc`/`shaderc` command-line tools; the tools docs list no
threading flags), and its runtime threading model is the API/render-thread
submission split, not asset loading. The first-tier precedents for this
ticket are Godot's threaded ResourceLoader, Unreal's streamable
manager/async loading thread, and (for the main-thread budget mechanism
specifically) Unity's Async Upload Pipeline. Recorded so the hardened
ticket does not imply a bgfx analog was consulted and found lacking.

---

## 2. Matrix

| Feature | First-tier precedent (cited) | Phase-4 disposition | Library/infra support (verified) | Proposed acceptance criterion |
|---|---|---|---|---|
| Worker/main split: parse, decode (meshopt/Draco), transcode, MikkTSpace, meshopt passes on workers; GPU uploads + registry mutation on main | Unreal: async loading thread does IO+deserialize and even constructs UObjects off-thread, gated by a GC-flag handshake finalized on the GameThread (Epic conceptual doc; voithos.io writeup — community, flagged); notably UE's `RequestAsyncLoad` must itself be CALLED from the GameThread (Epic staff forum: "We delegate to the GT to launch it because that's the limitation of that API") — RendererX's stricter main-thread marshalling is consistent with shipped first-tier practice, not overly conservative. Godot: `load_threaded_request` → WorkerThreadPool | consume-now | `rx_task` delivered: `parallelFor` (chunked, grain-auto), `postToMain()` (any-thread safe, mutex-guarded queue), `pumpMain()` (main-thread-guarded, `RX_ASSERT_MAIN_THREAD` — threading.md), dedicated IO pinned-task thread (`runOnIoThread`, scheduler.cpp); all main-thread-only mutators runtime-guarded (BindlessTable/Uploader/MaterialSystem/DeletionQueue — threading.md) | Debug-build thread-affinity asserts: registry mutation and every Uploader/BindlessTable call happen on the main thread (the existing `RX_ASSERT_MAIN_THREAD` guards make violations abort loudly — the test simply exercises the async path with guards on); worker stages touch no guarded API (a violation cannot pass CI by construction) |
| Byte-source reads on the IO thread, CPU stages on compute workers | The IO/compute split is the standing `rx_task` design (scheduler.cpp dedicates a pinned IO thread precisely so blocking reads never occupy a compute worker — in-repo precedent) | consume-now | `Scheduler::runOnIoThread` delivered and tested (scheduler.cpp) | Byte-source reads issued via `runOnIoThread`; decode/transcode/optimize stages run inside `parallelFor` workers; debug thread-id checks assert decode never runs on the IO thread (keeps the single IO thread available for reads) |
| Completion callback delivery | Unreal: completion delegate fires on the GameThread after the streamable handle completes; `StreamableManager` holds hard references "until the delegate is called" (Epic conceptual doc — a lifetime rule the registry must mirror). Godot: no callback — poll `load_threaded_get_status` then fetch | consume-now | `postToMain()`/`pumpMain()` FIFO contract documented in scheduler.h | `CompletionFn` runs on the main thread (thread-id assert), exactly once per import (counter assert), strictly after all registry mutation for that import is visible (callback body resolves every returned handle successfully); resources referenced by the in-flight import are owned by the pipeline until the callback returns — eviction/teardown cannot race it |
| Progress reporting granularity | Godot: single float ratio 0.0–1.0 via `load_threaded_get_status(path, progress_array)`, polled across frames (class reference: "the ratio of completion of the threaded loading"); Unreal exposes handle-level completion, not staged progress | consume-now (minimal contract) — see New gaps N2 for the host-facing API shape | Importer-side; no library dependency | Import exposes a poll-able progress snapshot: stage enum (Reading / Parsing / Decoding / Optimizing / Uploading / Done / Failed) + items-completed / items-total counts; progress is monotonic (test polls every frame during the slow-decode overlap test and asserts no regression and terminal arrival); granularity at least per-primitive and per-texture (matches the unit of worker fan-out) |
| Cancellation story | Godot: **no cancellation exists** — no API on ResourceLoader or WorkerThreadPool (class reference; community-confirmed "no cancel on ResourceLoader threaded loads", forum.godotengine.org/t/142397); the ecosystem pattern is generation-counter ABANDON (retrieve then discard). Unreal: `FStreamableHandle::CancelHandle()` "aborts loading and prevents callback execution" (ikrima.dev — community, medium confidence; Epic API pages dead) — i.e. documented cancel semantics are "suppress callback + release refs", NOT proven mid-IO abort | consume-now (abandon semantics) — see New gaps N1 for the ruling this needs | Importer-side: an atomic cancel flag checked at stage boundaries | `cancel(importHandle)` marks the job: in-flight worker stages run their current item to completion, no further stages start, no registry mutation from that job is applied after the flag is observed at a marshal point, the completion callback does NOT fire (a separate `onCancelled` path or a status in the result — pick one, document it), all partial resources are released (ASan-clean test); cancellation latency is bounded by one stage-item, not by the whole import; a cancel issued after completion is a documented no-op |
| Error propagation worker → caller | Godot: coarse `ThreadLoadStatus` enum (`THREAD_LOAD_FAILED` etc.) with no error object — a floor, not a target; RendererX can do strictly better by reusing the sync path's `ImportResult` taxonomy (matrix-issue02 §2D row 4). In-repo hard constraint: an exception escaping a `parallelFor` chunk ≥ 1 is **process-fatal by design** (threading.md audit finding F6: enkiTS dispatch has no handler; `std::terminate()`) | consume-now | `rx_task` contract documented (threading.md: chunked callbacks must be "noexcept-in-effect") | Every worker-stage body is exception-bounded: failures convert to error values marshalled to the main thread, never thrown across the chunk boundary (code-review-blocking rule + a test whose decode stage deliberately fails); a garbage file imported async yields the identical `ImportResult` error code as the sync path (paired assert); one failed primitive/texture degrades to fallback assets (D11) without failing the whole import unless the document itself is unreadable — same per-failure-mode semantics as sync |
| Determinism: async result deep-equals sync result | Godot implies same-loader-different-scheduling ("the load will block at this point like `load()` would" — class reference), and documents no stronger guarantee; RendererX asserts it by test instead of implying it | consume-now | Same stage functions shared by both paths (plan Task 15: the sync path parallelizes internally too — one pipeline, two completion styles); meshoptimizer/MikkTSpace are deterministic pure functions of their inputs | Ordering rule (the criterion that makes determinism true): pool uploads and registry insertions are applied in FILE ORDER at the marshal point, regardless of worker completion order. Tests: (a) async import of cube + DamagedHelmet deep-equals the sync import — mesh/submesh/instance counts, pool ranges, AABBs, material parameter sets, preserved skin/light/camera data; (b) two consecutive async imports of the same file into fresh registries produce identical results (run-to-run determinism); (c) results are independent of worker count (`Scheduler` constructed with 1 vs N workers) |
| Wall-clock main-thread-block assertion (2026-08-18 amendment) — threshold + measurement | Unity Async Upload Pipeline (primary, unity.com blog): `QualitySettings.asyncUploadTimeSlice` — "The amount of time in milliseconds spent uploading … on the render thread for each frame … **The default value is 2ms** … A value too large … might result in framerate hitching." Unreal: a named per-frame "unified time budget" for async asset+level streaming exists (`UEngine::HandleUnifiedStreaming` API doc — primary); community-tuned `s.AsyncLoadingTimeLimit` values cluster 0.1–5 ms (secondary). Frame math: 16.7 ms @60 fps, ~11.1 ms @90 fps | consume-now | Measurement is importer/test-side: `std::chrono::steady_clock` | **Proposed threshold: 2.0 ms** per individual main-thread call — each `pumpMain()` invocation and each upload-poll/`flush()` call — matching Unity's shipped default for the closest analog mechanism, sized for the Steam Deck floor. Measurement method: the overlap test (deliberately slow decode + a large texture/geometry payload, ≥300 rendered frames) wraps EVERY `pumpMain()` and upload-poll call in steady_clock timing and asserts max < threshold. Two-tier gate to reconcile with D18 (see Conflicts C1): **local/dev assertion at 2 ms** (published number, trend-tracked), **CI assertion at 10 ms** — the CI tier is a stall DETECTOR (a per-frame fence wait like today's upload.cpp:278 blocks for the full upload duration, far above 10 ms even on a noisy runner), not a perf trend, so it can block honestly despite >30% runner variance. The frame-counter assertion from the original plan text is retained alongside |
| Time-sliced upload marshalling (main-thread budget per pump) | Unity: the upload pipeline drains a ring buffer in ≤2 time-slices per frame; oversized single resources span multiple slices rather than raising the ceiling (same blog) | consume-now | D25 ticket API (Task 11) makes `flush()` non-blocking; ring reclamation keys off ticket completion | `pumpMain()` work for imports is budget-shaped: upload marshalling splits payloads so no single posted closure exceeds the 2 ms budget on the dev baseline (a Sponza-scale texture set imports without any single pump exceeding threshold — measured in the overlap test); the pipeline never compensates for a large asset by blocking longer, only by taking more frames |
| Poll, never block (D25/#28 GPU-side handoff) | D25 text: "poll in the frame loop, never a blocking wait"; issue #28 test contract ("timer around the call, not frame counters") | consume-now | `UploadTicket`/`isComplete()`/`wait()` land in Task 11 before this task | The async path calls `isComplete(ticket)` from the frame loop and never `wait(ticket)` (debug-build counter: wait-calls-from-async-path == 0, asserted in the overlap test); completed tickets release their staged resources promptly (ring does not exhaust during the overlap test despite continuous frames) |
| Tracy zone coverage | D3/Stage 0 Tracy integration delivered; plan Task 15 names "progress/Tracy zones" | consume-now | `RX_ZONE`/`RX_ZONE_DYNAMIC_NAME` + plots delivered (rx_core profile.h; TRACY_ON_DEMAND=ON — third_party/CMakeLists.txt:352-385) | Every pipeline stage (read, parse, decode, transcode, tangent, meshopt, AABB, upload-marshal, registry-commit) carries a named zone; per-import bytes-uploaded and items-completed feed Tracy plots; acceptance = zones/plots present in a documented manual capture procedure (MANUAL_VERIFICATION.md pattern) + code-review check — consistent with D18's rule that wall-clock traces inform, counters gate |
| Concurrent imports in flight | Unreal: multiple streamable handles with priorities; Godot: multiple queued threaded requests | consume-now (N concurrent, no priorities) | Scheduler/postToMain are many-producer safe (scheduler.h) | Two overlapping `importGltfAsync` calls (different files) complete with isolated, correct results (deep-compare each against its sync twin); completion order may differ from issue order (documented); priorities are out of scope (New gaps N4) |
| Shutdown / teardown with an import in flight | UE holds hard refs until delegates fire; Godot documents nothing (its own docs-issue #8047 flags lifecycle gaps — cautionary precedent) | consume-now | `Scheduler` shutdown drains outstanding IO and pinned work (scheduler.cpp WaitforAllAndShutdown paths, verified against enkiTS v1.12 source per in-file comments) | Destroying the Registry (or Scheduler) while an import is in flight is defined behavior: outstanding worker stages complete or are abandoned at stage boundaries, no callback fires after teardown begins (or fires with a Cancelled status — same single documented choice as the cancel row), process exits cleanly (ASan/LSan-clean test), no use-after-free of the byte source (its lifetime contract vs. the import is documented) |
| D24 interplay: what an in-flight import publishes | D24 residency-tolerant resolve (spec); simplest Phase-4 answer: publish nothing early | consume-now | Registry handle model (rx_core handle.h) | Handles for an import become resolvable only at completion-callback time (no placeholder handles mid-import in Phase 4 — recorded as the deliberate scope bound; streaming-phase revisits partial residency); resolving a handle from a DIFFERENT completed import during an in-flight one is unaffected (test overlaps a resolve-heavy render loop with an import) |
| Sync path parallelism parity | Plan Task 15: "the SYNC importGltf also parallelizes per-primitive work internally (parallelism is the default, not an async-only property)" — D4-amended contract (threading.md "Parallelism is the default, not a mode") | consume-now | `parallelFor` grain-auto | Sync import of DamagedHelmet exercises `parallelFor` (Tracy zones show multi-worker occupancy in the documented capture); sync-vs-async results identical (determinism row); no "parallel on/off" flag exists in any import API signature (review-blocking) |

---

## 3. Conflicts (coordinator adjudicates; both sides quoted)

- **C1 — D18 vs. the D25 wall-clock assertion.** D18:
  "Wall-clock/Tracy numbers are published as artifacts … trend-tracked
  but **never CI-blocking** (>30% shared-runner variance)." D25/plan
  Task 15 (2026-08-18 amendment): "the overlap test gains a
  **wall-clock** main-thread-block assertion (no single
  `pumpMain()`/upload call blocks past threshold) … the counter-only
  criterion cannot detect a per-frame fence stall" — and tests run in CI.
  Both are right about different things: a 2 ms budget is a perf number
  (D18 says don't gate CI on it); an unbounded fence wait is a
  correctness bug whose signature (tens of ms, the full upload duration —
  upload.cpp:278 today) is an order of magnitude above runner noise. The
  matrix proposes the two-tier resolution (2 ms local assertion +
  published number; 10 ms CI stall-detector assertion) — needs an
  explicit ruling so D18's "never CI-blocking" is amended with the
  stall-detector carve-out rather than silently contradicted.
- **C2 — issue #22 body's plan-task pointer is stale.** Body opens "Plan
  Task 12" while the 2026-08-18 amendment renumbers to Task 15; Task 12
  is now GeometryPool. Cosmetic, but the hardened rewrite should fix the
  opening line (same class of fix as issue #2's "Plan Task 10").

## 4. New gaps (absent from the entire planning universe; checked against the master registry deferred list, FG1-FG12, and the phase spec)

- **N1 — Cancellation/abandon semantics are unplanned.** Neither issue
  #22, plan Task 15, D5/D25, nor any registry entry mentions cancelling
  an in-flight import. Godot ships without it (and its ecosystem
  workaround is exactly the abandon pattern); Unreal ships
  `CancelHandle`. A middleware evaluation will ask (level unload during
  streaming is the standard scenario). Proposed fit: the abandon
  semantics in this matrix's cancellation row are Phase-4-cheap (one
  atomic flag + stage-boundary checks + a defined callback outcome); a
  richer prioritized-cancel story belongs to the streaming phase.
  Coordinator decides which side of the line Phase 4 takes — the matrix
  row is written to be implementable now.
- **N2 — Host-facing progress API shape.** Plan Task 15 says
  "progress/Tracy zones" without a contract; Tracy is dev-only, not a
  host API. The matrix row proposes the minimal poll-able snapshot
  (stage enum + counts — richer than Godot's single float). Whether that
  surface is internal-C++-only this phase (like the rest of scene/asset
  per D23) or shapes the SDK ABI is an SDK-phase question; registering it
  keeps the SDK spec obligated to answer it (same pattern as FG3/FG5).
- **N3 — Host-configurable upload time-slice budget.** Unity exposes the
  budget as a quality setting (`asyncUploadTimeSlice`, plus buffer-size
  knobs); RendererX's 2 ms is proposed as a constant this phase. A
  host-tunable knob (loading screens want a bigger slice — Unity's own
  documented practice) is FG12-adjacent (frames-in-flight/latency knobs)
  and fits the same profiling/SDK phase; one registry line proposed.
- **N4 — Import prioritization between concurrent jobs.** Unreal has
  streamable priorities; nothing in the planning universe covers
  priority between overlapping RendererX imports. Streaming phase
  (residency/eviction policy will need the same priority concept);
  no Phase-4 work beyond documenting "no priorities".

## 5. Verification health

**Verified first-hand (2026-08-18):** Godot ResourceLoader class
reference + background-loading tutorial (primary, fetched; progress
array semantics, ThreadLoadStatus enum values, no-cancel API surface);
Unity Async Upload Pipeline blog (primary, fetched; the 2 ms default
quoted verbatim); Unreal conceptual async-loading doc +
`HandleUnifiedStreaming` API doc (primary, fetched — proves a named
per-frame streaming time budget exists); bgfx tools/overview docs
(primary, fetched; offline pipeline confirmed, no threading flags
documented); in-repo: scheduler.h/scheduler.cpp (postToMain/pumpMain
contracts, IO thread), threading.md (guards, chunk-0 rule, F6 exception
fatality), upload.cpp:278.

**Community-sourced (flagged, medium confidence):** UE
`FStreamableHandle::CancelHandle` semantics (Epic's API-reference pages
for it are dead links — 404-redirect confirmed — and Epic's site search
returns no results; ikrima.dev + forum testimony used instead); the UE
GameThread-only `RequestAsyncLoad` constraint (Epic staff forum post);
UE off-thread UObject construction with GC-flag handshake (independent
blog); `s.AsyncLoadingTimeLimit` community tuning range (0.1–5 ms);
Godot's no-cancel confirmation (official docs show no API — the absence
is primary; the "this is deliberate" framing is forum-sourced).

**Not verifiable this session:** Unreal's numeric default for the
unified streaming time budget (the mechanism is primary-sourced; the
number is not — the proposal therefore anchors on Unity's primary-sourced
2 ms, not on Unreal's unpublished default). No dead ends beyond the Epic
API-reference pages noted above.
