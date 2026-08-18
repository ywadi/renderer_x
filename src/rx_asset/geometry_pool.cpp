#include <rx_asset/geometry_pool.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/upload.h>
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <algorithm>
#include <utility>

namespace rx::asset {

namespace {

constexpr size_t kInvalidBlockIndex = static_cast<size_t>(-1);

}  // namespace

std::unique_ptr<GeometryPool> GeometryPool::create(rx::rhi::Allocator& allocator, rx::rhi::Device& device,
                                                     rx::rhi::Uploader& uploader, const PoolConfig& config) {
    RX_ASSERT_MAIN_THREAD("GeometryPool::create");

    const bool bdaEnabled = device.supportsBufferDeviceAddress();
    RX_LOG_INFO(
        "GeometryPool::create: chunk sizes vertex={}B index={}B, bufferDeviceAddress {} [D26.4, enablement only]",
        config.vertexChunkBytes, config.indexChunkBytes, bdaEnabled ? "ENABLED" : "not supported on this device");

    // Private constructor -- create() is a static member of this class, so
    // it has direct access; std::make_unique cannot be used against a
    // private constructor without an extra friend/tag-type indirection
    // this class has no other need for.
    return std::unique_ptr<GeometryPool>(new GeometryPool(allocator, uploader, device.device(), bdaEnabled, config));
}

GeometryPool::GeometryPool(rx::rhi::Allocator& allocator, rx::rhi::Uploader& uploader, VkDevice device,
                            bool bufferDeviceAddressEnabled, PoolConfig config)
    : allocator_(allocator),
      uploader_(uploader),
      device_(device),
      bufferDeviceAddressEnabled_(bufferDeviceAddressEnabled),
      config_(config) {}

GeometryPool::~GeometryPool() {
    destroyAll();
}

void GeometryPool::destroyAll() {
    for (Block& block : blocks_) {
        // vmaDestroyVirtualBlock() itself asserts (VMA_ASSERT) if any
        // suballocation was never individually freed nor cleared via
        // vmaClearVirtualBlock() first -- per its own doc comment
        // (vk_mem_alloc.h). vmaClearVirtualBlock() is the documented
        // "free everything at once" teardown call, exactly right for a
        // whole-pool destructor (never used for a single MeshRange's
        // free(), which must only release ITS OWN range -- see free()
        // below).
        if (block.vertexVirtualBlock != VK_NULL_HANDLE) {
            vmaClearVirtualBlock(block.vertexVirtualBlock);
            vmaDestroyVirtualBlock(block.vertexVirtualBlock);
        }
        if (block.indexVirtualBlock != VK_NULL_HANDLE) {
            vmaClearVirtualBlock(block.indexVirtualBlock);
            vmaDestroyVirtualBlock(block.indexVirtualBlock);
        }
    }
    blocks_.clear();
}

bool GeometryPool::addBlock(VkDeviceSize minVertexBytes, VkDeviceSize minIndexBytes) {
    const VkDeviceSize requestedVertexBytes = std::max(config_.vertexChunkBytes, minVertexBytes);
    const VkDeviceSize requestedIndexBytes = std::max(config_.indexChunkBytes, minIndexBytes);

    // ELEMENT-UNIT virtual blocks (empirically load-bearing -- see the
    // class header comment's "Suballocation units" paragraph): the vertex
    // virtual block's addressable space is `vertexElementCapacity`
    // PoolVertex-sized UNITS, not bytes; the index virtual block's is
    // `indexElementCapacity` uint32_t-sized units. Rounds the requested
    // byte size DOWN to a whole number of elements, then sizes the REAL
    // Buffer to exactly that many elements' worth of bytes -- so the
    // virtual block's element-addressable range always matches the real
    // buffer's element-addressable range exactly (never claims a
    // fractional trailing element the buffer doesn't actually have room
    // for).
    const VkDeviceSize vertexElementCapacity = requestedVertexBytes / sizeof(PoolVertex);
    const VkDeviceSize indexElementCapacity = requestedIndexBytes / sizeof(uint32_t);
    const VkDeviceSize vertexBytes = vertexElementCapacity * sizeof(PoolVertex);
    const VkDeviceSize indexBytes = indexElementCapacity * sizeof(uint32_t);

    // TRANSFER_SRC_BIT alongside TRANSFER_DST_BIT on both buffers, even
    // though production draw code only ever reads them as
    // VERTEX_BUFFER_BIT/INDEX_BUFFER_BIT: mirrors rx::rhi::MeshBuffers::
    // create()'s identical precedent (mesh_buffers.cpp) -- "it costs
    // nothing on any target this engine cares about, and it is what lets
    // a debug readback (or a future GPU-side consumer, e.g. building an
    // acceleration structure straight from these buffers) copy out of
    // them directly instead of needing a separate shadow copy." Load-
    // bearing for this class's own tests (geometry_pool_test.cpp's
    // byte-exact readback proofs read block buffers via vkCmdCopyBuffer).
    VkBufferUsageFlags vertexUsage =
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBufferUsageFlags indexUsage =
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    // [D26.4] The usage bit that is genuinely unpurchasable later: a block
    // created without it can never retroactively gain a valid device
    // address without destroying and recreating (and thus re-uploading
    // everything in) that VkBuffer -- see the class header comment and
    // gate matrix-issue21's BDA rows.
    if (bufferDeviceAddressEnabled_) {
        vertexUsage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        indexUsage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    // [D24(a)] MemoryCategory::GeometryPool threaded through the SAME
    // choke point (Allocator::createDeviceLocalBuffer()) every other
    // device-local buffer in this engine already uses -- this is what
    // makes a chunk allocation show up in Allocator::report()'s
    // "geometry-pool" category for free, with no separate ledger.
    auto vertexBuffer =
        allocator_.createDeviceLocalBuffer(vertexBytes, vertexUsage, rx::rhi::MemoryCategory::GeometryPool);
    if (!vertexBuffer.has_value()) {
        RX_LOG_ERROR("GeometryPool::addBlock: vertex chunk buffer allocation failed ({} bytes)", vertexBytes);
        return false;
    }

    auto indexBuffer =
        allocator_.createDeviceLocalBuffer(indexBytes, indexUsage, rx::rhi::MemoryCategory::GeometryPool);
    if (!indexBuffer.has_value()) {
        RX_LOG_ERROR("GeometryPool::addBlock: index chunk buffer allocation failed ({} bytes)", indexBytes);
        return false;
    }

    // TLSF by construction: VmaVirtualBlockCreateInfo::flags is left at 0
    // -- the default metadata class whenever
    // VMA_VIRTUAL_BLOCK_CREATE_LINEAR_ALGORITHM_BIT is NOT set (verified
    // directly against the vendored vk_mem_alloc.h, matrix-issue21's own
    // "VmaVirtualBlock allocation algorithm" row) -- NEVER set that flag
    // here: it selects a stack/ring/free-at-once allocator structurally
    // unsuited to a pool with arbitrary alloc/free (D9).
    //
    // `size` here is `vertexElementCapacity`/`indexElementCapacity` --
    // ELEMENT counts, not bytes; see this method's own comment above and
    // the class header's "Suballocation units" paragraph for why.
    VmaVirtualBlockCreateInfo vertexBlockInfo{};
    vertexBlockInfo.size = vertexElementCapacity;
    VmaVirtualBlock vertexVirtualBlock = VK_NULL_HANDLE;
    if (vmaCreateVirtualBlock(&vertexBlockInfo, &vertexVirtualBlock) != VK_SUCCESS) {
        RX_LOG_ERROR("GeometryPool::addBlock: vmaCreateVirtualBlock (vertex) failed ({} elements)",
                     vertexElementCapacity);
        return false;
    }

    VmaVirtualBlockCreateInfo indexBlockInfo{};
    indexBlockInfo.size = indexElementCapacity;
    VmaVirtualBlock indexVirtualBlock = VK_NULL_HANDLE;
    if (vmaCreateVirtualBlock(&indexBlockInfo, &indexVirtualBlock) != VK_SUCCESS) {
        RX_LOG_ERROR("GeometryPool::addBlock: vmaCreateVirtualBlock (index) failed ({} elements)",
                     indexElementCapacity);
        vmaDestroyVirtualBlock(vertexVirtualBlock);
        return false;
    }

    Block block{std::move(*vertexBuffer),
                std::move(*indexBuffer),
                vertexVirtualBlock,
                indexVirtualBlock,
                vertexBytes,
                indexBytes,
                /*vertexBufferAddress=*/0,
                /*indexBufferAddress=*/0,
                /*vertexAllocations=*/{},
                /*indexAllocations=*/{}};

    if (bufferDeviceAddressEnabled_) {
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = block.vertexBuffer.handle();
        block.vertexBufferAddress = vkGetBufferDeviceAddress(device_, &addressInfo);
        addressInfo.buffer = block.indexBuffer.handle();
        block.indexBufferAddress = vkGetBufferDeviceAddress(device_, &addressInfo);
    }

    RX_LOG_INFO("GeometryPool::addBlock: new block {} created (vertex={}B/{} elements index={}B/{} elements, total "
                "blocks now {})",
                blocks_.size(), vertexBytes, vertexElementCapacity, indexBytes, indexElementCapacity,
                blocks_.size() + 1);
    blocks_.push_back(std::move(block));
    return true;
}

void GeometryPool::discardOrphanedLastBlock() {
    Block& block = blocks_.back();
    // Both call sites guarantee zero live suballocations remain in
    // either virtual block by this point (see this method's own header
    // comment) -- vmaDestroyVirtualBlock() is safe directly, no
    // vmaClearVirtualBlock() needed. Plain vmaDestroyVirtualBlock() calls
    // (not the checked pattern addBlock()'s own error paths use) are
    // correct here: both handles are guaranteed non-null (this block was
    // fully constructed by a successful addBlock() call before either
    // upload() call site reaches here).
    vmaDestroyVirtualBlock(block.vertexVirtualBlock);
    vmaDestroyVirtualBlock(block.indexVirtualBlock);
    // pop_back() runs Block's implicit destructor on the removed element,
    // which destroys vertexBuffer/indexBuffer via rx::rhi::Buffer's own
    // RAII (releasing the D24(a) accounting entry and the real VMA
    // allocation) -- no separate buffer cleanup needed here.
    blocks_.pop_back();
}

MeshRange GeometryPool::suballocateAndRecord(std::span<const PoolVertex> vertices, std::span<const uint32_t> indices) {
    if (vertices.empty() || indices.empty()) {
        RX_LOG_ERROR("GeometryPool::upload: rejected -- vertices ({}) and indices ({}) must both be non-empty",
                     vertices.size(), indices.size());
        return MeshRange{};
    }

    const auto vertexElementCount = static_cast<VkDeviceSize>(vertices.size());
    const auto indexElementCount = static_cast<VkDeviceSize>(indices.size());
    const VkDeviceSize vertexBytes = vertexElementCount * sizeof(PoolVertex);
    const VkDeviceSize indexBytes = indexElementCount * sizeof(uint32_t);

    size_t targetBlockIndex = kInvalidBlockIndex;
    // ELEMENT offsets (PoolVertex count / uint32_t count), never byte
    // offsets -- see addBlock()'s "ELEMENT-UNIT virtual blocks" comment.
    // Because the virtual block's own addressable unit IS one element,
    // `alignment` below is left at 0 (VMA's documented "no special
    // alignment beyond the unit itself" sentinel, a trivially valid
    // power-of-two-or-zero value) -- there is no separate byte-alignment
    // step to get right or wrong: every returned offset is an INTEGER
    // element count by construction, so `vertexOffset`/`firstIndex` below
    // are exact with zero division, zero truncation risk, and zero
    // dependency on 48 (PoolVertex's stride) happening to be a power of
    // two -- it is not (48 = 16*3), which is exactly why this class does
    // NOT pass byte-domain alignment=48 to VMA: `VmaVirtualAllocationCreateInfo::
    // alignment` is documented "Must be power of two" (vk_mem_alloc.h),
    // and passing 48 there was empirically verified (this task) to
    // produce a garbage, non-48-aligned offset -- see the task report for
    // the reproduction. The index side (4-byte uint32_t, alignment=4,
    // which IS a power of two) would have worked with the byte-domain
    // approach, but the SAME element-unit design is used for both sides
    // for symmetry and because it is strictly simpler (no alignment
    // parameter to reason about on either side at all).
    VkDeviceSize vertexElementOffset = 0;
    VmaVirtualAllocation vertexAlloc = VK_NULL_HANDLE;
    VkDeviceSize indexElementOffset = 0;
    VmaVirtualAllocation indexAlloc = VK_NULL_HANDLE;

    // Try every EXISTING block, in creation order, before growing --
    // never a hidden compaction/defrag step (D9): a block either has a
    // single free hole big enough for both requests right now, or it
    // doesn't, full stop.
    for (size_t i = 0; i < blocks_.size() && targetBlockIndex == kInvalidBlockIndex; ++i) {
        Block& candidate = blocks_[i];

        VmaVirtualAllocationCreateInfo vertexCreateInfo{};
        vertexCreateInfo.size = vertexElementCount;
        VmaVirtualAllocation vertexCandidate = VK_NULL_HANDLE;
        VkDeviceSize vertexOffsetCandidate = 0;
        if (vmaVirtualAllocate(candidate.vertexVirtualBlock, &vertexCreateInfo, &vertexCandidate,
                                &vertexOffsetCandidate) != VK_SUCCESS) {
            continue;
        }

        VmaVirtualAllocationCreateInfo indexCreateInfo{};
        indexCreateInfo.size = indexElementCount;
        VmaVirtualAllocation indexCandidate = VK_NULL_HANDLE;
        VkDeviceSize indexOffsetCandidate = 0;
        if (vmaVirtualAllocate(candidate.indexVirtualBlock, &indexCreateInfo, &indexCandidate,
                                &indexOffsetCandidate) != VK_SUCCESS) {
            // Roll back the vertex-side suballocation this block already
            // granted -- this block cannot host the whole mesh, so it
            // must end this call with no partial allocation left behind.
            vmaVirtualFree(candidate.vertexVirtualBlock, vertexCandidate);
            continue;
        }

        targetBlockIndex = i;
        vertexElementOffset = vertexOffsetCandidate;
        vertexAlloc = vertexCandidate;
        indexElementOffset = indexOffsetCandidate;
        indexAlloc = indexCandidate;
    }

    if (targetBlockIndex == kInvalidBlockIndex) {
        // Exhaustion (or the pool is still empty): grow by one block,
        // sized to fit this request even if it exceeds the configured
        // chunk default. addBlock() may reallocate blocks_ (std::vector
        // growth) -- every reference into blocks_ below is re-fetched by
        // INDEX after this call, never held across it.
        if (!addBlock(vertexBytes, indexBytes)) {
            RX_LOG_ERROR("GeometryPool::upload: failed to grow a new block for {} vertex bytes / {} index bytes",
                         vertexBytes, indexBytes);
            return MeshRange{};
        }
        targetBlockIndex = blocks_.size() - 1;
        Block& freshBlock = blocks_[targetBlockIndex];

        VmaVirtualAllocationCreateInfo vertexCreateInfo{};
        vertexCreateInfo.size = vertexElementCount;
        if (vmaVirtualAllocate(freshBlock.vertexVirtualBlock, &vertexCreateInfo, &vertexAlloc,
                                &vertexElementOffset) != VK_SUCCESS) {
            RX_LOG_ERROR(
                "GeometryPool::upload: vertex suballocation failed on a block just sized to fit it -- should be "
                "unreachable; discarding the orphaned block");
            // [Fix round 1, review minor] Nothing was ever allocated from
            // EITHER of this block's virtual blocks yet -- safe to
            // destroy and pop directly, no vmaVirtualFree needed first.
            discardOrphanedLastBlock();
            return MeshRange{};
        }

        VmaVirtualAllocationCreateInfo indexCreateInfo{};
        indexCreateInfo.size = indexElementCount;
        if (vmaVirtualAllocate(freshBlock.indexVirtualBlock, &indexCreateInfo, &indexAlloc, &indexElementOffset) !=
            VK_SUCCESS) {
            RX_LOG_ERROR(
                "GeometryPool::upload: index suballocation failed on a block just sized to fit it -- should be "
                "unreachable; discarding the orphaned block");
            // [Fix round 1, review minor] Release the vertex-side
            // suballocation that DID succeed first, so the block carries
            // zero live suballocations before discardOrphanedLastBlock()
            // destroys its virtual blocks.
            vmaVirtualFree(freshBlock.vertexVirtualBlock, vertexAlloc);
            discardOrphanedLastBlock();
            return MeshRange{};
        }
    }

    // Re-fetch by index: addBlock() above may have reallocated blocks_.
    Block& target = blocks_[targetBlockIndex];

    target.vertexAllocations.emplace(vertexElementOffset, vertexAlloc);
    target.indexAllocations.emplace(indexElementOffset, indexAlloc);

    // [D25] Consumes the Task 11 UploadTicket path -- never assumes the
    // old synchronous-flush contract. RECORDS the copy only (records onto
    // this pool's Uploader's active command buffer) -- neither flush() nor
    // wait() happen here; see this method's two callers (upload()/
    // uploadDeferred() below) for who owns each half of that contract.
    //
    // uploadToBuffer() needs a BYTE destination offset -- the
    // multiplication below (element offset * element stride) can never
    // lose precision (unlike a division, it has no remainder to drop),
    // so it is always exact.
    const VkDeviceSize vertexByteOffset = vertexElementOffset * sizeof(PoolVertex);
    const VkDeviceSize indexByteOffset = indexElementOffset * sizeof(uint32_t);
    uploader_.uploadToBuffer(target.vertexBuffer, vertexByteOffset, vertices.data(), vertexBytes);
    uploader_.uploadToBuffer(target.indexBuffer, indexByteOffset, indices.data(), indexBytes);

    // Element offsets, exact by construction (see above) -- NOT a byte
    // offset divided down. `vertexOffset`/`firstIndex` are the plain
    // element-count values vmaVirtualAllocate() returned; no division
    // occurs anywhere on this path.
    MeshRange range;
    range.blockId = static_cast<uint32_t>(targetBlockIndex);
    range.firstIndex = static_cast<uint32_t>(indexElementOffset);
    range.indexCount = static_cast<uint32_t>(indices.size());
    range.vertexOffset = static_cast<int32_t>(vertexElementOffset);
    return range;
}

MeshRange GeometryPool::upload(std::span<const PoolVertex> vertices, std::span<const uint32_t> indices) {
    RX_ASSERT_MAIN_THREAD("GeometryPool::upload");

    // Per D25's documented sync convenience, this call does exactly ONE
    // flush()+wait() for its own batch before returning -- both spans
    // share that one flush, never a separate wait per suballocation --
    // mirroring rx::rhi::MeshBuffers::create()'s identical "convenience
    // wrapper" contract, so the range this call returns is always
    // immediately safe to bind/draw.
    MeshRange range = suballocateAndRecord(vertices, indices);
    if (range.indexCount == 0) {
        // Nothing was recorded (empty input, already logged there, or a
        // real suballocation failure, also already logged there) -- per
        // this class's own contract, GeometryPool never returns a
        // zero-indexCount range for a request that actually succeeded, so
        // this is a reliable "nothing to flush" signal. Skipping matters:
        // flushing/waiting here anyway would not be meaningless in the
        // sense of no-op -- it would flush/wait on whatever a PRIOR caller
        // already recorded but had not yet flushed on this same Uploader,
        // which is not this call's job to do.
        return range;
    }
    uploader_.wait(uploader_.flush());
    return range;
}

MeshRange GeometryPool::uploadDeferred(std::span<const PoolVertex> vertices, std::span<const uint32_t> indices) {
    RX_ASSERT_MAIN_THREAD("GeometryPool::uploadDeferred");
    return suballocateAndRecord(vertices, indices);
}

rx::rhi::UploadTicket GeometryPool::flushPendingUploads() {
    RX_ASSERT_MAIN_THREAD("GeometryPool::flushPendingUploads");
    return uploader_.flush();
}

bool GeometryPool::isUploadComplete(rx::rhi::UploadTicket ticket) const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::isUploadComplete");
    return uploader_.isComplete(ticket);
}

void GeometryPool::free(const MeshRange& range) {
    RX_ASSERT_MAIN_THREAD("GeometryPool::free");

    if (range.blockId >= blocks_.size()) {
        RX_LOG_ERROR("GeometryPool::free: blockId {} is out of range ({} blocks live)", range.blockId,
                     blocks_.size());
        return;
    }

    Block& block = blocks_[range.blockId];
    // MeshRange's fields ARE the element offsets upload() originally
    // allocated at (see that method's own comment) -- looked up directly,
    // no byte conversion.
    const auto vertexElementOffset = static_cast<VkDeviceSize>(range.vertexOffset);
    const auto indexElementOffset = static_cast<VkDeviceSize>(range.firstIndex);

    auto vertexIt = block.vertexAllocations.find(vertexElementOffset);
    auto indexIt = block.indexAllocations.find(indexElementOffset);
    if (vertexIt == block.vertexAllocations.end() || indexIt == block.indexAllocations.end()) {
        RX_LOG_ERROR(
            "GeometryPool::free: MeshRange{{blockId={}, firstIndex={}, vertexOffset={}}} does not match a live "
            "suballocation -- double-free or a range this pool never returned",
            range.blockId, range.firstIndex, range.vertexOffset);
        return;
    }

    // No defragmentation (D9): this only releases the range back to its
    // block's own TLSF free-list for future reuse (vmaVirtualFree
    // coalesces adjacent free space internally) -- it never moves any
    // other live suballocation's data.
    vmaVirtualFree(block.vertexVirtualBlock, vertexIt->second);
    vmaVirtualFree(block.indexVirtualBlock, indexIt->second);
    block.vertexAllocations.erase(vertexIt);
    block.indexAllocations.erase(indexIt);
}

void GeometryPool::bind(VkCommandBuffer cmd, uint32_t blockId) const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::bind");

