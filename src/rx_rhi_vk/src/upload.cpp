#include <rx_rhi_vk/upload.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/texture.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <cstring>
#include <utility>

namespace rx::rhi {

namespace {

// vkCmdCopyBufferToImage requires bufferOffset to be a multiple of 4 (and,
// for best behavior across formats, of the texel block size); vkCmdCopyBuffer
// has no such requirement but aligning every reservation uniformly is
// simpler than special-casing buffer-vs-image copies here. 16 covers every
// texel block size this engine's Phase 2 formats use (RGBA8 = 4 bytes,
// RGBA16F = 8 bytes, etc.) with room to spare.
constexpr VkDeviceSize kRingAlignment = 16;

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

std::optional<Uploader> Uploader::create(Allocator& allocator, Device& device, VkDeviceSize ringBufferSize) {
    VkDevice vkDevice = device.device();
    VkQueue queue = device.graphicsQueue();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // RESET_COMMAND_BUFFER_BIT: beginRecordingIfNeeded() re-begins the same
    // single command buffer every flush() cycle for this Uploader's whole
    // lifetime, via an explicit vkResetCommandBuffer -- same reasoning as
    // rx::rhi::CommandContext's own pool (command.cpp).
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device.graphicsQueueFamily();

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::create: vkCreateCommandPool failed");
        return std::nullopt;
    }

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = pool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vkDevice, &cmdAllocInfo, &cmd) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::create: vkAllocateCommandBuffers failed");
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return std::nullopt;
    }

    // Created unsignaled -- flush() always submits before it ever waits on
    // this fence, so there is no "first wait must return immediately"
    // requirement the way FrameSync's per-slot fences have.
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(vkDevice, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::create: vkCreateFence failed");
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return std::nullopt;
    }

    auto ringBuffer = allocator.createUploadRingBuffer(ringBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!ringBuffer.has_value()) {
        RX_LOG_ERROR("Uploader::create: failed to allocate the {}-byte staging ring buffer", ringBufferSize);
        vkDestroyFence(vkDevice, fence, nullptr);
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return std::nullopt;
    }

    return Uploader(vkDevice, queue, pool, cmd, fence, std::move(*ringBuffer), ringBufferSize);
}

void Uploader::beginRecordingIfNeeded() {
    if (recording_) {
        return;
    }

    vkResetCommandBuffer(cmd_, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd_, &beginInfo) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader: vkBeginCommandBuffer failed");
        return;
    }
    recording_ = true;
}

bool Uploader::reserveRingSpace(VkDeviceSize size, VkDeviceSize& outOffset) {
    if (size == 0) {
        outOffset = 0;
        return true;
    }
    if (size > ringBufferSize_) {
        RX_LOG_ERROR("Uploader: upload of {} bytes exceeds the {}-byte staging ring buffer's total capacity", size,
                     ringBufferSize_);
        return false;
    }

    VkDeviceSize aligned = alignUp(ringCursor_, kRingAlignment);
    if (aligned + size > ringBufferSize_) {
        // Not enough room left before the ring buffer's end -- flush what
        // is already recorded (submits + waits, per the class comment),
        // which resets ringCursor_ back to 0, then restart there. Already
        // known to fit: `size <= ringBufferSize_` was just checked above.
        flush();
        aligned = 0;
    }

    outOffset = aligned;
    ringCursor_ = aligned + size;
    return true;
}

