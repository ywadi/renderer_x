#include "transient_pool.h"

#include <rx_core/log.h>
#include <rx_rhi_vk/deletion_queue.h>

#include <memory>
#include <utility>

namespace rx::graph::detail {

void TransientPool::beginRealizeBatch() {
    imageClaimedThisBatch_.assign(images_.size(), false);
    bufferClaimedThisBatch_.assign(buffers_.size(), false);
    storageImageClaimedThisBatch_.assign(storageImages_.size(), false);
}

std::optional<uint32_t> TransientPool::acquireImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                                                      VkSampleCountFlagBits samples, uint64_t currentFrame) {
    for (uint32_t i = 0; i < images_.size(); ++i) {
        const PooledImage& entry = images_[i];
        if (imageClaimedThisBatch_[i] || !entry.texture.has_value()) {
            continue;
        }
        if (entry.format == format && entry.extent.width == extent.width && entry.extent.height == extent.height &&
            entry.usage == usage && entry.samples == samples) {
            imageClaimedThisBatch_[i] = true;
            images_[i].lastUsedFrame = currentFrame;
            return i;
        }
    }

    auto texture = rx::rhi::Texture2D::create(physicalDevice_, device_, *allocator_, extent, format, usage,
                                                /*requestedMipLevels=*/1);
    if (!texture.has_value()) {
        RX_LOG_ERROR("rx_graph: TransientPool::acquireImage: Texture2D::create failed ({}x{}, format={})",
                     extent.width, extent.height, static_cast<int>(format));
        return std::nullopt;
    }

    PooledImage entry;
    entry.format = format;
    entry.extent = extent;
    entry.usage = usage;
    entry.samples = samples;
    entry.texture = std::move(*texture);
    entry.lastFrameFinalStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    entry.lastFrameFinalAccess = VK_ACCESS_2_MEMORY_WRITE_BIT;
    entry.lastUsedFrame = currentFrame;

    images_.push_back(std::move(entry));
    imageClaimedThisBatch_.push_back(true);
    return static_cast<uint32_t>(images_.size() - 1);
}

std::optional<uint32_t> TransientPool::acquireStorageImage(VkFormat format, VkExtent2D extent, uint32_t mipLevels,
                                                             uint32_t arrayLayers, bool cube,
                                                             VkImageUsageFlags usage, uint64_t currentFrame) {
    for (uint32_t i = 0; i < storageImages_.size(); ++i) {
        const PooledStorageImage& entry = storageImages_[i];
        if (storageImageClaimedThisBatch_[i] || !entry.texture.has_value()) {
            continue;
        }
        if (entry.format == format && entry.extent.width == extent.width && entry.extent.height == extent.height &&
            entry.mipLevels == mipLevels && entry.arrayLayers == arrayLayers && entry.cube == cube &&
            entry.usage == usage) {
            storageImageClaimedThisBatch_[i] = true;
            storageImages_[i].lastUsedFrame = currentFrame;
            return i;
        }
    }

    auto texture = rx::rhi::StorageImage::create(physicalDevice_, device_, *allocator_, extent, format, usage,
                                                  mipLevels, arrayLayers, cube);
    if (!texture.has_value()) {
        RX_LOG_ERROR("rx_graph: TransientPool::acquireStorageImage: StorageImage::create failed ({}x{}, "
                     "format={}, mipLevels={}, arrayLayers={}, cube={})",
                     extent.width, extent.height, static_cast<int>(format), mipLevels, arrayLayers, cube);
        return std::nullopt;
    }

    PooledStorageImage entry;
    entry.format = format;
    entry.extent = extent;
    entry.mipLevels = mipLevels;
    entry.arrayLayers = arrayLayers;
    entry.cube = cube;
    entry.usage = usage;
    entry.texture = std::move(*texture);
    entry.lastFrameFinalStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    entry.lastFrameFinalAccess = VK_ACCESS_2_MEMORY_WRITE_BIT;
    entry.lastUsedFrame = currentFrame;

    storageImages_.push_back(std::move(entry));
    storageImageClaimedThisBatch_.push_back(true);
    return static_cast<uint32_t>(storageImages_.size() - 1);
}

