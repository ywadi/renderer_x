#pragma once

// Thread-affinity (D5, Phase 4 Stage 1 Task 12): GeometryPool's suballocation
// state (per-block VmaVirtualBlock TLSF metadata) is NOT internally
// synchronized -- VMA's own virtual-block API documents itself as "not
// thread-safe... should not be used from multiple threads simultaneously,
// must be synchronized externally" (vendored vk_mem_alloc.h) -- so
// create()/upload()/free()/bind() are main-thread-only, matching every
// other GPU-object-mutation entry point in this codebase (BindlessTable/
// Uploader/DeletionQueue/Allocator -- see docs/threading.md).
//
// [Fix round 1, review finding] EVERY OTHER PUBLIC METHOD IS ALSO
// MAIN-THREAD-ONLY -- stats()/blockCount()/bufferDeviceAddressEnabled()/
// vertexBufferDeviceAddress()/indexBufferDeviceAddress() (and the
// test/diagnostic accessors below them) all read `blocks_` and/or call
// vmaGetVirtualBlockStatistics() against live VmaVirtualBlocks that
// upload()/free() mutate WITHOUT a lock -- VMA's own docs require
// external synchronization for reads too, and `blocks_.size()`/
// `blocks_[i]` race a concurrent `push_back()` reallocation exactly like
// any other unsynchronized std::vector. An earlier version of this
// comment claimed these were "safe from any thread holding a valid
// reference" -- that was not backed by anything (no atomic state the way
// rx::rhi::Allocator::report()/setCurrentFrameIndex() have
// MemoryAccounting's atomic counters to justify their own any-thread
// claim). Narrowed to main-thread-only, matching D5's posture for the
// whole class; every one of these now carries the same
// RX_ASSERT_MAIN_THREAD guard the mutators do, so the contract is
// enforced, not just documented. HUD/Tracy consumers of stats() already
// run on the main thread, so this costs nothing real.

#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/upload.h>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace rx::rhi {
class Device;
class Uploader;
}  // namespace rx::rhi

namespace rx::asset {

// [D8] One interleaved vertex format for ALL pooled static geometry:
// position (f32x3) | normal (f32x3) | tangent (f32x4, w = handedness) |
// uv0 (f32x2) = 48 bytes, pinned by the static_assert below. Chosen over a
// packed encoding (1010102-style normals etc.) for Phase 4 simplicity --
// packing is a recorded optimization candidate once meshlets arrive.
// Existing procedural samples keep their own layouts unchanged; this
// format serves imported content (Task 13+) and any other pooled
// geometry.
struct PoolVertex {
    float px = 0.0F, py = 0.0F, pz = 0.0F;
    float nx = 0.0F, ny = 0.0F, nz = 0.0F;
    float tx = 0.0F, ty = 0.0F, tz = 0.0F, tw = 0.0F;
    float u = 0.0F, v = 0.0F;
};
static_assert(sizeof(PoolVertex) == 48, "PoolVertex must stay exactly 48 bytes -- spec D8, pinned");
static_assert(alignof(PoolVertex) == 4, "PoolVertex must stay 4-byte aligned (no hidden padding) -- spec D8");

// A suballocated draw range within one GeometryPool block: `firstIndex`/
// `indexCount` address the block's shared index buffer, `vertexOffset` is
// added to every fetched index before it addresses the block's shared
// vertex buffer (vkCmdDrawIndexed's own `vertexOffset` semantics) --
// exactly the firstIndex/vertexOffset addressing D9/D26 commit to (the
// GPU-driven-ready layout). Both `firstIndex` and `vertexOffset` are
// ELEMENT counts, never byte offsets. GeometryPool's internal suballocator
// (geometry_pool.cpp) allocates directly in ELEMENT units (one PoolVertex
// / one uint32_t per unit, not bytes) specifically so these fields are the
// suballocator's raw returned offset with NO division step anywhere on
// the path that produces them -- see addBlock()'s "ELEMENT-UNIT virtual
// blocks" comment for why: VMA's `VmaVirtualAllocationCreateInfo::
// alignment` must be a power of two, and 48 (PoolVertex's byte stride) is
// not, so a byte-domain "alignment = 48" request (as an earlier version
// of this class attempted, matching the gate matrix's literal wording)
// was empirically verified, this task, to produce a non-48-aligned
// offset -- the element-unit design sidesteps the problem entirely rather
// than working around VMA's precondition.
struct MeshRange {
    uint32_t blockId = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t vertexOffset = 0;

