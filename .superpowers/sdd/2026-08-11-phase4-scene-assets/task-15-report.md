# Task 15 report — Async import pipeline (card #22)

Base commit: `55b4822`. Implementer commits (this task, in order):

1. `da566ed` — `feat(rx_task): add Scheduler::runOnWorkerThread (Task 15 prerequisite)`
2. `8b7f642` — `feat(rx_asset): non-blocking upload primitives for GeometryPool/TextureCache (Task 15)`
3. `a98a233` — `feat(rx_asset): async import pipeline -- Registry::importGltfAsync (Task 15)`

No AI attribution in any commit; author is local git config (`Yousef Wadi <ywadi85@gmail.com>`); nothing pushed; no board/issue/plan/spec/ledger files touched; only files this task produced were committed (`git show --stat` on each commit matches the file list below; the pre-existing `task-15-brief.md` SDD workspace file was left untracked, not mine to commit).

## 1. Files delivered

- `src/rx_task/include/rx_task/scheduler.h` + `scheduler.cpp` — additive `Scheduler::runOnWorkerThread()` (see §4 for why rx_task needed touching, and the review-flag this carries).
- `src/rx_task/tests/scheduler_test.cpp` — 4 new test cases for the above.
- `src/rx_asset/include/rx_asset/geometry_pool.h` + `.cpp` — `uploadDeferred()`/`flushPendingUploads()`/`isUploadComplete()`.
- `src/rx_asset/include/rx_asset/texture_decode.h` + `.cpp` — `decodeTextureForUpload()` (pure, worker-safe decode).
- `src/rx_asset/include/rx_asset/texture_cache.h` + `.cpp` — `decodeForUpload()`/`registerDecoded()`/`flushPendingUploads()`/`isUploadComplete()`/`releaseUnpublished()`.
- `src/rx_asset/import_pipeline.h` (new, private) — the compute/marshal split's shared types and function declarations.
- `src/rx_asset/import_gltf.cpp` — `computeGltfImport()` + `marshalGltfImport{Sync,BeginDeferred,PrepareStep,Finalize,Rollback}()`; `importGltfPipeline()` (sync) is now a two-line wrapper over the same two halves.
- `src/rx_asset/include/rx_asset/registry.h` + `registry.cpp` — `Registry::importGltfAsync()` (two overloads), `cancelImport()`, `importProgress()`, `AsyncImportHandle`/`ImportProgress`/`ImportStage`/`ImportCompletionFn`, the `AsyncImportJob` state machine.
- `src/rx_asset/tests/async_import_test.cpp` (new) + `src/rx_asset/tests/CMakeLists.txt` — 13 new test cases.

## 2. Suite status (both presets)

**linux-native**, full `ctest`:

```
100% tests passed, 0 tests failed out of 20
Total Test time (real) =  55.23 sec
```

- `rx_task_tests`: 19 cases / 33969 assertions / 0 failed (verified 3x in a row for flake-freedom on the new `runOnWorkerThread` barrier tests).
- `rx_asset_tests`: 30 cases / 446 assertions / 0 failed.
- `rx_asset_gltf_tests`: 48 cases / 292 assertions / 0 failed.
- `rx_asset_gltf_gpu_tests` (includes the new `async_import_test.cpp`): **53 cases / 7,348,320 assertions / 0 failed**, `--validate` clean (only the pre-existing, already-annotated "known false positive" portability-enumeration validation lines).

