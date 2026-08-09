# Phase 1 Completion Plan (supersedes Tasks 7-15 of the 2026-08-09 plan)

> **For agentic workers:** Executed via subagent-driven development. Each task is dispatched to a fresh implementer with this plan's task section as its brief. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Finish Phase 1 — a spec-correct Vulkan 1.3 device/swapchain/RHI, a pixel-verified triangle sample with a production-grade frame loop, green CI on both build targets, and deployed sample binaries.

**Why this plan exists:** A direct audit of the original plan's remaining tasks (7-15) found real defects: rendering into a never-acquired swapchain image (spec violation), per-frame semaphore destruction races, VMA integration that would assert/fail-to-link (missing `*2` function pointers for API 1.3, missing `VMA_IMPLEMENTATION`, missing sibling-scope propagation), a test with use-after-free teardown ordering and a non-compiling struct, a meaningless no-op build-budget check, and no samples/deployment story. This plan fixes all of it and resequences the work. The original plan's Tasks 7-15 are superseded and must not be executed.

**Architecture:** Same stack as built so far: zig cross-toolchain (both presets green), dep-cache (spdlog, SDL3, Vulkan-Headers 1.4.357, vk-bootstrap 556b79b), volk compiled into `rx_rhi_vk` (`VK_NO_PROTOTYPES`), `rx_core`/`rx_platform`/`rx_rhi_vk` static libs. Vulkan 1.3 baseline: dynamic rendering + synchronization2 only.

**Tech Stack additions this plan:** VMA v3.4.0 (FetchContent, source-only), Slang v2026.14.1 (prebuilt binaries only), GitHub Actions CI, `gh` for releases.

## Global Constraints

- Vulkan is the only GPU backend. Baseline features: dynamic rendering + synchronization2 (Vulkan 1.3) — no mesh shaders, no HW ray tracing, nothing beyond 1.3 core required at runtime. Headers are 1.4.357 (types only, additive); `require_api_version(1, 3, 0)` stays.
- Target platforms: Windows + Linux (Steam Deck = `linux-native`). Both `cmake --preset linux-native` and `cmake --preset windows-cross-zig` must configure+build cleanly after every task.
- Pinned deps build once via the dep-cache; cache hits must trigger zero compilation. Slang is never compiled from source.
- Warm-cache builds of project code: under 1 minute.
- No AI attribution in any commit (per `/media/ywadi/second/renderer_x/CLAUDE.md`). Verified directly after every task.
- No placeholder/TODO code. Prefer ready-made libraries over from-scratch code (CLAUDE.md policy).
- **Every Vulkan usage must be spec-valid, not merely driver-tolerated.** Zero validation errors is an acceptance bar for every GPU-touching task (modulo the documented layer false-positive guard in `context.cpp`).

## As-built reality (binding facts for all tasks; the original plan's snippets predate these)