    bool operator==(const MeshRange&) const = default;
};

// Default chunk sizes [D9]. new-chunk-on-exhaustion grows blocks at these
// sizes (or, when a single upload() call's own vertex/index payload
// exceeds one default chunk outright, a block sized to fit that one
// payload -- see geometry_pool.cpp's addBlock()).
inline constexpr VkDeviceSize kDefaultVertexChunkBytes = 64ull * 1024 * 1024;
inline constexpr VkDeviceSize kDefaultIndexChunkBytes = 32ull * 1024 * 1024;

struct PoolConfig {
    VkDeviceSize vertexChunkBytes = kDefaultVertexChunkBytes;
    VkDeviceSize indexChunkBytes = kDefaultIndexChunkBytes;
};

// Per-block bytes used/capacity -- the FAST vmaGetVirtualBlockStatistics()
// tier (matrix-issue21's "Stats API surface" row), cheap enough to call
// every frame; there is no per-frame detailed/string dump here (VMA's
// slower vmaCalculateVirtualBlockStatistics()/vmaBuildVirtualBlockStatsString()
// tiers are reserved for an explicit on-demand debug dump, never exercised
// by stats() itself).
struct PoolBlockStats {
    VkDeviceSize vertexBytesUsed = 0;
    VkDeviceSize vertexBytesCapacity = 0;
    VkDeviceSize indexBytesUsed = 0;
    VkDeviceSize indexBytesCapacity = 0;
};

// Aggregate pool stats. `blockCount` [gate ruling #21] feeds #27's
// unbounded-growth visibility: GeometryPool never caps growth itself, and
// a rising block count across frames is the observable signal a future
// eviction policy (streaming phase) reacts to. Tracy plots are the
// CALLER's responsibility -- stats() returns a plain snapshot, exactly
// like rx::rhi::RxMemoryReport (memory_report.h) -- see a sample's HUD
// code for where this feeds in.
struct PoolStats {
    uint32_t blockCount = 0;
    VkDeviceSize vertexBytesUsed = 0;
    VkDeviceSize vertexBytesCapacity = 0;
    VkDeviceSize indexBytesUsed = 0;
    VkDeviceSize indexBytesCapacity = 0;
    std::vector<PoolBlockStats> blocks;
};

// GeometryPool owns the engine's pooled global vertex/index storage [D9]:
// one DEVICE_LOCAL vertex buffer + one DEVICE_LOCAL index buffer per
// "block" (chunk), each pair suballocated independently via its own
// VmaVirtualBlock (VMA's CPU-side TLSF virtual allocator -- see the
// vendored vk_mem_alloc.h; NEVER
// VMA_VIRTUAL_BLOCK_CREATE_LINEAR_ALGORITHM_BIT, which is a stack/ring
// allocator unsuited to arbitrary free/reuse). A mesh's vertex range and
// index range always land in the SAME block (same blockId) -- draws bind
// one block's pair (bind()) and address both ranges with plain
// firstIndex/vertexOffset (vkCmdDrawIndexed), never a cross-block
// reference.
//
// Growth: upload() tries every existing block in creation order; the
// first one with room for BOTH the incoming vertices AND indices wins.
// If none fits, a new block is created (sized to PoolConfig's chunk
// defaults, or larger if the incoming payload alone exceeds a default
// chunk) and the suballocation is guaranteed to succeed there.
//
// SUBALLOCATION UNITS: each block's vertex/index VmaVirtualBlock is
// created and addressed in ELEMENT units (one PoolVertex / one uint32_t
// per unit), not bytes -- see geometry_pool.cpp's addBlock()/upload() for
// the full rationale. In short: VMA's `VmaVirtualAllocationCreateInfo::
// alignment` must be a power of two, and PoolVertex's 48-byte stride is
// not (48 = 16*3) -- a byte-domain "alignment = 48" request was
// empirically verified, this task, to produce a non-48-aligned offset.
// Allocating in element units instead sidesteps the problem structurally:
// every returned offset is already the exact `vertexOffset`/`firstIndex`
// value, with no division (and therefore no truncation risk) anywhere on
// the path.
//
// NO DEFRAGMENTATION [D9]: free() only releases a range back to its
// block's own TLSF metadata for future reuse (vmaVirtualFree, which
// coalesces adjacent free space) -- it never moves live data, and
// GeometryPool never compacts across blocks either. A request that
// cannot fit any single free hole -- even when the block's TOTAL free
// bytes would be enough -- takes the same new-block path as true
// exhaustion; this is the deliberate, tested absence of compaction, not
// an oversight (see geometry_pool_test.cpp's checkerboard fragmentation
// test).
class GeometryPool {
public:
    GeometryPool(const GeometryPool&) = delete;
    GeometryPool& operator=(const GeometryPool&) = delete;
    GeometryPool(GeometryPool&&) = delete;
    GeometryPool& operator=(GeometryPool&&) = delete;
    ~GeometryPool();

