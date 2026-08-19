# RendererX Threading Contract (D5)

Phase 4 introduces the engine's first real multi-threaded work (parallel
asset import/decode, parallel culling and draw-list building, parallel
secondary command recording) via `rx::task::Scheduler`
(`src/rx_task/include/rx_task/scheduler.h`), the engine's sole task
scheduler [spec D2]. This document is the contract every subsystem —
existing and future — is held to as that work lands, and it is what every
new public header's one-line thread-affinity note (see the rule at the
bottom of this file) points back to. Several of the concrete
main-thread-only mutators below (`BindlessTable`/`Uploader`/
`MaterialSystem`/`DeletionQueue::retire()`) now back this contract with a
dev-time `RX_ASSERT_MAIN_THREAD` runtime guard [Phase 4 Task 7 fix round 1,
`rx_core/include/rx_core/debug_checks.h`] — a call from a chunk >= 1 fails
loudly (logged, then `std::abort()`) instead of silently corrupting shared
state; see "Main-thread-only" below for exactly which methods.

## The rule

**GPU-object mutation stays main-thread-only.** None of the subsystems
listed below grow locks in Phase 4. Workers do pure CPU work and hand
results back to the main thread through `rx::task::Scheduler`'s
`postToMain()`/`pumpMain()` handoff. This is a deliberate, explicit scoping
decision, not an oversight: retrofitting concurrency into already-reviewed
Phase 2/3 subsystems is high-risk and low-need for what Phase 4 actually
requires, and the handoff pattern below fully serves every asset-loading
use case this phase has. Truly concurrent GPU-object creation (e.g. several
threads each submitting to their own queue) is a streaming-phase design,
explicitly deferred.

## Main-thread-only

Call these ONLY from the thread that owns the `VkDevice`/render loop (in
`rx::task::Scheduler` terms: the thread that called `Scheduler::create()`,
i.e. the thread that also calls `pumpMain()`). Entries marked **[guarded]**
carry a dev-time `RX_ASSERT_MAIN_THREAD` runtime check [Phase 4 Task 7 fix
round 1] at the very top of the named method(s): a call from any other
thread logs an ERROR naming the API and calls `std::abort()` rather than
silently mutating shared state from the wrong thread. The guard is compiled
in whenever `RX_DEBUG_CHECKS` is ON (default in both dev presets,
independent of `NDEBUG` — see `rx_core/include/rx_core/debug_checks.h`'s
own header comment for why a bare `assert()` would not have worked here)
and compiles to nothing at all when it is OFF:

- **`rx::rhi::BindlessTable`** — `registerSampledImage()`/
  `registerSampler()`/`registerStorageBuffer()`/`release()` **[guarded, all
  four]**
  (`src/rx_rhi_vk/include/rx_rhi_vk/bindless.h`)
- **`rx::rhi::Uploader`** — `uploadToBuffer()`/`uploadToImage()`/`flush()`/
  `isComplete()`/`wait()` **[guarded, all five]** [Phase 4 Task 11, spec
  D25 as amended by gate ruling RC4]: `flush()` gained its guard this task
  because it is no longer reachable only via an already-guarded
  uploadTo*() call — `flush()` with nothing recorded (returning an
  already-complete `UploadTicket`) is now a legitimate standalone call, so
  the old "always called from within an already-guarded call" reasoning no
  longer holds. `isComplete()`/`wait()` are new this task; both poll/wait
  on this Uploader's own timeline semaphore, which the Vulkan spec itself
  permits from any thread with no external synchronization
  (`vkGetSemaphoreCounterValue`/`vkWaitSemaphores` need none) — the guard
  here is this project's own D5 policy scoping, matching every other
  Uploader entry point's identical scoping, not a hazard these two
  introduce. `flush()` now returns a pollable `UploadTicket` instead of
  blocking; existing callers that need the old always-blocking contract
  (e.g. `MeshBuffers::create()`) call `wait()` on the returned ticket
  explicitly.
  (`src/rx_rhi_vk/include/rx_rhi_vk/upload.h`)
