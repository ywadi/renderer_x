# Task 4 Report: Uploader, DeletionQueue, Texture2D + mips, MeshBuffers

## Summary

Implemented the full Task 4 scope: `rx::rhi::Uploader` (ReBAR-aware staging
ring buffer + synchronous batched `flush()`), `rx::rhi::DeletionQueue`
(fence-gated deferred destruction, decoupled from any Vulkan state),
`rx::rhi::Texture2D` (VMA image + view with blit-chain mip generation and
a format-feature-checked single-mip fallback), and `rx::rhi::MeshBuffers`
(device-local vertex/index buffers populated through `Uploader`). Also
closed the tracked API gap from Phase 1 Task 3's review: `Buffer::flush()`/
`Buffer::invalidate()` now wrap `vmaFlushAllocation`/`vmaInvalidateAllocation`
directly, and `clear_color_test.cpp`'s host-coherence proxy `REQUIRE` loop
was replaced with a real `invalidate()` call. stb_image is wired into
`third_party/CMakeLists.txt` (fetched, compiled once in `stb_impl.cpp`) per
spec Fixed decision #10, even though no call site exists yet (Uploader
takes decoded pixels in, not a file path, matching the brief's own
interface — the sample tasks will be the first real `stbi_load()` caller).
`FrameSync` gained the one accessor `DeletionQueue` needs:
`frameNumber()`, a monotonic counter distinct from the existing
`currentFrameIndex()` mod-2 slot index. Ten test cases across three new
test files plus the two updated existing test files. Full `ctest` is
green (6/6, 22/22 sub-cases, 625/625 assertions) on linux-native, zero
unexpected validation errors; `windows-cross-zig` configures and builds
clean including all new/changed files.

## Implementation

### 1. `Buffer::flush()`/`Buffer::invalidate()` (buffer.h/buffer.cpp)

Thin wrappers over `vmaFlushAllocation`/`vmaInvalidateAllocation`
(`offset`, `size = VK_WHOLE_SIZE` defaults), logging `RX_LOG_ERROR` on the
rare non-`VK_SUCCESS` return, no-op on an invalid/moved-from `Buffer`.
Documented as safe (and cheap — VMA itself skips the real
`vkFlush/InvalidateMappedMemoryRanges` call) to invoke unconditionally
regardless of whether the underlying memory type is actually coherent.
Used wherever mapped memory crosses a CPU↔GPU handoff in this task's own
code: `Uploader`'s ring-buffer writes call `flush()` before the GPU reads
them (`vkCmdCopyBuffer`/`vkCmdCopyBufferToImage` source); every test's
readback buffer calls `invalidate()` before the host reads it.

`clear_color_test.cpp`'s pre-existing coherence proxy (`REQUIRE` that
every `HOST_VISIBLE` memory type on the device is also `HOST_COHERENT`)
was removed and replaced with a real `readback->invalidate()` call before
the `memcpy` — per the brief's "your call, explain it," this is the
correct simplification: it's now provably correct even on a hypothetical
non-coherent host-visible heap, not just the coherent-in-practice
hardware this machine happens to expose. `buffer_test.cpp` had no such
proxy guard (pure CPU-write/CPU-read, no GPU write involved) and needed
no change.

### 2. New `Allocator` methods (buffer.h/buffer.cpp)

- `createDeviceLocalBuffer(size, usage)` — `VMA_MEMORY_USAGE_AUTO`, no
  `HOST_ACCESS_*` flags, no `MAPPED_BIT` → device-local only. Backs
  `MeshBuffers`.
- `createUploadRingBuffer(size, usage) -> UploadBufferResult{Buffer,
  bool deviceLocal}` — the exact ReBAR-aware pattern from [R:C1]:
  `AUTO + HOST_ACCESS_SEQUENTIAL_WRITE_BIT | HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT
  | MAPPED_BIT`; `deviceLocal` is classified once via
  `vmaGetMemoryTypeProperties` on the resulting `VmaAllocationInfo::memoryType`
  (no `VkPhysicalDevice` needed for this specific check). Backs `Uploader`.
- `raw() -> VmaAllocator` — minimal accessor `Texture2D::create()` (a
  different TU) needs to call `vmaCreateImage`/`vmaDestroyImage` directly,
  without making `buffer.h`/`buffer.cpp` know anything about `Texture2D`.

