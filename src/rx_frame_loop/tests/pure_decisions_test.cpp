// pure_decisions_test.cpp -- [Phase 5 Task 5, ticket #41 rows 7/8] Device-
// free tests for rx_frame_loop's pure decision functions, written FIRST
// per this phase's TDD constraint (mirroring this codebase's own
// established pure-function/state-machine test pattern, e.g.
// rx_rhi_vk/tests/surface_loss_test.cpp's isSurfaceLossResult() tests).
// These are the promoted, byte-for-byte-preserved logic of
// samples/09_scene/window_resize.h's pixelSizeRequiresRecreate()/
// f11TogglesFullscreen()/shouldSkipTeardownAfterDeviceLoss() -- every
// TEST_CASE below was ALREADY green against that sample-local file before
// promotion; re-run here against the new engine-owned home to prove the
// move was behavior-preserving.
#include <doctest/doctest.h>
#include <rx_frame_loop/present_loop.h>

using namespace rx::frame_loop;

// --- pixelSizeRequiresRecreate() ------------------------------------------

TEST_CASE("pixelSizeRequiresRecreate(): false when observed matches lastHandled exactly (no resize)") {
    CHECK_FALSE(pixelSizeRequiresRecreate(VkExtent2D{800, 600}, VkExtent2D{800, 600}));
}

TEST_CASE("pixelSizeRequiresRecreate(): true when either dimension differs") {
    CHECK(pixelSizeRequiresRecreate(VkExtent2D{800, 600}, VkExtent2D{801, 600}));
    CHECK(pixelSizeRequiresRecreate(VkExtent2D{800, 600}, VkExtent2D{800, 601}));
    CHECK(pixelSizeRequiresRecreate(VkExtent2D{800, 600}, VkExtent2D{1920, 1080}));
}

TEST_CASE("pixelSizeRequiresRecreate(): false whenever `observed` has a zero dimension -- the pre-first-event "
          "sentinel AND a genuine minimize both stay owned by the acquire/present-driven NeedsRecreate path, "
          "never proactively triggered from here [revert-discrimination: an implementation that dropped this "
          "guard would fire a spurious recreation on frame 1 of every run, before lastPixelSizeEvent() ever "
          "observes a real event]") {
    CHECK_FALSE(pixelSizeRequiresRecreate(VkExtent2D{800, 600}, VkExtent2D{0, 0}));
    CHECK_FALSE(pixelSizeRequiresRecreate(VkExtent2D{800, 600}, VkExtent2D{0, 600}));
    CHECK_FALSE(pixelSizeRequiresRecreate(VkExtent2D{800, 600}, VkExtent2D{800, 0}));
    // Even when lastHandled ITSELF happens to be {0,0} (degenerate/startup
    // case) -- the zero-guard is on `observed` alone, unconditionally.
    CHECK_FALSE(pixelSizeRequiresRecreate(VkExtent2D{0, 0}, VkExtent2D{0, 0}));
}

// --- f11TogglesFullscreen() ------------------------------------------------

TEST_CASE("f11TogglesFullscreen(): true when the UI does NOT claim the keyboard") {
    CHECK(f11TogglesFullscreen(/*imguiWantsKeyboard=*/false));
}

TEST_CASE("f11TogglesFullscreen(): false when the UI DOES claim the keyboard [a future keyboard-focused HUD "
          "widget must be able to claim F11 for itself without also toggling fullscreen underneath it]") {
    CHECK_FALSE(f11TogglesFullscreen(/*imguiWantsKeyboard=*/true));
}

// --- shouldSkipTeardownAfterDeviceLoss() [Issue #74] -----------------------

TEST_CASE("shouldSkipTeardownAfterDeviceLoss(): true ONLY for the exact compound condition -- VK_ERROR_DEVICE_LOST "
          "AND surfaceLost -- reproduced directly on real NVIDIA/Xcb hardware [revert-discrimination: an "
          "implementation that checked either condition alone would either (a) skip teardown after an unrelated "
          "device loss while the surface is still alive -- masking a real, more serious bug -- or (b) never skip "
          "teardown for the one compound case this exists to handle, reintroducing the unfiltered validation "
          "errors Issue #74 fixed]") {
    CHECK(shouldSkipTeardownAfterDeviceLoss(VK_ERROR_DEVICE_LOST, /*surfaceLost=*/true));
}

TEST_CASE("shouldSkipTeardownAfterDeviceLoss(): false when the device is lost but the surface is still alive -- "
          "stays a hard, caller-visible failure, never silently masked") {
    CHECK_FALSE(shouldSkipTeardownAfterDeviceLoss(VK_ERROR_DEVICE_LOST, /*surfaceLost=*/false));
}

TEST_CASE("shouldSkipTeardownAfterDeviceLoss(): false when the surface is lost but vkDeviceWaitIdle() itself "
          "succeeded (VK_SUCCESS) -- the ordinary, already-handled Issue #73 clean-stop path, not this narrower "
          "one-layer-deeper fix") {
    CHECK_FALSE(shouldSkipTeardownAfterDeviceLoss(VK_SUCCESS, /*surfaceLost=*/true));
}

TEST_CASE("shouldSkipTeardownAfterDeviceLoss(): false for an unrelated VkResult entirely, regardless of "
          "surfaceLost") {
    CHECK_FALSE(shouldSkipTeardownAfterDeviceLoss(VK_ERROR_OUT_OF_DEVICE_MEMORY, /*surfaceLost=*/true));
    CHECK_FALSE(shouldSkipTeardownAfterDeviceLoss(VK_ERROR_OUT_OF_DEVICE_MEMORY, /*surfaceLost=*/false));
}
