# Phase 4 Research: Present-Mode Control, SDL3 Input, ImGui Backends, CI Gating

**Date:** 2026-08-11  
**Scope:** Research only — no implementation, citations per claim.

---

## 1. Present-Mode Control & Swapchain Recreation

### vk-bootstrap SwapchainBuilder API

**Present Mode Setting:**
- Method: `SwapchainBuilder::set_desired_present_mode(VkPresentModeKHR)` – part of builder chain before `.build()` [1]
- Default fallback: tries `VK_PRESENT_MODE_MAILBOX_KHR` if available, falls back to `VK_PRESENT_MODE_FIFO_KHR` [1]
- Modes available:
  - **FIFO_KHR**: Hard VSync, waits for next vertical blank (guaranteed on all platforms) [1, 2]
  - **MAILBOX_KHR**: Overwrites pending image if another rendered before display; no tearing; preferred for uncapped framerate [2, 3]
  - **IMMEDIATE_KHR**: Instant presentation; tearing possible; lowest latency [2, 3]

### Swapchain Recreation Flow

**Key constraint:** No direct toggle between modes. Must recreate swapchain.

**Destruction/Recreation sequence:** [4, 5]
- Call `vkAcquireNextImageKHR()` and `vkQueuePresentKHR()` will return `VK_ERROR_OUT_OF_DATE_KHR` on incompatibility (e.g., window resize, vsync toggle)
- Create new swapchain via `VkSwapchainCreateInfoKHR` with `oldSwapchain` field pointing to previous chain
- Destroy old swapchain only after current frames in-flight complete

**Synchronization objects & frames-in-flight:** [4, 5]
- Semaphores keyed by **swapchain image count** (e.g., per-image render/present semaphores): must destroy & recreate, as image count may change
- Semaphores keyed by **MAX_FRAMES_IN_FLIGHT**: survive recreation untouched
- Rationale: image count can change between swapchain generations; frame-in-flight count stays constant

**Example fallback ladder (recommended):** [3]
- vsync-on (user pref) → FIFO guaranteed
- vsync-off → try MAILBOX → try IMMEDIATE → fallback to FIFO (safe, works everywhere)

### Platform-Specific Availability

**Linux + X11 + Mesa:** [3, 6]
- FIFO works but can cause **severe system-wide stuttering** when other windows are moved/resized [6]
- MAILBOX & IMMEDIATE preferred if available
- Mesa Wayland reports `minImageCount=4` for MAILBOX due to internal buffering [3]

**Windows:** [3, 6]
- FIFO does not cause stuttering issues [6]
- MAILBOX & IMMEDIATE widely available

**Takeaway for Linux/X11:** Avoid FIFO for user-facing rendering if MAILBOX available; consider per-platform quirks in fallback ladder.

---

## 2. SDL3 Input APIs

### Relative Mouse Mode & Raw Deltas

**Window-relative mouse mode:**
- Function: `SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled)` [7]
- Hides cursor, grabs input to window, reports motion relative to last position
- Replaces deprecated `SDL_SetRelativeMouseMode()` from SDL2 [7]