    // Builds an empty pool (zero blocks -- the first upload() creates the
    // first block on demand, per the growth policy above).
    //
    // `allocator`/`uploader` are stored BY REFERENCE: this GeometryPool
    // must not outlive either (nor the rx::rhi::Device/Context beneath
    // them) -- the same lifetime discipline every other Allocator-derived
    // RAII type in this codebase already follows (see rx_rhi_vk/buffer.h's
    // own comment on Buffer). Chunk buffers are allocated through
    // `allocator` via its existing createDeviceLocalBuffer() choke point,
    // tagged rx::rhi::MemoryCategory::GeometryPool [D24(a)] -- this is
    // what lets a chunk allocation show up in the SAME accounting report
    // (Allocator::report()) the rest of the engine already reads, rather
    // than a private, invisible ledger.
    //
    // `device` is consulted only during this call, to resolve
    // bufferDeviceAddressEnabled() below [D26.4] via
    // rx::rhi::Device::supportsBufferDeviceAddress() -- its VkDevice
    // handle is cached, not the C++ reference.
    //
    // Deviation from the plan's illustrative interface sketch
    // (`create(rhi::Device&, rhi::Uploader&, const PoolConfig&)`): this
    // signature adds `rx::rhi::Allocator&` as an explicit parameter. This
    // is necessary, not incidental -- the #27 accounting-integration
    // acceptance criterion requires chunk buffers to land in the SAME
    // MemoryAccounting ledger the rest of the engine reads, which only
    // holds if GeometryPool is handed the caller's real Allocator rather
    // than constructing (or being handed) a private one of its own; see
    // the task report for the full rationale.
    //
    // Always succeeds today (returns a non-null pointer) -- kept
    // std::unique_ptr, matching the plan's own interface, for forward
    // compatibility with a future fallible construction step.
    static std::unique_ptr<GeometryPool> create(rx::rhi::Allocator& allocator, rx::rhi::Device& device,
                                                  rx::rhi::Uploader& uploader,
                                                  const PoolConfig& config = PoolConfig{});

