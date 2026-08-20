#pragma once
// samples/09_scene/window_resize.h -- pure decision logic for Issue #36
// (live window resizing + a runtime fullscreen toggle in --present mode),
// pulled out of main.cpp exactly like fly_camera.h/mouse_capture.h's own
// established "device-free pure logic gets its own header, so it is
// unit-testable without an SDL window or a VkDevice" precedent in this
// sample. Two independent decisions live here:
//
//   - f11TogglesFullscreen(): the same "compose with ImGui's own
//     WantCaptureKeyboard at the call site" shape mouse_capture.h's
//     escTogglesCapture() already establishes for Esc -- deliberately its
//     own named function rather than reusing escTogglesCapture() or
//     fly_camera.h's keyboardDrivesCamera() (both are `!imguiWantsKeyboard`
//     today too), for the identical reason mouse_capture.h's own comment
//     gives: these gate different call sites free to diverge later.
//
//   - pixelSizeRequiresRecreate(): the trigger for main.cpp's runPresent()
//     loop to proactively call the SAME Device::recreateSwapchain() path
//     NeedsRecreate/fullscreen already drive, from a live
//     SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED observation
//     (rx::platform::Window::lastPixelSizeEvent()) rather than waiting for
//     vkAcquireNextImageKHR/vkQueuePresentKHR to eventually report
//     VK_ERROR_OUT_OF_DATE_KHR/VK_SUBOPTIMAL_KHR -- on some platforms/
//     drivers that signal can lag a live drag-resize by several frames,
//     during which the OLD-sized swapchain image would be presented
//     stretched into the NEW-sized window.
//
//   - graphNeedsRecompileForExtent() [Issue #73 round-review hardening]:
//     whether recreateSwapchainAndDependents() needs to re-run
//     RenderGraph::compile()+Executor::realize() at all after a
//     successful Device::recreateSwapchain() -- skipped when the extent
//     didn't actually change (e.g. a present-mode-only vsync toggle).
#include <vulkan/vulkan.h>

namespace rx::samples9 {

// [Issue #36] main.cpp's pumpEvents() `preDispatch` seam calls this on
// every real SDL_EVENT_KEY_DOWN whose `scancode == SDL_SCANCODE_F11` AND
// whose `repeat` field is false (edge-triggered, exactly like Esc/
// escTogglesCapture()'s own contract) -- only when this returns true does
// the caller flip the runtime fullscreen toggle, so a future keyboard-
// focused HUD widget can still claim F11 for itself without also toggling
// fullscreen underneath it.
[[nodiscard]] inline bool f11TogglesFullscreen(bool imguiWantsKeyboard) { return !imguiWantsKeyboard; }

// [Issue #36] `lastHandled` is the extent this run's own swapchain was
// last (re)built against (tracked by runPresent(), updated after every
// successful recreation); `observed` is
// rx::platform::Window::lastPixelSizeEvent()'s current value, drained by
// pumpEvents() this same frame. Returns true only when a recreation should
// be proactively triggered THIS frame.
//
// `observed` with either dimension == 0 NEVER triggers a recreation here --
// two distinct reasons converge on the same guard:
//   1. lastPixelSizeEvent() itself starts at {0, 0} before the first real
//      SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED event this window ever observes
//      (window.h's own documented contract) -- treating that pre-first-
//      event sentinel as "the window just resized to zero" would fire a
//      spurious recreation on frame 1 of every --present run, before any
//      resize ever happened.
//   2. window.h's own header comment is explicit that
//      lastPixelSizeEvent()/minimizedEventObserved() are "OPTIMIZATION/
//      LOGGING SIGNAL ONLY" -- the AUTHORITATIVE suspended-present decision
//      is rx::rhi::Device::recreateSwapchain()'s own extent-query-driven
//      guard, never an SDL event. A genuine minimize is already caught by
//      this loop's pre-existing NeedsRecreate handling (the driver reports
//      VK_ERROR_OUT_OF_DATE_KHR once the real surface shrinks), which then
//      calls that same authoritative, live-queried guard -- this function
//      only decides whether to PROACTIVELY get ahead of a live drag-resize
//      the driver hasn't signaled yet, never whether to suspend.
[[nodiscard]] inline bool pixelSizeRequiresRecreate(VkExtent2D lastHandled, VkExtent2D observed) {
    if (observed.width == 0 || observed.height == 0) {
        return false;
    }
    return observed.width != lastHandled.width || observed.height != lastHandled.height;
}

// [Issue #73 round-review hardening] Pure decision, used by runPresent()'s
// recreateSwapchainAndDependents() (main.cpp): does RenderGraph::compile()
// (and the Executor::realize() that must follow any compile() that
// actually changed a resource shape) need to run again for `newExtent`,
// given the graph was last compiled against `lastCompiledExtent`? Every
// Device::recreateSwapchain() success rebuilds the whole VkSwapchainKHR
// (a NEW set of VkImages), but a present-mode-only change (the HUD vsync
// checkbox: same width/height, different VkPresentModeKHR) never changes
// any SwapchainRelative resource's real pixel extent -- recompiling/
// re-realizing the render graph's "hdr"/"depth" transients for THAT case
// is pure redundant GPU-idle-time work (this project's own "performance
// is an exit criterion" posture, CLAUDE.md), not a correctness
// requirement the way it is for a genuine resize.
//
// Deliberately its OWN function rather than a call to
// pixelSizeRequiresRecreate() above under a confusing name at this new
// call site, even though the underlying comparison is the same shape:
// that function's whole contract is built around
// Window::lastPixelSizeEvent()'s own {0, 0} pre-first-event sentinel
// (see its own comment) -- a concern that does NOT apply here.
// recreateSwapchainAndDependents() only ever reaches this call AFTER
// Device::recreateSwapchain() has already succeeded AND neither
// isSuspended() nor isSurfaceLost() is true, so `newExtent` (read from
// Device::swapchainExtent() at that point) is already known-nonzero --
// a narrower, simpler contract that does not need (and should not
// silently inherit) that other function's zero-guard.
[[nodiscard]] inline bool graphNeedsRecompileForExtent(VkExtent2D lastCompiledExtent, VkExtent2D newExtent) {
    return newExtent.width != lastCompiledExtent.width || newExtent.height != lastCompiledExtent.height;
}

}  // namespace rx::samples9