**Mouse motion events:**
- Event type: `SDL_EVENT_MOUSE_MOTION` [7, 8]
- Fields: `xrel` and `yrel` as **floats** (higher precision than SDL2's int) [8]
- Semantics: relative motion since last motion event; works even if cursor reaches screen edge [7, 8]

**Cursor control:** [7]
- Show/hide via standard SDL windowing APIs (documented in CategoryMouse)
- Relative mode implicitly hides cursor

### Gamepad APIs

**Enumeration & opening:** [9, 10]
- Event: `SDL_EVENT_GAMEPAD_ADDED` fired when gamepad connected
- Function: `SDL_OpenGamepad(SDL_JoystickID instance_id)` returns `SDL_Gamepad*`
- Requires: `SDL_Init(SDL_INIT_GAMEPAD)` [9, 10]

**Reading axes:**
- Function: `SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)` [9, 10]
- Axis enum: `SDL_GAMEPAD_AXIS_LEFTX/Y`, `SDL_GAMEPAD_AXIS_RIGHTX/Y`, `SDL_GAMEPAD_AXIS_LEFT_TRIGGER`, `SDL_GAMEPAD_AXIS_RIGHT_TRIGGER` [9, 10]
- Return range:
  - Thumbsticks: -32768 (up/left) to +32767 (down/right) [10]
  - Triggers: 0 (released) to +32767 (fully pressed), never negative [10]

**Deadzone:**
- Thumbstick deadzone centered ~8000 units from zero [10]
- Advanced UIs allow per-gamepad calibration [10]

**Steam Deck integration:** [11]
- SDL3 recognizes Steam Deck as Xbox-like controller by default [11]
- SteamAPI_InitEx() must be called before SDL_Init() for Steam games [11]
- **Limitation:** Gyro and back paddles not detected even with `SDL_JOYSTICK_HIDAPI_STEAMDECK` hint; reported consistently across multiple decks [11]

---

## 3. Dear ImGui Backends & Configuration

### Current Version & Licensing

- **Latest stable:** v1.92.6 (built 2026-02-25 on master branch) [12]
- **License:** MIT (applies to all branches including docking) [12, 13]
- **Docking branch:** Active at `github.com/ocornut/imgui#docking`, kept in sync with master [12]

### SDL3 Platform Backend

- **File:** `imgui_impl_sdl3.cpp` (platform layer) [12]
- **Renderer options:**
  - `imgui_impl_sdlrenderer3.cpp` (2D SDL renderer)
  - `imgui_impl_sdlgpu3.cpp` (SDL_GPU unified graphics API, translates to Vulkan/DirectX 12/Metal) [12]

### Vulkan Renderer Backend with Dynamic Rendering

**File:** `imgui_impl_vulkan.cpp` [14]

**Dynamic rendering setup (required for modern Vulkan engines):** [14, 15]
- Set `ImGui_ImplVulkan_InitInfo.UseDynamicRendering = true`
- Set `ImGui_ImplVulkan_InitInfo.ColorAttachmentFormat` to your swapchain format
- Provide `VkPipelineRenderingCreateInfo` (colorAttachmentCount, pColorAttachmentFormats)
- Rationale: Uses dynamic rendering instead of VkRenderPass [14, 15]

**Initialization parameters:** [14, 15]
- **Required:** Instance, PhysicalDevice, Device, Queue, MinImageCount (>= 2)
- **Descriptor pool:** Either `DescriptorPool` (user-managed) OR `DescriptorPoolSize` (ImGui manages), **not both** [14]
- **Image count:** ImageCount >= MinImageCount [14]
- Example pool: ~1000 descriptors of various types suffices [14]

**Integration with custom frame graphs:** [14, 15]
- **Descriptor pool ownership:** If you provide DescriptorPool, ImGui does not manage its lifetime; you own cleanup
- **Font upload:** Occurs during `ImGui_ImplVulkan_Init()` — ensure upload/transfer queues ready before init
- **Potential pitfalls:**
  - Pool exhaustion if frame submissions stall
  - Premature pool destruction if ImGui still references descriptors
  - Mismatch between declared MinImageCount and actual frames-in-flight can cause synchronization issues [14]

---

## 4. CI Performance Regression Gating

### Primary Tools & Baseline Storage

**Primary action:** `github-action-benchmark` [16, 17]
- Consumes benchmark output files (format varies by language/tool)
- Stores historical baselines in GitHub Pages branch OR artifacts
- Posts regression alerts via commit comments or workflow failure
- Supports multiple languages: Rust, Go, C++, JavaScript, Python, polyglot [17]

**Alternative specialized tools:**
- `github-action-pull-request-benchmark` – compares PR vs base branch, posts PR comment [18]
- **Bencher** – dedicated continuous benchmarking platform with advanced analysis [18, 19]
- **Gungraun** (Rust) – uses instruction counts instead of wall-clock time [19]

### Baseline & Threshold Strategy

- Baselines typically stored **per-branch or per-commit** in repo or artifact storage
- Threshold: configurable percentage (e.g., 1.5% gate, catch ~1% false positive rate with tuning) [19]
- Gate enforcement: threshold violation can block PR merge or post warning comment [16, 17, 18]

### Handling Runner Noise

**Shared GitHub-hosted runners:** [19, 20]
- Wall-clock timing variance: **>30%** typical (no control over CPU scaling, co-tenancy, memory pressure, I/O) [20]
- **NOT recommended** for strict timing gates without filtering [20]

**Bare metal / dedicated runners:** [20]
- Variance: <2% achievable [20]
- Preferred for reliable wall-clock benchmarking [20]

**Alternative: Algorithmic counters instead of wall-clock:** [19, 20]
- Instruction counts (e.g., `PERF_COUNT_HW_INSTRUCTIONS` on Linux) are stable across runs on shared runners [19, 20]
- Measures logical work, not hardware artifacts [19, 20]
- **Recommended hybrid approach:** Gate on instruction count (stable, deterministic); publish wall-clock metrics for reference/trending but do not gate [20]

### Real-World Example Practices

- **Gungraun** (Rust benchmarking): gates on instruction counts via perf, optionally runs wall-clock in parallel for trend visibility [19]
- **Bencher** ecosystem: supports both wall-clock and algorithmic metrics, recommends instruction counts for CI on shared infrastructure [19]
- **Trivial case:** Store baseline in repo at `benchmarks/baseline.json`, compare PR results via action, threshold at ±1.5% [16]

---

## Summary & Recommendations for RendererX

1. **Present modes:** Use `set_desired_present_mode()` builder call; implement fallback: MAILBOX → IMMEDIATE → FIFO on vsync-off, always FIFO on vsync-on. Swapchain recreation requires cleanup of image-count semaphores but not frame-in-flight semaphores. On Linux/X11, prefer MAILBOX/IMMEDIATE over FIFO.

2. **SDL3 input:** Use `SDL_SetWindowRelativeMouseMode()`, read xrel/yrel (floats) from `SDL_EVENT_MOUSE_MOTION`. For gamepads: `SDL_EVENT_GAMEPAD_ADDED`, `SDL_GetGamepadAxis()`, deadzone ~8000. Steam Deck works but lacks gyro/back paddle detection.

3. **ImGui:** Version 1.92.6, MIT license, docking actively maintained. For Vulkan integration: enable `UseDynamicRendering`, provide descriptor pool (either user or ImGui-managed), declare `ColorAttachmentFormat`. Watch descriptor pool exhaustion and frame-in-flight count mismatch.

4. **CI gating:** Use `github-action-benchmark` or Bencher for baseline tracking. On shared runners, gate on instruction counts (Gungraun, perf) not wall-clock time; publish wall-clock as trend metric. Threshold ~1.5% balances signal/noise.

---

## Sources

[1] [vk-bootstrap Getting Started Documentation](https://charles-lunarg.github.io/vk-bootstrap/docs/getting_started.html)  
[2] [Vulkan Tutorial: Swap Chain](https://vulkan-tutorial.com/Drawing_a_triangle/Presentation/Swap_chain)  
[3] [Vulkan Guide: Vulkan Initialization](https://vkguide.dev/docs/chapter-1/vulkan_init_flow/)  
[4] [Vulkan Docs: Swap Chain Recreation](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/04_Swap_chain_recreation.html)  
[5] [Vulkan Samples: Swapchain Recreation](https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html)  
[6] [Maister's Graphics Adventures: WSI in Vulkan for Retro Emulators](https://themaister.net/blog/2018/09/09/the-state-of-window-system-integration-wsi-in-vulkan-for-retro-emulators/)  
[7] [SDL3 Wiki: SDL_SetWindowRelativeMouseMode](https://wiki.libsdl.org/SDL3/SDL_SetWindowRelativeMouseMode)  
[8] [SDL3 Wiki: SDL_MouseMotionEvent](https://wiki.libsdl.org/SDL3/SDL_MouseMotionEvent)  
[9] [SDL3 Wiki: CategoryGamepad](https://wiki.libsdl.org/SDL3/CategoryGamepad)  
[10] [SDL3 Wiki: SDL_GetGamepadAxis](https://wiki.libsdl.org/SDL3/SDL_GetGamepadAxis)  
[11] [GitHub libsdl-org/SDL Issue #9148: Steam Deck Reported as Steam Virtual Gamepad](https://github.com/libsdl-org/SDL/issues/9148)  
[12] [Dear ImGui GitHub Repository](https://github.com/ocornut/imgui)  
[13] [Dear ImGui Licenses](https://www.dearimgui.com/licenses/)  
[14] [imgui_impl_vulkan.h Backend Header](https://github.com/ocornut/imgui/blob/master/backends/imgui_impl_vulkan.h)  
[15] [Vulkan Guide: Setting Up ImGui](https://vkguide.dev/docs/new_chapter_2/vulkan_imgui_setup/)  
[16] [GitHub Action: Continuous Benchmark](https://github.com/benchmark-action/github-action-benchmark)  
[17] [GitHub Marketplace: Continuous Benchmark Action](https://github.com/marketplace/actions/continuous-benchmark)  
[18] [GitHub Action: Pull Request Benchmark](https://github.com/openpgpjs/github-action-pull-request-benchmark)  
[19] [Bencher: How to Use in GitHub Actions](https://bencher.dev/docs/how-to/github-actions/)  
[20] [Benchmarking in Noisy CI Environments: CodSpeed & Alternatives](https://codspeed.io/blog/benchmarks-in-ci-without-noise)
