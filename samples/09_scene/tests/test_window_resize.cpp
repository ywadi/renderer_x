// samples/09_scene/tests/test_window_resize.cpp -- Issue #36 (live window
// resizing + a runtime fullscreen toggle in --present mode) and its Issue
// #73 round-review hardening follow-up: device-free coverage for
// ../window_resize.h's pure decisions, matching test_mouse_capture.cpp's
// own established shape for this sample (truth tables + revert-
// discrimination cases, no rx::platform::Window/SDL/VkDevice anywhere in
// this file).
#include "../window_resize.h"

#include <doctest/doctest.h>

using rx::samples9::f11TogglesFullscreen;
using rx::samples9::graphNeedsRecompileForExtent;
using rx::samples9::pixelSizeRequiresRecreate;
using rx::samples9::shouldSkipTeardownAfterDeviceLoss;

// --- f11TogglesFullscreen() -----------------------------------------------
TEST_CASE("f11TogglesFullscreen(): true (F11 should toggle) when ImGui does NOT claim the keyboard") {
    CHECK(f11TogglesFullscreen(/*imguiWantsKeyboard=*/false));
}

TEST_CASE("f11TogglesFullscreen(): false (F11 must NOT toggle) when ImGui DOES claim the keyboard -- mirrors "
          "escTogglesCapture()'s own Esc gate (mouse_capture.h) so a future keyboard-focused HUD widget can claim "
          "F11 too") {
    CHECK_FALSE(f11TogglesFullscreen(/*imguiWantsKeyboard=*/true));
}

TEST_CASE("f11TogglesFullscreen(): full 2-row truth table [revert-discrimination -- an implementation that "
          "returned imguiWantsKeyboard verbatim instead of negating it fails both rows here]") {
    CHECK(f11TogglesFullscreen(false) == true);
    CHECK(f11TogglesFullscreen(true) == false);
}

// --- pixelSizeRequiresRecreate() ------------------------------------------
TEST_CASE("pixelSizeRequiresRecreate(): false before any real SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED event -- the "
          "{0,0} pre-first-event sentinel (Window::lastPixelSizeEvent()'s own documented starting value) must "
          "never itself read as 'the window resized to zero'") {
    CHECK_FALSE(pixelSizeRequiresRecreate(VkExtent2D{1280, 720}, VkExtent2D{0, 0}));
}

TEST_CASE("pixelSizeRequiresRecreate(): false when observed has a zero dimension even if lastHandled differs -- "
          "this optimization signal must never itself decide suspended/resume (window.h's own documented "
          "invariant); a genuine minimize is caught by Device::recreateSwapchain()'s own live-queried guard "
          "instead, via the pre-existing NeedsRecreate path") {
    CHECK_FALSE(pixelSizeRequiresRecreate(VkExtent2D{1280, 720}, VkExtent2D{0, 480}));
    CHECK_FALSE(pixelSizeRequiresRecreate(VkExtent2D{1280, 720}, VkExtent2D{640, 0}));
}

TEST_CASE("pixelSizeRequiresRecreate(): false when observed exactly matches lastHandled -- idempotent, no "
          "spurious recreation once a resize has already been applied") {
    CHECK_FALSE(pixelSizeRequiresRecreate(VkExtent2D{1280, 720}, VkExtent2D{1280, 720}));
}

TEST_CASE("pixelSizeRequiresRecreate(): true when either dimension genuinely differs and both are nonzero -- "
          "growing, shrinking, and single-axis-only changes all count") {
    CHECK(pixelSizeRequiresRecreate(VkExtent2D{1280, 720}, VkExtent2D{1920, 1080}));  // grow both.
    CHECK(pixelSizeRequiresRecreate(VkExtent2D{1280, 720}, VkExtent2D{640, 480}));    // shrink both.
    CHECK(pixelSizeRequiresRecreate(VkExtent2D{1280, 720}, VkExtent2D{1280, 800}));   // height only.
    CHECK(pixelSizeRequiresRecreate(VkExtent2D{1280, 720}, VkExtent2D{1024, 720}));   // width only.
}