    if (blockId >= blocks_.size()) {
        RX_LOG_ERROR("GeometryPool::bind: blockId {} is out of range ({} blocks live)", blockId, blocks_.size());
        return;
    }

    const Block& block = blocks_[blockId];
    VkBuffer vertexBufferHandle = block.vertexBuffer.handle();
    VkDeviceSize vertexBindOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBufferHandle, &vertexBindOffset);
    vkCmdBindIndexBuffer(cmd, block.indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
}

uint32_t GeometryPool::blockCount() const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::blockCount");
    return static_cast<uint32_t>(blocks_.size());
}

bool GeometryPool::bufferDeviceAddressEnabled() const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::bufferDeviceAddressEnabled");
    return bufferDeviceAddressEnabled_;
}

PoolStats GeometryPool::stats() const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::stats");
    PoolStats out;
    out.blockCount = static_cast<uint32_t>(blocks_.size());
    out.blocks.reserve(blocks_.size());

    for (const Block& block : blocks_) {
        // VmaStatistics::allocationBytes/blockBytes are reported in
        // WHATEVER UNITS the virtual block was created with -- since
        // these two blocks are sized in ELEMENT counts, not bytes (see
        // addBlock()'s own comment), the raw values here are element
        // counts too; multiplying back by each side's element stride is
        // what turns them into the byte-denominated PoolStats/
        // PoolBlockStats public contract.
        VmaStatistics vertexStats{};
        vmaGetVirtualBlockStatistics(block.vertexVirtualBlock, &vertexStats);
        VmaStatistics indexStats{};
        vmaGetVirtualBlockStatistics(block.indexVirtualBlock, &indexStats);

        PoolBlockStats blockStats;
        blockStats.vertexBytesUsed = vertexStats.allocationBytes * sizeof(PoolVertex);
        blockStats.vertexBytesCapacity = block.vertexCapacityBytes;
        blockStats.indexBytesUsed = indexStats.allocationBytes * sizeof(uint32_t);
        blockStats.indexBytesCapacity = block.indexCapacityBytes;

        out.vertexBytesUsed += blockStats.vertexBytesUsed;
        out.vertexBytesCapacity += blockStats.vertexBytesCapacity;
        out.indexBytesUsed += blockStats.indexBytesUsed;
        out.indexBytesCapacity += blockStats.indexBytesCapacity;
        out.blocks.push_back(blockStats);
    }

    return out;
}

