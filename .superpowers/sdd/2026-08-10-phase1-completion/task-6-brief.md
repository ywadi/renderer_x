### Task 6: Production frame loop — FrameSync, present mode, samples structure

**Files:**
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/frame_sync.h`, `src/rx_rhi_vk/src/frame_sync.cpp`
- Modify: `samples/01_triangle/main.cpp` (add `--present` mode), `src/rx_rhi_vk/CMakeLists.txt`
- Create: `samples/README.md`, `MANUAL_VERIFICATION.md`

**Interfaces:**
- Consumes: `Device` acquire/present/recreate (Task 1), pipeline/draw code (Task 5).
- Produces: `rx::rhi::FrameSync` with `static create(VkDevice, uint32_t queueFamily, uint32_t swapchainImageCount) -> std::optional<FrameSync>`; per-frame accessors for the current frame's fence/command buffer/imageAvailable semaphore; per-swapchain-image renderFinished semaphores; `framesInFlight()` (= 2); `onSwapchainRecreated(uint32_t newImageCount)`; destructor requires the caller to have reached device idle (document it — the loop below guarantees it).

**The two defects this replaces (original Task 13):** per-frame semaphores were created and destroyed every frame, destroyed immediately after `vkQueuePresentKHR` with no guarantee the presentation engine had consumed them (validation error / UB), and every frame did `runOnce` + `vkQueueWaitIdle` (full serialization). This task implements the canonical frames-in-flight pattern instead:

- **FrameSync owns:** per frame-in-flight (2): fence (created signaled), imageAvailable semaphore, command pool (`RESET_COMMAND_BUFFER` not needed — reset the pool), one primary command buffer. Per swapchain image: renderFinished semaphore (indexed by acquired image index — a renderFinished semaphore must not be reused until its image is re-acquired, which is exactly what per-image indexing guarantees).
- **Loop (in `--present` mode):** pump events (SDL_EVENT_QUIT → break); wait+reset current frame's fence; `device->acquireNextImage(frame.imageAvailable)` — on NeedsRecreate: `vkDeviceWaitIdle`, `recreateSwapchain`, `frameSync.onSwapchainRecreated(...)`, recreate per-image views, continue; reset pool, record into the acquired swapchain image (transition UNDEFINED→COLOR_ATTACHMENT_OPTIMAL, dynamic rendering with viewport/scissor from `swapchainExtent()`, draw triangle, transition →PRESENT_SRC_KHR); `vkQueueSubmit` waiting imageAvailable @ COLOR_ATTACHMENT_OUTPUT, signaling renderFinished[imageIndex], fencing the frame fence; `device->present(imageIndex, renderFinished[imageIndex])` — NeedsRecreate handled as above, DeviceLost → log + exit loop; advance frame index mod 2.
- **Shutdown:** `vkDeviceWaitIdle` BEFORE destroying FrameSync, views, pipeline — the only point sync objects may die.
- Per-swapchain-image views are created once after device creation and after each recreate — never per frame.
- This is the first and only place swapchain images are written — and only ever the acquired index.

**FrameSync test** (in `rx_rhi_vk_tests`): create a real windowed device (existing test pattern, skip-guarded), `FrameSync::create`, run 3 iterations of the full loop headlessly (hidden window — present may return NeedsRecreate; that path exercising recreate is a pass, not a failure), assert zero validation errors, clean teardown.

**Samples structure:** `samples/README.md` — what each sample shows, how to build/run both modes on Linux/Windows/Steam Deck, expected output (screenshot description). `MANUAL_VERIFICATION.md` at repo root — per-platform checklist (Linux `--present`, Windows .exe copy + run, Steam Deck Desktop Mode run; record distro/GPU/driver for each; the binary is statically linked — only the .exe/binary needs copying).

**Verify:** `--present` opens a window on this machine showing a white triangle on black, resizes without validation errors, closes cleanly; headless mode still exits 0; full ctest green; both presets build; commit clean.

---