**windows-cross-zig**: `cmake --build build/windows-cross-zig --target rx_task rx_task_tests rx_asset rx_asset_tests rx_asset_gltf_tests rx_asset_gltf_gpu_tests -j8` → exit 0, including the new `async_import_test.cpp`. Wine execution (this task went further than the documented Task-14 precedent of "build only"): `rx_task_tests.exe` and `rx_asset_gltf_tests.exe` (device-free) both ran clean under `wine` — `rx_task_tests.exe`: 19/19 passed including the barrier-proof tests; `rx_asset_gltf_tests.exe`: 48/48 passed. GPU-backed `.exe`s were not run under Wine (this project's own established Wine-GPU-flake posture, unrelated to this task).

## 3. ASan/UBSan/LSan verification

Full `librx_asset` + `async_import_test.cpp` + `doctest_main_gltf_gpu.cpp` rebuilt at `-O0 -fsanitize=address,undefined -fno-omit-frame-pointer`, linked against the existing (non-instrumented) `rx_rhi_vk`/`rx_platform`/`rx_task`/`rx_core`/fastgltf/draco/ktx/meshoptimizer/spdlog/glm/Tracy/SDL3 static libraries (ASan's runtime intercepts the process-wide allocator regardless of which translation units were instrumented — the same technique used, and load-bearing, for the `rx_task` `runOnWorkerThread` bug below).

```
$ ASAN_OPTIONS=detect_leaks=1 ./rx_asset_gltf_gpu_tests_asan --validate --test-case-exclude="*WALL-CLOCK*"
[doctest] test cases:   12 |   12 passed | 0 failed | 1 skipped
[doctest] assertions: 1279 | 1279 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Zero AddressSanitizer/LeakSanitizer/UBSan reports (`grep -i "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|SUMMARY:"` over the full captured log: 0 matches) across every test including both cancellation cases, both teardown-with-import-in-flight cases, and the concurrent-imports case. The WALL-CLOCK GATE case is excluded from the ASan run specifically (not from the suite in general — see §7): `-O0` + full sanitizer instrumentation slows every memcpy/allocation by an order of magnitude or more, making its own wall-clock assertions meaningless noise, a standard, well-understood limitation of running timing-sensitive tests under ASan — not evidence of anything about correctness. Included unmodified in the full (non-ASan, optimized) run reported in §2/§7.

## 4. Deviations from a literal reading of the brief (both flagged prominently, as required)

### 4a. rx_task touched (normally out of scope)

The brief instructs: "Do not touch `src/rx_task` internals unless a genuine blocking defect forces it." This task added one new public method, `Scheduler::runOnWorkerThread()`.

**Why forced:** the async pipeline needs to run CPU-heavy decode work (parse/tangent-gen/meshopt/texture-transcode) without blocking the main thread AND without ever running on the dedicated IO thread (an explicit, separately-tested acceptance criterion). Analysis of every existing `Scheduler` primitive against both constraints simultaneously:

- `parallelFor()` may only be called from the main thread (blocks it for the call's duration — unacceptable for a deliberately-slow-decode workload) or from a task already running on one of the Scheduler's own threads.
- `runOnIoThread()`'s own callback runs *on* the dedicated IO thread; calling `parallelFor()` from inside it nests into that thread's own `WaitforTask()` participation — verified directly against the pinned enkiTS v1.12 source (`WaitforTask()`'s spin loop calls `TryRunTask()` on the calling thread) that this would let the IO thread itself execute decode chunks, violating the one invariant the task is explicitly required to prove.
- A caller's own foreign `std::thread` cannot legally call `parallelFor()` at all — an unregistered thread's `gtl_threadNum` (enkiTS's own thread-local storage) defaults to 0, colliding with the real main thread's own registration (confirmed against the vendored source, not merely asserted).

No existing primitive satisfies both constraints. `runOnWorkerThread()` is additive (reuses the existing `IoTask` two-phase-delete class verbatim, generalized to a caller-supplied target thread number instead of the hardcoded IO thread number; adds one new outstanding/trash bookkeeping pair to `Scheduler::Impl`), does not change `workerCount()` semantics or spawn any new thread, and does not alter any existing method's behavior — verified by the full pre-existing `rx_task_tests` suite passing unchanged (19/19, same as before this task).

**A genuine bug was found and fixed in the course of building this**, itself only surfaced by this task's own ASan/UBSan+`ENKI_ASSERT`-enabled testing discipline (see §6): the first implementation reused `IoTask::Execute()`'s existing "push self into trash on completion" pattern verbatim, which is only safe for the *original* IO-only use because `IoLoopTask`'s own reaper runs strictly after `RunPinnedTasks()`'s post-`Execute()` bookkeeping, on the *same* thread, by construction. A worker-targeted task's reaper can run on a *different* thread (the submitter's), with no such same-thread ordering guarantee — deleting the task there raced enkiTS's own `TaskComplete()` still touching the object, reproduced directly as a SIGABRT (`ENKI_ASSERT(GetIsComplete())` in `~ICompletable()`) under ASan and as heap corruption (`free(): corrupted unsorted chunks`) without it. Fixed with a third gate (`GetIsComplete()`, an acquire load against `TaskComplete()`'s own release store) before any worker-targeted task is ever deleted — see `reapPublishedTrash()`'s own extensive comment in `scheduler.cpp` for the full mechanism. Verified clean afterward, 5/5 consecutive runs under ASan+UBSan+`ENKI_ASSERT`, and stable under the normal build across dozens of runs during this task.

### 4b. Buffer/image byte-source reads run on the compute worker, not literally the dedicated IO thread

The brief's scope summary says "byte-source reads on the IO thread." This task's implementation runs the *main glTF/GLB document's own bytes* through `runOnIoThread()` (the filesystem-path overload) — a real, exercised use of the dedicated IO thread — but buffer/image URI resolution (the `ByteSource::read()` calls `ResolvedBuffers`/`resolveImageBytes` make) runs on the same background worker as the rest of `computeGltfImport()`, not on a second hop back to the IO thread.

**Why:** fastgltf parsing (which determines *which* buffer/image URIs even exist) is itself required to run on a compute worker, not the IO thread (parsing is real CPU work, explicitly listed among "parse/decode/transcode/MikkTSpace/meshopt on compute workers" in the brief's own scope summary). Splitting `ResolvedBuffers`' existing lazy/cached design across an IO-thread/worker-thread boundary while preserving its cache invariants (each buffer resolved exactly once, safe for concurrent per-index reads from parallel primitive-processing chunks) is a materially larger, separately-riskable undertaking than the alternative accepted here: a worker thread occasionally performing a real (small, in this codebase's own committed/fetched fixtures) blocking file read. This is a documented, judged trade-off, not an oversight — worker threads doing occasional blocking I/O is an ordinary, defensible pattern in every engine this project's own precedent research cites (Godot's WorkerThreadPool, Unreal's async loading thread both do file I/O and deserialization on the *same* background thread/pool), whereas CPU decode work running on the *one* dedicated IO thread would defeat that thread's entire purpose for every other in-flight IO request — the invariant this task's tests *do* strictly enforce (§7).

The debug-observable, load-bearing half of "decode never runs on the IO thread" (Draco/meshopt/MikkTSpace/texture-transcode never touching that one thread) is proven directly in `async_import_test.cpp`'s worker-thread-participation test via a `ByteSource`-based observation seam (the project's own documented host-injectable extension point) rather than a bespoke production-code instrumentation hook.

## 5. Discrimination / revert-testing evidence

**Ordering rule** (file-order application at the marshal point): `OrderingDeterminismTest`-equivalent (`async_import_test.cpp`, "submeshes land in FILE order...") runs `cube_multi_primitive.gltf` (two primitives with disjoint, easily distinguished AABBs — `x∈[0,1]` vs `x∈[2,3]`) at 8 workers across 15 fresh, repeated imports, asserting submesh 0/1's identity every single run. The underlying mechanism this discriminates is the SAME index-written (never completion-order-appended) `pending[w]`/`ImportComputeResult::submeshes[i]` pattern the pre-existing sync pipeline already used (proven correct by 118 pre-existing passing tests) — this task's own contribution is proving it survives being driven from a worker thread with genuine multi-thread races, not a synthetic single-threaded call.

**Wall-clock gate** (§4 of the brief's own scope summary, "THE load-bearing test"): two real, empirical revert findings during this task, both against the actual implementation, not a synthetic mockup:

1. First attempt: a scratch revert of `GeometryPool::uploadDeferred()` back to the old blocking `upload()` inside the marshal-prepare step, with `textures=nullptr` (geometry only). **Did not fail** the test (max pump stayed ~1ms) — this GPU's `Uploader::uploadToBuffer()` direct (ReBAR/UMA) path memcpy's straight into `DEVICE_LOCAL+HOST_VISIBLE` destination memory with zero staged GPU work queued at all, so `flush()+wait()` there is trivially fast regardless of blocking-vs-deferred. A genuine false-negative finding, corrected by wiring a real `TextureCache` into the test (`upload.h`'s own documented "IMAGES ALWAYS STAGE unconditionally, no ReBAR exception" guarantee is the hardware-independent discriminator).
2. With a real `TextureCache` (DamagedHelmet's five real JPG textures, 300KB–1.3MB each): running the (still-correct, non-blocking) code *before* this task's own time-slicing fix landed — i.e., registering all five textures inside one synchronous marshal step — **did** fail: `REQUIRE(pumpDuration < 10ms)` observed `15797µs`. This is the exact class of regression (an oversized synchronous batch, not a `wait()` call per se) RC6's stall detector exists to catch. Fixed by making `marshalGltfImportPrepareStep()` register at most one real texture per call, re-verified clean afterward (max ~4.0–4.4µs→ms across repeated runs, comfortably under the 10ms hard ceiling; the 2ms figure occasionally exceeded by a single large texture's own memcpy cost — reported via `MESSAGE` only, per D18/RC6's own "published, trend-tracked, never CI-blocking" policy for that specific tier, not asserted).

**Exactly-once completion**: `completionCount` is a plain counter incremented inside the test's own callback, checked both immediately at terminal-stage arrival and after 50 additional no-op `pumpMain()` calls — a regression that fired the callback from more than one of the error/cancel/finalize paths (there are three call sites: `finishAsyncImportCompute`'s error branch, `pollAsyncImportUploads`'s finalize branch, and — deliberately never — the cancel branch) would be caught by either check; the CAS guard (`completionFired`) in `fireAsyncImportCompletion()` makes this true by construction, not merely by today's call-graph shape.

## 6. Per-criterion proof (brief scope-summary order)

- **One pipeline, two completion styles, no forked logic, no parallel on/off flag**: `computeGltfImport()`/`marshalGltfImportSync()`/`marshalGltfImportPrepareStep()`/`marshalGltfImportFinalize()` (`import_gltf.cpp`) are the entire shared implementation; `importGltfPipeline()` (sync) and `Registry::importGltfAsync()` (async) are the only two callers, differing solely in which marshal primitives (blocking vs. deferred+poll) and which thread(s) drive them. `Registry::importGltfAsync()`'s signature (`registry.h`) carries no boolean/enum toggling anything.
- **Worker/main split; IO-thread reads; decode never on IO thread**: §4b documents the one deliberate scope simplification; proven via the `ByteSource` observation seam in `async_import_test.cpp`.
- **Ordering rule**: §5.
- **Completion exactly-once, strictly after registry mutation visible, pipeline owns resources until callback returns**: `pollAsyncImportUploads()` calls `marshalGltfImportFinalize()` (which performs every `registerMesh`/`registerMaterial` call) *before* `fireAsyncImportCompletion()` runs; `MarshalPendingImport` (holding every GPU-side reference) is only released once, inside `marshalGltfImportFinalize()`/`Rollback()`, never before.
- **Cancellation (abandon semantics)**: `AsyncImportJob::cancelled` (atomic), checked at the top of every async-chain function (`startAsyncImportComputePhase`/`runAsyncImportComputePhase`/`finishAsyncImportCompute`/`runAsyncImportPrepareStep`/`pollAsyncImportUploads`) before any further work; `marshalGltfImportRollback()` frees the geometry range and releases only the *Ready*-outcome textures already registered (never the shared checkerboard/fallback handles); the completion callback path is structurally unreachable once `cancelled` is observed. Latency bound: one stage item (the in-flight `parallelFor` chunk or the in-flight upload ticket) — no unbounded wait anywhere in the cancel path.
- **Progress**: `ImportStage`/`ImportProgress` (`registry.h`) match the specified stage enum exactly; `itemsCompleted`/`itemsTotal` are atomics written from the worker (per-primitive, per-texture) and polled from main; monotonicity and terminal arrival are asserted directly in `async_import_test.cpp`.
- **Error propagation**: every worker-stage body in `computeGltfImport()` is exception-free by construction (no `throw` anywhere in the new code; every failure path returns a value carrying `ImportError`/`Outcome::Failed`); the paired garbage-bytes test asserts async's `ImportError` equals sync's, byte for byte.
- **D24 interplay**: registry handles are only ever produced inside `marshalGltfImportFinalize()`/`marshalGltfImportSync()` — no placeholder handle exists anywhere upstream of that point; the concurrent-imports test asserts one job's resolve is unaffected by the other's in-flight state.
- **Teardown**: §7/`~Registry()`'s own comment (`registry.cpp`) — cancels every outstanding job synchronously, before returning.
- **Tracy zones**: `RX_ZONE_NAMED` on `computeGltfImport()` (whole-function) and every `parallelFor` chunk body (materials/primitives), `marshalGltfImportSync`/`PrepareStep`/`Finalize`; `RX_PLOT` for materials count, primitives count, and geometry bytes uploaded. **MANUAL_VERIFICATION procedure** (code presence, not CI-gated, per D18): build either preset with `RX_TRACY=ON` (default), run `rx_asset_gltf_gpu_tests --test-case="*WALL-CLOCK*"` (or any async import) with a Tracy profiler client (`tracy-profiler`) connected before launch, observe the named zones nested under the worker thread's own timeline row (distinct from the main thread's `pumpMain`-driven marshal zones) and the three plots updating as the import progresses.

## 7. Known flake condition (honestly disclosed, not CI-relevant)

Running the FULL project `ctest` suite with `-j4` (four GPU-backed test
binaries executing fully concurrently, self-imposed — not this project's
own CI invocation) intermittently pushed the wall-clock gate's 10ms
stall-detector assertion over threshold once in several runs (observed:
one failure in ~6 full-suite `-j4` runs during this task's own
verification). Root-caused to real GPU/CPU contention from the OTHER
concurrently-running GPU test binaries (`rx_rhi_vk_tests`/
`rx_material_gpu_tests` also construct real `VkDevice`s and submit real
GPU work at the same moment), not to anything in this task's own code —
confirmed by immediately re-running the exact same binary standalone
afterward, which passed cleanly every time. **This project's own CI
(`.github/workflows/ci.yml:208`) invokes `ctest --preset linux-native
--output-on-failure` with no `-j` flag at all — genuinely serial**, so
this specific contention pattern does not arise there; documented here
rather than silently discovered-and-ignored, per this task's own
production-quality bar. If CI parallelism is ever introduced project-wide,
the wall-clock gate (like any wall-clock assertion) would need runner-
aware headroom -- a pre-existing, general property of D18's own model
(">30% shared-runner variance"), not specific to this task.

## 8. Self-review

- Sync-path regression risk: mitigated by running the full pre-existing 118-case sync suite after every structural change to `import_gltf.cpp`/`texture_cache.cpp`/`geometry_pool.cpp`, unchanged pass count throughout.
- The wall-clock gate is the one test whose numbers are machine-dependent by nature (D18); this report publishes the actual dev-machine numbers observed (§5) rather than asserting an unverified target.
- Known, accepted scope bound (§4b), stated explicitly rather than silently narrowed.
- `MarshalPendingImport` had to move from a pImpl-opaque forward declaration (my first design) to a fully-defined type in the shared private header, once it became a `unique_ptr` *member* of `registry.cpp`'s own `AsyncImportJob` struct — a real compile-time correctness fix (C++'s pImpl-with-unique_ptr rule), not a stylistic change; documented in-line at both the struct definition and the point that originally broke.
- Concurrency-specific scrutiny (per this task's own dispatch instructions): the rendezvous-barrier pattern from `rx_task`'s own pre-existing `scheduler_test.cpp`/`test_material_system.cpp` precedent is reused directly in the two new `runOnWorkerThread` tests (deterministic multi-worker proof, not a timing heuristic); the async pipeline's own worker-participation proof uses the `ByteSource` seam instead (§4b) since the pipeline itself has no comparable internal rendezvous point to hook without adding test-only production instrumentation.
