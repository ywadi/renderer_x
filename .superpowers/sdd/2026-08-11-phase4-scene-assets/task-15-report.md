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

## Fix round 1 — CRITICAL UAF fix (TSAN), Important items, mandatory D24 overlap upgrade

Base for this round: `3a36955` (the commit above). Implementer commits (in order):

1. `2ed0599` — `fix(rx_task): replace per-submission pinned tasks with persistent tasks + closure queues`
2. `80082f3` — `fix(rx_asset): gate async rollback on ticket completion; direct wait/exhaustion asserts; literal D24 overlap test`
3. (this commit) — `docs: task 15 fix round 1 report`

No AI attribution in any commit; author/committer is local git config (`Yousef Wadi <ywadi85@gmail.com>`); nothing pushed; no board/issue/plan/spec/ledger files touched (`git show --stat` on each commit matches the file lists below); a concurrent, unrelated CI-red fix task landed `7cc685f` in `src/rx_rhi_vk` partway through this round — confirmed not a rebase situation (this round's own commits were still entirely uncommitted working-tree changes when `7cc685f` landed, so they simply layered on top of the then-current `HEAD`; the linux-native object files affected by `7cc685f` were already rebuilt on disk, at the same content, before this round's own verification passes ran) and `src/rx_rhi_vk` was not touched by this round's own commits.

### Trigger: independent review verdict

Independent review (full TSAN pass, not merely ASan/UBSan) found the shared two-phase-delete mechanism protecting per-submission `enki::IPinnedTask` objects — originally Task 2's own `IoTask` fix, reused verbatim by this task's `runOnWorkerThread()` — still races under adversarial scheduler churn, 100% reproducible, and traced the exact enkiTS mechanism: `TaskScheduler::RunPinnedTasks()` does `Execute(); m_RunningCount.fetch_sub(...); TaskComplete(pTask_, ...)`, and `TaskComplete()` keeps reading/writing the task object for several more instructions *after* the `fetch_sub` that makes `GetIsComplete()` observe `true`. A reaper gated on that signal (this task's own third gate, added to close the *previous* review's finding) can delete the object while the executing thread is still inside `TaskComplete()` — a genuine use-after-free. The review additionally confirmed, by building the unmodified base-commit `runOnIoThread()`-only design under the same TSAN flags, that the same race class already existed there, inherited from Stage 0's own F1 fix (that audit's TSAN bar was met on the ordinary suite, not under adversarial churn) — not introduced by this task, but this task's `runOnWorkerThread()` addition gave it a second call site.

