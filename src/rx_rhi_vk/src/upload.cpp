#include <rx_rhi_vk/upload.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/texture.h>
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <algorithm>
#include <cstdint>
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

// Half-open interval overlap test -- used by reserveRingSpace() to decide
// whether a candidate write range would stomp on a still-unreclaimed
// PendingRegion [matrix-issue28 rows 5/6].
bool regionsOverlap(VkDeviceSize start, VkDeviceSize size, VkDeviceSize otherStart, VkDeviceSize otherEnd) {
    return !(start + size <= otherStart || otherEnd <= start);
}

// [Texture-path round, item B -- the 16MB staging-cap fix] Whether `level`
// fits through this Uploader's ring in ONE copy trip (`true`), or needs
// uploadImageMips()'s own CHUNKED-ROW path (`false`) -- and, on the
// chunked branch, `outBytesPerRow` is the per-image-row byte pitch that
// path splits on. A level's bytes divide evenly into whole image rows
// (`level.size % level.extent.height == 0`) for every level this engine's
// own upload call sites ever produce: uncompressed RGBA8 (the stb decode
// path, D10 Option A's own full mip chain -- always true, size ==
// width*height*4) and any block-compressed KTX2 level whose true height is
// itself a multiple of the format's block height (every level this large
// in practice -- a level anywhere near the 16MB cap is never a 1-2 texel
// sub-block MIP TAIL, which is always a few dozen bytes at most). A level
// that fails this divisibility check is reported as "cannot chunk" rather
// than silently mis-sliced -- see uploadImageMips()'s own validation loop
// for the caller-visible failure this produces.
bool levelNeedsChunking(const Uploader::ImageMipLevel& level, VkDeviceSize ringBufferSize, VkDeviceSize& outBytesPerRow) {
    outBytesPerRow = 0;
    if (level.size <= ringBufferSize) {
        return false;
    }
    if (level.extent.height == 0 || level.size % level.extent.height != 0) {
        return true;  // needs chunking, but outBytesPerRow == 0 signals "cannot safely chunk" to the caller
    }
    outBytesPerRow = level.size / level.extent.height;
    return true;
}

}  // namespace

std::optional<Uploader> Uploader::create(Allocator& allocator, Device& device, VkDeviceSize ringBufferSize) {
    VkDevice vkDevice = device.device();
    VkQueue queue = device.graphicsQueue();

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // RESET_COMMAND_BUFFER_BIT: beginRecordingIfNeeded() individually
    // resets whichever command-buffer slot it picks (never the whole pool
    // at once) every time a new batch starts recording -- see the class
    // comment's "COMMAND-BUFFER ROTATION" paragraph in upload.h.
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device.graphicsQueueFamily();

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::create: vkCreateCommandPool failed");
        return std::nullopt;
    }

    // Timeline semaphore [Phase 4 Task 11, spec D25 as amended by gate
    // ruling RC4]: the ticket-completion primitive for this Uploader's
    // whole lifetime, replacing the single reused VkFence every flush()
    // used to vkResetFences()+resubmit into (matrix-issue28 row 1 --
    // reset-while-referenced hazard the moment flush() stops blocking).
    // Core Vulkan 1.2 (VK_KHR_timeline_semaphore's functionality is fully
    // subsumed into 1.2 core, confirmed against the Khronos registry man
    // page), no extension gating needed on this project's Vulkan 1.3
    // baseline. Starts at counter value 0 -- see UploadTicket's own
    // comment on why that makes value == 0 a safe "always complete"
    // sentinel with no special-casing needed in isComplete()/wait().
    VkSemaphoreTypeCreateInfo timelineTypeInfo{};
    timelineTypeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineTypeInfo.initialValue = 0;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &timelineTypeInfo;

    VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &timelineSemaphore) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::create: vkCreateSemaphore (timeline) failed");
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return std::nullopt;
    }

    // MemoryCategory::Staging [Phase 4 Task 10, spec D24(a)]: this ring
    // buffer IS the staging ring the category names -- the one real,
    // in-scope call site this task itself categorizes explicitly (every
    // other existing call site keeps createUploadRingBuffer()'s own
    // Staging default anyway, so this is a documentation-of-intent change,
    // not a behavior change). Unchanged by Task 11.
    auto ringBuffer =
        allocator.createUploadRingBuffer(ringBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryCategory::Staging);
    if (!ringBuffer.has_value()) {
        RX_LOG_ERROR("Uploader::create: failed to allocate the {}-byte staging ring buffer", ringBufferSize);
        vkDestroySemaphore(vkDevice, timelineSemaphore, nullptr);
        vkDestroyCommandPool(vkDevice, pool, nullptr);
        return std::nullopt;
    }

    return Uploader(vkDevice, queue, pool, timelineSemaphore, std::move(*ringBuffer), ringBufferSize);
}

