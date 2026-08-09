# Task 3 Report: CommandContext + sync2 barriers + offscreen clear-color gate

Status: **DONE**

Commit: `a4739c8` — "Add rx_rhi_vk::CommandContext + sync2 transitionImage; headless clear-color test"

## What was built

### `src/rx_rhi_vk/include/rx_rhi_vk/command.h` / `src/rx_rhi_vk/src/command.cpp`

- `rx::rhi::CommandContext`: move-only RAII owning a single `VkCommandPool` (created with `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`) against a `VkDevice`/`VkQueue`/queue-family triple.
  - `static create(VkDevice, VkQueue, uint32_t queueFamily) -> std::optional<CommandContext>` — `vkCreateCommandPool`, `std::nullopt` + `RX_LOG_ERROR` on failure.
  - `runOnce(const std::function<void(VkCommandBuffer)>& record, VkSemaphore wait = VK_NULL_HANDLE, VkPipelineStageFlags waitStage = 0, VkSemaphore signal = VK_NULL_HANDLE)`: allocates one primary command buffer, begins it with `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT`, invokes `record`, ends it, submits with optional wait/signal semaphores (only populated in `VkSubmitInfo` when non-null, matching the brief's default-null-for-setup-work intent), `vkQueueWaitIdle`, then `vkFreeCommandBuffers`. Every step checks its `VkResult` and bails (logging + freeing the command buffer) rather than continuing on a bad handle.
  - Destructor/move-assign destroy the pool via `vkDestroyCommandPool` (which implicitly frees every command buffer ever allocated from it), following the same self-move-guarded move pattern as `Device`/`Allocator`/`Buffer`.
  - Header comment is explicit that `runOnce` is a synchronous setup/test utility only — the real frame loop (Task 6) will manage its own command buffers/fences instead, per the brief.
- Free function `rx::rhi::transitionImage(VkCommandBuffer, VkImage, VkImageLayout oldLayout, VkImageLayout newLayout)`: builds one `VkImageMemoryBarrier2` (`VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT` + `VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT` on both src/dst, `VK_REMAINING_MIP_LEVELS`/`VK_REMAINING_ARRAY_LAYERS`, `VK_IMAGE_ASPECT_COLOR_BIT`, `VK_QUEUE_FAMILY_IGNORED` on both sides) and records it via one `VkDependencyInfo` + `vkCmdPipelineBarrier2`, exactly per the brief.

### `src/rx_rhi_vk/tests/clear_color_test.cpp`

One flat `TEST_CASE`, pure headless (no `rx::platform::Window`, no `VkSurfaceKHR`, no swapchain):

1. `Context::create({}, true)` → headless, validated instance.
2. `vkb::PhysicalDeviceSelector` with `VkPhysicalDeviceVulkan13Features{dynamicRendering, synchronization2}`, `set_minimum_version(1,3)`, `select()`; `vkb::DeviceBuilder(...).build()`; `volkLoadDevice`; graphics queue + family via `get_queue`/`get_queue_index`.
3. Raw `vkCreateImage`/`vkAllocateMemory`/`vkBindImageMemory`/`vkCreateImageView` for a 4×4 `VK_FORMAT_R8G8B8A8_UNORM` image (`COLOR_ATTACHMENT | TRANSFER_SRC`, device-local).
4. Inner `{ }` scope: `CommandContext::create` → `runOnce` (transition UNDEFINED→COLOR_ATTACHMENT_OPTIMAL, `vkCmdBeginRendering`/`vkCmdEndRendering` with clear color `{1,0,0,1}` LOAD_OP_CLEAR/STORE_OP_STORE, transition →TRANSFER_SRC_OPTIMAL) → `Allocator::createRaw` + `createHostVisibleBuffer(..., TRANSFER_DST)` → `runOnce` (`vkCmdCopyImageToBuffer`) → `memcpy` the mapped pointer into a local `std::array<uint8_t, 64>`. Scope closes: readback `Buffer`, `Allocator`, `CommandContext` destroyed in reverse order, device still alive.
5. After the scope: `vkDeviceWaitIdle` + `vkDestroyImageView`/`vkDestroyImage`/`vkFreeMemory` + `vkb::destroy_device` — device destroyed last, nothing after it.
6. Assertions: `pixels[0..3] == {255,0,0,255}`, `CHECK_FALSE(ctx->hasValidationErrors())`.

Added to `rx_rhi_vk_tests` in `src/rx_rhi_vk/CMakeLists.txt` (`src/command.cpp` to the library, `tests/clear_color_test.cpp` to the test binary); comments updated to describe both.

## Two deviations from the brief, verified against source (not assumed)

1. **Dropped `defer_surface_initialization()`.** The brief's plan called for `selector.defer_surface_initialization().set_minimum_version(...)...`. Running it produced a real, reproducible validation error: `VUID-vkCreateDevice-ppEnabledExtensionNames-01387: Missing extension required by the device extension VK_KHR_swapchain: VK_KHR_surface`. Root-caused directly against vk-bootstrap's pinned commit (`556b79b165386f6c1a18362d30f2a076fdaa2778`) source:
   - `PhysicalDeviceSelector`'s constructor already sets `criteria.require_present = !instance.headless` — for our headless `Context`, `require_present` is already `false`, so `select()` never needed the surface-present check `defer_surface_initialization()` exists to bypass.
   - `DeviceBuilder::build()` unconditionally adds `VK_KHR_SWAPCHAIN_EXTENSION_NAME` whenever `physical_device.surface != VK_NULL_HANDLE || physical_device.defer_surface_initialization` — calling `defer_surface_initialization()` on a selector with no surface at all requests `VK_KHR_swapchain` anyway, which then fails because the headless instance never enabled the prerequisite `VK_KHR_surface` instance extension.

   Fix: omit the call entirely. This test never creates a `VkSurfaceKHR`/`VkSwapchainKHR`, so there's no later surface attachment to defer for — this is the *correct* headless selection, not a workaround. Full reasoning is inline as a comment at the call site.

2. **Verified host-coherence instead of assuming it.** The brief's readback step relies on reading `Buffer::mappedData()` directly after `runOnce()`'s `vkQueueWaitIdle()`, with no `vkInvalidateMappedMemoryRanges`/`vmaInvalidateAllocation` call. Checked directly against VMA v3.4.0's `FindMemoryPreferences()`: for `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT` (what `Allocator::createHostVisibleBuffer` uses), only `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT` is a hard requirement — `HOST_COHERENT` is never required, only `HOST_CACHED` is avoided. `Buffer`/`Allocator`'s public API (Task 2, not modified here) has no path to the underlying `VmaAllocation` to call `vmaInvalidateAllocation()` explicitly. Rather than silently assume coherence, the test queries `vkGetPhysicalDeviceMemoryProperties` and `REQUIRE`s that every `HOST_VISIBLE` memory type on the selected physical device is also `HOST_COHERENT` before doing the raw `memcpy` — turning an unverified assumption into a checked, self-documenting precondition (confirmed true on the dev machine: 3 host-visible types, all coherent).

## Verify

Clean rebuild, `linux-native`:
```
$ rm -rf build/linux-native && cmake --preset linux-native      # clean configure
$ cmake --build --preset linux-native                          # 36/36, zero warnings, zero errors
$ ctest --preset linux-native --output-on-failure
100% tests passed, 0 tests failed out of 4
```
Direct run (`./rx_rhi_vk_tests --success`): 5 test cases, 63 assertions, 0 failed. `clear_color_test.cpp` specifically: all `REQUIRE`s pass (including the new host-coherence check, `4 != 0` i.e. `HOST_COHERENT` bit set, on all 3 host-visible memory types this GPU exposes), `pixels[0..3] == {255,0,0,255}`, `CHECK_FALSE(ctx->hasValidationErrors())` true. Only logged output besides successes is the pre-existing, already-documented `isKnownPortabilityEnumerationLayerBug` false positive — zero real validation errors.

Order-independence (the whole point of the doctest_main.cpp warm-up this test relies on): re-ran the full binary with `--order-by=rand --rand-seed=1`, `--rand-seed=42`, and `--order-by=reverse` — all three: 5/5 test cases, 59/59 assertions passed. Repeated the full `ctest` 3× — stable, no flakiness.

Clean rebuild, `windows-cross-zig`:
```
$ rm -rf build/windows-cross-zig && cmake --preset windows-cross-zig   # clean configure
$ cmake --build --preset windows-cross-zig                             # 36/36, zero warnings, zero errors
```
`rx_rhi_vk_tests.exe` (including `command.cpp.obj`, `clear_color_test.cpp.obj`) links successfully.

## Self-review

- Teardown ordering (the brief's core concern): confirmed by reading the test top-to-bottom — `CommandContext`/`Allocator`/readback `Buffer` are all declared inside the inner `{ }` scope and destroyed there, in reverse declaration order, while the raw `VkDevice` is still alive; `vkDeviceWaitIdle`/image-view/image/memory/`vkb::destroy_device` all run afterward, device last. No RAII object's destructor runs after `vkb::destroy_device`.
- No default-constructed-by-value `Context`/`Device` anywhere: the test holds `std::optional<Context>` (via `auto ctx = Context::create(...)`) and raw `vkb::Device`/`VkDevice` handles directly — no helper struct, matching the brief's stated preference for "a flat single-function test body with an inner scope."
- `runOnce`'s wait/signal/waitStage parameters default to `VK_NULL_HANDLE`/`0` and are only wired into `VkSubmitInfo` when non-null — verified this compiles and behaves correctly for both call sites in the test (neither passes them, both work).
- `transitionImage` is exactly the barrier shape the brief specified — verified by re-reading `VkImageMemoryBarrier2`'s fields against volk's Vulkan 1.3 headers before writing it, not assumed.
- Zero validation errors confirmed by direct test-binary run output, not just asserted in code; also confirmed order-independence explicitly (random seeds + reverse) since this is a headless test joining a binary whose other tests are windowed.
- `git status`/`git diff --stat` checked before committing: only the four files this task owns were staged — no incidental changes to `progress.md` or any other concurrently-touched file.
- Commit message contains no AI attribution (verified via `git log -1 --format='%B'`); author identity untouched.

## Concerns

- None new beyond the pre-existing vk-bootstrap process-wide function-pointer cache landmine (documented on `Context::create`, worked around structurally in `doctest_main.cpp` since Task 1) — this test relies on that warm-up for safety in any test order and was explicitly verified against it (random seeds + reverse order above).
- The host-coherence `REQUIRE` added in this test is a real, verified precondition for *this* test's correctness, but it's local to this one test file — any future test or production code that reads back GPU writes through `Allocator::createHostVisibleBuffer`'s mapped pointer without an explicit invalidate is relying on the same unstated assumption (true on every desktop driver in practice, per VMA's own docs, but not enforced by `Buffer`/`Allocator`'s API). Worth a note for whoever builds a "real" readback path later — either expose an invalidate path on `Buffer`, or document the assumption at the `Allocator` level instead of re-deriving it per call site.
