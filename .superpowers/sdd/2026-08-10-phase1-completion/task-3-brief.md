### Task 3: CommandContext + sync2 barriers + offscreen clear-color gate

**Files:**
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/command.h`, `src/rx_rhi_vk/src/command.cpp`
- Create: `src/rx_rhi_vk/tests/clear_color_test.cpp`
- Modify: `src/rx_rhi_vk/CMakeLists.txt`

**Interfaces:**
- Consumes: `Context::vkbInstance()` (headless device selection), `Allocator::createRaw` (Task 2).
- Produces: `rx::rhi::CommandContext` with `static create(VkDevice, VkQueue, uint32_t queueFamily) -> std::optional<CommandContext>` and `runOnce(const std::function<void(VkCommandBuffer)>&, VkSemaphore wait = VK_NULL_HANDLE, VkPipelineStageFlags waitStage = 0, VkSemaphore signal = VK_NULL_HANDLE)` — allocate/begin/record/end/submit(+optional wait/signal semaphores)/`vkQueueWaitIdle`/free. Free function `rx::rhi::transitionImage(VkCommandBuffer, VkImage, VkImageLayout oldL, VkImageLayout newL)` using `VkImageMemoryBarrier2` + `vkCmdPipelineBarrier2` (ALL_COMMANDS + MEMORY_READ|WRITE both sides, `VK_REMAINING_MIP_LEVELS`/`ARRAY_LAYERS`, color aspect). `runOnce` is a synchronous setup/test utility — the real frame loop (Task 6) does NOT use it.

**Test — offscreen clear via dynamic rendering, pure headless (no window/surface/swapchain):**
- Headless device selection: `Context::create({}, true)` (headless instance), then `vkb::PhysicalDeviceSelector selector(ctx->vkbInstance()); selector.defer_surface_initialization().set_minimum_version(1, 3).set_required_features_13(features13).select()`, `vkb::DeviceBuilder(...).build()`, `volkLoadDevice`, graphics queue + index.
- **Teardown ordering — the original plan's version of this test had two hard bugs; do not reproduce them:**
  1. Its helper struct held `rx::rhi::Context ctx;` by value with a default constructor — `Context` is not default-constructible; hold `std::optional<Context>` (or structure the test as a single flat function body, which is simpler and preferred).
  2. It called `vkDestroyDevice` manually at the end of the test scope while the `Allocator`, readback `Buffer`, and `CommandContext` RAII objects were still alive — their destructors then ran against a destroyed `VkDevice` (use-after-free). Correct structure: create the raw device first, then open an inner `{ }` scope containing CommandContext/Allocator/Buffer/image/view work, close the scope (all RAII destroyed, in inverse order, against a live device), then `vkDeviceWaitIdle` + destroy image view/image/memory (if not already inside the scope) + `vkDestroyDevice` last.
- Body: create a 4x4 `VK_FORMAT_R8G8B8A8_UNORM` image (COLOR_ATTACHMENT | TRANSFER_SRC, device-local, raw `vkCreateImage`/`vkAllocateMemory`/`vkBindImageMemory`/`vkCreateImageView`); `runOnce`: transition UNDEFINED→COLOR_ATTACHMENT_OPTIMAL, `vkCmdBeginRendering` with clear color {1,0,0,1} loadOp CLEAR storeOp STORE, `vkCmdEndRendering`, transition →TRANSFER_SRC_OPTIMAL; `Allocator::createRaw` + host-visible readback buffer (TRANSFER_DST); `runOnce`: `vkCmdCopyImageToBuffer`; assert first pixel bytes == (255,0,0,255); zero validation errors.

**Verify:** both presets build; full ctest green; commit clean.

---

