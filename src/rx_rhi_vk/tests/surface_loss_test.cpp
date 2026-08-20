// [Phase 4 Task 17 follow-up, Issue #73] Device-free tier for
// isSurfaceLossResult() (device.h) -- pure classifier, zero Vulkan device
// needed, mirroring eviction_contract_test.cpp's own "pure CPU logic" shape
// among this binary's test files. The real end-to-end path (a genuinely
// destroyed native window making Device::recreateSwapchain() enter the
// surface-lost terminal state via this SAME classifier) is
// MANUAL_VERIFICATION-only -- see this task's own report: no CI driver/
// display backend this repo's fixtures use can be made to genuinely
// destroy a live window out from under a running process the way a
// real desktop's window manager (or `xdotool windowclose`, the tool used to
// find and verify this) can, mirroring window_state_test.cpp's own "matrix
// row 6" precedent for the zero-extent guard's DI-seam split.
#include <doctest/doctest.h>
#include <rx_rhi_vk/device.h>

using rx::rhi::isSurfaceLossResult;

TEST_CASE("isSurfaceLossResult(): true for VK_ERROR_SURFACE_LOST_KHR -- the Vulkan spec's own documented code for "
          "a surface whose native window is gone") {
    CHECK(isSurfaceLossResult(VK_ERROR_SURFACE_LOST_KHR));
}

TEST_CASE("isSurfaceLossResult(): true for VK_ERROR_INITIALIZATION_FAILED -- NOT a spec-documented return for "
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR, but empirically what this project's own verified NVIDIA/Xcb "
          "loader stack returns for a destroyed-native-window surface [Issue #73's own reproduction]") {
    CHECK(isSurfaceLossResult(VK_ERROR_INITIALIZATION_FAILED));
}

TEST_CASE("isSurfaceLossResult(): false for genuine, unrelated errors -- these must still hard-fail "
          "Device::recreateSwapchain() rather than being silently treated as 'the window is just gone' "
          "[revert-discrimination: an implementation that classified ANY non-VK_SUCCESS result as surface-loss "
          "would incorrectly pass VK_ERROR_OUT_OF_HOST_MEMORY/VK_ERROR_OUT_OF_DEVICE_MEMORY here too]") {
    CHECK_FALSE(isSurfaceLossResult(VK_ERROR_OUT_OF_HOST_MEMORY));
    CHECK_FALSE(isSurfaceLossResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
    CHECK_FALSE(isSurfaceLossResult(VK_ERROR_OUT_OF_DATE_KHR));
    CHECK_FALSE(isSurfaceLossResult(VK_SUCCESS));
}

// [Round-review hardening] Its own dedicated TEST_CASE, deliberately
// separate from the general "genuine, unrelated errors" one above -- a
// lost DEVICE is a categorically different, always-fatal condition (the
// whole VkDevice this Device wraps is gone, not just this one surface),
// explicitly excluded FIRST in isSurfaceLossResult()'s own implementation
// (checked before either VK_ERROR_SURFACE_LOST_KHR/
// VK_ERROR_INITIALIZATION_FAILED match) so it can never be reclassified
// as "just the window closing" no matter how the rest of that function's
// logic evolves. This test exists to keep that guarantee load-bearing and
// visible on its own, not merely incidental to a broader list.
TEST_CASE("isSurfaceLossResult(): false for VK_ERROR_DEVICE_LOST, unconditionally -- a lost device is never "
          "inferred as 'the window is just gone' [Issue #73 round-review hardening]") {
    CHECK_FALSE(isSurfaceLossResult(VK_ERROR_DEVICE_LOST));
}
