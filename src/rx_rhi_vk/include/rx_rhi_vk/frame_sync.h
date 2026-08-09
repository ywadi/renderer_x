#pragma once
#include <volk.h>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace rx::rhi {

// FrameSync owns every synchronization primitive and per-frame command
// buffer the canonical "frames in flight" present loop needs, built once and
// reused for the lifetime of the loop -- never recreated per frame. This is
// the structural fix for the two defects an earlier, rejected design of this
// loop had:
//
//   1. Per-frame semaphores created AND destroyed every frame, with the
//      destroy happening immediately after vkQueuePresentKHR -- with no
//      fence or idle wait guaranteeing the presentation engine had actually
//      finished consuming them first. Destroying a semaphore the
//      presentation engine might still be waiting on is undefined behavior
//      (and a validation error on any layer that catches it). FrameSync
//      instead creates every semaphore/fence/pool once in create() and only
//      ever destroys them in its destructor -- which callers must not invoke
//      until the device has reached a real idle point (see the destructor
//      contract below).
//   2. vkQueueWaitIdle every frame (full CPU/GPU serialization, one frame at
//      a time, no overlap). FrameSync's per-frame-in-flight fences let the
//      CPU record and submit frame N+1 while frame N-1 (two frames back, for
//      framesInFlight() == 2) is still draining on the GPU -- the loop only
//      ever waits on ONE fence per iteration (the frame slot it's about to
//      reuse), never the whole queue.
//
// Ownership split (this is the part that is easy to get backwards):
//   - Per FRAME-IN-FLIGHT (framesInFlight() == 2, a fixed small number
//     bounding how many frames' worth of CPU-side resources exist
//     concurrently): one fence (created already signaled, so the very first
//     wait on it returns immediately), one imageAvailable semaphore, one
//     command pool, and one primary command buffer allocated from that pool.
//     These do not depend on the swapchain's image count and are never
//     touched by onSwapchainRecreated().
//   - Per SWAPCHAIN IMAGE: one renderFinished semaphore, indexed by the
//     *acquired* image index (not the frame-in-flight index). This is
//     required, not a style choice: a binary semaphore must not be
//     re-signaled while a previous signal of it is still unconsumed, and the
//     only thing that determines when it is safe to reuse a given
//     presentable image's "rendering finished" semaphore is that image being
//     re-acquired -- which is exactly what per-image indexing guarantees and
//     per-frame-in-flight indexing (2 slots cycling independently of which
//     image got acquired) would NOT guarantee, since acquisition order is
//     not required to match frame-in-flight order 1:1 once the swapchain has
//     more than framesInFlight() images (the common case: most swapchains
//     have 3+ images).
//
// Destructor contract (load-bearing, not a style note): FrameSync's
// destructor destroys every fence/semaphore/pool it owns unconditionally,
// with no internal wait. The caller MUST have already driven the owning
// VkDevice to a real idle point (vkDeviceWaitIdle, or an equivalent
// per-object fence/idle guarantee covering every submission and present that
// touched these objects) before a FrameSync goes out of scope or is
// otherwise destroyed. The canonical present loop this class exists for
// satisfies this by calling vkDeviceWaitIdle once at shutdown, before
// destroying FrameSync (and the per-swapchain-image views, and the
// pipeline) -- that is the only point in that loop's lifetime any of these
// objects may legally die. The same discipline applies to
// onSwapchainRecreated(): it destroys and rebuilds the per-image
// renderFinished semaphores, so it must only be called once the device has
// reached idle (the present loop's NeedsRecreate path already does this,
// via vkDeviceWaitIdle immediately before Device::recreateSwapchain()).
class FrameSync {
public:
    // Fixed at 2: enough to let the CPU stay one frame ahead of the GPU
    // without letting an unbounded number of frames queue up (which would
    // trade latency for no meaningful additional throughput on typical
    // present-mode-bound swapchains). Not a tunable today -- every per-
    // frame-in-flight array in this class is sized to this exact constant.
    static constexpr uint32_t kFramesInFlight = 2;

    FrameSync(FrameSync&&) noexcept;
    FrameSync& operator=(FrameSync&&) noexcept;
    FrameSync(const FrameSync&) = delete;
    FrameSync& operator=(const FrameSync&) = delete;

