### Task 5: Triangle correctness gate — offscreen render + pixel readback (full stack, spec-valid)

**Files:**
- Create: `samples/01_triangle/main.cpp`, `samples/01_triangle/CMakeLists.txt`
- Modify: root `CMakeLists.txt` (`add_subdirectory(samples/01_triangle)`)

**Interfaces:**
- Consumes: everything from Tasks 1-4 (`Window`, `Context`, `Device`, `Allocator`, `Buffer`, `CommandContext`, `transitionImage`, `RX_TRIANGLE_*_SPV`).
- Produces: executable `sample_01_triangle`; ctest entry `sample_01_triangle_headless` (runs the binary with no args). Task 6 extends this same `main.cpp` with `--present`.

**Correctness requirement that supersedes the original plan:** the original Task 12 rendered into `swapchainImages()[0]` without ever acquiring it — a Vulkan spec violation (the application does not own a presentable image until `vkAcquireNextImageKHR` returns it; layout transitions and rendering on an un-acquired image are invalid regardless of whether a driver tolerates it). This task renders into a **dedicated offscreen `VkImage`** instead. The full stack is still exercised — window, surface, `Device::create` (swapchain created and queried, just not written to), pipeline, draw, readback.

**Body (headless mode, the default):**
1. Window (hidden, 256x256) → extensions (exit 1 with `RX_LOG_ERROR` if empty) → `Context::create(extensions, true)` → surface → `Device::create` → `Allocator::create` → `CommandContext::create(device->device(), device->graphicsQueue(), device->graphicsQueueFamily())`.
2. Offscreen target: 256x256 image in `device->swapchainFormat()` (COLOR_ATTACHMENT | TRANSFER_SRC — created via raw Vulkan like Task 3's test, or a small local helper), plus its view. Using the swapchain format keeps one pipeline valid for both this offscreen target and Task 6's real swapchain rendering; the readback assertions use only grayscale colors (white triangle, black clear), which are byte-order-identical in RGBA and BGRA, so the format's channel order cannot break them — keep it that way.
3. Pipeline: shader modules from `RX_TRIANGLE_VERT_SPV`/`RX_TRIANGLE_FRAG_SPV` files; empty pipeline layout; dynamic rendering (`VkPipelineRenderingCreateInfo` with the target format), dynamic viewport/scissor, no vertex input (SV_VertexID generation), no cull, no blend, 1 sample.
4. Record via `runOnce`: transition offscreen image UNDEFINED→COLOR_ATTACHMENT_OPTIMAL; begin rendering (clear black); set viewport/scissor; bind; draw 3; end; transition →TRANSFER_SRC_OPTIMAL.
5. Readback via `runOnce` + host-visible buffer; assert center pixel (128,150) is white (>200 per channel), corner (10,10) is black (<20), `!ctx->hasValidationErrors()`. Log `triangle readback PASSED`/`FAILED`, exit 0/1.
6. Teardown: all RAII; explicit `vkDestroy*` for the raw image/view/pipeline/layout/shader modules before their owning device dies (RAII scope ordering as in Task 3 — device outlives everything it allocated).
7. Register in ctest: `add_test(NAME sample_01_triangle_headless COMMAND sample_01_triangle)`.

**Verify:** binary prints PASSED and exits 0 on this machine (real GPU + display); full ctest green; both presets build; commit clean.

---

