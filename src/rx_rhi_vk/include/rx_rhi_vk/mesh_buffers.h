#pragma once
#include <rx_rhi_vk/buffer.h>
#include <cstdint>
#include <optional>
#include <utility>

namespace rx::rhi {

class Uploader;

// Two device-local (Allocator::createDeviceLocalBuffer() -- no host
// visibility at all) buffers, vertex and index, populated exactly once
// via an rx::rhi::Uploader and thereafter read-only from the GPU's
// perspective -- the mesh-storage building block Task 6+'s sample
// geometry is built on.
//
// Move-only; both underlying rx::rhi::Buffer members already handle
// their own vmaDestroyBuffer teardown correctly on move/destruction, so
// MeshBuffers itself needs no raw handles, no destroyAll(), and its move
// constructor/assignment are safely `= default` (unlike most other RAII
// types in this library, which own at least one raw Vulkan handle
// directly and must hand-write their move operations to null the source --
// see e.g. rx::rhi::Uploader's own comment on exactly this point).
//
// MeshBuffers must not outlive the Allocator (and beneath it, Device/
// Context) its two Buffers were allocated from -- same discipline as
// every other Allocator-derived RAII type in this library (see buffer.h).
class MeshBuffers {
public:
    MeshBuffers(MeshBuffers&&) noexcept = default;
    MeshBuffers& operator=(MeshBuffers&&) noexcept = default;
    MeshBuffers(const MeshBuffers&) = delete;
    MeshBuffers& operator=(const MeshBuffers&) = delete;
    ~MeshBuffers() = default;

    // Allocates a device-local vertex buffer (`vertexBytes`, usage
    // VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
    // and index buffer (`indexBytes`, usage
    // VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
    // via `allocator`, then uploads `vertexData`/`indexData` into them
    // through `uploader` and calls `uploader.wait(uploader.flush())`
    // itself before returning -- so by the time this call returns, both
    // buffers are already fully populated and safe to bind/draw from
    // immediately (no separate flush()/wait() needed by the caller for
    // just this call). [Phase 4 Task 11, spec D25] `Uploader::flush()`
    // itself no longer blocks; this factory keeps its OWN pre-Task-11
    // blocking contract on purpose, via that explicit wait(ticket) --
    // documented as a deliberate convenience, per plan Task 11's own
    // text, not an accidental holdover. `indexCount` is stored verbatim
    // for indexCount() below (this class does not infer it from
    // indexBytes/index-type size, since a caller already knows it and a
    // stored value is unambiguous regardless of index type).
    //
    // Returns std::nullopt (logged) if either buffer allocation or either
    // upload fails.
    static std::optional<MeshBuffers> create(Allocator& allocator, Uploader& uploader, const void* vertexData,
                                              VkDeviceSize vertexBytes, const void* indexData,
                                              VkDeviceSize indexBytes, uint32_t indexCount,
                                              VkIndexType indexType = VK_INDEX_TYPE_UINT32);

    VkBuffer vertexBuffer() const { return vertexBuffer_.handle(); }
    VkBuffer indexBuffer() const { return indexBuffer_.handle(); }
    VkDeviceSize vertexBufferSize() const { return vertexBuffer_.size(); }
    VkDeviceSize indexBufferSize() const { return indexBuffer_.size(); }
    uint32_t indexCount() const { return indexCount_; }
    VkIndexType indexType() const { return indexType_; }

private:
    MeshBuffers(Buffer vertexBuffer, Buffer indexBuffer, uint32_t indexCount, VkIndexType indexType)
        : vertexBuffer_(std::move(vertexBuffer)),
          indexBuffer_(std::move(indexBuffer)),
          indexCount_(indexCount),
          indexType_(indexType) {}

    Buffer vertexBuffer_;
    Buffer indexBuffer_;
    uint32_t indexCount_ = 0;
    VkIndexType indexType_ = VK_INDEX_TYPE_UINT32;
};

}  // namespace rx::rhi
