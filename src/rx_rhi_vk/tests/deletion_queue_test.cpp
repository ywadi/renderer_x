#include <doctest/doctest.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <VkBootstrap.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

// The first three TEST_CASEs below are pure logic tests -- no Vulkan
// object, no device, nothing -- exercising DeletionQueue's own bookkeeping
// contract in isolation. The last is a real-Vulkan stress test proving
// that contract holds for genuine, asynchronously in-flight GPU resources,
// not just plain closures. Pure headless (no rx::platform::Window, no
// VkSurfaceKHR) exactly like clear_color_test.cpp/bindless_test.cpp --
// protected by tests/doctest_main.cpp's process-wide vk-bootstrap warm-up,
// same as those two files.

TEST_CASE("DeletionQueue defers a retired destructor until its frame's fence signals, then runs it exactly once") {
    rx::rhi::DeletionQueue queue;
    int destroyCount = 0;
    queue.retire([&destroyCount] { ++destroyCount; }, /*frameIndex=*/5);
    CHECK(queue.pendingCount() == 1);

    // Earlier frames signaling must not touch it -- this is the
    // "not destroyed until fence signal" half of the brief's own test
    // description.
    queue.onFrameFenceSignaled(3);
    CHECK(destroyCount == 0);
    queue.onFrameFenceSignaled(4);
    CHECK(destroyCount == 0);
    CHECK(queue.pendingCount() == 1);

    // Its own frame signaling runs it exactly once.
    queue.onFrameFenceSignaled(5);
    CHECK(destroyCount == 1);
    CHECK(queue.pendingCount() == 0);

    // A later signal must not run it again -- it was already removed.
    queue.onFrameFenceSignaled(10);
    CHECK(destroyCount == 1);
}

TEST_CASE("DeletionQueue runs several retired destructors in retire() order once each becomes due, independent of "
          "signal order") {
    rx::rhi::DeletionQueue queue;
    std::vector<int> ranOrder;
    queue.retire([&ranOrder] { ranOrder.push_back(1); }, 1);
    queue.retire([&ranOrder] { ranOrder.push_back(2); }, 2);
    queue.retire([&ranOrder] { ranOrder.push_back(3); }, 4);
    CHECK(queue.pendingCount() == 3);

    // Signaling frame 2 must run BOTH frame-1 and frame-2 items (every
    // frameIndex <= 2), in the order they were retired, and leave the
    // frame-4 item untouched.
    queue.onFrameFenceSignaled(2);
    REQUIRE(ranOrder.size() == 2);
    CHECK(ranOrder[0] == 1);
    CHECK(ranOrder[1] == 2);
    CHECK(queue.pendingCount() == 1);

    queue.onFrameFenceSignaled(4);
    REQUIRE(ranOrder.size() == 3);
    CHECK(ranOrder[2] == 3);
    CHECK(queue.pendingCount() == 0);
}

TEST_CASE("DeletionQueue::flushAll runs every pending destructor regardless of frame index, invokes waitIdleFirst, "
          "and leaves the queue empty") {
    rx::rhi::DeletionQueue queue;
    int count = 0;
    queue.retire([&count] { ++count; }, 100);
    queue.retire([&count] { ++count; }, 200);

    bool waited = false;
    queue.flushAll([&waited] { waited = true; });

    CHECK(waited);
    CHECK(count == 2);
    CHECK(queue.pendingCount() == 0);

    // Safe (and a true no-op beyond invoking waitIdleFirst again) on an
    // already-empty queue.
    bool waitedAgain = false;
    queue.flushAll([&waitedAgain] { waitedAgain = true; });
    CHECK(waitedAgain);
    CHECK(count == 2);
}

namespace {

// Pure headless fixture -- no window/surface/swapchain -- matching
// bindless_test.cpp's own HeadlessBindlessFixture pattern exactly, minus
// anything this test doesn't need (descriptor indexing, sync2). This
// test's real Vulkan resources are host-visible/device-local buffers and
// a plain vkCmdCopyBuffer, so a minimum-1.3-version device with no extra
// feature structs is enough.
struct HeadlessFixture {
    rx::rhi::Context context;
    vkb::Device vkbDevice;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkQueue queue;
    uint32_t queueFamily;
};

HeadlessFixture makeHeadlessFixture() {
    auto context = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    vkb::PhysicalDeviceSelector selector(context->vkbInstance());
    auto physResult = selector.set_minimum_version(1, 3).select();
    REQUIRE(physResult.has_value());

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    REQUIRE(deviceResult.has_value());
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    auto queueResult = vkbDevice.get_queue(vkb::QueueType::graphics);
    REQUIRE(queueResult.has_value());
    auto queueIndexResult = vkbDevice.get_queue_index(vkb::QueueType::graphics);
    REQUIRE(queueIndexResult.has_value());

    return HeadlessFixture{std::move(*context), vkbDevice, vkbDevice.device, vkbDevice.physical_device.physical_device,
                            queueResult.value(), queueIndexResult.value()};
}

}  // namespace

