# RendererX Threading Contract (D5)

Phase 4 introduces the engine's first real multi-threaded work (parallel
asset import/decode, parallel culling and draw-list building, parallel
secondary command recording) via `rx::task::Scheduler`
(`src/rx_task/include/rx_task/scheduler.h`), the engine's sole task
scheduler [spec D2]. This document is the contract every subsystem —
existing and future — is held to as that work lands, and it is what every
new public header's one-line thread-affinity note (see the rule at the
bottom of this file) points back to.

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
i.e. the thread that also calls `pumpMain()`):

- **`rx::rhi::BindlessTable`** — `registerSampledImage()`/
  `registerSampler()`/`registerStorageBuffer()`/`release()`
  (`src/rx_rhi_vk/include/rx_rhi_vk/bindless.h`)
- **`rx::rhi::Uploader`** — `uploadToBuffer()`/`uploadToImage()`/`flush()`
  (`src/rx_rhi_vk/include/rx_rhi_vk/upload.h`)
- **`rx::material::MaterialSystem`** — `loadMaterial()`/`getPipeline()`
  (and every other method on this type — it is not internally
  synchronized at all, matching `rx::graph::RenderGraph`/`Executor`'s
  existing scoping)
  (`src/rx_material/include/rx_material/material_system.h`)
- **`rx::rhi::DeletionQueue`** — `retire()`/`onFrameFenceSignaled()`/
  `flushAll()` (`src/rx_rhi_vk/include/rx_rhi_vk/deletion_queue.h`)
- **`rx::asset::GeometryPool`** (future, Stage 1) — suballocation is a
  main-thread-owned `VmaVirtualBlock` operation, same rationale as the
  four above.
- **`rx::scene::*Manager` registries** (future, Stage 2) —
  `RenderableManager`/`TransformManager`/`LightManager` mutation
  (create/set/destroy) stays main-thread-only; read-only SoA traversal for
  culling (below) does not.

## Worker-allowed

Safe to run on any `rx::task::Scheduler` worker (inside a `parallelFor()`
chunk) or the dedicated IO thread (`runOnIoThread()`):

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
  rule above without exception.

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
every `parallelFor()` worker — see `scheduler.h`'s own doc comments for
exactly how enkiTS's `IPinnedTask` mechanism backs it. A worker (or the IO
thread itself) doing I/O-adjacent work still finishes by calling
`postToMain()` for whatever step actually needs to touch the GPU-object
APIs above.

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
