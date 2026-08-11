#include <doctest/doctest.h>
#include <rx_rhi_vk/device.h>
#include <rx_platform/window.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace {

// Shared setup for both test cases below: a hidden window, a validated
// Context built from that window's required instance extensions, and the
// VkSurfaceKHR to hand to Device::create(). Returns an empty optional (with
// a MESSAGE explaining why) if this machine's video/Vulkan backend can't
// support it -- mirroring rx_platform's own window_test.cpp skip-guard
// pattern -- but on a machine with a real display and driver this must
// actually produce a value, never silently skip.
struct DeviceTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    VkSurfaceKHR surface;
};

std::optional<DeviceTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping Device test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping Device test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    return DeviceTestFixture{std::move(*window), std::move(*context), surface};
}

}  // namespace

TEST_CASE("Device::create builds device, queues, and swapchain against a real surface") {
    auto fixture = makeFixture("rx_rhi_vk_device_test_create");
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());

    CHECK(device->physicalDevice() != VK_NULL_HANDLE);
    CHECK(device->device() != VK_NULL_HANDLE);
    CHECK(device->graphicsQueue() != VK_NULL_HANDLE);
    CHECK(device->presentQueue() != VK_NULL_HANDLE);
    CHECK(device->swapchain() != VK_NULL_HANDLE);
    CHECK(device->swapchainImages().size() > 0);
    CHECK(device->swapchainFormat() != VK_FORMAT_UNDEFINED);
    CHECK(device->swapchainExtent().width > 0);
    CHECK(device->swapchainExtent().height > 0);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Device acquire/present round-trip succeeds without device loss") {
    auto fixture = makeFixture("rx_rhi_vk_device_test_roundtrip");
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());

    VkDevice vkDevice = device->device();

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    REQUIRE(vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &acquireSemaphore) == VK_SUCCESS);
    REQUIRE(vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &renderFinishedSemaphore) == VK_SUCCESS);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence submitFence = VK_NULL_HANDLE;
    REQUIRE(vkCreateFence(vkDevice, &fenceInfo, nullptr, &submitFence) == VK_SUCCESS);

    // Throwaway command pool/buffer, local to this test: the shared
    // CommandContext arrives in a later task, and this test is meant to
    // stay self-contained until then.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = device->graphicsQueueFamily();
    VkCommandPool commandPool = VK_NULL_HANDLE;
    REQUIRE(vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &commandPool) == VK_SUCCESS);

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(vkDevice, &cmdAllocInfo, &cmd) == VK_SUCCESS);

    auto acquire = device->acquireNextImage(acquireSemaphore);
    REQUIRE(acquire.status == rx::rhi::SwapchainStatus::Ok);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    REQUIRE(vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS);

    // No rendering happens in this test -- just the mandatory
    // UNDEFINED -> PRESENT_SRC_KHR layout transition every acquired image
    // needs before it can be presented.
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = device->swapchainImages()[acquire.imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    // srcStage must be COLOR_ATTACHMENT_OUTPUT -- the same stage the submit
    // below waits the acquire semaphore at. A semaphore wait only orders the
    // waited stages (and logically-later ones) after the presentation
    // engine's acquire read; a TOP_OF_PIPE-sourced layout transition is NOT
    // ordered by that wait and races the acquire (flagged by
    // synchronization validation's present-engine tracking, layers >= ~1.3.240).
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    REQUIRE(vkEndCommandBuffer(cmd) == VK_SUCCESS);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &acquireSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinishedSemaphore;

    REQUIRE(vkQueueSubmit(device->graphicsQueue(), 1, &submitInfo, submitFence) == VK_SUCCESS);
    REQUIRE(vkWaitForFences(vkDevice, 1, &submitFence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);

    auto presentStatus = device->present(acquire.imageIndex, renderFinishedSemaphore);
    CHECK(presentStatus != rx::rhi::SwapchainStatus::DeviceLost);

    // Never destroy a semaphore the presentation engine might still be
    // consuming without an idle/fence guarantee first: the fence wait above
    // only covers the submit, not the present, so wait for the device to go
    // fully idle before tearing down.
    vkDeviceWaitIdle(vkDevice);

    vkDestroyCommandPool(vkDevice, commandPool, nullptr);
    vkDestroyFence(vkDevice, submitFence, nullptr);
    vkDestroySemaphore(vkDevice, renderFinishedSemaphore, nullptr);
    vkDestroySemaphore(vkDevice, acquireSemaphore, nullptr);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// Present-mode control [Phase 4 Task 6, spec seed 1]. Same fixture and same
// acquire/record/submit/present shape as the round-trip test directly
// above -- including its fixed COLOR_ATTACHMENT_OUTPUT barrier srcStage,
// which matters here for exactly the reason that test's own comment gives:
// a TOP_OF_PIPE-sourced layout transition is not ordered by the acquire
// semaphore's wait and would race the acquire under synchronization
// validation. This test drives that same shape against a swapchain that
// has gone through Device::setPresentMode()/recreateSwapchain() rather
// than the one straight out of Device::create(), which is the one thing
// it adds.
TEST_CASE("Device present-mode ladder: VsyncOn is FIFO; VsyncOff recreates cleanly to an available mode") {
    auto fixture = makeFixture("rx_rhi_vk_device_test_present_mode");
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());

    // Creation-time default [Task 6]: explicit FIFO under
    // PresentMode::VsyncOn -- replacing vk-bootstrap's previous implicit
    // MAILBOX-if-available preference. See device.cpp's Device::create()
    // comment at its swapchain-builder call site.
    CHECK(device->presentMode() == VK_PRESENT_MODE_FIFO_KHR);

    // Independently compute what the ladder SHOULD pick for VsyncOff on
    // this exact surface, by querying vkGetPhysicalDeviceSurfacePresentModesKHR
    // directly here -- the same call device.cpp's selectPresentMode() makes
    // internally, but re-done from scratch in this test rather than trusted
    // from the production code under test. This is what makes the
    // assertion below non-vacuous [fix round 1, task-6-review.md Medium]: a
    // selector hardwired to always return FIFO would fail this comparison
    // on any surface that reports MAILBOX or IMMEDIATE available, whereas
    // a bare "is it one of the three legal modes" check could not tell a
    // hardwired-FIFO selector apart from a working ladder. Still
    // driver-independent -- this makes no assumption about WHICH optional
    // modes this surface supports, only that whatever the ladder picks
    // matches applying the documented preference order (MAILBOX, else
    // IMMEDIATE, else FIFO) to the REAL availability list queried here.
    uint32_t supportedModeCount = 0;
    REQUIRE(vkGetPhysicalDeviceSurfacePresentModesKHR(device->physicalDevice(), fixture->surface,
                                                       &supportedModeCount, nullptr) == VK_SUCCESS);
    std::vector<VkPresentModeKHR> supportedModes(supportedModeCount);
    REQUIRE(vkGetPhysicalDeviceSurfacePresentModesKHR(device->physicalDevice(), fixture->surface,
                                                       &supportedModeCount,
                                                       supportedModes.data()) == VK_SUCCESS);
    auto supports = [&supportedModes](VkPresentModeKHR mode) {
        return std::find(supportedModes.begin(), supportedModes.end(), mode) != supportedModes.end();
    };
    VkPresentModeKHR expectedAfterVsyncOff = VK_PRESENT_MODE_FIFO_KHR;
    if (supports(VK_PRESENT_MODE_MAILBOX_KHR)) {
        expectedAfterVsyncOff = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (supports(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        expectedAfterVsyncOff = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    // setPresentMode() only records the request; recreateSwapchain() (the
    // very same NeedsRecreate path a real window resize already drives) is
    // what actually applies it -- no second recreation flow exists to call
    // instead.
    device->setPresentMode(rx::rhi::PresentMode::VsyncOff);
    REQUIRE(device->recreateSwapchain(fixture->surface));

    // Discriminates a broken/hardwired selector on every driver: on a
    // surface that reports MAILBOX or IMMEDIATE available, a selector that
    // always returns FIFO now fails here, regardless of what this specific
    // dev/CI machine happens to support (the fallback-to-FIFO branch itself
    // stays inspection-verified -- see this file's/task-6-report.md's own
    // disclosure -- since no available surface here actually lacks both
    // optional modes to force it).
    CHECK(device->presentMode() == expectedAfterVsyncOff);

    VkDevice vkDevice = device->device();

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    REQUIRE(vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &acquireSemaphore) == VK_SUCCESS);
    REQUIRE(vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &renderFinishedSemaphore) == VK_SUCCESS);

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence submitFence = VK_NULL_HANDLE;
    REQUIRE(vkCreateFence(vkDevice, &fenceInfo, nullptr, &submitFence) == VK_SUCCESS);

    // Throwaway command pool/buffer, local to this test -- same rationale
    // as the round-trip test above.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = device->graphicsQueueFamily();
    VkCommandPool commandPool = VK_NULL_HANDLE;
    REQUIRE(vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &commandPool) == VK_SUCCESS);

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = commandPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(vkDevice, &cmdAllocInfo, &cmd) == VK_SUCCESS);

    auto acquire = device->acquireNextImage(acquireSemaphore);
    REQUIRE(acquire.status == rx::rhi::SwapchainStatus::Ok);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    REQUIRE(vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS);

    // No rendering happens in this test -- just the mandatory
    // UNDEFINED -> PRESENT_SRC_KHR layout transition every acquired image
    // needs before it can be presented.
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = device->swapchainImages()[acquire.imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    // srcStage must be COLOR_ATTACHMENT_OUTPUT -- the same stage the submit
    // below waits the acquire semaphore at. See the round-trip test above
    // for the full rationale (a TOP_OF_PIPE-sourced layout transition is
    // NOT ordered by that wait and races the acquire under synchronization
    // validation).
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    REQUIRE(vkEndCommandBuffer(cmd) == VK_SUCCESS);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &acquireSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinishedSemaphore;

    REQUIRE(vkQueueSubmit(device->graphicsQueue(), 1, &submitInfo, submitFence) == VK_SUCCESS);
    REQUIRE(vkWaitForFences(vkDevice, 1, &submitFence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);

    auto presentStatus = device->present(acquire.imageIndex, renderFinishedSemaphore);
    CHECK(presentStatus != rx::rhi::SwapchainStatus::DeviceLost);

    // Same ordering rationale as the round-trip test above: never destroy a
    // semaphore the presentation engine might still be consuming without an
    // idle/fence guarantee first.
    vkDeviceWaitIdle(vkDevice);

    vkDestroyCommandPool(vkDevice, commandPool, nullptr);
    vkDestroyFence(vkDevice, submitFence, nullptr);
    vkDestroySemaphore(vkDevice, renderFinishedSemaphore, nullptr);
    vkDestroySemaphore(vkDevice, acquireSemaphore, nullptr);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
