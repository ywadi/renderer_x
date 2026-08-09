#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>

// This test case is pinned to a test suite that sorts after device_test.cpp's
// (unnamed, i.e. "") suite under --order-by=suite (set for this whole binary
// in CMakeLists.txt) so that it always runs *after* device_test.cpp's
// windowed Device::create() tests within this one process. That ordering is
// not cosmetic: it works around a confirmed defect in the pinned
// vk-bootstrap commit (556b79b165386f6c1a18362d30f2a076fdaa2778).
// vk-bootstrap resolves instance-level function pointers (detail::
// vulkan_functions() in VkBootstrap.cpp) into a process-wide singleton that
// is populated from the *first* vkb::Instance ever built in the process and
// is then permanently reused -- init_instance_funcs() no-ops on every later
// call, and nothing in vk-bootstrap's public API resets it. This test
// builds Context::create({}, true), i.e. headless=true, so vk-bootstrap
// never enables VK_KHR_surface on it; if this ran first, every later
// PhysicalDeviceSelector::select() in the process (used by
// Device::create()) would inherit a null fp_vkGetPhysicalDeviceSurfaceSupportKHR
// cached from this instance and crash the moment it needs to check present
// support (confirmed via gdb: SIGSEGV in vkb::detail::get_present_queue_index,
// VkBootstrap.cpp:1103, called from PhysicalDeviceSelector::is_device_suitable).
// Running the windowed Device tests first instead means the process-wide
// cache is warmed up with real, valid surface function pointers before this
// headless test ever runs, and this test's own behavior/assertions are
// otherwise unchanged from Task 6.
TEST_CASE("Context::create succeeds with no required extensions and reports no validation errors" *
          doctest::test_suite("zz_run_after_windowed_device_tests")) {
    auto ctx = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->instance() != VK_NULL_HANDLE);
    CHECK_FALSE(ctx->hasValidationErrors());
}
