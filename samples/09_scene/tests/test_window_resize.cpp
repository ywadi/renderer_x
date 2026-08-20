// samples/09_scene/tests/test_window_resize.cpp -- Issue #36 (live window
// resizing + a runtime fullscreen toggle in --present mode): device-free
// coverage for ../window_resize.h's two pure decisions, matching
// test_mouse_capture.cpp's own established shape for this sample (truth
// tables + revert-discrimination cases, no rx::platform::Window/SDL/
// VkDevice anywhere in this file).
#include "../window_resize.h"

#include <doctest/doctest.h>

using rx::samples9::f11TogglesFullscreen;
using rx::samples9::pixelSizeRequiresRecreate;

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