    // Suballocates `vertices.size()` PoolVertex elements and
    // `indices.size()` uint32_t elements from the first block with room
    // for both (or a fresh block, per the growth policy), uploads both
    // spans through this pool's Uploader (consuming its UploadTicket path
    // [D25] -- never assumes the old synchronous-flush contract), and
    // returns the resulting MeshRange.
    //
    // Per D25's documented sync convenience, this call does exactly ONE
    // flush()+wait() for its own batch before returning -- both spans
    // share that one flush, never a separate wait per suballocation --
    // mirroring rx::rhi::MeshBuffers::create()'s identical "convenience
    // wrapper" contract, so the range this call returns is always
    // immediately safe to bind/draw.
    //
    // `vertices` and `indices` must both be non-empty. Returns a
    // default-constructed (blockId/firstIndex/indexCount/vertexOffset all
    // 0) MeshRange, logged, on any allocation/upload failure -- callers
    // that need to distinguish that from a legitimate empty-range result
    // should check `indices.empty()` themselves first, since GeometryPool
    // never returns a zero-indexCount range for a non-empty request.
    //
    // Main-thread-only (D5).
    MeshRange upload(std::span<const PoolVertex> vertices, std::span<const uint32_t> indices);

    // [Phase 4 Stage 1 Task 15, spec D25/RC6] Non-blocking sibling of
    // upload() above: suballocates and RECORDS the copy identically (same
    // shared private helper -- no forked logic), but does NOT call
    // flush()/wait() itself. The returned MeshRange's offsets are valid
    // immediately (suballocation is a pure CPU-side VMA-virtual-block
    // operation with no GPU dependency), but the underlying bytes are only
    // guaranteed present on the GPU once the caller has separately called
    // flushPendingUploads() and observed the returned ticket complete (via
    // isUploadComplete() or this pool's own Uploader) -- exists so the
    // async import pipeline's main-thread marshal step can record work
    // without ever blocking (D25's own "poll, never a blocking wait"
    // invariant; the async path must never call upload()'s own wait()).
    // Same failure contract as upload() (a default MeshRange, logged).
    //
    // Main-thread-only (D5).
    MeshRange uploadDeferred(std::span<const PoolVertex> vertices, std::span<const uint32_t> indices);

    // [Task 15] Forwards to this pool's own Uploader::flush() -- submits
    // every uploadDeferred() batch recorded since the last flush and
    // returns a pollable UploadTicket (D25). A caller that only ever uses
    // upload() (never uploadDeferred()) has no need to call this directly.
    //
    // Main-thread-only (D5).
    rx::rhi::UploadTicket flushPendingUploads();

    // [Task 15] Forwards to this pool's own Uploader::isComplete() -- pure
    // poll, never blocks. Pairs with flushPendingUploads()/
    // uploadDeferred() above for the async, never-wait() path.
    //
    // Main-thread-only (D5).
    [[nodiscard]] bool isUploadComplete(rx::rhi::UploadTicket ticket) const;

    // [Fix round 1] Test/diagnostic-only: how many times THIS pool has
    // ever called Uploader::wait() -- i.e. how many times upload() (the
    // blocking convenience, never uploadDeferred()) has run. The async
    // import pipeline exclusively uses uploadDeferred(), so this counter
    // staying at its pre-import baseline throughout an async import is a
    // DIRECT proof of the "wait-calls-from-async-path == 0" invariant
    // (matrix-issue22), not merely inferred from wall-clock timing.
    //
    // Main-thread-only (D5).
    [[nodiscard]] size_t waitCallCountForTesting() const;

    // Releases `range`'s vertex and index suballocations back to their
    // block's TLSF metadata (vmaVirtualFree) for future reuse -- no
    // defragmentation, no data movement (D9). `range` must be a value
    // this GeometryPool's own upload() previously returned and that has
    // not already been freed -- a double-free or an unrecognized range is
    // a logged error, not UB.
    //
    // Main-thread-only (D5).
    void free(const MeshRange& range);

    // Binds `blockId`'s vertex buffer (binding 0, offset 0) and index
    // buffer (offset 0, VK_INDEX_TYPE_UINT32 -- D9/gate ruling #21's
    // u32-only policy) onto `cmd`. Every vkCmdDrawIndexed call recorded
    // afterward (until the next bind()) must use a MeshRange whose
    // blockId matches.
    //
    // Main-thread-only (D5) -- like every other GPU-object-mutation entry
    // point in this class: this method only ever reads this pool's own
    // bookkeeping state (its Buffer handles), never a worker-recorded
    // command buffer's contents, but the guard matches every sibling
    // entry point's identical scoping per this project's own convention.
    void bind(VkCommandBuffer cmd, uint32_t blockId) const;