    // See the class-level comment above: the caller must have already
    // brought the device to a real idle point before this runs.
    ~FrameSync();

    static std::optional<FrameSync> create(VkDevice device, uint32_t queueFamily, uint32_t swapchainImageCount);

    static constexpr uint32_t framesInFlight() { return kFramesInFlight; }

    // Index of the frame-in-flight slot every current*() accessor below
    // currently reads from; advanced by advanceFrame().
    uint32_t currentFrameIndex() const { return currentFrame_; }

    // Current frame-in-flight slot's fence. Created VK_FENCE_CREATE_SIGNALED_BIT
    // so the loop's very first wait on any slot returns immediately. The
    // loop's contract with this fence: wait on it before touching this
    // slot's command pool/buffer again (the wait is what proves the GPU is
    // done with whatever this slot last submitted), but only reset it once
    // the loop has confirmed it is actually going to record and submit again
    // this iteration -- see frame_sync.h's discussion in the present loop's
    // own comments (samples/01_triangle/main.cpp) for why resetting
    // unconditionally before acquireNextImage() would deadlock the very next
    // wait whenever acquire itself reports NeedsRecreate.
    VkFence currentFence() const { return fences_[currentFrame_]; }

    // Current frame-in-flight slot's "image available" semaphore -- signal
    // target for Device::acquireNextImage(), wait source for the submit that
    // renders into the image it signals for.
    VkSemaphore currentImageAvailableSemaphore() const { return imageAvailable_[currentFrame_]; }

    // Current frame-in-flight slot's command pool. Reset with
    // vkResetCommandPool() once per iteration that actually records (not
    // vkResetCommandBuffer() on the single buffer allocated from it --
    // resetting the whole pool is why this pool was never created with
    // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT).
    VkCommandPool currentCommandPool() const { return commandPools_[currentFrame_]; }

    // Current frame-in-flight slot's single primary command buffer,
    // allocated once from currentCommandPool() at create() time. Valid to
    // re-begin every iteration after that pool has been reset.
    VkCommandBuffer currentCommandBuffer() const { return commandBuffers_[currentFrame_]; }

    // renderFinished semaphore for swapchain image `imageIndex` (the index
    // Device::acquireNextImage() returned) -- signaled by the submit that
    // renders into that image, waited on by Device::present() for that same
    // image. See the class-level comment for why this must be indexed by
    // image, not by frame-in-flight slot.
    VkSemaphore renderFinishedSemaphore(uint32_t imageIndex) const { return renderFinished_[imageIndex]; }

    // Advances to the next frame-in-flight slot ((current + 1) % framesInFlight()).
    // Call exactly once per loop iteration that completes a full
    // acquire/record/submit/present cycle -- not on an iteration that bails
    // out early via NeedsRecreate's `continue` path, since that iteration
    // never submitted anything against the current slot.
    void advanceFrame() { currentFrame_ = (currentFrame_ + 1) % kFramesInFlight; }

    // Rebuilds the per-swapchain-image renderFinished semaphores for a new
    // image count after Device::recreateSwapchain() -- the per-frame-in-
    // flight objects (fences/imageAvailable semaphores/command pools/command
    // buffers) are untouched, since none of them depend on how many
    // swapchain images exist. Caller must have reached device idle first
    // (see the class-level destructor contract; the present loop's
    // NeedsRecreate path already guarantees this by construction). Returns
    // false (logged) on any vkCreateSemaphore failure, leaving this
    // FrameSync with however many of the new semaphores were successfully
    // created before the failure -- callers should treat false as fatal to
    // the present loop, the same way Device::recreateSwapchain() failing is.
    bool onSwapchainRecreated(uint32_t newImageCount);

private:
    FrameSync() = default;

    bool createRenderFinishedSemaphores(uint32_t count);
    void destroyRenderFinishedSemaphores();
    void destroyAll();

    VkDevice device_ = VK_NULL_HANDLE;

    std::array<VkFence, kFramesInFlight> fences_{};
    std::array<VkSemaphore, kFramesInFlight> imageAvailable_{};
    std::array<VkCommandPool, kFramesInFlight> commandPools_{};
    std::array<VkCommandBuffer, kFramesInFlight> commandBuffers_{};

    std::vector<VkSemaphore> renderFinished_;

    uint32_t currentFrame_ = 0;
};

}  // namespace rx::rhi
