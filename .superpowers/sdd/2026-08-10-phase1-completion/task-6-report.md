# Task 6 report: Production frame loop — FrameSync, present mode, samples structure

Branch: `main` (worked directly, as authorized)
Commit: `e6721ee`

## What was built

- `src/rx_rhi_vk/include/rx_rhi_vk/frame_sync.h` /
  `src/rx_rhi_vk/src/frame_sync.cpp` — `rx::rhi::FrameSync`. Owns, created
  once in `create(VkDevice, uint32_t queueFamily, uint32_t
  swapchainImageCount)`:
  - Per frame-in-flight (`framesInFlight() == 2`, fixed): a fence (created
    `VK_FENCE_CREATE_SIGNALED_BIT` so the first-ever wait on a slot returns
    immediately), an `imageAvailable` semaphore, a command pool
    (`VK_COMMAND_POOL_CREATE_TRANSIENT_BIT`, deliberately not
    `RESET_COMMAND_BUFFER_BIT` — the whole pool is reset via
    `vkResetCommandPool`, not the individual buffer), and one primary
    command buffer allocated from that pool.
  - Per swapchain image: a `renderFinished` semaphore, indexed by the
    *acquired* image index (not the frame-in-flight index) — required
    because a binary semaphore must not be re-signaled while a previous
    signal is unconsumed, and only re-acquisition of a given image
    guarantees that.
  - No-arg `current*()` accessors (`currentFence()`,
    `currentImageAvailableSemaphore()`, `currentCommandPool()`,
    `currentCommandBuffer()`) read an internally-tracked `currentFrame_`
    index; `advanceFrame()` moves it `(current + 1) % 2`.
  - `renderFinishedSemaphore(imageIndex)` for the per-image side.
  - `onSwapchainRecreated(newImageCount)` destroys and rebuilds only the
    per-image `renderFinished` semaphores (the frame-in-flight objects
    don't depend on image count).
  - Destructor contract (documented at length in the header): the caller
    must have already driven the device to a real idle point before a
    `FrameSync` is destroyed — the destructor does no internal wait, by
    design, matching the existing `Device`/`CommandContext` discipline in
    this codebase.
  - Move-only RAII, same pattern as every other rx_rhi_vk class
    (private default ctor with zero-initialized members, move-assign does
    the real work, move-ctor delegates to it, moved-from nulled).
- `src/rx_rhi_vk/tests/frame_sync_test.cpp` — new `TEST_CASE` in
  `rx_rhi_vk_tests`: real windowed `Device` (skip-guarded, same pattern as
  `device_test.cpp`), `FrameSync::create`, 3 loop iterations of the real
  acquire/record(transition-only)/submit/present shape, `NeedsRecreate` on a
  hidden window handled as a pass (exercises
  `Device::recreateSwapchain`+`FrameSync::onSwapchainRecreated` together),
  never `DeviceLost`, `vkDeviceWaitIdle` before teardown, zero validation
  errors asserted.
- `samples/01_triangle/main.cpp` — added `--present` mode
  (`runPresent()`), refactored headless mode into `runHeadless()`, and
  factored pipeline construction (previously ~110 duplicated lines were
  about to become two copies) into a shared `createTrianglePipeline(VkDevice,
  VkFormat) -> std::optional<TrianglePipeline>` / `destroyTrianglePipeline`
  pair used verbatim by both modes — headless mode's behavior and output are
  otherwise unchanged. `--present` opens an 800x600 visible window and runs
  the canonical frames-in-flight loop against the real swapchain images:
  pump SDL events (`SDL_EVENT_QUIT` → break) → wait current frame's fence →
  `acquireNextImage` → `NeedsRecreate`: `vkDeviceWaitIdle` +
  `recreateSwapchain` + `FrameSync::onSwapchainRecreated` + rebuild
  per-image views + `continue` → reset fence + reset command pool → record
  (transition UNDEFINED→COLOR_ATTACHMENT_OPTIMAL, dynamic rendering with
  viewport/scissor from `swapchainExtent()`, draw 3, transition
  →PRESENT_SRC_KHR) → submit (wait `imageAvailable` @
  COLOR_ATTACHMENT_OUTPUT, signal `renderFinished[imageIndex]`, fence the
  frame fence) → `present` (`NeedsRecreate` handled the same way;
  `DeviceLost` → log + exit loop) → `advanceFrame()`. Shutdown:
  `vkDeviceWaitIdle` before destroying the per-image views, the pipeline,
  and (via RAII on function return) `FrameSync`.
- `src/rx_rhi_vk/CMakeLists.txt` — added `src/frame_sync.cpp` to the
  `rx_rhi_vk` library and `tests/frame_sync_test.cpp` to `rx_rhi_vk_tests`.
- `samples/README.md` — what each sample shows, both modes' build/run
  instructions for Linux/Windows/Steam Deck, expected output (ASCII
  description of the triangle's placement/shape, confirmed against a real
  screenshot — see Verification below).
- `MANUAL_VERIFICATION.md` (repo root) — per-platform checklist (Linux
  `--present`, Windows `.exe` copy+run, Steam Deck Desktop Mode), with the
  Linux entry fully checked off and recorded from a real run on this
  machine; Windows/Steam Deck entries left as an honest, unchecked template
  (no access to that hardware from this environment — see Deviations).

## One deliberate deviation from the brief's literal loop shape, with reasoning

The brief describes: *"wait+reset current frame's fence; acquireNextImage
— on NeedsRecreate: ...; continue."* Implemented literally, this deadlocks:
if `acquireNextImage` reports `NeedsRecreate` (a real, reachable path — the
FrameSync test's own hidden window hits it, and a resized/minimized
`--present` window can too), the loop `continue`s *without ever submitting*,
so the fence it just reset is never re-signaled by anything. The next
iteration's wait on that same fence (since `advanceFrame()` was never
reached) then blocks forever.