    // Snapshot of bytes used/capacity (vertex + index, summed across
    // every block) plus block COUNT [gate ruling #21] and a per-block
    // breakdown. Cheap: calls VMA's FAST vmaGetVirtualBlockStatistics()
    // tier per block, never the slow detailed/string tiers.
    //
    // Main-thread-only (D5) [fix round 1] -- vmaGetVirtualBlockStatistics()
    // reads the SAME live VmaVirtualBlock upload()/free() mutate without a
    // lock (VMA requires external synchronization for this call too, not
    // just the mutating ones), so this is not safe from another thread
    // despite being a "read". See this header's own top-of-file comment.
    PoolStats stats() const;

    // Main-thread-only (D5) [fix round 1] -- reads blocks_.size(), which
    // races a concurrent upload()-triggered push_back() reallocation.
    uint32_t blockCount() const;

    // [D26.4] True iff this pool's chunk buffers were created with
    // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT and this pool's Allocator
    // was built with VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT --
    // resolved once at create() from rx::rhi::Device::
    // supportsBufferDeviceAddress(). Enablement only: nothing in Phase 4
    // consumes the addresses below.
    //
    // Main-thread-only (D5) [fix round 1] -- this specific field is set
    // once at construction and never mutated again, so reading it alone
    // would in fact be safe from any thread; guarded anyway for a single
    // uniform contract across every accessor in this class (D5's own
    // posture for the whole class, per the coordinator's fix-round
    // ruling), rather than a field-by-field carve-out a caller has to
    // remember.
    bool bufferDeviceAddressEnabled() const;

    // [D26.4] vkGetBufferDeviceAddress() for `blockId`'s vertex/index
    // buffer -- always 0 when bufferDeviceAddressEnabled() is false (no
    // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT usage bit was ever
    // requested on a block buffer in that case, so querying one would be
    // a validation error, not merely a meaningless value; these two
    // methods simply never call the Vulkan function at all when
    // disabled).
    //
    // Main-thread-only (D5) [fix round 1] -- indexes into blocks_, same
    // hazard as blockCount() above.
    VkDeviceAddress vertexBufferDeviceAddress(uint32_t blockId) const;
    VkDeviceAddress indexBufferDeviceAddress(uint32_t blockId) const;

    // Test/diagnostic accessors -- production code has no need for them.
    // The REAL VMA-reported allocated bytes for `blockId`'s vertex/index
    // chunk buffer (rx::rhi::Buffer::allocatedBytes()), which may exceed
    // the nominal chunk size requested (PoolConfig::vertexChunkBytes/
    // indexChunkBytes, or whatever addBlock() sized this specific block
    // to) if VMA pads for alignment/block granularity -- this is what the
    // #27 accounting-integration test compares Allocator::report()'s
    // delta against, since MemoryAccounting records the REAL allocated
    // size, not the nominal request (see rx_rhi_vk/memory_report.h's own
    // comment on Buffer::allocatedBytes()).
    //
    // Main-thread-only (D5) [fix round 1] -- indexes into blocks_, same
    // hazard as blockCount() above; every doctest caller already runs
    // single-threaded on the main thread, so this costs the tests nothing.
    VkDeviceSize vertexBufferAllocatedBytes(uint32_t blockId) const;
    VkDeviceSize indexBufferAllocatedBytes(uint32_t blockId) const;