### 3. `FrameSync::frameNumber()` (frame_sync.h/frame_sync.cpp)

One `uint64_t frameNumber_` member, incremented alongside
`currentFrame_` inside `advanceFrame()`. Documented explicitly as the
value `DeletionQueue`'s `retire()`/`onFrameFenceSignaled()` should be
keyed on in production code — `currentFrameIndex()` cycles mod
`kFramesInFlight` (0, 1, 0, 1, ...) and cannot disambiguate "frame 5"
from "frame 7," which a real deferred-destruction queue needs to be able
to do.

### 4. `rx::rhi::DeletionQueue` (deletion_queue.h/deletion_queue.cpp)

Deliberately owns **no** Vulkan state — `retire(std::function<void()>,
uint64_t frameIndex)` appends to a `std::vector<Item>`;
`onFrameFenceSignaled(completedFrameIndex)` runs (in original retire()
order) and removes every item with `frameIndex <= completedFrameIndex`
via a two-pass rebuild (avoids relying on `std::remove_if`/`erase_if`'s
unspecified per-element call count for a predicate with side effects);
`flushAll(waitIdleFirst)` runs everything unconditionally, invoking
`waitIdleFirst` first if provided. The destructor logs
(`RX_LOG_ERROR`) rather than crashing if anything is still pending —
it cannot itself guarantee device idle, so it can't safely run
destructors, but it also can't silently let the mistake pass.

**Documented pitfall (real, not hypothetical — hit during this task's
own test-writing):** `std::function<void()>` requires its target to be
copy-constructible. A lambda that captures a move-only resource
(`Buffer`, `Texture2D`, ...) directly by move fails to compile the
moment it's handed to `std::function`. The header now documents the
standard fix (wrap in `std::shared_ptr` first) prominently, since every
real caller of `retire()` will hit this the first time they try to
retire a real GPU resource.

### 5. `rx::rhi::Texture2D` (texture.h/texture.cpp)