Design direction evolved twice before implementation started (both preserved here for the record): the initial ruling called for "recycle, don't free" (a same-thread-reaped free-list, deleted only in `~Scheduler()`); a follow-up coordinator/user review then preferred the stronger variant actually implemented — eliminating per-submission enkiTS task objects entirely (persistent tasks + closure queues) rather than pooling their lifecycle, on the grounds that even a carefully-gated free-list still requires a same-thread-reap invariant that is easy to get subtly wrong, where the persistent-task design removes the question structurally (see docs/threading.md's own "why the fix is structurally different, not a fourth gate" reasoning). A user directive arriving mid-round additionally upgraded every finding in the round — including the ones the review itself had framed as minor — to mandatory, in-round closure; nothing in this round was deferred.

### The fix

`src/rx_task/scheduler.cpp` — full rewrite of the pinned-task dispatch mechanism, documented in depth in `docs/threading.md`'s new "Pinned-task dispatch: persistent tasks, not per-submission ones" section (added this round). Summary: exactly two `enki::IPinnedTask` objects exist for a `Scheduler`'s entire lifetime (one pinned to the dedicated IO thread, one pinned to a second dedicated worker-task-lane thread — `config.numTaskThreadsToCreate` is now `workerCount + 2`, not `+1`), both allocated once at `Scheduler::create()` (the only two `AddPinnedTask()` calls left anywhere in the file) and freed only in `~Scheduler()`, strictly after `WaitforAllAndShutdown()` returns (destructor step ordering: stop intake → `ClosureQueue::requestShutdown()` on both queues → `WaitforAllAndShutdown()` → tally any leaked/dropped closures). `runOnIoThread(fn)`/`runOnWorkerThread(fn)` now just push `fn` onto the relevant `ClosureQueue` (mutex + condition_variable + `std::deque<std::function<void()>>`) and return — nothing is registered with enkiTS per submission, and nothing per-submission is ever reaped by anyone. Each persistent task's own `Execute()` loops `waitAndDrain()` → invoke each closure → repeat, until shutdown; every closure is invoked and destroyed on the same thread that drained it, by ordinary RAII — no other thread ever touches it, so there is no completion signal to get right or wrong. The old `IoTask`/`IoLoopTask` classes, `reapPublishedTrash()`, and the round-robin `nextWorkerTaskThread` cursor are all deleted, not merely disabled — there is nothing left to reap, so the "make the cursor atomic" Important item from the review is moot by construction.

Behavioral change, tested: `runOnWorkerThread()` no longer round-robins across every worker thread — it dispatches every closure onto the one dedicated worker-task-lane thread, strictly FIFO. `scheduler_test.cpp`'s corresponding test was rewritten to assert exactly that (`order[i] == i` for 500 submissions, all observed thread ids identical) instead of the old distinct-threads assertion.

A new permanent regression test was added: `TEST_CASE("Scheduler: adversarial churn -- repeated construct/burst-submit-both-primitives/destroy cycles with multiple concurrent submitter threads (TSAN harness; permanent regression test)")` — 100 rounds, each constructing a fresh `Scheduler(2)`, spawning 8 submitter threads that each push 20 IO + 20 worker closures, joining the submitters (deliberately *not* waiting for the closures themselves to run), then immediately destructing the `Scheduler` — racing `WaitforAllAndShutdown()` against in-flight submission/execution on every round. This is the TSAN harness the categorization below was run against, and it stays in the suite permanently (not a scratch tool).

### TSAN evidence — 10/10 runs clean of any report touching this design's own code

Manual procedure (also now the documented one in `docs/threading.md`): `scheduler.cpp`/`scheduler_test.cpp`/`doctest_main.cpp` compiled via a full CMake configure (`--toolchain cmake/toolchains/linux-native.cmake`, `CMAKE_CXX_FLAGS="-fsanitize=thread -O1 -g -fno-omit-frame-pointer"`, `CMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"`, same zig-cxx-linux compiler as the real presets), run 10 times with `TSAN_OPTIONS=halt_on_error=0` so a run keeps going and reports everything rather than stopping at the first hit.

All 10 runs, full tails (every run: 20/20 test cases passed, 35066/35066 assertions passed — the adversarial churn test above is the 20th case, confirming the run covers the current design, not a stale pre-churn-test build):

```
RUN 1:  [doctest] test cases:    20 |    20 passed | 0 failed | 0 skipped
        [doctest] assertions: 35066 | 35066 passed | 0 failed |
        [doctest] Status: SUCCESS!
        ThreadSanitizer: reported 59 warnings
RUN 2:  ... Status: SUCCESS!  ThreadSanitizer: reported 67 warnings
RUN 3:  ... Status: SUCCESS!  ThreadSanitizer: reported 52 warnings
RUN 4:  ... Status: SUCCESS!  ThreadSanitizer: reported 68 warnings
RUN 5:  ... Status: SUCCESS!  ThreadSanitizer: reported 47 warnings
RUN 6:  ... Status: SUCCESS!  ThreadSanitizer: reported 45 warnings
RUN 7:  ... Status: SUCCESS!  ThreadSanitizer: reported 46 warnings
RUN 8:  ... Status: SUCCESS!  ThreadSanitizer: reported 51 warnings
RUN 9:  ... Status: SUCCESS!  ThreadSanitizer: reported 57 warnings
RUN 10: ... Status: SUCCESS!  ThreadSanitizer: reported 77 warnings
```

Total 569 warnings across the 10 runs (test process itself never crashes or fails an assertion in any run — `halt_on_error=0` lets every run finish and report everything TSAN found, by design). Every one of the 569 was categorized by its `SUMMARY: ThreadSanitizer:` line and full stack frames; the breakdown (deduplicated signatures, occurrence counts across all 10 runs):

- **~350 reports**: `std::__1::__function::__value_func<...>::operator()`/vptr-ctor-vs-virtual-call races inside libc++'s own `std::function` type erasure, entered through `Scheduler::parallelFor()`'s lambda dispatch (`scheduler.cpp:372:5`, `enki::TaskSetPartition` handler) and the corresponding `scheduler_test.cpp` parallelFor-callback lambdas. This is `parallelFor()`'s own publish-path noise class, **unchanged code this round** — `parallelFor()` itself was not touched.
- **~130 reports**: doctest-internal (`doctest.h:1642` `StringContains::StringContains`, `doctest.h:2002`/`2007` `Expression_lhs::operator==`/`operator<=`) and `operator delete` — toolchain/test-framework allocator noise, present at comparable density in the base-commit control build below.
- **~85 reports**: `scheduler_test.cpp` lines inside existing rendezvous-barrier/parallelFor test bodies (deliberate, intentional shared-counter touches from multiple threads — the tests' own verification mechanism, pre-existing before this round, unrelated to `ClosureQueue`).
- **3 reports**: `rx::task::(anonymous namespace)::ClosureQueueLoopTask::Execute()` at `scheduler.cpp:159:50` — the `queue_.waitAndDrain()` call site. Inspected all three in full: every one is a "read on the pinned thread's first `Execute()` call (via `enki::TaskScheduler::RunPinnedTasks()`) races the object's own construction on the main thread (`Scheduler::Scheduler()`, `make_unique<ClosureQueueLoopTask>`)" pattern — TSAN cannot see the happens-before edge because it flows entirely through enkiTS's own internal `AddPinnedTask()`/pinned-task dispatch, not through anything TSAN recognizes (a mutex/atomic acquire-release pair). This is the exact same *class* of noise as `parallelFor()`'s own construction-vs-first-dispatch pattern above — structurally, "hand a freshly-constructed object to enkiTS, which runs it on another thread" is enkiTS's single most basic operation; if this pattern were unsafe in practice, every enkiTS user's `AddPinnedTask()`/`AddTaskSetToPipe()` call would be. Confirmed pre-existing, same shape, on the OLD per-submission `IoTask::Execute()`/`fn_()` call (base-commit control build, 24 occurrences across 10 runs — see below) — and confirmed the new design cuts this noise class's frequency drastically, since construction now happens (at most) twice per `Scheduler` lifetime instead of once per submission.
- **0 reports** touch `ClosureQueue::push()`, `ClosureQueue::waitAndDrain()`'s own mutex/condvar/deque state, `ClosureQueue::requestShutdown()`, `ClosureQueue::sizeForTesting()`, `~Scheduler()`, `WaitforAllAndShutdown()`, or any delete/free-vs-still-executing pattern — i.e., zero reports resembling the original CRITICAL finding in any way, across 10 full runs of the adversarial churn harness plus the full suite.

**Base-commit control build** (methodology requested by the review, to distinguish "pre-existing enkiTS/toolchain noise" from "this round's own code"): the unmodified pre-Task-15 `runOnIoThread()`-only `scheduler.cpp`/`scheduler_test.cpp` at commit `55b4822`, built under the identical TSAN flags, run 10 times:

```
RUN 1:  15/15 passed, 33942/33942 assertions, Status: SUCCESS!  82 warnings
RUN 2:  ... SUCCESS!  87 warnings
RUN 3:  ... SUCCESS!  66 warnings
RUN 4:  ... SUCCESS!  78 warnings
RUN 5:  ... SUCCESS!  75 warnings
RUN 6:  ... SUCCESS!  93 warnings
RUN 7:  ... SUCCESS!  79 warnings
RUN 8:  ... SUCCESS!  64 warnings
RUN 9:  ... SUCCESS!  61 warnings
RUN 10: ... SUCCESS!  68 warnings
```

Total 753 warnings across 10 base-commit runs (all 15/15 passing — this is *unmodified, non-adversarial* code; even ordinary `Scheduler` usage produces this volume of TSAN noise on this toolchain purely from the same-class publish-path patterns above). Same signature families as the fixed build (`std::function`/vptr, doctest-internal, `scheduler_test.cpp` rendezvous bodies), *plus* 24 occurrences of `scheduler.cpp:111:40` inside the OLD `IoTask::Execute()`'s own `fn_();` call — the base commit's own instance of the exact "construction vs. first-dispatch" pattern discussed above, now on a per-submission object instead of a per-Scheduler-lifetime one. This is the review's own already-recorded finding (identical race independently reproduced on the unmodified base commit) corroborated directly here, not merely cited.

**Honest note on evidence provenance**: an earlier TSAN pass during this round was run before `scheduler_test.cpp`'s adversarial churn test existed (19 test cases, not 20) and its full per-run logs were not preserved in a form suitable for re-categorization here. The 10-run sets pasted above were re-run fresh, against the exact current source (confirmed via the 20-test-case count in every tail) immediately before writing this section, specifically to avoid citing stale evidence.

### ASan/UBSan cross-check

Separate full CMake configure (`CMAKE_CXX_FLAGS="-fsanitize=address,undefined -O1 -g -fno-omit-frame-pointer"`), same toolchain:

- `rx_task_tests`: 3/3 runs, exit 0, 20/20 cases, 35066/35066 assertions, zero `ERROR: AddressSanitizer` / `ERROR: LeakSanitizer` / `runtime error:` / `UndefinedBehaviorSanitizer` matches in any log.
- `rx_asset_gltf_gpu_tests` (`--test-case-exclude="*WALL-CLOCK*"`): 53/53 cases, 590,687/590,687 assertions, zero sanitizer defects. The WALL-CLOCK GATE test alone, run under the same ASan/UBSan build, fails only its own `REQUIRE(pumpDuration < 10ms)` timing assertion (no sanitizer report at all) — the same pre-existing, documented sanitizer-instrumentation-overhead exclusion as §3 of this report's original section (unmodified, unrelated to this round).

### Important items — resolution

- **`docs/threading.md`'s D5 contract now names `runOnWorkerThread()`** as a third worker-allowed context (alongside `parallelFor()` chunks and `runOnIoThread()`), and the new "Pinned-task dispatch" section is cross-referenced from "The handoff pattern".
- **`nextWorkerTaskThread` atomic — MOOT.** The round-robin cursor no longer exists; `runOnWorkerThread()` dispatches onto the one persistent worker-task-lane thread via the closure queue, nothing to make atomic.
- **`wait-calls-from-async-path == 0` direct assertion**: `GeometryPool::waitCallCountForTesting()` / `TextureCache::waitCallCountForTesting()`, incremented at the one call site in each class that still calls `Uploader::wait()` (§ commit message above). Added in `rx_asset`'s own layer, per the revoked `rx_rhi_vk` accessor authorization. Asserted directly in the WALL-CLOCK GATE test (baseline captured before the import, unchanged asserted after).
- **Ring/pool no-exhaustion direct check**: `GeometryPool::stats()`/`blockCount()` before (asserted zero on the fresh fixture) and after (asserted `blockCount >= 1`, `vertexBytesUsed`/`indexBytesUsed` both `> 0` and `<= capacity`) the WALL-CLOCK GATE import.
- **Cancel-mid-upload rollback test**: added (`async_import_test.cpp`, "cancelImport() after >=1 real GPU resource (texture AND geometry) is already registered rolls BOTH back"). Forces cancellation after `GeometryPool::stats().blockCount > 0` (which, by `marshalGltfImportPrepareStep()`'s own ordering, guarantees every texture slot is already registered too), asserts both a geometry-bytes-used delta back to zero and a `liveTextureCountForTesting()` delta back to baseline (after `deletionQueue.onFrameFenceSignaled(0)`, preceded by `vkDeviceWaitIdle()` — see below), ASan-clean (part of the 53/53 clean run above). **This test found and drove a real production fix**, not just a passing assertion — see the commit message on `80082f3` and the "genuine bug found" note below.
- **D24 concurrent-resolve test — literal overlap, not reworded** (mandatory upgrade, message b): rewritten from "two overlapping imports, D24 note only" into "a resolve-heavy loop against an ALREADY-COMPLETED import's handles runs correctly and repeatedly WHILE a second, independent async import is PROVABLY still in flight" — Import A is driven fully to completion first; Import B is held open via `SlowRecordingByteSource`; the test polls B's `importProgress()` fresh on every loop iteration and only counts/asserts a resolve when B's own progress proves it is not yet terminal; `CHECK(overlapResolves >= 20)` proves sustained, genuine overlap rather than one lucky iteration.

### Genuine bug found and fixed this round (disclosed prominently, per this task's own precedent)

Writing the cancel-mid-upload test surfaced a real defect in the *original* Task 15 rollback path (not this round's rx_task fix): `runAsyncImportPrepareStep()`/`pollAsyncImportUploads()` called `marshalGltfImportRollback()` unconditionally the instant `cancelled` was observed, with no regard for whether the upload tickets `marshalGltfImportPrepareStep()` had already issued were actually complete on the GPU. First run of the new test reproduced this as a real, validation-layer-confirmed `vkDestroyImage(...)` "in use by a command buffer" error — not a test artifact (confirmed by first trying `vkDeviceWaitIdle()` alone in the test as a workaround, which fixed the *symptom*, then tracing the actual cause and fixing it at the source instead). Two real hazards existed: (1) `GeometryPool::free()` returns the freed suballocation range to the block's TLSF metadata for *immediate* reuse by the very next `upload()`/`uploadDeferred()` call — a write-after-write race against the still-in-flight old copy if the old copy had not actually finished; (2) `TextureCache::releaseUnpublished()`'s deferred `DeletionQueue` reclaim could destroy a `VkImage` a still-in-flight transfer command buffer was writing into. Fixed with `rollbackAsyncImportWhenSafe()` (`registry.cpp`): polls `marshalGltfImportUploadsComplete()` (D25's own non-blocking poll, the same primitive the non-cancelled path already uses) and only calls `marshalGltfImportRollback()` once every ticket prepare() issued has genuinely finished — never a blocking wait, re-posting via `postToMain()` exactly like the ordinary poll loop. The `vkDeviceWaitIdle()` retained in the test before its own `onFrameFenceSignaled(0)` call is therefore redundant-but-cheap extra insurance for the test's own subsequent `DeletionQueue`-driven destroy, not a workaround for a still-open bug.

**Known, untested, narrower residual concern** (disclosed, not fixed this round — no test reaches it, and reaching it safely likely requires understanding `rx_rhi_vk`'s `Uploader` batching internals more deeply than this round's `rx_rhi_vk`-off-limits constraint allows): if cancellation lands *mid* per-texture-slot registration — after `TextureCache::registerDecoded()` has been called for some slots but *before* `flushPendingUploads()` has issued a ticket for that batch — `marshalGltfImportUploadsComplete()` trivially returns true for the not-yet-issued texture ticket (nothing to wait for), so `rollbackAsyncImportWhenSafe()` would release those handles immediately. Whether that specific narrow window is safe depends on whether `Uploader` ever coalesces a not-yet-flushed recorded copy command with a *later*, unrelated ticket's flush — if so, destroying the target image first would leave a dangling reference in that later flush. No test in this repository (before or after this round) exercises this exact window; flagged here rather than silently left undiscovered.

### Full re-verification

- **linux-native, serial** (`ctest --preset linux-native --output-on-failure -R "rx_task_tests|rx_asset_gltf_gpu_tests"`, no `-j`, matching CI's exact invocation): 100% passed, run 4 times in a row for flake-freedom (`rx_task_tests` ~1.0-1.4s, `rx_asset_gltf_gpu_tests` ~38-43s each run). Direct binary run: `rx_task_tests` 20/20 cases, 35,066/35,066 assertions; `rx_asset_gltf_gpu_tests` 54/54 cases (up from 53 — the new cancel-mid-upload test), 8,381,004/8,381,004 assertions.
- **windows-cross-zig + Wine** (device-free only, per this task's own established posture): `rx_task_tests.exe` 20/20 passed including the adversarial churn test; `rx_asset_gltf_tests.exe` 48/48 passed (device-free target, does not link `rx_rhi_vk` or exercise the async pipeline — confirms `rx_asset`'s library changes still cross-compile and run correctly).
- Confirmed no rebase was needed against the concurrent `7cc685f` (`rx_rhi_vk`) landing — see this section's own opening paragraph.

### Self-review / concerns

- The design actually implemented (persistent tasks + closure queues) is the coordinator/user-preferred *stronger* variant, superseding both the initial "recycle, don't free" ruling and an interim "sequence-number-gated same-thread reclaim" design considered but not implemented once the stronger direction arrived first.
- TSAN evidence in this section was regenerated from scratch immediately before writing it, specifically because an earlier in-round TSAN pass was found (by its own 19-vs-20 test-case tail) to predate the adversarial churn test — see the "Honest note on evidence provenance" above. Citing that earlier pass would have been evidence for a slightly different (less adversarial) build; regenerating was the more defensible choice, not the more convenient one.
- The genuine rollback bug this round's own new test surfaced (previous section) was not something any review flagged explicitly — it was found by actually exercising the cancel-mid-upload path for the first time, which is itself the argument for why the mandatory-upgrade policy (no deferred fixes) is the right posture: a test written only to satisfy a checklist item ended up finding a real defect once actually run against real GPU resources.
- The narrower mid-registration rollback window (previous section) is left as an honestly-disclosed, untested gap rather than force-fixed within `rx_rhi_vk`'s off-limits boundary this round — flagged for a follow-up card rather than silently narrowed.
- Sync-path regression risk: `marshalGltfImportRollback()`/`marshalGltfImportPrepareStep()`/`marshalGltfImportUploadsComplete()` themselves are unchanged this round (only their call sites in `registry.cpp` changed, and only on the async abandon path) — the sync path (`marshalGltfImportSync()`) does not call any of this round's new code at all.

## Fix round 2 — a second CRITICAL rollback bug, closing round 1's own disclosed residual, and a test that was masking its own target

Base for this round: `68e2208` (round 1's own report commit). Implementer commits (in order):

1. `aef0b08` — `docs(rx_task): fix stale runOnWorkerThread comment (round-robin -> single lane)`
2. `92f34d2` — `fix(rx_asset): close two more rollback hazards -- shared checkerboard destruction and unticketed texture release`
3. (this commit) — `docs: task 15 fix round 2 report`

No AI attribution in any commit; author/committer is local git config (`Yousef Wadi <ywadi85@gmail.com>`); nothing pushed; no board/issue/plan/spec/ledger files touched. `src/rx_rhi_vk` was open to this round (the `7cc685f` CI-red fix landed and was approved before this round started) but was not touched — every fix this round landed in `rx_asset`, as the review itself anticipated was the likely outcome.

### Trigger: independent re-review of round 1

Re-review confirmed the CRITICAL (persistent-task/closure-queue) fix cleanly addressed — an independent 6-run TSAN pass found zero reports touching `ClosureQueue`/teardown/delete-vs-executing, corroborating this report's own categorization against a base-commit control build; destructor ordering, same-thread closure free, and the process-fatal contract were all verified against the full source. The D24 literal-overlap test was confirmed genuinely discriminating (7/7 runs, 8.79M assertions). Two new, more serious findings forced this round, plus one that turned out to be this round's own doing (item 2 below), plus a documentation nit (item 4):

### ITEM 1 (CRITICAL) — cancelling mid-registration could destroy the shared checkerboard fallback

**Mechanism** (review's own trace, independently reproduced here): `computeGltfImport()`'s `fillRef()` lambda (`import_gltf.cpp`) pre-sets every PRESENT texture ref's handle to the shared checkerboard fallback at COMPUTE time (worker thread) — this is the value every renderer-facing consumer would see if this import never finished. `marshalGltfImportPrepareStep()` overwrites that placeholder with a real (or failure-fallback) handle only when THIS slot's own marshal turn arrives, one slot per `pumpMain()` tick. `marshalGltfImportRollback()`, however, selected its release targets by COMPUTE-time state alone (`slot.attempted && decoded.outcome == Ready`) — with no check on whether marshal had actually reached that slot. A cancellation caught between two slot registrations (ordinary timing, any multi-texture asset, no adversarial timing needed) therefore called `TextureCache::releaseUnpublished()` on every NOT-YET-MARSHALLED present slot too, whose handle was still the shared checkerboard — releasing (and, once the deletion queue's reclaim ran, destroying) the ONE fallback texture every other still-live import and every fallback-rendered material in the whole `TextureCache` depends on.

**Fix**: `PendingTextureLoad` (`import_pipeline.h`) gained a `registered` flag, set only at the actual `registerDecoded()` call site inside `marshalGltfImportPrepareStep()` — true only once marshal has genuinely reached and processed this slot. `marshalGltfImportRollback()`'s release loop now gates on `slot.registered` (not `slot.attempted`) as its PRIMARY condition, with `decoded.outcome == Ready` still required separately (a registered-but-Failed/Checkerboard-outcome slot's handle is `applyDecodeResult()`'s own shared failure/checkerboard fallback too, not a per-import allocation — both gates are independently necessary).

**Covering test** (`async_import_test.cpp`, "cancelImport() after EXACTLY 1 of N texture slots has been registered..."): drives DamagedHelmet's async import (5 present, real texture slots) one `pumpMain()` tick at a time until `liveTextureCountForTesting()` first becomes EXACTLY baseline+1 (a precise, non-approximated "1 of 5 registered, 4 still ahead" checkpoint — only a `Ready`-outcome `registerDecoded()` call increments live count at all, per `TextureCache::applyDecodeResult()`), cancels, drains, and asserts `liveTextureCountForTesting()` never drops below baseline at any point and equals baseline exactly after the real reclaim; separately resolves the checkerboard handle itself afterward and checks `isFallback`/`resident`/`width`/`height` are all still exactly what they always were.

**Revert evidence** (scratch, in-place edit reverted immediately after collecting evidence — `git diff` confirmed clean before and after): reverting the rollback loop's gate from `slot.registered` back to `slot.attempted` reproduced the bug on 6/6 runs. Exact assertion values from one run (`-s` verbose mode):

```
/rx_asset/tests/async_import_test.cpp:1067: ERROR: CHECK( fixture->textures->liveTextureCountForTesting() == liveTexturesBefore ) is NOT correct!
  values: CHECK( 3 == 4 )

/rx_asset/tests/async_import_test.cpp:1074: ERROR: CHECK( cb.resident ) is NOT correct!
  values: CHECK( false )
```

Baseline 4 (checkerboard + 3 role fallbacks), dropped to 3 — one MORE release than the single real texture actually registered — and the checkerboard's own record came back non-resident, both exactly matching the mechanism above. A `VUID-vkDestroyImage`-class validation error did NOT reproduce in this repository's specific 6 additional revert runs (nothing else in this test's own scope happens to touch the destroyed bindless slot afterward) — the `liveTextureCountForTesting()`/`resident` evidence above is the reliable, deterministic proof this report relies on, not an incidental validation-layer catch; disclosed honestly rather than overclaiming a VUID that this specific reproduction did not, in fact, produce. With the fix restored: 8/8 clean runs, `--validate` included, zero `[error]`/`FATAL`/`FAILURE` lines in any of them.

### ITEM 2 — the round-1 cancel-mid-upload test was not discriminating its own fix

Independent review reverted `rollbackAsyncImportWhenSafe()` (round 1's own ticket-completion gate) back to an unconditional, immediate `marshalGltfImportRollback()` call and found the round-1 covering test still passed 8/8 — because that test's own `vkDeviceWaitIdle()`, called immediately before the `DeletionQueue` reclaim it was asserting against, drains EVERY in-flight GPU submission (including the still-racing old copy) regardless of what the production code did, masking the race entirely.

**Fix**: removed the `vkDeviceWaitIdle()` call from the test's assertion path. Now, with the ticket-completion gate intact, `onFrameFenceSignaled(0)` runs immediately after the rollback-drain loop with NO additional wait of any kind — safe only because `rollbackAsyncImportWhenSafe()` itself already confirmed every ticket complete first. Ordinary fixture teardown (`Device::~Device()`'s own unconditional `vkDeviceWaitIdle()`) remains the safety net for anything this call does not reclaim; nothing in the test relies on a manual wait for that.

**Revert evidence, both directions, freshly re-collected against the restructured test**:

- Reverted (`rollbackAsyncImportWhenSafe()` bypassed, direct unconditional `marshalGltfImportRollback()` call): 6/6 runs FAILED, every one reproducing the exact original VUID:
  ```
  [error] [vulkan validation] Validation Error: [ VUID-vkDestroyImage-image-01000 ]
  ... Cannot call vkDestroyImage on VkImage 0x220000000022[] that is currently in use by a command buffer.
  /rx_asset/tests/async_import_test.cpp:940: ERROR: CHECK_FALSE( fixture->context.hasValidationErrors() ) is NOT correct!
  [doctest] Status: FAILURE!
  ```
- Intact (fix restored, `git checkout` confirmed byte-identical to the committed state): 8/8 clean runs, `[doctest] Status: SUCCESS!` every time, zero validation errors.

This is now the actual discriminating test the brief always intended it to be.

### ITEM 3 — closing round 1's own disclosed residual for real

Round 1's report explicitly disclosed, as an untested, narrower residual concern: a texture slot can be REGISTERED (`TextureCache::registerDecoded()` already called, a real GPU-side copy command recorded) without yet having a TICKET (`TextureCache::flushPendingUploads()` not yet called — `marshalGltfImportPrepareStep()` yields after each individual registration, and the texture ticket is only issued once EVERY slot is done). `marshalGltfImportUploadsComplete()` has nothing to poll in that state and reports "nothing outstanding" — incorrectly, since the recorded copy has not even been submitted yet. Independent review confirmed this half is real (not merely theoretical) and narrower than ITEM 1.

**Characterization, precisely**: this window exists ONLY on the texture side. Geometry's own `marshalGltfImportPrepareStep()` branch calls `pool.uploadDeferred()` and `pool.flushPendingUploads()` together, unconditionally, inside the SAME function call with no `return`/yield between them — `pending.hasGeometry == true` and `pending.poolTicketIssued == false` can never coexist once prepare() has run at all. This IS provably unreachable by construction, not merely by observed behavior.

**Empirical reproduction**: this round's own new ITEM 1 covering test (cancel after exactly 1 of 5 texture slots) lands in exactly this window by construction (the texture ticket is never issued until all 5 slots are done, so cancelling after slot 1 is ALWAYS pre-ticket) — the first attempt at that test, with ITEM 1's fix already in place but ITEM 3's not yet, reproduced a real `UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkImage` validation error ("You are adding vkEndCommandBuffer() to VkCommandBuffer ... that is invalid because bound VkImage ... was destroyed") on 6/6 runs, plus a `liveTextureCountForTesting()` mismatch caused by an unrelated bug in the TEST's own rollback-drain loop (an early-exit heuristic that could not actually tell "rollback ran" from "cancellation was merely observed" on the texture side, unlike the geometry side's own reliable `GeometryPool::stats()` signal) — both are disclosed and fixed together below, kept distinct so neither masks the other's own evidence.

**Fix**: `marshalGltfImportEnsureRollbackTicketed(TextureCache*, MarshalPendingImport&)` (`import_pipeline.h`/`import_gltf.cpp`) — idempotent, called unconditionally as the FIRST step of every `rollbackAsyncImportWhenSafe()` invocation (`registry.cpp`), before the existing `marshalGltfImportUploadsComplete()` poll. If a texture batch was registered but never ticketed, it forces a real ticket into existence by calling `flushPendingUploads()` early (D25's own non-blocking flush; nothing about calling it ahead of the "normal" once-per-batch point is unsound — flush()'s contract is "submit whatever is recorded so far," not "only once, at the very end"). Once a real ticket exists, the existing poll-then-rollback machinery handles the rest unchanged.

**Enforced, not just documented**: both halves of this invariant class (geometry's provably-unreachable case, texture's caller-contract-enforced case) are backed by `checkRollbackTicketInvariant()`, a loud-failure check inside `marshalGltfImportRollback()` itself — deliberately NOT a bare `assert()` (this project's presets compile with `-DNDEBUG` in every configuration including CI, which would silently compile a bare `assert()` away entirely, exactly the failure mode `rx_core/debug_checks.h`'s own top comment documents at length for the same reason `RX_ASSERT_MAIN_THREAD` is not a bare `assert()` either). Gated by `RX_DEBUG_CHECKS` instead (this engine's own always-available switch, ON by default in both dev presets, independent of `NDEBUG`): on a violation, logs and calls `std::abort()`.

**Revert evidence for the enforcement itself**: temporarily commenting out the `marshalGltfImportEnsureRollbackTicketed()` call site in `rollbackAsyncImportWhenSafe()` (fix restored immediately after) reproduced an immediate, unambiguous, loud failure on 3/3 runs — not a silent corruption, not a flaky validation error, a deterministic crash with a named cause:

```
[error] rx_asset: marshalGltfImportRollback: invariant violated -- texture upload(s) registered without an
issued ticket -- call marshalGltfImportEnsureRollbackTicketed() before marshalGltfImportRollback()
/rx_asset/tests/async_import_test.cpp:954: FATAL ERROR: test case CRASHED: SIGABRT - Abort (abnormal termination) signal
[doctest] test cases:   1 |   0 passed | 1 failed | 54 skipped
```

With the fix (and the test's own settle-window rewrite, see below) both restored: 8/8 clean runs of the ITEM 1 covering test, `--validate` included, zero `[error]`/`FATAL` lines.

**The test's own bug, fixed alongside**: the ITEM 1 covering test's original rollback-drain loop broke out early on `cancelled == true && liveTextureCountForTesting() == baseline+1` — but on the texture side, `liveTextureCountForTesting()` does not change AT ALL until the later `onFrameFenceSignaled()` call, so that condition is true from the moment cancellation is first observed, regardless of whether `rollbackAsyncImportWhenSafe()`'s own ticket-completion poll has run even once. Replaced with a fixed, generous settle window (200 extra `pumpMain()` ticks driven after cancellation is first observed, well beyond what a single small JPEG decode's own GPU completion needs) — the geometry-side cancel-mid-upload test's own early-exit pattern remains correct as-is, since `GeometryPool::stats().vertexBytesUsed` dropping to zero IS a synchronous, reliable "`pool.free()` has now actually run" signal with no texture-side equivalent.

### Item 4 (doc) — stale `runOnWorkerThread()` comment

`scheduler.h`'s own doc comment for `runOnWorkerThread()` still described the PRE-fix-round-1 design (round-robin dispatch over the ordinary `parallelFor()` worker pool, "FIFO ordering across different target threads is not guaranteed", a reference to the since-deleted `IoLoopTask`) even though round 1's own commit message and `docs/threading.md` already documented the real, current mechanism. Rewritten to describe the single dedicated worker-task-lane thread, its FIFO contract, the corrected thread-count claim (`numTaskThreadsToCreate` is `workerCount()+2`, not `+1` — a new thread genuinely IS created once, at construction, contradicting the old comment's "no new thread is created" claim), and a pointer to `docs/threading.md`'s own "Pinned-task dispatch" section for the full mechanism.

### Full re-verification

- **linux-native, serial** (`ctest --preset linux-native --output-on-failure -R "rx_task_tests|rx_asset_gltf_gpu_tests"`, no `-j`): 100% passed, 3 runs in a row. Direct binary run: `rx_task_tests` 20/20 cases, 35,066/35,066 assertions (unchanged from round 1 — this round did not touch `scheduler.cpp`/`scheduler_test.cpp`, only a header comment); `rx_asset_gltf_gpu_tests` 55/55 cases (up from 54 — the new ITEM 1 covering test), 8,600,980/8,600,980 assertions.
- **ASan/UBSan**: fresh full rebuild (same `-fsanitize=address,undefined -O1 -g -fno-omit-frame-pointer` configure as round 1). `rx_task_tests`: 20/20, 35,066/35,066 assertions, zero sanitizer defects. `rx_asset_gltf_gpu_tests` (`--test-case-exclude="*WALL-CLOCK*"`, same pre-existing documented sanitizer-timing exclusion as before): 54/54, 603,067/603,067 assertions, zero sanitizer defects.
- **windows-cross-zig + Wine** (device-free only): `rx_task_tests.exe` 20/20 passed; `rx_asset_gltf_tests.exe` 48/48 passed.
- Confirmed no new `src/rx_rhi_vk` commit landed during this round (`HEAD` unchanged from round 1's own final commit until this round's own commits landed) — no rebase needed.

### Self-review / concerns

- ITEM 1 is the second CRITICAL-severity rollback defect this task has now shipped-then-fixed-in-round (the first being round 1's own ticket-completion gate). Both were found by tests written specifically to exercise the abandon/rollback path with real GPU resources already registered, not by static reasoning alone — reinforcing that this path's own state space (compute-time placeholder vs. marshal-time real value, registered-vs-ticketed) is subtle enough that only executing it for real, under adversarial-but-realistic timing, reliably finds its bugs.
- ITEM 2's own finding — that a test's own "extra safety" wait can silently defeat its discriminating purpose — is now a concrete, reproducible example worth remembering for any FUTURE GPU-resource-lifetime test in this codebase: a blanket `vkDeviceWaitIdle()` (or any device-wide barrier) placed between "the code under test does its thing" and "the assertion checks the result" should be treated as a discrimination hazard by default, not a harmless safety margin, unless the test has specifically verified (as this round now has, for this one test) that removing it does not change the pass/fail outcome for the CORRECT implementation.
- ITEM 3's fix is deliberately narrow (texture-only, matching the precisely-characterized reachable window) rather than a broad "always flush everything eagerly" change that would have been simpler to reason about but would have reintroduced exactly the RC6 wall-clock regression this task's own original wall-clock-gate test was built to catch (an early, oversized flush is closer in shape to the reverted "register everything in one synchronous call" regression than to the time-sliced design this task shipped).
- `checkRollbackTicketInvariant()` is new, narrowly-scoped, rx_asset-local infrastructure (not a new shared `rx_core` macro) — deliberately kept local rather than generalized into a project-wide assertion facility, since that would be a larger architectural change than this round's mandate covers; if a similar need arises elsewhere, `rx_core/debug_checks.h`'s own `RX_DEBUG_CHECKS`-gated pattern is the precedent to follow, and this function's own comment says so.
- The narrower residual disclosed at the end of round 1's own section (mid-registration cancellation before a ticket is issued) is now CLOSED by this round's ITEM 3 fix — that disclosure is superseded, not still open.