std::optional<uint32_t> TransientPool::acquireBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                       uint64_t currentFrame) {
    for (uint32_t i = 0; i < buffers_.size(); ++i) {
        const PooledBuffer& entry = buffers_[i];
        if (bufferClaimedThisBatch_[i] || !entry.buffer.has_value()) {
            continue;
        }
        if (entry.size == size && entry.usage == usage) {
            bufferClaimedThisBatch_[i] = true;
            buffers_[i].lastUsedFrame = currentFrame;
            return i;
        }
    }

    // DEVICE_LOCAL, per task-3-brief.md's transient-pool ambiguity
    // resolution -- reuses the same ReBAR-aware direct-upload-capable
    // allocation path every other real device-consuming buffer in this
    // engine goes through (buffer.h's own comment).
    auto buffer = allocator_->createDeviceLocalBuffer(size, usage);
    if (!buffer.has_value()) {
        RX_LOG_ERROR("rx_graph: TransientPool::acquireBuffer: createDeviceLocalBuffer failed (size={}, usage={})",
                     size, static_cast<uint32_t>(usage));
        return std::nullopt;
    }

    PooledBuffer entry;
    entry.size = size;
    entry.usage = usage;
    entry.buffer = std::move(*buffer);
    entry.lastFrameFinalStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    entry.lastFrameFinalAccess = VK_ACCESS_2_MEMORY_WRITE_BIT;
    entry.lastUsedFrame = currentFrame;

    buffers_.push_back(std::move(entry));
    bufferClaimedThisBatch_.push_back(true);
    return static_cast<uint32_t>(buffers_.size() - 1);
}

std::optional<uint32_t> TransientPool::acquireHistory(std::string_view name, VkFormat format, VkExtent2D extent,
                                                        VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                                                        uint64_t currentFrame, rx::rhi::DeletionQueue& deletionQueue,
                                                        bool* freshlyCreated) {
    *freshlyCreated = false;

    uint32_t index = static_cast<uint32_t>(pinned_.size());
    for (uint32_t i = 0; i < pinned_.size(); ++i) {
        if (pinned_[i].name == name) {
            index = i;
            break;
        }
    }

    const bool isNewEntry = (index == pinned_.size());
    if (isNewEntry) {
        PinnedHistoryEntry entry;
        entry.name = std::string(name);
        pinned_.push_back(std::move(entry));
    }

    PinnedHistoryEntry& entry = pinned_[index];
    const bool shapeMatches = !isNewEntry && entry.format == format && entry.extent.width == extent.width &&
                               entry.extent.height == extent.height && entry.usage == usage &&
                               entry.samples == samples && entry.slots[0].texture.has_value() &&
                               entry.slots[1].texture.has_value();
    if (shapeMatches) {
        return index;
    }

    // Either a brand new entry, or an existing one whose shape no longer
    // matches (a resize) -- either way, both slots need a fresh
    // Texture2D, and any PREVIOUSLY-live one must be retired (not
    // destroyed outright: the GPU may still be reading/writing it via an
    // in-flight submission from a past execute() call) rather than simply
    // dropped, exactly like sweepStale()'s own retirement of a stale
    // ordinary pooled entry.
    for (PinnedHistorySlot& slot : entry.slots) {
        if (slot.texture.has_value()) {
            auto keepAlive = std::make_shared<rx::rhi::Texture2D>(std::move(*slot.texture));
            slot.texture.reset();
            deletionQueue.retire([keepAlive] {}, currentFrame);
        }

        auto texture = rx::rhi::Texture2D::create(physicalDevice_, device_, *allocator_, extent, format, usage,
                                                    /*requestedMipLevels=*/1);
        if (!texture.has_value()) {
            RX_LOG_ERROR("rx_graph: TransientPool::acquireHistory: Texture2D::create failed for '{}' ({}x{}, format={})",
                         entry.name, extent.width, extent.height, static_cast<int>(format));
            return std::nullopt;
        }
        slot.texture = std::move(*texture);
        // Fresh state for a fresh (or resized-away) image -- a resize
        // discards whatever history content/sync state existed, same as
        // every other resized PhysicalResource's pooled backing.
        slot.barrierState = detail::ResourceBarrierState{};
        slot.everWrittenByRealPass = false;
    }

    entry.format = format;
    entry.extent = extent;
    entry.usage = usage;
    entry.samples = samples;
    *freshlyCreated = true;
    return index;
}

void TransientPool::touchImage(uint32_t index, uint64_t currentFrame) {
    images_.at(index).lastUsedFrame = currentFrame;
}

void TransientPool::touchBuffer(uint32_t index, uint64_t currentFrame) {
    buffers_.at(index).lastUsedFrame = currentFrame;
}