// The scenario the brief actually asks for end to end: real Vulkan
// buffers, real asynchronous (submitted-but-not-immediately-waited-on)
// GPU work, and many retire/signal cycles reusing a small, fixed number
// of "frame slots" -- exactly the pattern a real FrameSync-driven frame
// loop uses (FrameSync::kFramesInFlight == 2), just built by hand here
// since this test has no rx::rhi::Device/FrameSync (headless, no
// surface). If DeletionQueue destroyed anything even one frame too early,
// the validation layer's command-buffer-resource-tracking would catch a
// buffer being destroyed while a still-pending submission references it
// -- that is precisely what this test's zero-validation-errors assertion
// is checking for, not merely that the code runs without crashing.
TEST_CASE("DeletionQueue retire-while-in-flight stress: many retire/signal cycles reusing 2 frame slots, zero "
          "validation errors, no premature destroy") {
    HeadlessFixture fixture = makeHeadlessFixture();

    {
        auto allocator = rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());

        constexpr uint32_t kSlots = 2;
        constexpr int kIterations = 40;
        constexpr VkDeviceSize kBufferSize = 64;

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = fixture.queueFamily;
        VkCommandPool pool = VK_NULL_HANDLE;
        REQUIRE(vkCreateCommandPool(fixture.device, &poolInfo, nullptr, &pool) == VK_SUCCESS);

        std::array<VkCommandBuffer, kSlots> cmds{};
        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.commandPool = pool;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandBufferCount = kSlots;
        REQUIRE(vkAllocateCommandBuffers(fixture.device, &cmdAllocInfo, cmds.data()) == VK_SUCCESS);

        std::array<VkFence, kSlots> fences{};
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // SIGNALED at creation -- the loop's wait on each slot before its
        // first real use must return immediately, same reasoning as
        // FrameSync's own per-slot fences (frame_sync.h).
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < kSlots; ++i) {
            REQUIRE(vkCreateFence(fixture.device, &fenceInfo, nullptr, &fences[i]) == VK_SUCCESS);
        }

        rx::rhi::DeletionQueue deletionQueue;
        bool observedNonEmptyMidLoop = false;

        for (int i = 0; i < kIterations; ++i) {
            uint32_t slot = static_cast<uint32_t>(i) % kSlots;

            // Confirms slot `slot`'s previous submission (frame
            // i - kSlots, if this slot has been used before) has actually
            // completed on the GPU -- exactly the fence wait a real frame
            // loop performs before reusing a frame-in-flight slot.
            REQUIRE(vkWaitForFences(fixture.device, 1, &fences[slot], VK_TRUE, UINT64_MAX) == VK_SUCCESS);
            if (i >= static_cast<int>(kSlots)) {
                deletionQueue.onFrameFenceSignaled(static_cast<uint64_t>(i - static_cast<int>(kSlots)));
            }
            REQUIRE(vkResetFences(fixture.device, 1, &fences[slot]) == VK_SUCCESS);

            auto srcBuffer = allocator->createHostVisibleBuffer(kBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            REQUIRE(srcBuffer.has_value());
            std::array<uint8_t, kBufferSize> pattern{};
            for (size_t b = 0; b < pattern.size(); ++b) {
                pattern[b] = static_cast<uint8_t>((i + static_cast<int>(b)) & 0xFF);
            }
            std::memcpy(srcBuffer->mappedData(), pattern.data(), pattern.size());
            srcBuffer->flush();

            auto dstBuffer = allocator->createDeviceLocalBuffer(kBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            REQUIRE(dstBuffer.has_value());

            REQUIRE(vkResetCommandBuffer(cmds[slot], 0) == VK_SUCCESS);
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            REQUIRE(vkBeginCommandBuffer(cmds[slot], &beginInfo) == VK_SUCCESS);
            VkBufferCopy region{0, 0, kBufferSize};
            vkCmdCopyBuffer(cmds[slot], srcBuffer->handle(), dstBuffer->handle(), 1, &region);
            REQUIRE(vkEndCommandBuffer(cmds[slot]) == VK_SUCCESS);

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &cmds[slot];
            // Deliberately NOT waited on here (no vkQueueWaitIdle/
            // vkWaitForFences immediately after submit) -- both buffers
            // are genuinely in flight from this point until this same
            // slot's fence is confirmed signaled kSlots iterations later,
            // above. Retiring them now, tagged at frame `i`, is exactly
            // the "resource used by an in-flight frame" scenario the
            // brief's test description asks for.
            REQUIRE(vkQueueSubmit(fixture.queue, 1, &submitInfo, fences[slot]) == VK_SUCCESS);

            // std::function<void()> requires a copy-constructible target
            // -- a lambda that moves a move-only rx::rhi::Buffer directly
            // into its capture is not copy-constructible, so it must be
            // wrapped in a std::shared_ptr first (see deletion_queue.h's
            // own "STD::FUNCTION COPY-CONSTRUCTIBILITY PITFALL" comment;
            // this is a real std::function requirement, not a
            // DeletionQueue limitation).
            auto srcHolder = std::make_shared<rx::rhi::Buffer>(std::move(*srcBuffer));
            auto dstHolder = std::make_shared<rx::rhi::Buffer>(std::move(*dstBuffer));
            deletionQueue.retire([srcHolder, dstHolder] {}, static_cast<uint64_t>(i));

            if (i == 5) {
                // Concrete evidence the queue is really deferring, not
                // trivially discarding: several frames' worth of buffers
                // must still be pending at this point (only frames
                // 0..i-kSlots have had a chance to be confirmed done).
                observedNonEmptyMidLoop = deletionQueue.pendingCount() >= kSlots;
            }
        }

        CHECK(observedNonEmptyMidLoop);

        // Drain the last kSlots frames' worth of still-pending
        // retirements -- real device-idle wait first, per flushAll()'s
        // own contract, since nothing has confirmed those specific
        // submissions are done yet.
        deletionQueue.flushAll([&fixture] { vkDeviceWaitIdle(fixture.device); });
        CHECK(deletionQueue.pendingCount() == 0);

        for (VkFence fence : fences) {
            vkDestroyFence(fixture.device, fence, nullptr);
        }
        vkDestroyCommandPool(fixture.device, pool, nullptr);
    }

    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}
