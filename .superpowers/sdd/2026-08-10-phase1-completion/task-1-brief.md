### Task 1: Complete rx_rhi_vk::Device (device, queues, swapchain, acquire/present/recreate)

**Files:**
- Modify: `src/rx_rhi_vk/include/rx_rhi_vk/context.h`, `src/rx_rhi_vk/src/context.cpp` (store `vkb::Instance`)
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/device.h`, `src/rx_rhi_vk/src/device.cpp`
- Create: `src/rx_rhi_vk/tests/device_test.cpp`
- Modify: `src/rx_rhi_vk/CMakeLists.txt` (add device.cpp; add device_test.cpp; tests link `rx_platform`)

**Interfaces:**
- Consumes: `rx::platform::Window` (surface creation), `rx::rhi::Context`.
- Produces (later tasks rely on these exact names):
  - `Context::vkbInstance() const -> const vkb::Instance&`
  - `rx::rhi::SwapchainStatus` enum: `Ok, NeedsRecreate, DeviceLost`
  - `rx::rhi::AcquireResult { SwapchainStatus status; uint32_t imageIndex; }`
  - `rx::rhi::Device` with: `static create(Context&, VkSurfaceKHR) -> std::optional<Device>`, `physicalDevice()`, `device()`, `graphicsQueue()`, `graphicsQueueFamily() -> uint32_t`, `presentQueue()`, `swapchain()`, `swapchainImages() -> const std::vector<VkImage>&`, `swapchainFormat() -> VkFormat`, `swapchainExtent() -> VkExtent2D`, `acquireNextImage(VkSemaphore signal) -> AcquireResult`, `present(uint32_t imageIndex, VkSemaphore wait) -> SwapchainStatus`, `recreateSwapchain(VkSurfaceKHR) -> bool`.

**Step A — extend Context (do this first, it unblocks everything):** store the `vkb::Instance` by value (`#include <VkBootstrap.h>` in the header; forward declaration is not possible for a by-value member). Constructor takes `(vkb::Instance, std::shared_ptr<int>)`; `instance()`/`debugMessenger()` read through `vkbInstance_.instance` / `.debug_messenger`; destructor and move-assignment tear down via `vkb::destroy_instance(vkbInstance_)` (it destroys the messenger and instance together) guarded by `vkbInstance_.instance != VK_NULL_HANDLE`; moved-from objects null the handle. Preserve the false-positive guard and error-count mechanism untouched. Re-run `rx_rhi_vk_tests` (context test) before proceeding — this refactor must not break Task 6's shipped behavior.

**Step B — Device.** `Device::create`:
- Build `VkPhysicalDeviceVulkan13Features features13{}` with `.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES`, `.dynamicRendering = VK_TRUE`, `.synchronization2 = VK_TRUE`.
- `vkb::PhysicalDeviceSelector selector(context.vkbInstance()); selector.set_surface(surface).set_minimum_version(1, 3).set_required_features_13(features13).select();`
- `vkb::DeviceBuilder(physResult.value()).build()`; `volkLoadDevice(vkbDevice.device)`.
- Queues: `get_queue(vkb::QueueType::graphics)`, `get_queue_index(vkb::QueueType::graphics)`, `get_queue(vkb::QueueType::present)` — all checked, all stored.
- Swapchain: `vkb::SwapchainBuilder swapchainBuilder(vkbDevice, surface); swapchainBuilder.build()`; store `swapchain`, `get_images()`, `image_format`, `extent`.
- Every failure path: `RX_LOG_ERROR` with the vkb error message, return `std::nullopt`.
- Ownership: Device owns and destroys, in dtor/move-assign order: swapchain → device → surface. Document in the header that `Device::create` takes ownership of the surface. Move-only, moved-from handles nulled; move ctor may delegate to move-assign only because all members have `VK_NULL_HANDLE`/zero default initializers — keep those initializers.

`acquireNextImage(VkSemaphore signal)`: `vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, signal, VK_NULL_HANDLE, &idx)`; `VK_SUCCESS`/`VK_SUBOPTIMAL_KHR` → `{Ok, idx}` (suboptimal still owns the image — render then let present report NeedsRecreate); `VK_ERROR_OUT_OF_DATE_KHR` → `{NeedsRecreate, 0}`; `VK_ERROR_DEVICE_LOST` → log + `{DeviceLost, 0}`; anything else → log the VkResult + `{NeedsRecreate, 0}`.

`present(uint32_t imageIndex, VkSemaphore wait)`: standard `VkPresentInfoKHR` (wait semaphore only if non-null); `VK_SUCCESS` → Ok; `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR` → NeedsRecreate; `VK_ERROR_DEVICE_LOST` → log + DeviceLost; else log + NeedsRecreate.

`recreateSwapchain(VkSurfaceKHR surface)`: `vkDeviceWaitIdle` first; destroy old swapchain; rebuild via `vkb::SwapchainBuilder(physicalDevice_, device_, surface)` (raw-handle ctor — if this pinned vk-bootstrap commit's ctor signature differs, check `VkBootstrap.h` and adapt; there is also an overload taking explicit queue indices); refresh images/format/extent; false on failure with `RX_LOG_ERROR`.

**Step C — tests** (`device_test.cpp`, follows the existing window-based test pattern: skip gracefully with `MESSAGE` if `requiredVulkanInstanceExtensions()` is empty, but on this machine it must actually run — a silent skip is a failure to investigate):
1. Create window (hidden) → extensions → `Context::create(extensions, true)` → surface → `Device::create`. Assert: device/queues non-null, `swapchainImages().size() > 0`, `swapchainFormat() != VK_FORMAT_UNDEFINED`, extent non-zero, `!ctx->hasValidationErrors()`.
2. Acquire/present round-trip: create one `VkSemaphore`; `acquireNextImage(sem)` must return `Ok`; transition the acquired image `UNDEFINED → PRESENT_SRC_KHR` and submit with a raw `vkQueueSubmit` that waits on the semaphore at `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT` and signals a second semaphore + a fence; wait the fence; `present(imageIndex, renderFinishedSem)` must return `Ok` or `NeedsRecreate` (hidden-window presentation is implementation-defined), never `DeviceLost`; `vkDeviceWaitIdle` before destroying the semaphores/fence. (Record commands with a throwaway `VkCommandPool` created/destroyed locally — the shared CommandContext arrives in Task 3; keep this test self-contained.) The fence-wait before semaphore destruction is the point: never destroy a semaphore the presentation engine might still consume without an idle/fence guarantee — `vkDeviceWaitIdle` before teardown.
3. Both existing test cases (context) still pass; zero validation errors throughout.

**Verify:** `cmake --build --preset linux-native && ctest --preset linux-native --output-on-failure` all green; `cmake --build --preset windows-cross-zig` clean. Commit (no AI attribution — verify `git log -1 --format='%B'`).

---