VkDeviceAddress GeometryPool::vertexBufferDeviceAddress(uint32_t blockId) const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::vertexBufferDeviceAddress");
    if (!bufferDeviceAddressEnabled_ || blockId >= blocks_.size()) {
        return 0;
    }
    return blocks_[blockId].vertexBufferAddress;
}

VkDeviceAddress GeometryPool::indexBufferDeviceAddress(uint32_t blockId) const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::indexBufferDeviceAddress");
    if (!bufferDeviceAddressEnabled_ || blockId >= blocks_.size()) {
        return 0;
    }
    return blocks_[blockId].indexBufferAddress;
}

VkDeviceSize GeometryPool::vertexBufferAllocatedBytes(uint32_t blockId) const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::vertexBufferAllocatedBytes");
    if (blockId >= blocks_.size()) {
        return 0;
    }
    return blocks_[blockId].vertexBuffer.allocatedBytes();
}

VkDeviceSize GeometryPool::indexBufferAllocatedBytes(uint32_t blockId) const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::indexBufferAllocatedBytes");
    if (blockId >= blocks_.size()) {
        return 0;
    }
    return blocks_[blockId].indexBuffer.allocatedBytes();
}

VkBuffer GeometryPool::vertexBufferHandle(uint32_t blockId) const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::vertexBufferHandle");
    if (blockId >= blocks_.size()) {
        return VK_NULL_HANDLE;
    }
    return blocks_[blockId].vertexBuffer.handle();
}

VkBuffer GeometryPool::indexBufferHandle(uint32_t blockId) const {
    RX_ASSERT_MAIN_THREAD("GeometryPool::indexBufferHandle");
    if (blockId >= blocks_.size()) {
        return VK_NULL_HANDLE;
    }
    return blocks_[blockId].indexBuffer.handle();
}

}  // namespace rx::asset