bool Uploader::uploadToBuffer(Buffer& dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size) {
    RX_ZONE;
    if (size == 0) {
        return true;
    }

    // DIRECT PATH: `dst`'s own allocation (Allocator::
    // createDeviceLocalBuffer(), when `dst`'s usage carries a real
    // device-consuming bit) landed in memory that is BOTH DEVICE_LOCAL
    // and HOST_VISIBLE -- write straight into it, no staging copy, no
    // command recorded at all. See the class comment in upload.h for the
    // full mechanics and why this task's first implementation (flag on
    // the ring buffer instead of the destination) never actually engaged
    // this branch on any hardware.
    if (dst.directPathCapable() && dst.mappedData() != nullptr) {
        if (!loggedDirectPathOnce_) {
            RX_LOG_INFO(
                "Uploader::uploadToBuffer: destination memory is DEVICE_LOCAL + HOST_VISIBLE -- writing directly, "
                "no staging copy [R:C1]");
            loggedDirectPathOnce_ = true;
        }
        std::memcpy(static_cast<uint8_t*>(dst.mappedData()) + dstOffset, data, size);
        // Written-before-GPU-read (a later draw/dispatch reading this
        // buffer) on a possibly-non-coherent memory type -- Buffer::
        // flush() (this task's own API addition) makes this correct
        // regardless of coherence, exactly as it does for the ring
        // buffer's own writes below.
        dst.flush(dstOffset, size);
        everUsedDirectPath_ = true;
        return true;
    }

    // STAGING PATH: `dst` is not direct-path-capable (no device-consuming
    // usage bit, or this hardware genuinely has no memory type that is
    // both DEVICE_LOCAL and HOST_VISIBLE) -- go through the ring buffer,
    // exactly as this class always did.
    if (!loggedStagingPathOnce_) {
        RX_LOG_INFO(
            "Uploader::uploadToBuffer: destination memory is not DEVICE_LOCAL+HOST_VISIBLE -- staging through the "
            "ring buffer [R:C1]");
        loggedStagingPathOnce_ = true;
    }
    everUsedStagingPath_ = true;

    VkDeviceSize ringOffset = 0;
    if (!reserveRingSpace(size, ringOffset)) {
        return false;
    }

    std::memcpy(static_cast<uint8_t*>(ringBuffer_.mappedData()) + ringOffset, data, size);
    ringBuffer_.flush(ringOffset, size);

    beginRecordingIfNeeded();

    VkBufferCopy region{};
    region.srcOffset = ringOffset;
    region.dstOffset = dstOffset;
    region.size = size;
    vkCmdCopyBuffer(cmd_, ringBuffer_.handle(), dst.handle(), 1, &region);
    return true;
}