    // Test/diagnostic accessors -- production code has no need for them
    // (bind() is the sanctioned way to attach a block's buffers to a
    // command buffer for drawing). Raw handles for `blockId`'s vertex/
    // index chunk buffer, so a test can read back their content directly
    // (vkCmdCopyBuffer) to prove upload()'s returned MeshRange offsets are
    // byte-exact -- see geometry_pool_test.cpp. VK_NULL_HANDLE if
    // `blockId` is out of range.
    //
    // Main-thread-only (D5) [fix round 1] -- indexes into blocks_, same
    // hazard as blockCount() above.
    VkBuffer vertexBufferHandle(uint32_t blockId) const;
    VkBuffer indexBufferHandle(uint32_t blockId) const;

private:
    // One chunk: a device-local vertex+index Buffer pair plus their own
    // independent VmaVirtualBlock CPU-side suballocators (ELEMENT-offset
    // space -- one PoolVertex / one uint32_t per unit, NOT bytes, and NOT
    // tied to the Buffer's own VMA memory allocation -- see the class
    // comment's "SUBALLOCATION UNITS" paragraph). `*Allocations` maps a
    // live suballocation's ELEMENT offset to the VmaVirtualAllocation
    // handle vmaVirtualFree() needs -- MeshRange itself carries no such
    // handle (it is a plain, small, copyable POD per the plan's own
    // interface), so free() looks it up here directly by MeshRange's own
    // firstIndex/vertexOffset fields (already element offsets, no
    // conversion).
    struct Block {
        rx::rhi::Buffer vertexBuffer;
        rx::rhi::Buffer indexBuffer;
        VmaVirtualBlock vertexVirtualBlock = VK_NULL_HANDLE;
        VmaVirtualBlock indexVirtualBlock = VK_NULL_HANDLE;
        VkDeviceSize vertexCapacityBytes = 0;
        VkDeviceSize indexCapacityBytes = 0;
        VkDeviceAddress vertexBufferAddress = 0;
        VkDeviceAddress indexBufferAddress = 0;
        std::unordered_map<VkDeviceSize, VmaVirtualAllocation> vertexAllocations;
        std::unordered_map<VkDeviceSize, VmaVirtualAllocation> indexAllocations;
    };

    GeometryPool(rx::rhi::Allocator& allocator, rx::rhi::Uploader& uploader, VkDevice device,
                 bool bufferDeviceAddressEnabled, PoolConfig config);

    // [Task 15] Shared suballocation + record-only tail for both upload()
    // and uploadDeferred() above -- literally the same code either way
    // (no forked logic); the two public entry points differ ONLY in
    // whether they also flush()+wait() afterward.
    MeshRange suballocateAndRecord(std::span<const PoolVertex> vertices, std::span<const uint32_t> indices);

    // Creates a new block sized to max(config_.*ChunkBytes, minimum
    // required bytes) and appends it to blocks_. Returns false (logged)
    // on any allocation failure -- see geometry_pool.cpp for the exact
    // call sequence (createDeviceLocalBuffer with
    // MemoryCategory::GeometryPool [D24(a)] + the BDA usage bit when
    // enabled, then vmaCreateVirtualBlock for both buffers).
    bool addBlock(VkDeviceSize minVertexBytes, VkDeviceSize minIndexBytes);

    // [Fix round 1, review minor] Destroys blocks_.back()'s two
    // VmaVirtualBlocks (both, unconditionally) and pops it off blocks_.
    // Both call sites in upload() only ever reach this with ZERO live
    // suballocations remaining in either virtual block (any suballocation
    // that DID succeed on this block before the failure is freed by the
    // caller first) -- so vmaDestroyVirtualBlock() is safe here directly,
    // with no vmaClearVirtualBlock() needed first. Cleans up the
    // just-addBlock()'d block on the "should be unreachable" path where a
    // fresh block sized to fit the exact request still fails a
    // suballocation, so that defensive path leaves no orphaned block
    // (and, via ~Buffer(), no orphaned device-local memory) behind.
    void discardOrphanedLastBlock();

    void destroyAll();

    rx::rhi::Allocator& allocator_;
    rx::rhi::Uploader& uploader_;
    VkDevice device_ = VK_NULL_HANDLE;
    bool bufferDeviceAddressEnabled_ = false;
    PoolConfig config_;
    std::vector<Block> blocks_;
    size_t waitCallCountForTesting_ = 0;
};

}  // namespace rx::asset