- `rx::rhi::Context` (src/rx_rhi_vk/include/rx_rhi_vk/context.h) stores raw `VkInstance` + `VkDebugUtilsMessengerEXT` + `shared_ptr<int>` error count. It does NOT store or expose `vkb::Instance`. `context.cpp` contains a documented validation-layer false-positive guard (`isKnownPortabilityEnumerationLayerBug`) that must be preserved.
- `Vulkan::Headers` comes from the dep-cache (tag `vulkan-sdk-1.4.357.0`), promoted `IMPORTED_GLOBAL`. Never `find_package(Vulkan)` anywhere — it requires a loader import lib that doesn't exist when cross-compiling.
- volk is `FetchContent_Populate`d (source only) with `volk_SOURCE_DIR` explicitly propagated `PARENT_SCOPE` from third_party/CMakeLists.txt to the root scope (sibling-directory visibility). `volk.c` is compiled into `rx_rhi_vk`. Any new source-only fetch (VMA) needs the same propagation pattern.
- SDL3 consumers link `SDL3::SDL3-static` (the `SDL3::SDL3` ALIAS can't be promoted global).
- Windows RC compiler: `cmake/zig-wrappers/zig-rc-windows` (wraps `zig rc`); don't disturb.
- `rx_platform::Window` API: `create(title, w, h, visible) -> std::optional<Window>`, `sdlWindow()`, `pumpEvents()`, `requiredVulkanInstanceExtensions()`, `createVulkanSurface(VkInstance)`.
- Tests: doctest with a dedicated `doctest_main.cpp` TU per test executable (`DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`).
- The dep-cache key does NOT hash `CMAKE_ARGS` — if a task changes a cached dep's CMAKE_ARGS without changing its TAG, it must instruct a manual `.deps-cache` invalidation for that dep and say so in its report.

---

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

### Task 2: VMA integration done right (Allocator + host-visible Buffer)

**Files:**
- Modify: `third_party/CMakeLists.txt` (VMA v3.4.0 via guarded `FetchContent_Populate` + `set(vma_SOURCE_DIR ... PARENT_SCOPE)` — mirror the volk block and its comment style; VMA's own CMakeLists must NOT be added)
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/buffer.h`, `src/rx_rhi_vk/src/buffer.cpp`, `src/rx_rhi_vk/src/vma_impl.cpp`
- Create: `src/rx_rhi_vk/tests/buffer_test.cpp`
- Modify: `src/rx_rhi_vk/CMakeLists.txt` (add sources; add `${vma_SOURCE_DIR}/include` to PUBLIC include dirs)

**Interfaces:**
- Consumes: `Context`, `Device` (Task 1).
- Produces: `rx::rhi::Allocator` with `static create(Context&, Device&) -> std::optional<Allocator>`, `static createRaw(VkPhysicalDevice, VkDevice, VkInstance) -> std::optional<Allocator>` (shared impl; `create` delegates to `createRaw`), `createHostVisibleBuffer(VkDeviceSize, VkBufferUsageFlags) -> std::optional<Buffer>`; `rx::rhi::Buffer` with `handle()`, `mappedData()`, `size()`. Both move-only RAII.

**The three defects this task must not reproduce (the original plan had all three):**
1. **Function pointers:** use `#define VMA_STATIC_VULKAN_FUNCTIONS 0` and `#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1` (both defined before every `#include <vk_mem_alloc.h>` — put them in `buffer.h` above the include). Fill `VmaVulkanFunctions` with ONLY `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` (volk provides both globals) and let VMA fetch everything else itself. Do NOT hand-fill a partial 1.0-era table: with `vulkanApiVersion = VK_API_VERSION_1_3`, VMA requires the `*2` variants (`vkGetBufferMemoryRequirements2`, `vkBindBufferMemory2`, `vkGetPhysicalDeviceMemoryProperties2`, …) and asserts/fails if they're missing.
2. **Implementation TU:** `src/rx_rhi_vk/src/vma_impl.cpp` contains exactly:
   ```cpp
   #define VMA_STATIC_VULKAN_FUNCTIONS 0
   #define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
   #define VMA_IMPLEMENTATION
   #include <volk.h>
   #include <vk_mem_alloc.h>
   ```
   Nothing anywhere else defines `VMA_IMPLEMENTATION`.
3. **CMake scope:** `vma_SOURCE_DIR` must be explicitly propagated `PARENT_SCOPE` in third_party/CMakeLists.txt (same sibling-scope reason as volk — copy that block's comment rationale).

Buffer creation: `VMA_MEMORY_USAGE_AUTO` + `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT`; store `allocationInfo.pMappedData`. All failures `RX_LOG_ERROR` + nullopt. RAII: Buffer destroys via `vmaDestroyBuffer`; Allocator via `vmaDestroyAllocator`; move-only, nulled moved-from.

**Test** (window → context → surface → device → allocator, all RAII in one scope so destruction order is automatically inverse — allocator/buffer die before Device): write a 3-vertex float pattern into a host-visible vertex buffer, `memcmp` round-trip, size check, zero validation errors.

**Verify:** both presets build; full ctest green on linux-native; commit clean.

---

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

### Task 4: Slang prebuilt fetch + triangle shaders (parallel-safe lane)

**Files:**
- Create: `tools/fetch_slang.cmake`, `tools/fetch_slang_test.sh`
- Create: `shaders/triangle.vert.slang`, `shaders/triangle.frag.slang`, `shaders/CMakeLists.txt`, `shaders/tests/spirv_validity_test.cpp`
- Modify: root `CMakeLists.txt` (add `include(tools/fetch_slang.cmake)` before `add_subdirectory(shaders)`; add `add_subdirectory(shaders)`)

**Interfaces:**
- Produces: `RX_SLANGC` cache variable (path to fetched `slangc`); build targets `triangle_shaders` (custom target producing `${CMAKE_BINARY_DIR}/shaders/triangle.vert.spv` + `triangle.frag.spv`), cache variables `RX_TRIANGLE_VERT_SPV`/`RX_TRIANGLE_FRAG_SPV` (INTERNAL, absolute paths); ctest `shader_spirv_test`.

**Fetch script** (`tools/fetch_slang.cmake`): Slang `2026.14.1` prebuilt release archives from `https://github.com/shader-slang/slang/releases/download/v2026.14.1/` — `slang-2026.14.1-linux-x86_64-glibc-2.27.tar.gz` for Linux host; extract into `third_party/slang-prebuilt/<platform>/` with a `.rx-fetched` marker for idempotency; `file(DOWNLOAD ... STATUS)` checked, `FATAL_ERROR` on failure naming the URL; `file(ARCHIVE_EXTRACT)`. **Verify the archive's internal layout before assuming `bin/slangc`** — list the extracted tree and set `RX_SLANGC` to the real path (archives may or may not have a top-level directory). Note: slangc runs on the HOST, so always fetch the host (Linux) archive for the compiler even under the windows-cross-zig preset — shader compilation is host-side tooling; guard accordingly (`CMAKE_HOST_SYSTEM_NAME`, not `CMAKE_SYSTEM_NAME`).

**Shaders:** vertex shader generates a triangle from `SV_VertexID` (3 hardcoded NDC positions: `(0,-0.5) (0.5,0.5) (-0.5,0.5)`), solid white color; fragment returns it. Slang syntax: `[shader("vertex")]` / `[shader("fragment")]` entry points named `main`.

**Compile:** `add_custom_command` invoking `${RX_SLANGC} <src> -target spirv -profile sm_6_0 -entry main -o <out>`. **If this pinned slangc rejects any flag, run `${RX_SLANGC} -h`, adapt, and record the actual working flags in your report** — flag drift across Slang releases is expected; the build-and-run gate exists to catch it. The `.spv` outputs must be regenerated when the `.slang` sources change (DEPENDS).

**Test:** `shader_spirv_test` reads both `.spv` files, asserts the SPIR-V magic number `0x07230203` (first 4 bytes, little-endian). Paths injected via `target_compile_definitions`.

**Verify:** `cmake --preset linux-native && cmake --build --preset linux-native && ctest --preset linux-native -R shader_spirv_test`; `./tools/fetch_slang_test.sh` (asserts `slangc -v` reports 2026.14.1); re-configure a second time and confirm no re-download (marker works); `windows-cross-zig` still configures+builds. Commit clean.

---

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

### Task 7: CI matrix, real build-budget check, Wine test fix, sample artifacts

**Files:**
- Create: `.github/workflows/ci.yml`, `tools/check_build_budget.sh`
- Modify: `src/rx_core/tests/log_test.cpp` (one-line CRLF normalization)

**Fixes over the original Task 14:**
1. **Budget check must measure a real incremental build, not a no-op:** `tools/check_build_budget.sh <preset> [budget=60]` does `touch src/rx_core/src/log.cpp` (a leaf .cpp that forces recompile+relink of the dependent chain) BEFORE timing `cmake --build --preset <preset>`; fail if over budget.
2. **zig download integrity:** pin the known sha256 of `zig-x86_64-linux-0.16.0.tar.xz` (compute it from ziglang.org's published checksums — verify, don't invent) and `sha256sum -c` after download in both jobs.
3. **Wine ctest failure:** `log_test.cpp`'s captured-output comparison fails under Wine because Windows spdlog emits `\r\n`. Normalize before comparing (strip trailing `\r` and `\n` from the captured string; compare against `"hello 42"`). This is a 2-line test change that makes the test portable — do it in this task and confirm `rx_core_tests` passes under `wine` locally.
4. **Jobs:**
   - `linux-native` (ubuntu-latest): apt `ninja-build mesa-vulkan-drivers vulkan-tools libvulkan-dev xvfb`; zig install (checksummed); `actions/cache` on `.deps-cache` keyed on `hashFiles('third_party/CMakeLists.txt')`; configure; build; `xvfb-run -a ctest --preset linux-native --output-on-failure` (lavapipe provides the Vulkan device; the SDL window runs under Xvfb — if the GPU-dependent tests genuinely cannot run in this environment, they skip via their existing guards and the job must still prove `sample_01_triangle_headless` ran — investigate rather than accepting silent skips; if lavapipe genuinely can't run it, say so explicitly in the workflow with a comment and a dedicated `ctest -R` allowlist, never a silent pass); budget check.
   - `windows-cross-zig` (ubuntu-latest): apt `ninja-build wine xvfb`; zig (checksummed); `.deps-cache` cache; configure; build; `wine build/windows-cross-zig/tools/toolchain_check/toolchain_check.exe`; `xvfb-run -a ctest --preset windows-cross-zig -E 'rx_rhi_vk|sample' --output-on-failure` (GPU tests excluded — Wine in CI has no Vulkan; rx_core/rx_platform tests run, platform tests hit their skip guards gracefully); budget check.
   - Both jobs: upload the sample binary as a workflow artifact (`sample_01_triangle` / `sample_01_triangle.exe`) with `actions/upload-artifact`.
5. Validate YAML locally, commit, push, `gh run watch` until green — a red first run gets fixed in this task, not deferred.

**Verify:** both CI jobs green on GitHub (for real); local `wine`-run of rx_core_tests passes; budget script measures a real rebuild. Commit clean.

---

### Task 8: Phase 1 close-out — final review, release, deployed samples

Not an implementer task — coordination:
1. Final whole-branch review (Sonnet, `superpowers:requesting-code-review` template) over the whole phase (`merge base = e6afc9e`, the pre-implementation commit), pointed at the ledger's deferred-minor/parked list to triage what must be fixed before release. One fix-wave dispatch if findings, one scoped re-review.
2. `gh release create v0.1.0-phase1` with: Linux `sample_01_triangle` binary, Windows `sample_01_triangle.exe` (from a fresh local build of both presets), release notes summarizing what Phase 1 delivers and how to run the sample on each platform.
3. Root `README.md` quickstart (build presets, run the sample, screenshot description) — dispatched as a Haiku doc task, reviewed.
4. Ledger closed out; plan marked complete.

---

## Execution order, model tiers, parallelism

| Order | Task | Model | Lane |
|---|---|---|---|
| 1 | Task 1 Device | Sonnet | main |
| 1 (parallel) | Task 4 Slang+shaders | Sonnet | worktree (disjoint files; merged by coordinator before Task 5) |
| 2 | Task 2 VMA | Sonnet | main |
| 3 | Task 3 Commands+clear gate | Sonnet | main |
| 4 | Task 5 Triangle gate | Sonnet | main (needs Task 4 merged) |
| 5 | Task 6 FrameSync+samples | Sonnet | main |
| 6 | Task 7 CI+artifacts | Sonnet | main |
| 7 | Task 8 close-out | coordinator + Sonnet review + Haiku README | main |

Every task: Sonnet reviewer, task-scoped gate, direct commit-hygiene check by the coordinator, ledger entry. All tasks end with both presets building and full ctest green on linux-native.