bool Uploader::uploadToImage(Texture2D& dst, const void* pixels, VkDeviceSize pixelBytes, bool generateMips) {
    RX_ZONE;
    if (pixelBytes == 0) {
        return true;
    }

    VkDeviceSize ringOffset = 0;
    if (!reserveRingSpace(pixelBytes, ringOffset)) {
        return false;
    }

    std::memcpy(static_cast<uint8_t*>(ringBuffer_.mappedData()) + ringOffset, pixels, pixelBytes);
    ringBuffer_.flush(ringOffset, pixelBytes);

    beginRecordingIfNeeded();

    // Every level starts VK_IMAGE_LAYOUT_UNDEFINED (image creation's
    // initialLayout) -- transitioning the whole resource at once via the
    // existing whole-image helper is correct and simpler than a
    // level-0-only transition here, since every level but 0 needs this
    // exact same UNDEFINED -> TRANSFER_DST_OPTIMAL step before
    // recordMipChainBlit() below can transition them again individually.
    transitionImage(cmd_, dst.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.bufferOffset = ringOffset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {dst.extent().width, dst.extent().height, 1};
    vkCmdCopyBufferToImage(cmd_, ringBuffer_.handle(), dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (generateMips) {
        // Handles every level's per-level barriers + blits, and leaves
        // every level (including 0) in SHADER_READ_ONLY_OPTIMAL --
        // including the level-0 TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL
        // step this copy's destination layout above sets up for.
        //
        // No `&& dst.mipLevels() > 1` guard here (an earlier version of
        // this code had one): recordMipChainBlit() already handles
        // mipLevels() == 1 correctly and cheaply via its own early
        // return (a single transitionLevelRange() call, identical to
        // this method's own `else` branch below) -- adding a redundant
        // guard here would just be duplicated logic with no behavioral
        // difference, and it made recordMipChainBlit()'s early-return
        // branch unreachable through this public API at all (flagged in
        // this task's own review; see texture_test.cpp's dedicated
        // single-mip test).
        dst.recordMipChainBlit(cmd_);
    } else {
        transitionImage(cmd_, dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return true;
}

void Uploader::flush() {
    if (!recording_) {
        return;
    }

    if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::flush: vkEndCommandBuffer failed");
        recording_ = false;
        ringCursor_ = 0;
        return;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd_;

    if (vkResetFences(device_, 1, &fence_) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::flush: vkResetFences failed");
    }
    if (vkQueueSubmit(queue_, 1, &submitInfo, fence_) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::flush: vkQueueSubmit failed");
        recording_ = false;
        ringCursor_ = 0;
        return;
    }

    // Synchronous by design -- see the class comment in upload.h for why
    // this is an acceptable Phase 2 choice.
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);

    recording_ = false;
    ringCursor_ = 0;
}

Uploader::Uploader(Uploader&& other) noexcept
    : device_(other.device_),
      queue_(other.queue_),
      pool_(other.pool_),
      cmd_(other.cmd_),
      fence_(other.fence_),
      ringBuffer_(std::move(other.ringBuffer_)),
      ringBufferSize_(other.ringBufferSize_),
      ringCursor_(other.ringCursor_),
      recording_(other.recording_),
      everUsedDirectPath_(other.everUsedDirectPath_),
      everUsedStagingPath_(other.everUsedStagingPath_),
      loggedDirectPathOnce_(other.loggedDirectPathOnce_),
      loggedStagingPathOnce_(other.loggedStagingPathOnce_) {
    other.device_ = VK_NULL_HANDLE;
    other.queue_ = VK_NULL_HANDLE;
    other.pool_ = VK_NULL_HANDLE;
    other.cmd_ = VK_NULL_HANDLE;
    other.fence_ = VK_NULL_HANDLE;
    other.ringBufferSize_ = 0;
    other.ringCursor_ = 0;
    other.recording_ = false;
    other.everUsedDirectPath_ = false;
    other.everUsedStagingPath_ = false;
    other.loggedDirectPathOnce_ = false;
    other.loggedStagingPathOnce_ = false;
}

Uploader& Uploader::operator=(Uploader&& other) noexcept {
    if (this != &other) {
        destroyAll();

        device_ = other.device_;
        queue_ = other.queue_;
        pool_ = other.pool_;
        cmd_ = other.cmd_;
        fence_ = other.fence_;
        // Buffer's own move-assignment destroys ringBuffer_'s previous
        // contents (if any) before taking over other.ringBuffer_'s.
        ringBuffer_ = std::move(other.ringBuffer_);
        ringBufferSize_ = other.ringBufferSize_;
        ringCursor_ = other.ringCursor_;
        recording_ = other.recording_;
        everUsedDirectPath_ = other.everUsedDirectPath_;
        everUsedStagingPath_ = other.everUsedStagingPath_;
        loggedDirectPathOnce_ = other.loggedDirectPathOnce_;
        loggedStagingPathOnce_ = other.loggedStagingPathOnce_;

        other.device_ = VK_NULL_HANDLE;
        other.queue_ = VK_NULL_HANDLE;
        other.pool_ = VK_NULL_HANDLE;
        other.cmd_ = VK_NULL_HANDLE;
        other.fence_ = VK_NULL_HANDLE;
        other.ringBufferSize_ = 0;
        other.ringCursor_ = 0;
        other.recording_ = false;
        other.everUsedDirectPath_ = false;
        other.everUsedStagingPath_ = false;
        other.loggedDirectPathOnce_ = false;
        other.loggedStagingPathOnce_ = false;
    }
    return *this;
}

Uploader::~Uploader() {
    // Auto-flush pending (recorded-but-not-yet-submitted) work before
    // tearing anything down -- see the class comment in upload.h. Safe
    // unconditionally: flush() itself no-ops if nothing is recording.
    flush();
    destroyAll();
}

void Uploader::destroyAll() {
    if (fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, fence_, nullptr);
    }
    if (pool_ != VK_NULL_HANDLE) {
        // Implicitly frees cmd_ (allocated from this pool) -- no separate
        // vkFreeCommandBuffers call needed, same as FrameSync's pools.
        vkDestroyCommandPool(device_, pool_, nullptr);
    }
}

}  // namespace rx::rhi