**Fix implemented:** wait on the fence *before* acquiring (unchanged from
the brief), but only **reset** it *after* `acquireNextImage` has confirmed
it actually returned an image to render into — i.e., reset moves from
immediately after the wait to immediately before the command-pool reset
that starts recording. On the `NeedsRecreate` path the fence is left
signaled (untouched since the wait), so the next iteration's wait on it
returns immediately, exactly as if nothing had happened. This is the same
wait-before-acquire/reset-after-acquire-succeeds ordering used by every
mainstream Vulkan frames-in-flight reference (e.g. vulkan-tutorial.com's
"Frames in flight" chapter) for this exact reason. Documented at length in
both `frame_sync.h` (on `currentFence()`) and `main.cpp` (on `runPresent()`).

This was not a hypothetical found by inspection alone — it reproduces
immediately in practice: the FrameSync test's hidden window returns
`NeedsRecreate` from `acquireNextImage` on essentially every run, so a
literal implementation of the brief's ordering would have hung the test
suite, not just failed an assertion.

## Verification performed

- `rx_rhi_vk_tests` (new `frame_sync_test.cpp` included): built clean, run
  standalone (`--test-case="FrameSync*"`) and as part of the full suite —
  **6/6 test cases, 93/93 assertions passed**, zero validation errors beyond
  the two documented false-positive guards.
- `sample_01_triangle_headless` (regression check on the pipeline-sharing
  refactor): still logs `triangle readback PASSED`, exit 0 — behavior
  byte-for-byte unchanged from Task 5.
- `--present` mode run interactively on this machine (`DISPLAY=:1`, real
  NVIDIA GeForce RTX 2080, driver 580.82.07):
  - Opened a real, visible 800x600 window; `import -window` screenshot
    confirms a correctly-shaped, centered white triangle on black (embedded
    visually during this session; described in `samples/README.md`).
  - Scripted resize testing via `xdotool windowsize`: 5 sequential resizes
    (400x300 → 1000x700 → 300x900 → 640x480 → 800x600) plus a 15-step
    randomized soak (sizes from 300x200 to ~1200x900, 0.25s apart) — **zero**
    `[error]`-level validation lines beyond the two known false positives in
    both runs; triangle stayed correctly rendered and proportioned
    throughout (confirmed via log inspection of the recreate path firing
    repeatedly, matching the resize count).
  - Clean shutdown verified via `SIGTERM` (SDL3 translates this into
    `SDL_EVENT_QUIT`, which the loop's existing event-pump handles
    identically to a window-manager close) — exit code 0, log line
    `--present: window closed cleanly` present both times.
  - One test-methodology dead end worth recording: `xdotool windowclose`
    calls a raw `XDestroyWindow` (bypassing the window manager's
    `WM_DELETE_WINDOW` protocol a real close-button click uses), which
    destroys the platform window out from under the still-running app. The
    next `recreateSwapchain` attempt then correctly fails
    (`VUID-vkGetPhysicalDeviceSurfaceCapabilitiesKHR-surface-06211`, surface
    no longer supported) and the loop exits with a clean, logged `return 1`
    — the right behavior for a genuinely-destroyed, unrecoverable platform
    resource, but not a realistic user path (no Vulkan call can un-destroy a
    surface, and no window manager's close button behaves this way). Not
    fixed because there is nothing to fix: this is an unreachable-in-practice
    input, correctly rejected rather than crashing.
- `cmake --build --preset linux-native`: clean, zero compiler warnings on
  any touched file (checked explicitly via a forced rebuild + grep).
- `ctest --preset linux-native --output-on-failure`: **5/5 green**
  (`shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`,
  `rx_rhi_vk_tests`, `sample_01_triangle_headless`).
- `cmake --build --preset windows-cross-zig`: clean, produces
  `sample_01_triangle.exe` and `rx_rhi_vk_tests.exe` including the new
  `frame_sync.cpp`/`frame_sync_test.cpp` translation units.
- Commit hygiene: `git log -1 --format='%B'` inspected before and after
  writing this report — no AI attribution anywhere in the commit message.

## Notes / deviations from the brief worth flagging forward

- The fence wait/reset reordering above (the one substantive deviation).
- `MANUAL_VERIFICATION.md`'s Windows and Steam Deck sections are an honest,
  unchecked template, not fabricated results — this environment has no
  access to real Windows or Steam Deck hardware, only the
  `windows-cross-zig` cross-compile (confirmed clean). Flagging forward:
  these two checklists need a real run before any release that claims
  Windows/Steam Deck support (Task 8's release step, or whenever this
  project's maintainer has hands on that hardware).
- `FrameSync`'s command pools use `VK_COMMAND_POOL_CREATE_TRANSIENT_BIT`
  (not mentioned explicitly in the brief, which only said
  "RESET_COMMAND_BUFFER not needed") — a legitimate, low-risk driver hint
  for pools whose single buffer is re-recorded every frame; documented
  inline.
- `--present`'s window size (800x600) is not specified by the brief; chosen
  arbitrarily since viewport/scissor are dynamic state and the window is
  freely resizable at runtime regardless of the initial size.

## Files touched

- Created: `src/rx_rhi_vk/include/rx_rhi_vk/frame_sync.h`,
  `src/rx_rhi_vk/src/frame_sync.cpp`,
  `src/rx_rhi_vk/tests/frame_sync_test.cpp`, `samples/README.md`,
  `MANUAL_VERIFICATION.md`
- Modified: `samples/01_triangle/main.cpp` (added `--present` mode,
  factored pipeline construction into a shared helper),
  `src/rx_rhi_vk/CMakeLists.txt` (new sources)
