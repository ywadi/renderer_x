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

    // Both buffers carry a real device-consuming usage bit
    // (VERTEX_BUFFER_BIT/INDEX_BUFFER_BIT) on top of TRANSFER_DST_BIT --
    // exactly what Uploader::uploadToBuffer()'s direct-path check needs
    // Allocator::createDeviceLocalBuffer() to have requested
    // ALLOW_TRANSFER_INSTEAD_BIT against (see that method's own comment).
    // On a ReBAR-enabled/unified-memory device, both uploads below skip
    // the staging ring entirely.
    if (!uploader.uploadToBuffer(*vertexBuffer, 0, vertexData, vertexBytes)) {
        RX_LOG_ERROR("MeshBuffers::create: vertex upload failed");
        return std::nullopt;
    }
    // REAL BUG FIX: flush immediately after the vertex upload succeeds,
    // rather than deferring to a single flush() call at the very end
    // after both uploads. If the vertex upload took the staging path, it
    // only RECORDED a copy into Uploader's command buffer -- nothing
    // touches the GPU until flush() submits it. Without this early
    // flush(), a subsequent index-upload failure below hits `return
    // std::nullopt` before flush() ever runs, so `vertexBuffer`'s RAII
    // destructor fires (destroying its VkBuffer) while that copy is still
    // only recorded, never submitted -- and once flush() eventually runs
    // on some *later* Uploader use, it would submit a copy command
    // referencing an already-destroyed buffer. Flushing here makes the
    // vertex copy submitted and (per Uploader::flush()'s synchronous
    // contract) complete before vertexBuffer can ever be destroyed,
    // independent of whether the index upload below succeeds. flush() is
    // a safe no-op if the vertex upload took the direct (non-staging)
    // path instead (nothing was recorded, so there is nothing to submit).
    uploader.flush();
    if (!uploader.uploadToBuffer(*indexBuffer, 0, indexData, indexBytes)) {
        RX_LOG_ERROR("MeshBuffers::create: index upload failed");
        return std::nullopt;
    }
    uploader.flush();

    return MeshBuffers(std::move(*vertexBuffer), std::move(*indexBuffer), indexCount, indexType);
}

}  // namespace rx::rhi