TEST_CASE("pixelSizeRequiresRecreate(): revert-discrimination -- an implementation that compared ONLY width (or "
          "ONLY height) instead of both would pass the single-axis cases above by accident but fail to notice a "
          "same-width-different-height OR same-height-different-width change is still exercised distinctly here") {
    CHECK(pixelSizeRequiresRecreate(VkExtent2D{800, 600}, VkExtent2D{800, 601}));
    CHECK(pixelSizeRequiresRecreate(VkExtent2D{800, 600}, VkExtent2D{801, 600}));
    CHECK_FALSE(pixelSizeRequiresRecreate(VkExtent2D{800, 600}, VkExtent2D{800, 600}));
}

// --- graphNeedsRecompileForExtent() [Issue #73 round-review hardening] ----
TEST_CASE("graphNeedsRecompileForExtent(): false when the extent is unchanged -- the exact scenario a "
          "present-mode-only recreation (the HUD vsync toggle) hits: Device::recreateSwapchain() rebuilds the "
          "whole VkSwapchainKHR, but the real pixel extent it reports back is identical, so recompiling the "
          "render graph would be pure redundant work") {
    CHECK_FALSE(graphNeedsRecompileForExtent(VkExtent2D{1280, 720}, VkExtent2D{1280, 720}));
}

TEST_CASE("graphNeedsRecompileForExtent(): true when either dimension genuinely differs -- a real resize must "
          "still recompile [revert-discrimination: an implementation that always returned false here would "
          "silently reintroduce the exact stale-transient-extent bug this task's own #36 work fixed]") {
    CHECK(graphNeedsRecompileForExtent(VkExtent2D{1280, 720}, VkExtent2D{1920, 1080}));  // grow both.
    CHECK(graphNeedsRecompileForExtent(VkExtent2D{1280, 720}, VkExtent2D{1280, 800}));    // height only.
    CHECK(graphNeedsRecompileForExtent(VkExtent2D{1280, 720}, VkExtent2D{1024, 720}));    // width only.
}

// --- shouldSkipTeardownAfterDeviceLoss() [Issue #74] -----------------------
TEST_CASE("shouldSkipTeardownAfterDeviceLoss(): true ONLY for the exact reproduced compound condition -- "
          "VK_ERROR_DEVICE_LOST from vkDeviceWaitIdle() AND the surface already known lost") {
    CHECK(shouldSkipTeardownAfterDeviceLoss(VK_ERROR_DEVICE_LOST, /*surfaceLost=*/true));
}

TEST_CASE("shouldSkipTeardownAfterDeviceLoss(): false when the device is fine (VK_SUCCESS), regardless of "
          "surface-lost state -- the overwhelmingly common case: normal teardown proceeds exactly as before") {
    CHECK_FALSE(shouldSkipTeardownAfterDeviceLoss(VK_SUCCESS, /*surfaceLost=*/true));
    CHECK_FALSE(shouldSkipTeardownAfterDeviceLoss(VK_SUCCESS, /*surfaceLost=*/false));
}

TEST_CASE("shouldSkipTeardownAfterDeviceLoss(): false when the device is lost but the surface was NEVER known "
          "lost -- revert-discrimination against silently masking an unrelated, genuine device-loss bug (e.g. a "
          "mid-frame GPU crash while the window is still alive) as a clean exit; that case must keep failing "
          "hard via the pre-existing SwapchainStatus::DeviceLost/`ok = false` path, not this narrower fix") {
    CHECK_FALSE(shouldSkipTeardownAfterDeviceLoss(VK_ERROR_DEVICE_LOST, /*surfaceLost=*/false));
}

TEST_CASE("shouldSkipTeardownAfterDeviceLoss(): false for any other non-success VkResult even with the surface "
          "already known lost -- narrowly scoped to VK_ERROR_DEVICE_LOST specifically, not 'any wait failure'") {
    CHECK_FALSE(shouldSkipTeardownAfterDeviceLoss(VK_ERROR_OUT_OF_HOST_MEMORY, /*surfaceLost=*/true));
    CHECK_FALSE(shouldSkipTeardownAfterDeviceLoss(VK_ERROR_OUT_OF_DEVICE_MEMORY, /*surfaceLost=*/true));
    CHECK_FALSE(shouldSkipTeardownAfterDeviceLoss(VK_ERROR_SURFACE_LOST_KHR, /*surfaceLost=*/true));
}
