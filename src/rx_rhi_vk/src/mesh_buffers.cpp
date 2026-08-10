#include <rx_rhi_vk/mesh_buffers.h>
#include <rx_rhi_vk/upload.h>
#include <rx_core/log.h>
#include <utility>

namespace rx::rhi {

std::optional<MeshBuffers> MeshBuffers::create(Allocator& allocator, Uploader& uploader, const void* vertexData,
                                                VkDeviceSize vertexBytes, const void* indexData,
                                                VkDeviceSize indexBytes, uint32_t indexCount, VkIndexType indexType) {
    // TRANSFER_SRC_BIT alongside TRANSFER_DST_BIT on both buffers, even
    // though production draw code only ever reads them as
    // VERTEX_BUFFER_BIT/INDEX_BUFFER_BIT: it costs nothing on any target
    // this engine cares about, and it is what lets a debug readback (or a
    // future GPU-side consumer, e.g. building an acceleration structure
    // straight from these buffers) copy out of them directly instead of
    // needing a separate shadow copy.
    auto vertexBuffer = allocator.createDeviceLocalBuffer(
        vertexBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!vertexBuffer.has_value()) {
        RX_LOG_ERROR("MeshBuffers::create: vertex buffer allocation failed ({} bytes)", vertexBytes);
        return std::nullopt;
    }

    auto indexBuffer = allocator.createDeviceLocalBuffer(
        indexBytes,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!indexBuffer.has_value()) {
        RX_LOG_ERROR("MeshBuffers::create: index buffer allocation failed ({} bytes)", indexBytes);
        return std::nullopt;
    }

    if (!uploader.uploadToBuffer(vertexBuffer->handle(), 0, vertexData, vertexBytes)) {
        RX_LOG_ERROR("MeshBuffers::create: vertex upload failed");
        return std::nullopt;
    }
    if (!uploader.uploadToBuffer(indexBuffer->handle(), 0, indexData, indexBytes)) {
        RX_LOG_ERROR("MeshBuffers::create: index upload failed");
        return std::nullopt;
    }
    uploader.flush();

    return MeshBuffers(std::move(*vertexBuffer), std::move(*indexBuffer), indexCount, indexType);
}

}  // namespace rx::rhi