void TransientPool::touchStorageImage(uint32_t index, uint64_t currentFrame) {
    storageImages_.at(index).lastUsedFrame = currentFrame;
}

void TransientPool::sweepStale(uint64_t currentFrame, rx::rhi::DeletionQueue& deletionQueue) {
    for (PooledImage& entry : images_) {
        if (!entry.texture.has_value()) {
            continue;
        }
        // [Fix round 1, Minor finding]: >= , not > -- an entry last
        // touched at frame F is idle during calls F+1..F+kStaleAfterExecutes
        // (kStaleAfterExecutes idle calls, matching the ruling's literal
        // "unused for kFramesInFlight+1 consecutive executes" threshold
        // exactly); `currentFrame - lastUsedFrame == kStaleAfterExecutes`
        // at the last of those calls, so retirement must trigger there,
        // not one call later.
        if (currentFrame - entry.lastUsedFrame >= kStaleAfterExecutes) {
            auto keepAlive = std::make_shared<rx::rhi::Texture2D>(std::move(*entry.texture));
            entry.texture.reset();
            deletionQueue.retire([keepAlive] {}, entry.lastUsedFrame);
        }
    }
    for (PooledBuffer& entry : buffers_) {
        if (!entry.buffer.has_value()) {
            continue;
        }
        // [Fix round 1, Minor finding]: >= , not > -- an entry last
        // touched at frame F is idle during calls F+1..F+kStaleAfterExecutes
        // (kStaleAfterExecutes idle calls, matching the ruling's literal
        // "unused for kFramesInFlight+1 consecutive executes" threshold
        // exactly); `currentFrame - lastUsedFrame == kStaleAfterExecutes`
        // at the last of those calls, so retirement must trigger there,
        // not one call later.
        if (currentFrame - entry.lastUsedFrame >= kStaleAfterExecutes) {
            auto keepAlive = std::make_shared<rx::rhi::Buffer>(std::move(*entry.buffer));
            entry.buffer.reset();
            deletionQueue.retire([keepAlive] {}, entry.lastUsedFrame);
        }
    }
    // [Task 2 (#38), gate ruling RC2] Same staleness rule as images_ above.
    for (PooledStorageImage& entry : storageImages_) {
        if (!entry.texture.has_value()) {
            continue;
        }
        if (currentFrame - entry.lastUsedFrame >= kStaleAfterExecutes) {
            auto keepAlive = std::make_shared<rx::rhi::StorageImage>(std::move(*entry.texture));
            entry.texture.reset();
            deletionQueue.retire([keepAlive] {}, entry.lastUsedFrame);
        }
    }
}

void TransientPool::retireAll(uint64_t currentFrame, rx::rhi::DeletionQueue& deletionQueue) {
    for (PooledImage& entry : images_) {
        if (!entry.texture.has_value()) {
            continue;
        }
        auto keepAlive = std::make_shared<rx::rhi::Texture2D>(std::move(*entry.texture));
        entry.texture.reset();
        deletionQueue.retire([keepAlive] {}, currentFrame);
    }
    for (PooledBuffer& entry : buffers_) {
        if (!entry.buffer.has_value()) {
            continue;
        }
        auto keepAlive = std::make_shared<rx::rhi::Buffer>(std::move(*entry.buffer));
        entry.buffer.reset();
        deletionQueue.retire([keepAlive] {}, currentFrame);
    }
    // [Task 2 (#38), gate ruling RC2]
    for (PooledStorageImage& entry : storageImages_) {
        if (!entry.texture.has_value()) {
            continue;
        }
        auto keepAlive = std::make_shared<rx::rhi::StorageImage>(std::move(*entry.texture));
        entry.texture.reset();
        deletionQueue.retire([keepAlive] {}, currentFrame);
    }
    // Phase 4 Task 1: pinned history entries are never touched by
    // sweepStale() (see that method's own comment) -- retireAll() is the
    // ONLY path that ever tears one down, at Executor shutdown.
    for (PinnedHistoryEntry& entry : pinned_) {
        for (PinnedHistorySlot& slot : entry.slots) {
            if (!slot.texture.has_value()) {
                continue;
            }
            auto keepAlive = std::make_shared<rx::rhi::Texture2D>(std::move(*slot.texture));
            slot.texture.reset();
            deletionQueue.retire([keepAlive] {}, currentFrame);
        }
    }
}

}  // namespace rx::graph::detail