void Uploader::beginRecordingIfNeeded() {
    if (recording_) {
        return;
    }

    // Pick any command-buffer slot whose last submission (if any) is
    // already confirmed complete; grow the rotation with a freshly
    // allocated buffer if none is free rather than block -- see the class
    // comment's "COMMAND-BUFFER ROTATION" paragraph in upload.h for why a
    // single reused buffer is unsafe once flush() no longer blocks, and
    // why "grow, never block" is the deliberate policy here (blocking
    // inside a call this class does not time-bound -- unlike flush()/
    // isComplete(), which the wall-clock test does bound -- would just
    // relocate the hazard rather than remove it).
    size_t freeSlot = kInvalidSlot;
    for (size_t i = 0; i < cmdSlots_.size(); ++i) {
        if (!cmdSlots_[i].everSubmitted || isComplete(cmdSlots_[i].lastTicket)) {
            freeSlot = i;
            break;
        }
    }

    if (freeSlot == kInvalidSlot) {
        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.commandPool = pool_;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandBufferCount = 1;

        VkCommandBuffer newCmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &cmdAllocInfo, &newCmd) != VK_SUCCESS) {
            RX_LOG_ERROR("Uploader: vkAllocateCommandBuffers failed while growing the command-buffer rotation");
            return;
        }
        cmdSlots_.push_back(CmdSlot{newCmd, UploadTicket{}, false});
        freeSlot = cmdSlots_.size() - 1;
    }

    vkResetCommandBuffer(cmdSlots_[freeSlot].cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmdSlots_[freeSlot].cmd, &beginInfo) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader: vkBeginCommandBuffer failed");
        return;
    }
    activeSlot_ = freeSlot;
    recording_ = true;
}

void Uploader::reclaimCompletedRegions() {
    while (!pendingRegions_.empty() && isComplete(pendingRegions_.front().ticket)) {
        pendingRegions_.pop_front();
    }
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
        // is already recorded (submits it and tracks its ring range in
        // pendingRegions_, per flush()'s own comment; does NOT wait) and
        // start a new "lap" at offset 0. Already known to fit: `size <=
        // ringBufferSize_` was just checked above.
        flush();
        ringCursor_ = 0;
        lastFlushRingCursor_ = 0;
        ++ringGeneration_;
        aligned = 0;
    }

    // D3D12-pattern ring reclamation [matrix-issue28 rows 5/6]: poll-
    // reclaim every already-complete entry at the front of the queue
    // first (never blocks); only if the requested range still overlaps
    // the OLDEST remaining entry, block on THAT SPECIFIC ticket (never a
    // whole-ring flush-and-wait) and retry. In steady state (a ring sized
    // comfortably above the working set) this loop's body never runs --
    // see upload_test.cpp's ring-wrap stress test, which asserts
    // blockingRingWaitCount() stays below the number of flush() calls
    // issued, proving partial reclaim actually engages rather than
    // degenerating into "block every time."
    reclaimCompletedRegions();
    while (!pendingRegions_.empty() &&
           regionsOverlap(aligned, size, pendingRegions_.front().start, pendingRegions_.front().end)) {
        ++blockingRingWaitCount_;
        wait(pendingRegions_.front().ticket);
        reclaimCompletedRegions();
    }

    outOffset = aligned;
    ringCursor_ = aligned + size;
    return true;
}

