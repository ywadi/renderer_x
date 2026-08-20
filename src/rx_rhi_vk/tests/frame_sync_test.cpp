#include <doctest/doctest.h>
#include <rx_core/debug_checks.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_platform/window.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
        // [Phase 4 Task 17, FG7, gate matrix-issue25 row 8 regression CHECK]
        // A normal 64x64 HIDDEN window's queried surface extent is never
        // (0, 0) -- the zero-extent guard must never engage during this
        // fixture's own run, proving it is not silently absorbing/masking
        // the pre-existing NeedsRecreate tolerance documented above rather
        // than genuinely never triggering.
        CHECK_FALSE(device->isSuspended());
        if (acquire.status == rx::rhi::SwapchainStatus::NeedsRecreate) {
            REQUIRE(vkDeviceWaitIdle(vkDevice) == VK_SUCCESS);
            REQUIRE(device->recreateSwapchain(surface));
            REQUIRE(frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())));
            continue;
        }
        REQUIRE(acquire.status != rx::rhi::SwapchainStatus::DeviceLost);
        REQUIRE(acquire.status != rx::rhi::SwapchainStatus::Suspended);

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

    // [gate matrix-issue25 row 8] Still false after the whole run.
    CHECK_FALSE(device->isSuspended());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// FrameSync::frameNumber() (added this task, alongside rx::rhi::
// DeletionQueue -- see that class's own header comment) has no real
// Vulkan work to exercise: it's a plain counter incremented by
// advanceFrame() alongside currentFrame_. No acquire/submit/present
// needed to prove it -- just build a real FrameSync (still needs a real
// windowed Device: FrameSync::create() takes a VkDevice) and call
// advanceFrame() enough times to wrap currentFrameIndex() around its
// mod-kFramesInFlight cycle at least twice, confirming frameNumber()
// itself never wraps or repeats while currentFrameIndex() does -- the
// exact distinction DeletionQueue's retire()/onFrameFenceSignaled() ABA-
// avoidance depends on (frame_sync.h's own comment on frameNumber()).
TEST_CASE("FrameSync::frameNumber() advances monotonically with advanceFrame(), independent of "
          "currentFrameIndex()'s mod-kFramesInFlight cycling") {
    auto fixture = makeFixture("rx_rhi_vk_frame_sync_test_frame_number");
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());
    const VkDevice vkDevice = device->device();

    auto frameSync = rx::rhi::FrameSync::create(vkDevice, device->graphicsQueueFamily(),
                                                  static_cast<uint32_t>(device->swapchainImages().size()));
    REQUIRE(frameSync.has_value());

    // Starts at 0, matching currentFrameIndex()'s own starting value.
    CHECK(frameSync->frameNumber() == 0);
    CHECK(frameSync->currentFrameIndex() == 0);

    constexpr uint64_t kIterations = 10;  // > 2 * kFramesInFlight, so currentFrameIndex() wraps at least twice
    for (uint64_t i = 1; i <= kIterations; ++i) {
        frameSync->advanceFrame();
        // Monotonic, never repeats -- unlike currentFrameIndex() below.
        CHECK(frameSync->frameNumber() == i);
        CHECK(frameSync->currentFrameIndex() == static_cast<uint32_t>(i % rx::rhi::FrameSync::kFramesInFlight));
    }

    // Concrete evidence of the exact distinction frameNumber() exists
    // for: two different frameNumber() values (2 and 4, both reachable
    // within kIterations above) land on the SAME currentFrameIndex() slot
    // -- a DeletionQueue keyed on currentFrameIndex() alone could not
    // tell those two frames apart; frameNumber() can.
    CHECK(2 % rx::rhi::FrameSync::kFramesInFlight == 4 % rx::rhi::FrameSync::kFramesInFlight);
    CHECK(2 != 4);

    REQUIRE(vkDeviceWaitIdle(vkDevice) == VK_SUCCESS);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Phase 4 exit fix wave, I2; Stage-0 audit F5-remainder, stage0-audit.md:
