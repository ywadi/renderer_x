#include <doctest/doctest.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_platform/window.h>
#include <cstdint>
#include <optional>
#include <utility>

namespace {

// Same skip-guarded windowed-device fixture pattern as device_test.cpp
// (kept local rather than shared -- these are separate translation units and
// device_test.cpp's fixture struct lives in its own anonymous namespace).
struct FrameSyncTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    VkSurfaceKHR surface;
};

std::optional<FrameSyncTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping FrameSync test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping FrameSync test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    return FrameSyncTestFixture{std::move(*window), std::move(*context), surface};
}

}  // namespace

// Exercises FrameSync against a real windowed Device across 3 loop
// iterations of the exact acquire/record/submit/present shape
// samples/01_triangle's --present mode uses (see that file's runPresent()),
// minus the actual triangle draw -- this test's job is FrameSync's
// bookkeeping (fence/semaphore lifetime, per-image vs per-frame-in-flight
// indexing, onSwapchainRecreated()), not pixel output, which is already
// covered by the headless correctness gate (sample_01_triangle_headless).
//
// On a hidden window, Device::acquireNextImage()/Device::present() are free
// to report NeedsRecreate (hidden-window presentation is implementation-
// defined, same rationale as device_test.cpp's round-trip test) -- taking
// that path and exercising FrameSync::onSwapchainRecreated() is itself a
// pass, not a failure, per the brief. What must never happen is
// SwapchainStatus::DeviceLost or a validation error.
TEST_CASE("FrameSync runs a real frames-in-flight acquire/submit/present loop") {
    auto fixture = makeFixture("rx_rhi_vk_frame_sync_test");
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());

    const VkDevice vkDevice = device->device();
    VkSurfaceKHR surface = fixture->surface;

    auto frameSync =
        rx::rhi::FrameSync::create(vkDevice, device->graphicsQueueFamily(),
                                    static_cast<uint32_t>(device->swapchainImages().size()));
    REQUIRE(frameSync.has_value());
    CHECK(rx::rhi::FrameSync::framesInFlight() == 2);

    for (int iteration = 0; iteration < 3; ++iteration) {
        fixture->window.pumpEvents();

        // Wait BEFORE acquiring, but do not reset yet -- resetting
        // unconditionally here (matching the frame_sync.h doc comment) would
        // deadlock the next iteration's wait whenever acquire itself reports
        // NeedsRecreate below, since nothing would ever re-signal this slot's
        // fence without an intervening submit.
        VkFence fence = frameSync->currentFence();
        REQUIRE(vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);

        auto acquire = device->acquireNextImage(frameSync->currentImageAvailableSemaphore());
        if (acquire.status == rx::rhi::SwapchainStatus::NeedsRecreate) {
            REQUIRE(vkDeviceWaitIdle(vkDevice) == VK_SUCCESS);
            REQUIRE(device->recreateSwapchain(surface));
            REQUIRE(frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())));
            continue;
        }
        REQUIRE(acquire.status != rx::rhi::SwapchainStatus::DeviceLost);

        REQUIRE(vkResetFences(vkDevice, 1, &fence) == VK_SUCCESS);

        VkCommandPool pool = frameSync->currentCommandPool();
        REQUIRE(vkResetCommandPool(vkDevice, pool, 0) == VK_SUCCESS);
        VkCommandBuffer cmd = frameSync->currentCommandBuffer();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        REQUIRE(vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS);

        // No rendering needed for this test -- just the mandatory
        // UNDEFINED -> PRESENT_SRC_KHR transition every acquired image needs
        // before it can legally be presented (mirrors device_test.cpp's
        // round-trip test, now driven through FrameSync's owned objects
        // instead of ad hoc locals).
        rx::rhi::transitionImage(cmd, device->swapchainImages()[acquire.imageIndex], VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        REQUIRE(vkEndCommandBuffer(cmd) == VK_SUCCESS);

        VkSemaphore waitSem = frameSync->currentImageAvailableSemaphore();
        VkSemaphore signalSem = frameSync->renderFinishedSemaphore(acquire.imageIndex);
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSem;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSem;

        REQUIRE(vkQueueSubmit(device->graphicsQueue(), 1, &submitInfo, fence) == VK_SUCCESS);

        auto presentStatus = device->present(acquire.imageIndex, signalSem);
        CHECK(presentStatus != rx::rhi::SwapchainStatus::DeviceLost);
        if (presentStatus == rx::rhi::SwapchainStatus::NeedsRecreate) {
            REQUIRE(vkDeviceWaitIdle(vkDevice) == VK_SUCCESS);
            REQUIRE(device->recreateSwapchain(surface));
            REQUIRE(frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())));
        }

        frameSync->advanceFrame();
    }

    // Destructor contract (frame_sync.h): FrameSync must not be destroyed
    // until the device is idle. `frameSync` and `device` both unwind at the
    // end of this scope in reverse declaration order (frameSync first, then
    // device) -- this explicit wait is what makes that safe, exactly as the
    // production present loop's shutdown sequence does.
    REQUIRE(vkDeviceWaitIdle(vkDevice) == VK_SUCCESS);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
