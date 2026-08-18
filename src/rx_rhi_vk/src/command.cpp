#include <rx_rhi_vk/command.h>
#include <rx_core/log.h>
#include <utility>

namespace rx::rhi {

std::optional<CommandContext> CommandContext::create(VkDevice device, VkQueue queue, uint32_t queueFamily) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &pool);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("vkCreateCommandPool failed: VkResult={}", static_cast<int>(result));
        return std::nullopt;
    }

    return CommandContext(device, queue, pool);
}

void CommandContext::runOnce(const std::function<void(VkCommandBuffer)>& record, VkSemaphore wait,
                              VkPipelineStageFlags waitStage, VkSemaphore signal, uint64_t waitValue) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(device_, &allocInfo, &cmd);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("vkAllocateCommandBuffers failed: VkResult={}", static_cast<int>(result));
        return;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(cmd, &beginInfo);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("vkBeginCommandBuffer failed: VkResult={}", static_cast<int>(result));
        vkFreeCommandBuffers(device_, pool_, 1, &cmd);
        return;
    }

    record(cmd);

    result = vkEndCommandBuffer(cmd);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("vkEndCommandBuffer failed: VkResult={}", static_cast<int>(result));
        vkFreeCommandBuffers(device_, pool_, 1, &cmd);
        return;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    // [CI-red fix, matrix-issue28 follow-up] Chained whenever `wait` is
    // provided so a TIMELINE semaphore's target counter value travels with
    // the submission -- per the Vulkan spec this struct's value entry is
    // simply ignored if `wait` turns out to be an ordinary binary
    // semaphore instead, so no per-semaphore-type branching is needed here.
    VkTimelineSemaphoreSubmitInfo timelineWaitInfo{};
    timelineWaitInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    if (wait != VK_NULL_HANDLE) {
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &wait;
        submitInfo.pWaitDstStageMask = &waitStage;
        timelineWaitInfo.waitSemaphoreValueCount = 1;
        timelineWaitInfo.pWaitSemaphoreValues = &waitValue;
        submitInfo.pNext = &timelineWaitInfo;
    }
    if (signal != VK_NULL_HANDLE) {
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signal;
    }

    result = vkQueueSubmit(queue_, 1, &submitInfo, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("vkQueueSubmit failed: VkResult={}", static_cast<int>(result));
        vkFreeCommandBuffers(device_, pool_, 1, &cmd);
        return;
    }

    // Synchronous by design -- see the class-level comment on runOnce() in
    // command.h for why this is a setup/test-only utility, not something
    // the real frame loop can afford to call every frame.
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
}

CommandContext::CommandContext(CommandContext&& other) noexcept : CommandContext() {
    *this = std::move(other);
}

CommandContext& CommandContext::operator=(CommandContext&& other) noexcept {
    if (this != &other) {
        destroyAll();

        device_ = other.device_;
        queue_ = other.queue_;
        pool_ = other.pool_;

        other.device_ = VK_NULL_HANDLE;
        other.queue_ = VK_NULL_HANDLE;
        other.pool_ = VK_NULL_HANDLE;
    }
    return *this;
}

CommandContext::~CommandContext() {
    destroyAll();
}

void CommandContext::destroyAll() {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, pool_, nullptr);
    }
}

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                      VkImageAspectFlags aspectMask) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo depInfo{};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &depInfo);
}

}  // namespace rx::rhi