VMA-backed `VkImage` + `VkImageView`, move-only RAII. `create()` computes
`floor(log2(max(w,h))) + 1` for `requestedMipLevels == 0` (auto full
chain), else clamps the request down to that maximum. Before honoring
`mipLevels > 1`, queries `vkGetPhysicalDeviceFormatProperties` and
requires **both** `VK_FORMAT_FEATURE_BLIT_SRC_BIT` and `_DST_BIT` under
optimal tiling (the brief's shorthand names only `_DST_BIT`; a blit chain
needs `_SRC_BIT` equally, since every level but the last is a blit
source for the next — documented as a brief elaboration, not a spec
deviation) — unsupported falls back to a single mip level with
`RX_LOG_WARN`, never a hard failure. `usage` gets `TRANSFER_DST_BIT`
unconditionally and `TRANSFER_SRC_BIT` added iff `mipLevels > 1`.
`recordMipChainBlit()` records the per-level barrier + `vkCmdBlitImage`
sequence (own local `transitionLevelRange()` helper — a per-mip-level-
range version of `command.h`'s `transitionImage()`, same maximally-
conservative ALL_COMMANDS/MEMORY_READ|WRITE barrier, since
`transitionImage()` itself always covers `VK_REMAINING_MIP_LEVELS` and
can't express an arbitrary sub-range), leaving every level in
`SHADER_READ_ONLY_OPTIMAL`. The sRGB-averaging caveat [R:C2] is
documented both at the declaration (texture.h) and directly above the
`vkCmdBlitImage` call (texture.cpp) — vkCmdBlitImage's linear filter
averages texel values in the image's own stored encoding, which is
correct for UNORM but not for sRGB (the exact bug class libktx's `ktx
create` once shipped and fixed).

**Deviation from brief's shorthand signature** (documented, same
precedent as Task 3's `BindlessTable::create`): `create()` takes
`VkPhysicalDevice` (format-feature query) and `VkDevice`
(`vkCreateImageView`/`vkDestroyImageView`) in addition to `Allocator&` —
neither is reachable from `Allocator` alone.

### 6. `rx::rhi::Uploader` (upload.h/upload.cpp)

Owns a command pool/buffer/fence plus one persistent ring `Buffer`
(`Allocator::createUploadRingBuffer`, `VK_BUFFER_USAGE_TRANSFER_SRC_BIT`).
`uploadToBuffer`/`uploadToImage` reserve ring space (aligned to 16 bytes,
satisfying `vkCmdCopyBufferToImage`'s 4-byte `bufferOffset` VUID with
margin), `memcpy` + `Buffer::flush()`, then record the copy (plus, for
images, the `UNDEFINED → TRANSFER_DST_OPTIMAL` transition via the
existing `command.h::transitionImage()` and either
`Texture2D::recordMipChainBlit()` or a final transition to
`SHADER_READ_ONLY_OPTIMAL`) onto the internal command buffer — nothing
submits until `flush()`. If a reservation would run past the ring
buffer's end, `reserveRingSpace()` calls `flush()` internally first
(submits + waits everything recorded so far, frees the whole ring) and
restarts at offset 0 — verified by a dedicated test with a deliberately
tiny (64-byte) ring buffer and two 48-byte uploads. `usesDirectPath()`
reports which memory type the ring buffer actually landed in (logged
once at `create()`, never per-upload). The destructor auto-flushes
pending work before tearing down, so a caller that forgets a final
`flush()` never silently loses recorded-but-unsubmitted work.

Move ctor/assignment hand-written (not `= default`): this class owns raw
Vulkan handles (`pool_`/`cmd_`/`fence_`) alongside the RAII `ringBuffer_`
member, and a defaulted move would copy the raw handles without nulling
the source — the exact double-destroy hazard every other RAII type in
this library avoids by hand-writing its move operations. No plain
default constructor either, for the same reason `MeshBuffers`/`Texture2D`
don't have one: `Buffer`'s own default constructor is private
(friends-only), so a default `Uploader()` can't default-construct
`ringBuffer_`; `create()` builds every member up front and constructs
directly via a parameterized private constructor instead.

### 7. `rx::rhi::MeshBuffers` (mesh_buffers.h/mesh_buffers.cpp)

Two `Allocator::createDeviceLocalBuffer` buffers (vertex:
`VERTEX_BUFFER_BIT`, index: `INDEX_BUFFER_BIT`, both additionally
`TRANSFER_DST_BIT | TRANSFER_SRC_BIT` — SRC added beyond the brief's
literal ask so a debug readback, or a future GPU-side consumer, can copy
out of them directly; proven by `upload_test.cpp`'s own readback-from-
`mesh->vertexBuffer()`/`indexBuffer()` assertions), populated via
`Uploader::uploadToBuffer` + one `uploader.flush()` this call issues
itself. Move ctor/assignment are `= default` — unlike every other RAII
type mentioned above, `MeshBuffers` owns zero raw handles (only two
well-behaved `Buffer` members), so the default member-wise move is
correct: it invokes `Buffer`'s own public move operations, which already
null their source correctly.

### 8. stb_image wiring (third_party/CMakeLists.txt, stb_impl.cpp)

Same `FetchContent_Populate` + `PARENT_SCOPE` pattern as volk/VMA (stb
has no CMakeLists.txt at all, and no version tags — pinned to commit
`2c980bb59875b0d32144a71867fbdebb2f77cd20`, verified live against
`git ls-remote`/a real clone; also verified CMake's `FetchContent` +
`GIT_SHALLOW TRUE` correctly resolves an exact commit SHA, unlike a raw
`git clone --branch <sha>` which fails — confirmed with a throwaway
CMake project before relying on it). `src/rx_rhi_vk/src/stb_impl.cpp` is
the sole `STB_IMAGE_IMPLEMENTATION` TU, mirroring `vma_impl.cpp`. No call
site exists yet in this task (Texture2D/Uploader's own interfaces take
decoded pixel bytes in, exactly as the brief specifies) — the dependency
is fully wired end to end for the sample tasks that will call
`stbi_load()`.

### 9. Tests

- `buffer_test.cpp` (unchanged), `clear_color_test.cpp` (invalidate()
  replaces the coherence proxy, see §1).
- `upload_test.cpp` (windowed fixture, `Uploader::create()` needs a real
  `rx::rhi::Device&`): byte-exact round-trip (4096 bytes); ring-buffer
  auto-flush-mid-batch correctness (64-byte ring, two 48-byte uploads);
  oversize-upload clean rejection; `MeshBuffers::create` end-to-end
  (vertex+index byte-exact, both through a fresh `Uploader` copy AND
  directly out of `mesh->vertexBuffer()`/`indexBuffer()`).
- `deletion_queue_test.cpp`: three pure-logic cases (defer-until-signaled-
  run-exactly-once; multi-item FIFO-within-due-set ordering;
  `flushAll`'s `waitIdleFirst` + unconditional-run contract) plus the
  brief-mandated real-Vulkan stress test — headless fixture, 40
  iterations reusing 2 fence-guarded "frame slots" (mirroring
  `FrameSync::kFramesInFlight`), each iteration submitting a real,
  **not immediately waited on** `vkCmdCopyBuffer` referencing a fresh
  host-visible + device-local buffer pair, retired via `DeletionQueue`
  tagged at that iteration's frame index, destroyed only once that
  slot's fence is later confirmed signaled. Asserts `pendingCount() >=
  kSlots` mid-loop (proof the queue is really deferring, not trivially
  discarding) and zero validation errors at the end (the real proof
  nothing was destroyed while a pending submission still referenced it).
- `texture_test.cpp` (windowed fixture): 64x64 `R8G8B8A8_UNORM` upload
  with `generateMips=true` → `mipLevels() == 7` (auto full chain),
  deterministic gradient pixels, level-0 readback byte-exact, zero
  validation errors; explicit `requestedMipLevels` (3 honored verbatim;
  10 clamped down to a 4x4 extent's real maximum of 3).

## Verification

- **linux-native:** full build clean; `ctest`: **6/6 pass**
  (`shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`,
  `rx_shader_tests`, `rx_rhi_vk_tests`, `sample_01_triangle_headless`).
  `rx_rhi_vk_tests` run directly: **22 test cases / 625 assertions, 0
  failed.** Every `[error]`-level log line in a full direct run is one of
  three deliberately-exercised clean-rejection paths (two pre-existing
  from Tasks 2/3, plus this task's own oversize-ring-upload case) — no
  unexpected errors, and `grep` for `Validation Error` outside the
  pre-existing documented `known false positive` (portability-enumeration
  layer quirk) returns nothing. A warm incremental rebuild (touch one
  `.cpp`, rebuild) took ~2 seconds.
- **windows-cross-zig:** configure + full build clean, including every
  new/changed file (`upload.cpp`, `deletion_queue.cpp`, `texture.cpp`,
  `mesh_buffers.cpp`, `stb_impl.cpp`, and all three new test TUs)
  compiling and `rx_rhi_vk_tests.exe`/`sample_01_triangle.exe` linking
  successfully via zig/LLD.
- Commit hygiene: verified directly with `git log -1 --format='%B'`
  after committing (see commit list below) — no AI attribution.

## Deviations from brief / spec

1. **`Texture2D::create()` takes `VkPhysicalDevice` AND `VkDevice`** in
   addition to `Allocator&` (brief's shorthand: `create(Allocator&,
   extent, format, usage, mipLevels)`) — needed for the format-feature
   query and `vkCreateImageView`/`vkDestroyImageView` respectively;
   `Allocator` alone exposes neither. Same precedent/reasoning as Task
   3's `BindlessTable::create` deviation (task-3-report.md).
2. **Mip-chain format-feature check requires both `BLIT_SRC_BIT` and
   `BLIT_DST_BIT`**, not just `BLIT_DST_BIT` as the brief's shorthand
   names — a blit chain reads FROM level N (needs `BLIT_SRC_BIT`) to
   write TO level N+1 (needs `BLIT_DST_BIT`); checking only the
   destination bit would miss a device that can write-but-not-read-blit
   a format (rare, but the check is free to make correct). Strengthens
   correctness; does not relitigate the architecture.
3. **`MeshBuffers::create()` takes an explicit `indexCount` parameter**
   beyond the brief's bare shorthand — stored and exposed via
   `indexCount()` for direct `vkCmdDrawIndexed` use; the caller already
   knows this value unambiguously (independent of index type), so
   deriving it from `indexBytes`/`indexType` size would be strictly worse
   ergonomics for zero benefit.
4. Everything else matches the brief's interfaces: `Uploader::create`,
   `uploadToBuffer`/`uploadToImage`, synchronous `flush()`;
   `DeletionQueue::retire`/`onFrameFenceSignaled`/`flushAll`;
   `Buffer::flush`/`invalidate` wrapping the VMA calls exactly as
   specified.

## Concerns for the coordinator

1. **The single-mip-fallback path (`RX_LOG_WARN`, unsupported blit
   format) is implemented but not exercised by an automated test** — no
   format lacking `BLIT_SRC+DST` support under optimal tiling was found
   that would also be portable/deterministic across this project's
   actual test-running hardware set (NVIDIA, lavapipe/llvmpipe, and
   whatever CI/Wine ends up running) without risking flakiness; forcing
   it artificially (e.g. a compressed BC format needing its own optional
   feature + 4x4 block-aligned extents) seemed like it would trade one
   untested path for a new, less honest one. This mirrors Task 3's own
   precedent (`logDescriptorIndexingFeatureGaps`, task-3-report.md §1) of
   flagging an implemented-but-hardware-unexercisable path rather than
   forcing a fragile test.
2. **`std::function`'s copy-constructibility requirement is a real,
   sharp edge for every future `DeletionQueue::retire()` caller** —
   documented prominently in the header (the "STD::FUNCTION
   COPY-CONSTRUCTIBILITY PITFALL" section) and demonstrated in the
   stress test's own `std::make_shared<Buffer>(...)` pattern, but every
   Task 6/7 call site retiring a real `Texture2D`/`Buffer` will hit this
   the first time unless they've read that comment. Worth being aware of
   when reviewing those tasks' `retire()` call sites.
3. **`Uploader`'s ring-buffer auto-flush-on-overflow (`reserveRingSpace`)
   is a synchronous stall from the caller's perspective** — correct and
   simple (an explicit Phase 2 choice per the brief), but a caller doing
   many uploads through a too-small ring buffer will pay a submit+wait
   per overflow rather than getting one clean error; this is by design
   (never fails except on a genuinely-too-large single upload) but worth
   knowing if a future sample's ring-buffer sizing turns out too small
   in practice (default is 16 MiB).
4. **This task did not add a dedicated `Device::createDeletionQueueHook()`
   or similar** — `DeletionQueue` is deliberately Vulkan-agnostic per the
   brief's own interface (`retire(std::function<void()>, uint64_t)`), so
   the actual fence-wait-then-`onFrameFenceSignaled()` wiring into a real
   present loop is left to whichever task builds one (none in Phase 2's
   remaining scope needs a windowed present loop beyond what samples 5-7
   already have their own headless gates for) — flagging so it isn't
   mistaken for an oversight.

## Files created

- `src/rx_rhi_vk/include/rx_rhi_vk/upload.h`, `src/upload.cpp`
- `src/rx_rhi_vk/include/rx_rhi_vk/deletion_queue.h`, `src/deletion_queue.cpp`
- `src/rx_rhi_vk/include/rx_rhi_vk/texture.h`, `src/texture.cpp`
- `src/rx_rhi_vk/include/rx_rhi_vk/mesh_buffers.h`, `src/mesh_buffers.cpp`
- `src/rx_rhi_vk/src/stb_impl.cpp`
- `src/rx_rhi_vk/tests/upload_test.cpp`, `tests/deletion_queue_test.cpp`,
  `tests/texture_test.cpp`

## Files modified

- `src/rx_rhi_vk/include/rx_rhi_vk/buffer.h` / `src/buffer.cpp` —
  `Buffer::flush`/`invalidate`; `Allocator::createDeviceLocalBuffer`,
  `createUploadRingBuffer`, `raw()`.
- `src/rx_rhi_vk/include/rx_rhi_vk/frame_sync.h` / `src/frame_sync.cpp` —
  `frameNumber()` monotonic counter.
- `src/rx_rhi_vk/tests/clear_color_test.cpp` — coherence-proxy `REQUIRE`
  loop replaced with a real `Buffer::invalidate()` call.
- `third_party/CMakeLists.txt` — stb fetch.
- `src/rx_rhi_vk/CMakeLists.txt` — new sources, new include dir, new test
  TUs.

## Readiness for Task 5/6/7

`Uploader`/`Texture2D`/`MeshBuffers` are ready for Task 6's bindless-mesh
sample to consume directly (`Uploader::create` against a real windowed
`Device`, `Texture2D::create` + `uploadToImage` for each generated
texture, `MeshBuffers::create` for procedural geometry). `DeletionQueue`
is ready for Task 7's streaming sample's eviction path — the exact
scenario its own stress test already exercises end to end, just without
a real `BindlessTable::release()` call interleaved (that interleaving is
Task 7's own proof, per the as-built context's "release-safety contract"
note in `bindless.h`).
