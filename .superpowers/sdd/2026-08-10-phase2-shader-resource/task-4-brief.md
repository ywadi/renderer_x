### Task 4: Uploader, DeletionQueue, Texture2D + mips, MeshBuffers

**Files:**
- Modify: `third_party/CMakeLists.txt` (stb via FetchContent — header-only, `stb_image.h`; same Populate+PARENT_SCOPE pattern as volk/VMA)
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/upload.h`, `src/rx_rhi_vk/src/upload.cpp`
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/deletion_queue.h`, `src/rx_rhi_vk/src/deletion_queue.cpp`
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/texture.h`, `src/rx_rhi_vk/src/texture.cpp`
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/mesh_buffers.h`, `src/rx_rhi_vk/src/mesh_buffers.cpp`
- Create: tests for each (`upload_test.cpp`, `deletion_queue_test.cpp`, `texture_test.cpp`)

**Interfaces produced:**
- `rx::rhi::Uploader` — `create(Allocator&, Device&)`; `uploadToBuffer(dst VkBuffer, offset, data, size)`; `uploadToImage(dst Texture2D&, pixels, generateMips)`; direct path via `HOST_ACCESS_ALLOW_TRANSFER_INSTEAD` + fallback detection (check resulting memory-type properties once, log once) + staging path via ring buffer + `vkCmdCopyBuffer`/`vkCmdCopyBufferToImage` on the graphics queue [R:C1]. Synchronous flush API is acceptable this phase (`flush()` submits + fences); per-frame async batching is a later optimization — say so in the header.
- **Known API gap to close in this task (from Phase 1 Task 3's review):** `Allocator`/`Buffer` expose no flush/invalidate surface — the `VmaAllocation` is private, so `vmaFlushAllocation`/`vmaInvalidateAllocation` are unreachable, and Phase 1's readback test had to guard on device-wide host-coherence as a proxy. The Uploader work (readbacks, `ALLOW_TRANSFER_INSTEAD` paths, non-coherent memory) makes this load-bearing: add `Buffer::flush(offset,size)` / `Buffer::invalidate(offset,size)` (wrapping the VMA calls) and use them wherever mapped memory is read after GPU writes or written before GPU reads on possibly-non-coherent types.
- `rx::rhi::DeletionQueue` — `retire(std::function<void()>, uint64_t frameIndex)`; `onFrameFenceSignaled(frameIndex)` runs destructors whose frame completed; `flushAll(vkDeviceWaitIdle first)` for shutdown. Integrates with Phase 1 `FrameSync`'s frame indexing (add the minimal hook FrameSync needs — current frame counter accessor — if not already present).
- `rx::rhi::Texture2D` — VMA image + view, `create(Allocator&, extent, format, usage, mipLevels)`; mip generation via blit chain with per-level barriers, `VK_FORMAT_FEATURE_BLIT_DST_BIT` checked, unsupported → single mip + `RX_LOG_WARN` [R:C2]; sRGB-averaging caveat documented at the blit code.
- `rx::rhi::MeshBuffers` — device-local vertex+index buffers created through Uploader.

**Tests (headless):** buffer upload → GPU → copy back → byte-exact; image upload 64x64 with mips → readback level 0 exact, level count correct, validation clean; DeletionQueue: retire a buffer "used" by an in-flight (fence-unsignaled) frame, assert not destroyed until fence signal, then destroyed exactly once — and a stress loop (many retire/signal cycles) clean under validation.

**Verify:** full ctest green; both presets build; commit clean.

---

