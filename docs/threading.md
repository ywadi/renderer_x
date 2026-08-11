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
  flight command pool** — Task 7 forward-reference: one `VkCommandPool`
  per worker thread per frame-in-flight slot, reset as a whole pool once
  per frame, never touched by more than one thread at a time (a
  `VkCommandPool` is not internally synchronized — allocating/recording
  from the same pool on two threads concurrently is a Vulkan spec
  violation, not merely a performance concern). Each secondary buffer is
  recorded start-to-finish on the single worker thread that owns its
  pool; only the primary command buffer (main-thread-only, above) calls
  `vkCmdExecuteCommands()` to stitch them into the frame.

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
