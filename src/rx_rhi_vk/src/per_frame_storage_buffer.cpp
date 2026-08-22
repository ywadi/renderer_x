#include <rx_rhi_vk/per_frame_storage_buffer.h>

#include <rx_core/log.h>

#include <cstring>
#include <utility>

namespace rx::rhi {

namespace {
// [Phase 5 Task 15, #51] Dispatches to the ONE BindlessTable register
// method matching `kind` -- see BindlessTable's own header comment for why
// each storage-buffer-typed resource class needs its own named method
// (one binding per distinct Slang element TYPE) rather than one generic
// entry point.
BindlessHandle registerByKind(BindlessTable& bindless, BindlessResourceKind kind, VkBuffer buffer,
                               VkDeviceSize bytesPerSlot) {
    switch (kind) {
        case BindlessResourceKind::GenericStorageBuffer:
            return bindless.registerGenericStorageBuffer(buffer, bytesPerSlot);
        case BindlessResourceKind::ClusterLightBuffer:
            return bindless.registerClusterLightBuffer(buffer, bytesPerSlot);
        case BindlessResourceKind::StorageBuffer:
        default:
            return bindless.registerStorageBuffer(buffer, bytesPerSlot);
    }
}
}  // namespace

std::optional<PerFrameStorageBuffer> PerFrameStorageBuffer::create(Allocator& allocator, BindlessTable& bindless,
                                                                       VkDeviceSize bytesPerSlot,
                                                                       const void* initialData,
                                                                       uint32_t framesInFlight,
                                                                       BindlessResourceKind kind) {
    if (bytesPerSlot == 0) {
        RX_LOG_ERROR("rx::rhi::PerFrameStorageBuffer::create: bytesPerSlot must be > 0");
        return std::nullopt;
    }
    if (framesInFlight == 0) {
        RX_LOG_ERROR("rx::rhi::PerFrameStorageBuffer::create: framesInFlight must be > 0");
        return std::nullopt;
    }

    PerFrameStorageBuffer result;
    result.bytesPerSlot_ = bytesPerSlot;
    result.buffers_.resize(framesInFlight);
    result.handles_.resize(framesInFlight);

    for (uint32_t slot = 0; slot < framesInFlight; ++slot) {
        auto buffer = allocator.createHostVisibleBuffer(bytesPerSlot, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                          MemoryCategory::Internal);
        if (!buffer.has_value()) {
            RX_LOG_ERROR("rx::rhi::PerFrameStorageBuffer::create: createHostVisibleBuffer failed for slot {}", slot);
            // Release/destroy every slot successfully created before this
            // one -- `result` (and its own std::vector members) is
            // destroyed on this early return regardless (RAII), but the
            // bindless registrations already made need an explicit
            // release() first, exactly like this class's own destructor
            // contract documents.
            result.release(bindless);
            return std::nullopt;
        }
        if (initialData != nullptr) {
            std::memcpy(buffer->mappedData(), initialData, static_cast<size_t>(bytesPerSlot));
            buffer->flush();
        }

        BindlessHandle handle = registerByKind(bindless, kind, buffer->handle(), bytesPerSlot);
        if (!handle.isValid()) {
            RX_LOG_ERROR("rx::rhi::PerFrameStorageBuffer::create: register (kind={}) failed for slot {}",
                         static_cast<int>(kind), slot);
            result.release(bindless);
            return std::nullopt;
        }

        result.buffers_[slot] = std::move(buffer);
        result.handles_[slot] = handle;
    }
    return result;
}

bool PerFrameStorageBuffer::write(uint32_t frameSlot, const void* data, VkDeviceSize bytes) {
    if (frameSlot >= buffers_.size()) {
        RX_LOG_ERROR("rx::rhi::PerFrameStorageBuffer::write: frameSlot {} out of range (framesInFlight={})",
                     frameSlot, buffers_.size());
        return false;
    }
    if (bytes > bytesPerSlot_) {
        RX_LOG_ERROR("rx::rhi::PerFrameStorageBuffer::write: {} bytes exceeds this instance's own {}-byte slot "
                     "capacity",
                     bytes, bytesPerSlot_);
        return false;
    }
    Buffer& buffer = *buffers_[frameSlot];
    std::memcpy(buffer.mappedData(), data, static_cast<size_t>(bytes));
    buffer.flush();
    return true;
}

VkBuffer PerFrameStorageBuffer::bufferHandle(uint32_t frameSlot) const {
    if (frameSlot >= buffers_.size() || !buffers_[frameSlot].has_value()) {
        RX_LOG_ERROR("rx::rhi::PerFrameStorageBuffer::bufferHandle: frameSlot {} out of range (framesInFlight={})",
                     frameSlot, buffers_.size());
        return VK_NULL_HANDLE;
    }
    return buffers_[frameSlot]->handle();
}

uint32_t PerFrameStorageBuffer::bindlessIndex(uint32_t frameSlot) const {
    if (frameSlot >= handles_.size()) {
        RX_LOG_ERROR("rx::rhi::PerFrameStorageBuffer::bindlessIndex: frameSlot {} out of range (framesInFlight={})",
                     frameSlot, handles_.size());
        return 0;
    }
    return handles_[frameSlot].index();
}

void PerFrameStorageBuffer::release(BindlessTable& bindless) {
    for (BindlessHandle& handle : handles_) {
        if (handle.isValid()) {
            bindless.release(handle);
            handle = BindlessHandle{};
        }
    }
}

namespace detail {

const void* debugSlotBufferData(const PerFrameStorageBuffer& buffer, uint32_t slot) {
    if (slot >= buffer.buffers_.size() || !buffer.buffers_[slot].has_value()) {
        return nullptr;
    }
    return buffer.buffers_[slot]->mappedData();
}

}  // namespace detail

}  // namespace rx::rhi
