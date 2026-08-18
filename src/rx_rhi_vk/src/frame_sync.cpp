#include <rx_rhi_vk/frame_sync.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_core/log.h>
#include <utility>

namespace rx::rhi {

void FrameSync::advanceFrame(Allocator* allocatorForBudgetRefresh) {
    currentFrame_ = (currentFrame_ + 1) % kFramesInFlight;
    ++frameNumber_;
    if (allocatorForBudgetRefresh != nullptr) {
        // [Phase 4 Task 10, gate ruling #27] See this method's own header
        // comment in frame_sync.h -- this is the wired, once-per-frame
        // vmaSetCurrentFrameIndex() call the gate identified as missing
        // everywhere in this repository before this task.
        allocatorForBudgetRefresh->setCurrentFrameIndex(static_cast<uint32_t>(frameNumber_));
    }
}

std::optional<FrameSync> FrameSync::create(VkDevice device, uint32_t queueFamily, uint32_t swapchainImageCount) {
    FrameSync sync;
    sync.device_ = device;

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        // Signaled at creation so the loop's first-ever wait on this slot
        // (before anything has been submitted against it) returns
        // immediately instead of blocking forever.
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device, &fenceInfo, nullptr, &sync.fences_[i]) != VK_SUCCESS) {
            RX_LOG_ERROR("vkCreateFence failed for frame-in-flight slot {}", i);
            return std::nullopt;
        }

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device, &semInfo, nullptr, &sync.imageAvailable_[i]) != VK_SUCCESS) {
            RX_LOG_ERROR("vkCreateSemaphore(imageAvailable) failed for frame-in-flight slot {}", i);
            return std::nullopt;
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // TRANSIENT_BIT: every command buffer allocated from this pool is
        // re-recorded every frame the pool survives a reset for -- a hint
        // the driver can use to prefer allocation strategies suited to
        // short-lived, frequently-reset buffers. Deliberately NOT
        // RESET_COMMAND_BUFFER_BIT: this pool's single command buffer is
        // never individually reset -- the whole pool is reset instead (see
        // currentCommandPool()'s doc comment), so that per-buffer-reset
        // capability is not needed.
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &sync.commandPools_[i]) != VK_SUCCESS) {
            RX_LOG_ERROR("vkCreateCommandPool failed for frame-in-flight slot {}", i);
            return std::nullopt;
        }

        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.commandPool = sync.commandPools_[i];
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &cmdAllocInfo, &sync.commandBuffers_[i]) != VK_SUCCESS) {
            RX_LOG_ERROR("vkAllocateCommandBuffers failed for frame-in-flight slot {}", i);
            return std::nullopt;
        }
    }

    if (!sync.createRenderFinishedSemaphores(swapchainImageCount)) {
        return std::nullopt;
    }

    return sync;
}

bool FrameSync::createRenderFinishedSemaphores(uint32_t count) {
    renderFinished_.assign(count, VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < count; ++i) {
        if (vkCreateSemaphore(device_, &semInfo, nullptr, &renderFinished_[i]) != VK_SUCCESS) {
            RX_LOG_ERROR("vkCreateSemaphore(renderFinished) failed for swapchain image {}", i);
            return false;
        }
    }
    return true;
}

void FrameSync::destroyRenderFinishedSemaphores() {
    for (VkSemaphore sem : renderFinished_) {
        if (sem != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, sem, nullptr);
        }
    }
    renderFinished_.clear();
}

bool FrameSync::onSwapchainRecreated(uint32_t newImageCount) {
    // Caller contract: device must already be idle (see the class-level
    // comment in frame_sync.h) -- destroying these semaphores here assumes
    // nothing on the GPU is still signaling or waiting on any of them.
    destroyRenderFinishedSemaphores();
    return createRenderFinishedSemaphores(newImageCount);
}

FrameSync::FrameSync(FrameSync&& other) noexcept : FrameSync() {
    *this = std::move(other);
}

FrameSync& FrameSync::operator=(FrameSync&& other) noexcept {
    if (this != &other) {
        destroyAll();

        device_ = other.device_;
        fences_ = other.fences_;
        imageAvailable_ = other.imageAvailable_;
        commandPools_ = other.commandPools_;
        commandBuffers_ = other.commandBuffers_;
        renderFinished_ = std::move(other.renderFinished_);
        currentFrame_ = other.currentFrame_;
        frameNumber_ = other.frameNumber_;

        other.device_ = VK_NULL_HANDLE;
        other.fences_.fill(VK_NULL_HANDLE);
        other.imageAvailable_.fill(VK_NULL_HANDLE);
        other.commandPools_.fill(VK_NULL_HANDLE);
        other.commandBuffers_.fill(VK_NULL_HANDLE);
        other.renderFinished_.clear();
        other.currentFrame_ = 0;
        other.frameNumber_ = 0;
    }
    return *this;
}

FrameSync::~FrameSync() {
    destroyAll();
}

void FrameSync::destroyAll() {
    destroyRenderFinishedSemaphores();
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        // vkDestroyCommandPool implicitly frees every command buffer
        // allocated from it (including commandBuffers_[i]) -- no separate
        // vkFreeCommandBuffers call needed.
        if (commandPools_[i] != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, commandPools_[i], nullptr);
        }
        if (imageAvailable_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
        }
        if (fences_[i] != VK_NULL_HANDLE) {
            vkDestroyFence(device_, fences_[i], nullptr);
        }
    }
}

}  // namespace rx::rhi
