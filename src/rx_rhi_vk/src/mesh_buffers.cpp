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
    // REAL BUG FIX: flush+WAIT immediately after the vertex upload
    // succeeds, rather than deferring to a single flush() call at the
    // very end after both uploads. If the vertex upload took the staging
    // path, it only RECORDED a copy into Uploader's command buffer --
    // nothing touches the GPU until flush() submits it. Without this
    // early flush()+wait(), a subsequent index-upload failure below hits
    // `return std::nullopt` before the copy is confirmed complete, so
    // `vertexBuffer`'s RAII destructor fires (destroying its VkBuffer)
    // while that copy may still be running on the GPU -- corrupting or
    // crashing whenever it eventually completes against an
    // already-destroyed buffer. [Phase 4 Task 11, spec D25] flush() alone
    // no longer blocks -- MeshBuffers::create() is exactly the
    // "convenience wrapper" plan Task 11 names explicitly: it keeps its
    // OWN pre-Task-11 blocking contract on purpose (the whole point of
    // this factory is "both buffers are already fully populated and safe
    // to bind/draw from immediately" by the time it returns), via an
    // explicit wait(ticket) right after flush() -- byte-identical
    // external behavior to before, just no longer implicit. wait() is a
    // safe no-op if the vertex upload took the direct (non-staging) path
    // instead (nothing was recorded, so flush() returns the trivially-
    // complete ticket and wait() returns immediately).
    uploader.wait(uploader.flush());
    if (!uploader.uploadToBuffer(*indexBuffer, 0, indexData, indexBytes)) {
        RX_LOG_ERROR("MeshBuffers::create: index upload failed");
        return std::nullopt;
    }
    uploader.wait(uploader.flush());

    return MeshBuffers(std::move(*vertexBuffer), std::move(*indexBuffer), indexCount, indexType);
}

}  // namespace rx::rhi
