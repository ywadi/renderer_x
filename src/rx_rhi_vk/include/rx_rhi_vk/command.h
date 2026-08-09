#pragma once
#include <volk.h>
#include <cstdint>
#include <functional>
#include <optional>

namespace rx::rhi {

// CommandContext owns a single transient VkCommandPool (and the one-shot
// command buffers it hands out via runOnce()) against a specific
// VkDevice/VkQueue/queue-family triple. Move-only RAII: destroys the pool
// (which implicitly frees every command buffer ever allocated from it) on
// destruction or move-assignment.
//
// A CommandContext must not outlive the VkDevice it was created against --
// vkDestroyCommandPool needs that same VkDevice handle to still be valid.
// Declaring the owning Device (or whatever else owns the raw VkDevice)
// before any CommandContext created from it in the same scope is
// sufficient, per the usual RAII/reverse-destruction discipline already
// used by Context/Device/Allocator/Buffer in this library.
class CommandContext {
public:
    CommandContext(CommandContext&&) noexcept;
    CommandContext& operator=(CommandContext&&) noexcept;
    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;
    ~CommandContext();

    static std::optional<CommandContext> create(VkDevice device, VkQueue queue, uint32_t queueFamily);

    // Synchronous one-shot command-buffer helper: allocates a primary
    // command buffer from this context's pool, begins it
    // (ONE_TIME_SUBMIT_BIT), invokes `record` to fill it, ends it, submits
    // it to this context's queue -- optionally waiting on `wait` at
    // `waitStage` and/or signaling `signal`, both VK_NULL_HANDLE/0 by
    // default for plain setup work with no cross-queue/cross-frame
    // dependency -- blocks the calling thread until the submission
    // completes (vkQueueWaitIdle), then frees the command buffer.
    //
    // This is a synchronous setup/test utility only: vkQueueWaitIdle stalls
    // the entire queue on every call, which is unacceptable in a
    // steady-state per-frame render loop that wants multiple frames in
    // flight. The real frame loop (a later task) does not use runOnce(); it
    // manages its own command buffers and fences directly instead.
    void runOnce(const std::function<void(VkCommandBuffer)>& record, VkSemaphore wait = VK_NULL_HANDLE,
                 VkPipelineStageFlags waitStage = 0, VkSemaphore signal = VK_NULL_HANDLE);

private:
    CommandContext() = default;
    CommandContext(VkDevice device, VkQueue queue, VkCommandPool pool)
        : device_(device), queue_(queue), pool_(pool) {}

    void destroyAll();

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
};

// Records a full-resource image layout transition on `cmd` using
// synchronization2 (VkImageMemoryBarrier2 + vkCmdPipelineBarrier2):
// VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT and
// VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT on both the
// source and destination side, VK_REMAINING_MIP_LEVELS/
// VK_REMAINING_ARRAY_LAYERS, and the color aspect. This is deliberately the
// maximally-conservative barrier -- correct for any layout transition on
// any color image, at the cost of over-synchronizing -- not a
// finely-scoped, minimal-stall barrier; callers with real performance
// requirements should build their own VkImageMemoryBarrier2 with a tighter
// stage/access scope instead. Suitable for setup/test code and any path
// where correctness matters more than throughput.
void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);

}  // namespace rx::rhi