bool Uploader::uploadToBuffer(Buffer& dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size) {
    RX_ASSERT_MAIN_THREAD("Uploader::uploadToBuffer");
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
    vkCmdCopyBuffer(activeCmd(), ringBuffer_.handle(), dst.handle(), 1, &region);
    return true;
}

bool Uploader::uploadToImage(Texture2D& dst, const void* pixels, VkDeviceSize pixelBytes, bool generateMips) {
    RX_ASSERT_MAIN_THREAD("Uploader::uploadToImage");
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
    VkCommandBuffer cmd = activeCmd();

    // Every level starts VK_IMAGE_LAYOUT_UNDEFINED (image creation's
    // initialLayout) -- transitioning the whole resource at once via the
    // existing whole-image helper is correct and simpler than a
    // level-0-only transition here, since every level but 0 needs this
    // exact same UNDEFINED -> TRANSFER_DST_OPTIMAL step before
    // recordMipChainBlit() below can transition them again individually.
    transitionImage(cmd, dst.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.bufferOffset = ringOffset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {dst.extent().width, dst.extent().height, 1};
    vkCmdCopyBufferToImage(cmd, ringBuffer_.handle(), dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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
        dst.recordMipChainBlit(cmd);
    } else {
        transitionImage(cmd, dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return true;
}

bool Uploader::uploadImageMips(Texture2D& dst, std::span<const ImageMipLevel> levels) {
    RX_ASSERT_MAIN_THREAD("Uploader::uploadImageMips");
    RX_ZONE;
    if (levels.empty()) {
        RX_LOG_ERROR("Uploader::uploadImageMips: empty `levels` span -- at least one mip level is required");
        return false;
    }
    // [Texture-path round, item B] A level exceeding the ring's total
    // capacity is no longer an outright rejection -- see this method's own
    // header comment (upload.h) for the chunked-row design. It only fails
    // here (before ANY GPU work is recorded, preserving this method's own
    // "no partial-batch corruption" contract) when it genuinely CANNOT be
    // safely chunked: a degenerate zero-height level, or one whose byte
    // count doesn't divide evenly into whole image rows (levelNeedsChunking()'s
    // own header comment names exactly which real levels this excludes --
    // none, in this engine's current call sites).
    for (const ImageMipLevel& level : levels) {
        VkDeviceSize bytesPerRow = 0;
        if (!levelNeedsChunking(level, ringBufferSize_, bytesPerRow)) {
            continue;
        }
        if (bytesPerRow == 0) {
            RX_LOG_ERROR(
                "Uploader::uploadImageMips: level {} is {} bytes (extent {}x{}), exceeding the {}-byte staging "
                "ring buffer's total capacity, and its byte size does not divide evenly into whole image rows "
                "-- cannot chunk safely",
                level.mipLevel, level.size, level.extent.width, level.extent.height, ringBufferSize_);
            return false;
        }
        if (bytesPerRow > ringBufferSize_) {
            RX_LOG_ERROR(
                "Uploader::uploadImageMips: level {} is {} bytes, exceeding the {}-byte staging ring buffer's "
                "total capacity, and even a SINGLE row ({} bytes) does not fit -- cannot chunk",
                level.mipLevel, level.size, ringBufferSize_, bytesPerRow);
            return false;
        }
    }

    // Whole-resource UNDEFINED -> TRANSFER_DST_OPTIMAL transition, once,
    // before the FIRST level with real bytes to copy -- exactly
    // uploadToImage()'s own reasoning (every level starts UNDEFINED at
    // image-creation time; one whole-resource transition is correct and
    // simpler than a per-level one). Recorded lazily against whichever
    // command-buffer slot is active AT THAT POINT -- see this method's
    // own header comment (upload.h) for why a mid-batch ring wrap
    // (reserveRingSpace() self-flushing and starting a new recording
    // slot) does not require a SECOND transition: the underlying VkImage
    // is already, really, in TRANSFER_DST_OPTIMAL by the time any later
    // slot's commands execute (same queue, submission order preserved --
    // the same assumption this class's own ticket-ordering guarantee
    // already documents, class comment above).
    bool transitionedToDst = false;
    for (const ImageMipLevel& level : levels) {
        if (level.size == 0) {
            continue;
        }

        VkDeviceSize bytesPerRow = 0;
        if (!levelNeedsChunking(level, ringBufferSize_, bytesPerRow)) {
            // UNCHANGED single-copy path -- byte-for-byte the same
            // recording this method always did for a level that fits the
            // ring whole (every KTX2 level this engine has ever uploaded,
            // and any RGBA8 level small enough to fit in one trip).
            VkDeviceSize ringOffset = 0;
            if (!reserveRingSpace(level.size, ringOffset)) {
                return false;
            }
            std::memcpy(static_cast<uint8_t*>(ringBuffer_.mappedData()) + ringOffset, level.data, level.size);
            ringBuffer_.flush(ringOffset, level.size);

            beginRecordingIfNeeded();
            VkCommandBuffer cmd = activeCmd();
            if (!transitionedToDst) {
                transitionImage(cmd, dst.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                transitionedToDst = true;
            }

            // BLOCK-COMPRESSED ROW PITCH / SUB-BLOCK MIP TAILS -- see this
            // method's own header comment (upload.h) for the full Vulkan-
            // spec citation. bufferRowLength/bufferImageHeight are left at 0
            // (the VkBufferImageCopy default-initialized value) deliberately
            // -- "tightly packed, block-rounded" is exactly libktx's own
            // per-level byte layout, for every level including sub-block
            // tails.
            VkBufferImageCopy region{};
            region.bufferOffset = ringOffset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level.mipLevel;
            // [Phase 5 Task 6] `level.baseArrayLayer` (0 for every
            // pre-Task-6 caller, a cube face 0..5 otherwise) -- see
            // ImageMipLevel::baseArrayLayer's own header comment.
            region.imageSubresource.baseArrayLayer = level.baseArrayLayer;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {level.extent.width, level.extent.height, 1};
            vkCmdCopyBufferToImage(cmd, ringBuffer_.handle(), dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                    &region);
            continue;
        }

        // [Texture-path round, item B] CHUNKED-ROW path: this level's own
        // bytes exceed the ring's total capacity -- split it into
        // consecutive, contiguous row-groups, each staged through the
        // SAME ring buffer as its own independent reservation+copy trip
        // (D25's "chunked trips must complete before registration" is
        // automatic here: every chunk records a real vkCmdCopyBufferToImage,
        // and the ticket flush()/registerRealTexture() callers already
        // await covers ALL of them, same as any other recorded work this
        // Uploader ever produces -- no separate completion tracking
        // needed). `rowsPerChunk` is sized to the largest whole-row count
        // that fits in ONE reservation (`ringBufferSize_ / bytesPerRow`,
        // already proven <= ringBufferSize_/1 by the validation loop
        // above) -- bounded ring memory the whole time, exactly D24's own
        // "no unbounded growth" requirement; a giant level becomes many
        // bounded trips through the ring, never one giant allocation.
        const auto* srcBytes = static_cast<const uint8_t*>(level.data);
        const uint32_t rowsPerChunk =
            static_cast<uint32_t>(std::min<VkDeviceSize>(level.extent.height, ringBufferSize_ / bytesPerRow));
        uint32_t rowStart = 0;
        while (rowStart < level.extent.height) {
            uint32_t chunkRows = std::min(rowsPerChunk, level.extent.height - rowStart);
            VkDeviceSize chunkBytes = static_cast<VkDeviceSize>(chunkRows) * bytesPerRow;

            VkDeviceSize ringOffset = 0;
            if (!reserveRingSpace(chunkBytes, ringOffset)) {
                return false;
            }
            std::memcpy(static_cast<uint8_t*>(ringBuffer_.mappedData()) + ringOffset,
                        srcBytes + static_cast<VkDeviceSize>(rowStart) * bytesPerRow, chunkBytes);
            ringBuffer_.flush(ringOffset, chunkBytes);

            beginRecordingIfNeeded();
            VkCommandBuffer cmd = activeCmd();
            if (!transitionedToDst) {
                transitionImage(cmd, dst.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                transitionedToDst = true;
            }

            // Tightly packed WITHIN THIS CHUNK's own staged sub-buffer
            // (bufferRowLength/bufferImageHeight == 0, same "computed from
            // imageExtent" Vulkan copy rule the single-copy branch above
            // relies on) -- imageOffset.y advances this region to the
            // right vertical strip of the FULL level, so the final image
            // ends up identical to one big copy, just assembled from
            // several bounded ones.
            VkBufferImageCopy region{};
            region.bufferOffset = ringOffset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level.mipLevel;
            // [Phase 5 Task 6] Same `level.baseArrayLayer` threading as
            // the unchunked branch above -- proves the chunked-staging
            // path lands data in the CORRECT face's subresource, not just
            // the correct rows within one face (matrix row 12).
            region.imageSubresource.baseArrayLayer = level.baseArrayLayer;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, static_cast<int32_t>(rowStart), 0};
            region.imageExtent = {level.extent.width, chunkRows, 1};
            vkCmdCopyBufferToImage(cmd, ringBuffer_.handle(), dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                    &region);

            rowStart += chunkRows;
        }
    }

    beginRecordingIfNeeded();
    VkCommandBuffer cmd = activeCmd();
    if (!transitionedToDst) {
        // Every level had size == 0 (degenerate, not exercised by any
        // Task 14 fixture, but defensively handled rather than left
        // UNDEFINED): still transition so `dst` ends up sampleable.
        transitionImage(cmd, dst.image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    transitionImage(cmd, dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

UploadTicket Uploader::flush() {
    RX_ASSERT_MAIN_THREAD("Uploader::flush");

    if (!recording_) {
        // Nothing recorded since the last flush() (or since create()) --
        // matches this class's pre-Task-11 no-op flush() exactly, and per
        // matrix-issue28 rows 8/9 this is ALSO the correct ticket for a
        // batch that only ever took the direct memcpy path: zero fence/
        // semaphore machinery touched, ticket reports complete on this
        // same call stack, no waiting required.
        return UploadTicket{0, ringGeneration_};
    }

    CmdSlot& slot = cmdSlots_[activeSlot_];
    if (vkEndCommandBuffer(slot.cmd) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::flush: vkEndCommandBuffer failed");
        recording_ = false;
        activeSlot_ = kInvalidSlot;
        return UploadTicket{0, ringGeneration_};
    }

    // Monotonically increasing counter value this submission signals --
    // never reused, never reset (the whole point of the timeline-
    // semaphore redesign; see the class comment's "TICKET PRIMITIVE"
    // paragraph). A value is consumed here even if the submit below fails,
    // deliberately: no caller can be holding a ticket for a value that was
    // never actually signaled (flush() only ever returns the trivially-
    // complete sentinel on failure, below), so skipping a value on failure
    // is simpler and just as correct as trying to roll the counter back.
    const uint64_t ticketValue = ++lastQueuedValue_;

    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &ticketValue;

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = &timelineInfo;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &slot.cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &timelineSemaphore_;

    if (vkQueueSubmit(queue_, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::flush: vkQueueSubmit failed");
        recording_ = false;
        activeSlot_ = kInvalidSlot;
        return UploadTicket{0, ringGeneration_};
    }

    UploadTicket ticket{ticketValue, ringGeneration_};
    slot.lastTicket = ticket;
    slot.everSubmitted = true;

    // Track the ring range this batch actually wrote (buffer staging
    // and/or image staging both advance ringCursor_ identically) so
    // reserveRingSpace()'s reclamation loop can poll/wait on it later --
    // see the class comment's "RING RECLAMATION" paragraph. A batch that
    // recorded real GPU work but never touched the ring at all is not
    // possible: uploadToImage() always reserves ring space, and
    // uploadToBuffer()'s staging branch (the only branch that calls
    // beginRecordingIfNeeded()) does too -- so `recording_` being true
    // here guarantees ringCursor_ > lastFlushRingCursor_.
    if (ringCursor_ > lastFlushRingCursor_) {
        pendingRegions_.push_back(PendingRegion{ticket, lastFlushRingCursor_, ringCursor_});
        lastFlushRingCursor_ = ringCursor_;
    }

    recording_ = false;
    activeSlot_ = kInvalidSlot;
    return ticket;
}

bool Uploader::isComplete(UploadTicket ticket) const {
    RX_ASSERT_MAIN_THREAD("Uploader::isComplete");

    // The trivially-complete sentinel (see UploadTicket's own comment):
    // the timeline semaphore starts at 0 and only ever counts up, so
    // `current >= 0` is unconditionally true -- skip the Vulkan call
    // entirely rather than spending one just to confirm the obvious.
    if (ticket.value == 0) {
        return true;
    }

    uint64_t current = 0;
    VkResult result = vkGetSemaphoreCounterValue(device_, timelineSemaphore_, &current);
    if (result != VK_SUCCESS) {
        // Loud and named, never silently treated as either complete or
        // incomplete [plan Task 11 scope summary] -- this is genuinely
        // exceptional (device loss / out-of-memory territory, per the
        // vkGetSemaphoreCounterValue man page), not a normal "not yet"
        // outcome, so it gets its own ERROR line distinct from a plain
        // `false` return.
        RX_LOG_ERROR("Uploader::isComplete: vkGetSemaphoreCounterValue failed (VkResult {})",
                     static_cast<int>(result));
        return false;
    }
    return current >= ticket.value;
}

void Uploader::wait(UploadTicket ticket) const {
    RX_ASSERT_MAIN_THREAD("Uploader::wait");

    if (ticket.value == 0) {
        return;
    }

    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &timelineSemaphore_;
    waitInfo.pValues = &ticket.value;
    VkResult result = vkWaitSemaphores(device_, &waitInfo, UINT64_MAX);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("Uploader::wait: vkWaitSemaphores failed (VkResult {})", static_cast<int>(result));
    }
}

Uploader::Uploader(Uploader&& other) noexcept
    : device_(other.device_),
      queue_(other.queue_),
      pool_(other.pool_),
      timelineSemaphore_(other.timelineSemaphore_),
      lastQueuedValue_(other.lastQueuedValue_),
      cmdSlots_(std::move(other.cmdSlots_)),
      activeSlot_(other.activeSlot_),
      ringBuffer_(std::move(other.ringBuffer_)),
      ringBufferSize_(other.ringBufferSize_),
      ringCursor_(other.ringCursor_),
      lastFlushRingCursor_(other.lastFlushRingCursor_),
      pendingRegions_(std::move(other.pendingRegions_)),
      ringGeneration_(other.ringGeneration_),
      blockingRingWaitCount_(other.blockingRingWaitCount_),
      recording_(other.recording_),
      everUsedDirectPath_(other.everUsedDirectPath_),
      everUsedStagingPath_(other.everUsedStagingPath_),
      loggedDirectPathOnce_(other.loggedDirectPathOnce_),
      loggedStagingPathOnce_(other.loggedStagingPathOnce_) {
    other.device_ = VK_NULL_HANDLE;
    other.queue_ = VK_NULL_HANDLE;
    other.pool_ = VK_NULL_HANDLE;
    other.timelineSemaphore_ = VK_NULL_HANDLE;
    other.lastQueuedValue_ = 0;
    other.cmdSlots_.clear();
    other.activeSlot_ = kInvalidSlot;
    other.ringBufferSize_ = 0;
    other.ringCursor_ = 0;
    other.lastFlushRingCursor_ = 0;
    other.pendingRegions_.clear();
    other.ringGeneration_ = 0;
    other.blockingRingWaitCount_ = 0;
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
        timelineSemaphore_ = other.timelineSemaphore_;
        lastQueuedValue_ = other.lastQueuedValue_;
        cmdSlots_ = std::move(other.cmdSlots_);
        activeSlot_ = other.activeSlot_;
        // Buffer's own move-assignment destroys ringBuffer_'s previous
        // contents (if any) before taking over other.ringBuffer_'s.
        ringBuffer_ = std::move(other.ringBuffer_);
        ringBufferSize_ = other.ringBufferSize_;
        ringCursor_ = other.ringCursor_;
        lastFlushRingCursor_ = other.lastFlushRingCursor_;
        pendingRegions_ = std::move(other.pendingRegions_);
        ringGeneration_ = other.ringGeneration_;
        blockingRingWaitCount_ = other.blockingRingWaitCount_;
        recording_ = other.recording_;
        everUsedDirectPath_ = other.everUsedDirectPath_;
        everUsedStagingPath_ = other.everUsedStagingPath_;
        loggedDirectPathOnce_ = other.loggedDirectPathOnce_;
        loggedStagingPathOnce_ = other.loggedStagingPathOnce_;

        other.device_ = VK_NULL_HANDLE;
        other.queue_ = VK_NULL_HANDLE;
        other.pool_ = VK_NULL_HANDLE;
        other.timelineSemaphore_ = VK_NULL_HANDLE;
        other.lastQueuedValue_ = 0;
        other.cmdSlots_.clear();
        other.activeSlot_ = kInvalidSlot;
        other.ringBufferSize_ = 0;
        other.ringCursor_ = 0;
        other.lastFlushRingCursor_ = 0;
        other.pendingRegions_.clear();
        other.ringGeneration_ = 0;
        other.blockingRingWaitCount_ = 0;
        other.recording_ = false;
        other.everUsedDirectPath_ = false;
        other.everUsedStagingPath_ = false;
        other.loggedDirectPathOnce_ = false;
        other.loggedStagingPathOnce_ = false;
    }
    return *this;
}

Uploader::~Uploader() {
    // Auto-flush pending (recorded-but-not-yet-submitted) work, then wait
    // for EVERY ticket this Uploader ever queued -- not just the one this
    // final flush() itself returns, which could be the trivially-complete
    // sentinel even while an earlier, still-outstanding batch a caller
    // never waited/polled on is genuinely still running on the GPU. See
    // the class comment's paragraph on ticket ordering: waiting on the
    // single highest value ever queued transitively covers every earlier
    // one too, since they all signal the same monotonic counter in
    // submission order. Safe unconditionally on a moved-from Uploader:
    // flush() itself no-ops (recording_ is false), and
    // lastQueuedValue_ == 0 skips the wait below.
    flush();
    if (lastQueuedValue_ > 0) {
        wait(UploadTicket{lastQueuedValue_, ringGeneration_});
    }
    destroyAll();
}

void Uploader::destroyAll() {
    if (timelineSemaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, timelineSemaphore_, nullptr);
    }
    if (pool_ != VK_NULL_HANDLE) {
        // Implicitly frees every command buffer ever allocated from it
        // (every cmdSlots_ entry) -- no separate vkFreeCommandBuffers call
        // needed, same as FrameSync's own pools.
        vkDestroyCommandPool(device_, pool_, nullptr);
    }
}

}  // namespace rx::rhi