- **`rx::material::MaterialSystem`** — `loadMaterial()`/`getPipeline()`/
  `reloadChanged()`/`bindInstance()` **[guarded, all four]** (and every
  other method on this type — it is not internally synchronized at all,
  matching `rx::graph::RenderGraph`/`Executor`'s existing scoping)
  (`src/rx_material/include/rx_material/material_system.h`)
- **`rx::rhi::DeletionQueue`** — `retire()` **[guarded]**/
  `onFrameFenceSignaled()`/`flushAll()` (not guarded — the per-frame
  drain and shutdown paths, out of this fix round's scope; `retire()` is
  the one enqueue-style mutator a chunk >= 1 caller could plausibly reach
  mid-frame) (`src/rx_rhi_vk/include/rx_rhi_vk/deletion_queue.h`)
- **`rx::rhi::Allocator`** [Phase 4 Task 10, spec D24] —
  `setCurrentFrameIndex()`/`report()` (not guarded — no shared mutable
  state a chunk >= 1 caller could corrupt, since `MemoryAccounting`'s own
  counters are atomic; the main-thread-only convention here is about
  matching every other Allocator/Buffer/Texture2D GPU-object-mutation
  entry point's existing scoping, not a new hazard) — call
  `setCurrentFrameIndex()` once per frame (`FrameSync::advanceFrame()`
  does this automatically when passed this Allocator's address) so
  `report()`'s per-heap budget numbers stay fresh (see `memory_report.h`'s
  own comment on the `vmaSetCurrentFrameIndex()` staleness rule).
  (`src/rx_rhi_vk/include/rx_rhi_vk/buffer.h`)
- **`rx::asset::GeometryPool`** [Phase 4 Stage 1 Task 12, spec D9] —
  `create()`/`upload()`/`free()`/`bind()`/`stats()`/`blockCount()`/
  `bufferDeviceAddressEnabled()`/`vertexBufferDeviceAddress()`/
  `indexBufferDeviceAddress()` **[guarded, all nine]**: suballocation is
  a main-thread-owned `VmaVirtualBlock` operation (VMA's own
  virtual-block API documents itself as "not thread-safe... must be
  synchronized externally" — vendored `vk_mem_alloc.h`, and that
  requirement covers reads too, not just the mutators), same rationale
  as the four Allocator-derived types above. **[Fix round 1, review
  finding]** the read accessors were originally documented (incorrectly)
  as "safe from any thread holding a valid reference" — `stats()`/
  `blockCount()`/etc. read the SAME `blocks_`/`VmaVirtualBlock` state
  `upload()`/`free()` mutate unlocked, with no atomic counters the way
  `rx::rhi::Allocator::report()` has to justify an any-thread claim;
  narrowed to main-thread-only and guarded, matching D5's posture for
  the whole class. The test/diagnostic-only accessors
  (`vertexBufferAllocatedBytes()`/`indexBufferAllocatedBytes()`/
  `vertexBufferHandle()`/`indexBufferHandle()`) carry the identical
  guard for the same reason, though production code has no need for
  them. (`src/rx_asset/include/rx_asset/geometry_pool.h`)
- **`rx::scene::*Manager` registries** (future, Stage 2) —
  `RenderableManager`/`TransformManager`/`LightManager` mutation
  (create/set/destroy) stays main-thread-only; read-only SoA traversal for
  culling (below) does not. Not guarded — does not exist yet.
- **`rx::platform::Window`** [Phase 4 Task 20, gate ruling #14] —
  `pumpEvents()`/`setRelativeMouseMode()`/`relativeMouseModeWanted()`/
  `consumeMouseDelta()`/`setCursorVisible()`/`cursorVisible()`/
  `setCursorConfined()`/`cursorConfined()`/`poll()`/`isKeyDown()`
  **[guarded, all ten]** — the full input surface this task added, plus
  `pumpEvents()` itself (pre-existing, previously documented as
  main-thread-only only by comment, never enforced — now backed by the
  same runtime check as every other entry here). Matches D5's whole-class
  posture (`rx::asset::GeometryPool`'s precedent above): several of these
  (`isKeyDown()`, the gamepad axis/button reads inside `poll()`) are
  individually documented safe-from-any-thread by SDL3's own header, but
  the guard here is this project's own D5 policy scoping for one
  predictable contract across the whole class, not a hazard those
  particular calls introduce on their own — the same reasoning
  `rx::rhi::Uploader::isComplete()`/`wait()` already established above.
  `setFullscreen()`/`isFullscreen()` (Task 17) are NOT guarded — out of
  this task's touched-surface scope, unchanged. (`src/rx_platform/include/rx_platform/window.h`)
- **`rx::debug_ui::Overlay`** [Phase 4 Stage 2 Task 21, spec D20, gate
  ruling #16] — `create()`/`processEvent()`/`beginFrame()`/`addPass()`
  **[guarded, all four]**. Stricter than the usual "GPU-object mutation"
  framing above: this whole class wraps a single, process-global
  `ImGuiContext` (Dear ImGui is single-threaded by design, no internal
  synchronization at all), so every entry point is main-thread-only, not
  just the ones that touch a `VkDevice` directly.
  (`src/rx_debug_ui/include/rx_debug_ui/overlay.h`)
- **`rx::task::Scheduler`** — `pumpMain()` **[guarded]** [audit finding
  F5-partial]: runs whatever GPU-object-mutating closures `postToMain()`
  queued (the handoff pattern below), so it carries the identical
  main-thread-only contract as everything above it, just via a different
  entry point. `parallelFor()` is NOT main-thread-only itself (its
  documented contract is "calling thread, whichever one that is" — see
  `scheduler.h`); this guard is specifically about the pump point workers
  hand GPU mutation back through, not about `Scheduler` in general.
  (`src/rx_task/include/rx_task/scheduler.h`)

The `rx_material` public ABI surface (`api_impl.cpp`'s
`IRxMaterialSystem::loadMaterial()`/`createTexture2D()`) carries the
identical guard at its own entry point too, ahead of (and in addition to)
the internal `MaterialSystem` guard the call eventually reaches —
`docs`/spec do not separately list the ABI layer as its own
main-thread-only surface, but a violation there is caught just as loudly.

## Worker-allowed

Safe to run on any `rx::task::Scheduler` worker (inside a `parallelFor()`
chunk), the dedicated IO thread (`runOnIoThread()`), or the dedicated
worker-task lane thread (`runOnWorkerThread()`, [Phase 4 Stage 1 Task 15]
— see that method's own doc comment, `scheduler.h`, and "Pinned-task
dispatch" below for exactly why a THIRD dedicated thread exists and what
it is for: async-import-style background work that must run off both the
main thread and the IO thread, with a closure's own body still free to
fan out across the ordinary `[1, workerCount()]` pool via a nested
`parallelFor()` call):

- Pure CPU transforms (matrix math, AABB computation, skinning-data
  parsing/preservation).
- Parse/decode/transcode/optimize (fastgltf parsing, libktx Basis
  transcode, meshoptimizer passes, MikkTSpace tangent generation — Stage
  1).
- Culling (frustum/shadow-caster AABB tests over SoA bounds — Stage 2).
- Secondary command-buffer recording into a **per-thread, per-frame-in-
  flight command pool** — implemented [Phase 4 Task 7,
  `rx::graph::Executor::recordChunkedPass()`]: one `VkCommandPool` per
  (worker thread index, frame-in-flight slot) pair, created lazily, reset
  as a whole pool once per frame, never touched by more than one thread
  at a time (a `VkCommandPool` is not internally synchronized —
  allocating/recording from the same pool on two threads concurrently is
  a Vulkan spec violation, not merely a performance concern). Each
  secondary buffer is recorded start-to-finish on the single worker
  thread that owns its pool; only the primary command buffer
  (main-thread-only, above) calls `vkCmdExecuteCommands()` to stitch them
  into the frame.
  **One exception to "any worker":** a chunked pass's own **chunk 0 is
  guaranteed to run synchronously, on the calling (main) thread**, before
  any other chunk begins (`rx::graph::Pass::setExecuteChunked()`'s own
  doc comment has the full contract) — the one safe place a chunked pass
  may call a main-thread-only API it cannot avoid needing once per frame
  (discovered load-bearing, not merely convenient, migrating
  samples/06_materials' own forward pass: `MaterialSystem::bindInstance()`
  resolves a pipeline and streams a per-frame parameter UBO in one call,
  with no split resolve/record API to fall back on). Every chunk *other*
  than chunk 0 still follows the "any worker, never main-thread-only"
  rule above without exception — enforced at runtime, not merely
  documented, for every **[guarded]** entry in "Main-thread-only" above
  [Phase 4 Task 7 fix round 1]: calling one of those from chunk >= 1 now
  fails loudly (dev builds) instead of silently corrupting shared state.
  **An exception escaping a chunk >= 1 callback is process-fatal by
  design** [audit finding F6] — `rx::task::Scheduler::parallelFor()`'s
  underlying enkiTS worker-thread dispatch loop has no handler to catch
  into, so an uncaught exception there calls `std::terminate()` for the
  whole process, not just this one pass. `PassContext`'s resolvers
  document throwing (`std::out_of_range` — `executor.h`) and are legal to
  call from any chunk, but the SAME throw that is cleanly catchable on
  chunk 0 (still synchronous, on the main thread, per the paragraph
  above) is process-fatal on every other chunk — see
  `rx::graph::Pass::setExecuteChunked()`'s own doc comment for the full
  contract. In practice: a chunked callback must be noexcept-in-effect
  for any chunk index it cannot prove is 0.

## Parallelism is the default, not a mode

[Spec D4, amended] There is no on/off switch and no caller-chosen chunk
count anywhere engine-owned work runs. A pass provides either a
whole-pass callback (hand-written simple passes — the library model
means the engine cannot split code it does not own) or a chunked
callback; every chunked pass records in parallel unconditionally, with
the executor deriving chunk count from `rx::task::Scheduler` and
grain-based scaling making small workloads effectively serial at the cost
of one task submission — self-scaling, never toggled. All engine-owned
work (culling, draw-list building, import internals, and Stage 2's
scene-submit helper, which is the chunked callback for any scene-driven
pass) is parallel by default with no flags.

`rx::task::Scheduler::parallelFor()` is where this lands mechanically:
its `grainSize` parameter defaults to AUTO (`0`, or simply omitted via the
`parallelFor(itemCount, fn)` overload) — `Scheduler::autoGrainSize()`
computes `max(kMinGrain, itemCount / (workerCount() * 4))`, so a caller
never has to reason about chunk counts to get parallel execution, and a
tiny `itemCount` naturally collapses toward one or a few chunks rather
than needing a separate "don't bother, it's small" code path. A nonzero
`grainSize` still works (used verbatim) — kept as a measurement
affordance for controlled tuning, not a mode callers reach for. `--threads`
exists only in the stress benchmark (sample 07) as a measurement
instrument, not as an engine-wide parallelism switch: it configures how
many workers the benchmark's own `Scheduler` is constructed with, so the
benchmark can publish parallel-vs-single numbers — it does not gate
whether any engine-owned work runs in parallel, which is unconditional
regardless.

**A render-graph pass's own CHUNK COUNT is a separate formula from the
`autoGrainSize()` one above** — implemented
[`rx::graph::detail::chunkCountForWorkerCount()`, `executor.h`]:
`min(scheduler.workerCount(), kMaxChunksPerPass)`, a pure function of the
scheduler's own worker count, deliberately independent of any pass's own
workload size (`Pass::setExecuteChunked()`'s signature carries no such
hint at all — D4's "no caller-chosen count" applies here too). The
executor then fans those chunks out via ONE `parallelFor(chunkCount - 1,
grainSize=1, ...)` call (chunk 0 runs synchronously first — see
"Worker-allowed" above) — grain **1**, not AUTO, since each chunk index
is already the right-sized unit of work; `autoGrainSize()`'s own
formula is for splitting a *known itemCount* (culling, draw-list
building), a different problem `Pass::setExecuteChunked()` does not
have.

## Host-engine coexistence

[Master registry, `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`,
"Scheduler sharing with host engines" (committed 2026-08-11, SDK phase):
an embedding game engine must eventually be able to make the renderer's
task scheduler and its own job system ONE pool, never two politely-idle
ones that quietly starve each other for cache and scheduler time.
`rx::task::Scheduler` is built with that end state in mind, in three
parts:

1. **Idle behavior is bursty, not always-on.** Verified directly against
   the pinned enkiTS v1.12 source (not assumed): each internal worker's
   own dispatch loop (`TaskScheduler::TaskingThreadFunction`,
   `TaskScheduler.cpp:281-317`) spins with an increasing backoff for at
   most `gc_SpinCount = 10` attempts (`TaskScheduler.cpp:75`) after
   finding no work, then calls `WaitForNewTasks()`
   (`TaskScheduler.cpp:686-715`) — which double-checks for a
   just-arrived task first (avoiding a lost-wakeup race) and, if
   genuinely idle, blocks on a real OS semaphore
   (`SemaphoreWait()`: `WaitForSingleObject(..., INFINITE)` on Windows,
   `sem_wait()` on POSIX/Linux — `TaskScheduler.cpp:1336-1341` /
   `1421-1424`) until signaled. That is a genuine kernel-level blocking
   wait, not a spin loop burning CPU: an idle worker's steady-state cost
   is near-zero. The dedicated IO thread's own idle wait
   (`IoLoopTask::Execute()`'s `WaitForNewPinnedTasks()` call,
   `scheduler.cpp` — same underlying semaphore mechanism, traced in that
   file's own comments) additionally blocks on real I/O inside whatever
   `fn` a caller submitted. Renderer-core occupancy is therefore bursty
   by design: workers wake for a `parallelFor()` fan-out, run to
   completion, and go back to sleep on a kernel wait — never a
   background thread perpetually spinning and competing with a host
   application's own subsystems (audio, physics) while the renderer has
   nothing to do.
2. **Worker count is the consumer's budget, not a machine-wide default.**
   `Scheduler::create(workerCount)`'s `hardware_concurrency() - 1`
   default (`workerCount == 0`) is exactly that: a default for a
   STANDALONE consumer (this phase's own samples and tests) that owns the
   whole machine. An embedding engine is expected to construct its
   `Scheduler` with the worker budget IT has decided to grant the
   renderer — however it arrives at that number (a fraction of its own
   pool, a fixed count, its own scheduling policy) — rather than accept
   the hardware-derived default. Every `parallelFor()` call self-scales
   to whatever `workerCount()` the Scheduler was actually built with, via
   the auto-grain heuristic above — a smaller granted budget costs the
   caller nothing beyond passing that number to `create()`; there is no
   separate configuration surface to keep in sync.
3. **External-thread participation is the recorded SDK-phase end state,
   not implemented now.** The actual "one shared pool" end state is a
   host engine registering its OWN threads into the renderer's enkiTS
   scheduler (enkiTS's `RegisterExternalTaskThread()` /
   `numExternalTaskThreads` mechanism — see `TaskScheduler.h`'s own
   documentation of it), so host job-system work and renderer work
   interleave on literally the same OS threads instead of two separate,
   politely-idle pools. That is SDK-phase scope (the public ABI surface
   Phase 4 does not grow beyond the log sink, per D23) — recorded in the
   master registry, not implemented in `rx::task::Scheduler` today.

## The handoff pattern

Workers never call into a main-thread-only API directly. Instead:

```cpp
// On a worker (inside a parallelFor() chunk, or runOnIoThread()):
auto decoded = decodeTexture(bytes);           // pure CPU work
scheduler->postToMain([decoded = std::move(decoded), &bindless, &uploader] {
  // Runs later, on the main thread, when it calls pumpMain().
  uploader.uploadToImage(texture, decoded.pixels, decoded.byteSize, /*generateMips=*/true);
  bindless.registerSampledImage(texture.view(), texture.layout());
});

// Once per frame, on the main thread (the frame loop's own pump point):
scheduler->pumpMain();
```

`runOnIoThread()` is the other half of the same pattern for the
blocking-I/O side specifically (file reads, decode-adjacent syscalls that
would otherwise stall a `parallelFor()` worker): it runs `fn` on the
scheduler's one dedicated IO thread, FIFO, off the main thread and off
every `parallelFor()` worker. A worker (or the IO thread itself) doing
I/O-adjacent work still finishes by calling `postToMain()` for whatever
step actually needs to touch the GPU-object APIs above.

`runOnWorkerThread()` [Phase 4 Stage 1 Task 15] is a THIRD variant of the
same handoff shape, for CPU-heavy work (async-import decode/transcode)
that must run off both the main thread and the dedicated IO thread at
once — see "Pinned-task dispatch" immediately below for the mechanism and
exactly why it exists.

## Pinned-task dispatch: persistent tasks, not per-submission ones

[Phase 4 Stage 1 Task 15, fix round 1 — CRITICAL fix, TSAN-confirmed]
`runOnIoThread()` and `runOnWorkerThread()` do **not** create a new
enkiTS task per call. Exactly two `enki::IPinnedTask` objects exist for
this Scheduler's entire lifetime — one pinned to the dedicated IO thread,
one pinned to a second dedicated "worker task lane" thread — each
allocated once at `Scheduler::create()` and freed only in `~Scheduler()`,
strictly after `WaitforAllAndShutdown()` returns. Both permanently loop,
draining their own `ClosureQueue` (a plain `std::mutex` +
`std::condition_variable`-guarded FIFO of `std::function<void()>`,
the same shape as this file's own pre-existing `postToMain()`/
`pumpMain()` queue) and invoking each closure directly, same-thread, one
at a time — `runOnIoThread(fn)`/`runOnWorkerThread(fn)` just push `fn`
onto the relevant queue and return; nothing is ever registered with
enkiTS per submission, and nothing per-submission is ever reclaimed,
reaped, or freed by anyone.

**Why this exists (the bug it replaces):** the original design (Task 2's
own `IoTask`, later reused for Task 15's `runOnWorkerThread()`) allocated
one `enki::IPinnedTask` per submission and tried to safely `delete` it
once "done". Both gates tried (first `isPublished()` alone, Task 2's own
F1 fix; then `isPublished() && GetIsComplete()`, an interim Task 15 fix)
are provably unsafe: verified directly against the vendored enkiTS v1.12
source, `TaskScheduler::RunPinnedTasks(threadNum_, priority_)` does
`Execute(); m_RunningCount.fetch_sub(...); TaskComplete(pTask_, ...)` —
and `TaskComplete()` keeps reading/writing `pTask_` (`m_WaitingForTaskCount`,
`m_pDependents`, a redundant `m_RunningCount` store) for several more
instructions *after* the `fetch_sub` that makes `GetIsComplete()` observe
`true`. A reaper thread gated on that signal can delete the object while
the executing thread is still inside `TaskComplete()` — a genuine
use-after-free, 100% reproducible under `-fsanitize=thread` by an
adversarial construct→burst-submit→destroy churn harness
(`rx_task/tests/scheduler_test.cpp`'s own permanent
`PinnedTaskChurnTest`-shaped `TEST_CASE`) — see `task-15-report.md`'s
fix-round-1 section for the full before/after TSAN evidence, including
confirmation the SAME race class already existed, at comparable density,
in the pre-Task-15 `runOnIoThread()`-only design (base commit, this
repository's own Stage 0 work) — inherited, not introduced.

**Why the fix is structurally different, not a fourth gate:** no
per-object signal read from a *different* thread can ever safely answer
"has enkiTS fully finished touching this object" mid-lifetime — every
signal enkiTS's own pinned-task machinery exposes (`GetIsComplete()`, our
own `isPublished()`) can observe "looks done" strictly *before*
`TaskComplete()`'s own tail actually finishes. The only race-free signal
enkiTS provides at all is `WaitforAllAndShutdown()` itself — a *global*
barrier, not a per-object poll. Eliminating per-submission enkiTS tasks
entirely (persistent loop + our own mutex-guarded queue) sidesteps the
question altogether: a closure's only "objects" are its own captured
state and the local `std::function` holding it, both destroyed on the
SAME thread that invoked them, by ordinary RAII — nothing else ever
touches them, so there is no completion signal to get right or wrong.

**Manual TSAN verification procedure** [Stage 0 audit precedent,
task-2-report.md]: this is a genuine data-race class, not something
ASan/UBSan can catch (ASan's redzone/quarantine poisoning is routinely
defeated by fast allocator reuse under churn; TSAN's own happens-before
tracking is not). To re-verify by hand:

```sh
# Compile scheduler.cpp + scheduler_test.cpp + doctest_main.cpp with
# -fsanitize=thread (swap in for the normal -O2 -DNDEBUG flags; keep
# everything else -- include paths, defines -- identical to a normal
# linux-native compile_commands.json entry for these three files), link
# against the existing non-instrumented rx_core/spdlog/glm/Tracy/enkiTS
# static libraries (TSAN's runtime intercepts pthread/atomics process-wide
# regardless of which translation units were instrumented), then:
TSAN_OPTIONS="halt_on_error=0" ./rx_task_tests_tsan
```

Run at least 10 times. Categorize every `WARNING: ThreadSanitizer` report
by its stack frames: anything whose frames are entirely inside
`enki::TaskSet`/`Scheduler::parallelFor`/`AddTaskSetToPipe`/`WaitforTask`
(the `parallelFor()` publish path) or
`ClosureQueueLoopTask::Execute`/`AddPinnedTask`/`RunPinnedTasks`/
`WaitForNewPinnedTasks` (the two persistent loop tasks' own one-time
registration at construction) is the known, pre-existing enkiTS-internal
publish-synchronization noise class this section's own "why the fix is
structurally different" paragraph already accounts for (confirmed, not
assumed: build the exact base-commit `scheduler.cpp`/`scheduler_test.cpp`
under the identical TSAN flags and observe the same two categories at
comparable density). A report whose frames instead touch `ClosureQueue`'s
own methods (`push`/`waitAndDrain`/`requestShutdown`) — or anything else
outside those two known categories — is a genuine regression and blocks
the change that introduced it.

## Every new public header carries a one-line thread-affinity note

Per D5: any header added or touched from Phase 4 onward that exposes a
main-thread-only or worker-allowed API gets a one-line comment near its
class/function declaration pointing back to this file, e.g.:

```cpp
// Thread-affinity (D5, Phase 4): retire()/onFrameFenceSignaled()/flushAll()
// are main-thread-only -- see docs/threading.md.
class DeletionQueue {
```

This is deliberately terse (one line, not a restatement of this whole
document) — the point is that a reader hits a pointer to the actual
contract at the point of use, not a duplicate copy of it that can drift out
of sync.