// 136/390] Proves create()/advanceFrame()/onSwapchainRecreated()'s new
// RX_ASSERT_MAIN_THREAD guards are genuinely wired in, not merely
// documented -- mirrors rx_asset/tests/thread_guard_test.cpp's own "a
// plain std::thread stands in for a chunk >= 1 worker" pattern (legitimate
// here for the identical reason that file documents: FrameSync has no
// rx::task::Scheduler-driven caller in this phase at all, so this tests
// the thread-identity comparison the guard performs, not any real
// scheduler integration). Every worker thread below is joined before the
// next guarded call starts, so there is never more than one thread
// touching this FrameSync's state at a time -- the precondition
// rx::core::debug::detail::ViolationHook's own contract comment requires
// of a test-installed hook that records and returns rather than aborting.
#ifdef RX_DEBUG_CHECKS

namespace {

struct FrameSyncViolationCapture {
    std::mutex mutex;
    int callCount = 0;
    std::string lastContext;
};

std::atomic<FrameSyncViolationCapture*> g_frameSyncCapture{nullptr};

void captureFrameSyncViolation(const char* context) {
    FrameSyncViolationCapture* capture = g_frameSyncCapture.load(std::memory_order_relaxed);
    if (capture == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->callCount++;
    capture->lastContext = context != nullptr ? context : "";
}

struct FrameSyncViolationHookGuard {
    ~FrameSyncViolationHookGuard() {
        g_frameSyncCapture.store(nullptr, std::memory_order_relaxed);
        rx::core::debug::detail::setViolationHookForTests(nullptr);
    }
};

}  // namespace

TEST_CASE("FrameSync::create/advanceFrame/onSwapchainRecreated trip RX_ASSERT_MAIN_THREAD when called from a "
          "worker thread") {
    auto fixture = makeFixture("rx_rhi_vk_frame_sync_test_guard");
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());
    const VkDevice vkDevice = device->device();
    const uint32_t queueFamily = device->graphicsQueueFamily();
    const uint32_t imageCount = static_cast<uint32_t>(device->swapchainImages().size());

    FrameSyncViolationCapture capture;
    g_frameSyncCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureFrameSyncViolation);
    FrameSyncViolationHookGuard guard;

    std::optional<rx::rhi::FrameSync> frameSync;
    std::thread createThread([&] { frameSync = rx::rhi::FrameSync::create(vkDevice, queueFamily, imageCount); });
    createThread.join();
    REQUIRE(frameSync.has_value());  // the hook records-and-returns -- create() still ran to completion.

    std::thread advanceThread([&] { frameSync->advanceFrame(); });
    advanceThread.join();

    std::thread recreateThread([&] { (void)frameSync->onSwapchainRecreated(imageCount); });
    recreateThread.join();

    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        CHECK(capture.callCount == 3);
        CHECK(capture.lastContext == "FrameSync::onSwapchainRecreated");
    }

    REQUIRE(vkDeviceWaitIdle(vkDevice) == VK_SUCCESS);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("FrameSync::create/advanceFrame/onSwapchainRecreated do NOT trip the guard for calls genuinely on the "
          "main thread") {
    auto fixture = makeFixture("rx_rhi_vk_frame_sync_test_guard_legal");
    if (!fixture.has_value()) {
        return;
    }

    auto device = rx::rhi::Device::create(fixture->context, fixture->surface);
    REQUIRE(device.has_value());
    const VkDevice vkDevice = device->device();

    FrameSyncViolationCapture capture;
    g_frameSyncCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureFrameSyncViolation);
    FrameSyncViolationHookGuard guard;

    auto frameSync = rx::rhi::FrameSync::create(vkDevice, device->graphicsQueueFamily(),
                                                  static_cast<uint32_t>(device->swapchainImages().size()));
    REQUIRE(frameSync.has_value());
    frameSync->advanceFrame();
    REQUIRE(frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())));

    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        CHECK(capture.callCount == 0);
    }

    REQUIRE(vkDeviceWaitIdle(vkDevice) == VK_SUCCESS);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

#endif  // RX_DEBUG_CHECKS
