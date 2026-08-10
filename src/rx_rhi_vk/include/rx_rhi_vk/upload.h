#pragma once
#include <rx_rhi_vk/buffer.h>
#include <cstdint>
#include <optional>
#include <utility>

namespace rx::rhi {

class Device;
class Texture2D;

// Gets CPU-side bytes into GPU-visible/device-local Vulkan resources:
// uploadToBuffer() for an arbitrary destination VkBuffer, uploadToImage()
// for a Texture2D (optionally generating its full mip chain via
// Texture2D::recordMipChainBlit() in the same submission) [spec Fixed
// decision #7, R:C1].
//
// SYNCHRONOUS, BATCHED FLUSH -- read before assuming per-call semantics:
// uploadToBuffer()/uploadToImage() only RECORD commands (a
// vkCmdCopyBuffer/vkCmdCopyBufferToImage, plus any mip-chain blit) onto
// this Uploader's own internal command buffer; nothing touches the GPU
// until flush() actually submits everything recorded so far and BLOCKS
// the calling thread on a fence wait until it completes. This lets
// several uploads share one submission (cheaper than one submit-and-wait
// per resource) while keeping this phase's API deliberately simple: a
// fully synchronous flush() is an explicitly acceptable Phase 2 choice
// [R:C1] -- "Synchronous flush API is acceptable this phase ... per-frame
// async batching is a later optimization." A future Uploader revision
// replacing this with a fence the caller polls (instead of blocking on)
// would change flush()'s contract but not uploadToBuffer()/
// uploadToImage()'s.
//
// STAGING RING BUFFER + REBAR DETECTION [R:C1]: one persistent,
// CPU-writable ring buffer (Allocator::createUploadRingBuffer(), using
// VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT) backs
// every upload. VMA tries to place it in memory that is BOTH
// DEVICE_LOCAL and HOST_VISIBLE first (ReBAR-enabled desktop GPUs, or a
// unified-memory APU like Steam Deck) for a fast on-device copy;
// hardware without that (the common non-ReBAR desktop case) transparently
// falls back to plain HOST_VISIBLE system memory, and the exact same
// vkCmdCopyBuffer/vkCmdCopyBufferToImage code path below still works
// correctly either way -- just over a slower, real PCIe-bound transfer
// instead of an on-device one. Which one was obtained is detected once at
// create() (never per-upload) and logged once via RX_LOG_INFO; see
// usesDirectPath(). Every mapped write into the ring buffer is followed
// by Buffer::flush() (this task's own Buffer API addition) before the GPU
// reads it, so this is correct even on a hypothetical non-coherent
// HOST_VISIBLE memory type, not just the coherent-in-practice hardware
// this engine has been tested against.
//
// If a single upload's write position would run past the ring buffer's
// end before the pending batch is flushed, this Uploader transparently
// calls flush() itself first (submitting + waiting on everything
// recorded so far, which frees the whole ring buffer for reuse since
// flush() is synchronous) and then continues writing from offset 0 --
// callers never need to reason about ring-buffer capacity themselves
// unless a single upload's size exceeds the ring buffer's total capacity,
// which fails cleanly (logged, returns false) rather than silently
// corrupting anything.
class Uploader {
public:
    Uploader(Uploader&&) noexcept;
    Uploader& operator=(Uploader&&) noexcept;
    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    // Auto-flushes any pending (recorded-but-not-yet-submitted) uploads
    // before tearing down the ring buffer/command pool/fence, so a
    // caller that forgets an explicit final flush() never silently loses
    // work -- see flush()'s own comment for why this is safe to do here
    // unconditionally (it blocks on the same fence wait flush() always
    // does).
    ~Uploader();

    static constexpr VkDeviceSize kDefaultRingBufferSize = 16u * 1024u * 1024u;  // 16 MiB

    // Builds this Uploader's internal command pool/buffer/fence against
    // `device`'s graphics queue, and its staging ring buffer (`usage`
    // VK_BUFFER_USAGE_TRANSFER_SRC_BIT only -- this buffer is always a
    // copy source, never a real destination) of `ringBufferSize` bytes
    // via `allocator`. Returns std::nullopt (logged) on any failure.
    static std::optional<Uploader> create(Allocator& allocator, Device& device,
                                           VkDeviceSize ringBufferSize = kDefaultRingBufferSize);

    // Copies `size` bytes from `data` into `dst` at `dstOffset`, through
    // the ring buffer, recording a vkCmdCopyBuffer onto this Uploader's
    // internal command buffer -- see the class comment for why nothing
    // actually reaches the GPU until flush(). `dst` is assumed to already
    // be sized/allocated by the caller (e.g. via
    // Allocator::createDeviceLocalBuffer()) with VK_BUFFER_USAGE_TRANSFER_DST_BIT.
    // Returns false (logged) only if `size` exceeds this Uploader's total
    // ring-buffer capacity outright -- every other case (including a
    // same-batch auto-flush to reclaim ring space) succeeds. A `size` of
    // 0 is a no-op that returns true.
    bool uploadToBuffer(VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size);

    // Copies `pixelBytes` bytes from `pixels` (tightly packed, matching
    // `dst`'s extent/format texel layout) into mip level 0 of `dst`,
    // through the ring buffer, then transitions `dst` to
    // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL -- via
    // Texture2D::recordMipChainBlit() (generating the full mip chain in
    // the same recorded batch) when `generateMips` is true AND
    // `dst.mipLevels() > 1`, or via a single whole-image layout
    // transition otherwise. As with uploadToBuffer(), nothing reaches the
    // GPU until flush(). Returns false (logged) only if `pixelBytes`
    // exceeds this Uploader's total ring-buffer capacity outright. A
    // `pixelBytes` of 0 is a no-op that returns true.
    bool uploadToImage(Texture2D& dst, const void* pixels, VkDeviceSize pixelBytes, bool generateMips);

    // Submits every uploadTo*() call recorded since the last flush() (or
    // since create()), waits (via this Uploader's own dedicated fence)
    // for the GPU to finish, then resets the ring buffer's write cursor
    // back to 0 and the command buffer for reuse. Safe to call with
    // nothing pending (no-op).
    void flush();

    // Whether the ring buffer ended up in DEVICE_LOCAL memory (the
    // ReBAR/integrated-memory fast path) as opposed to the plain
    // HOST_VISIBLE fallback -- see the class comment. Informational only;
    // does not change which Vulkan calls uploadTo*() issues.
    bool usesDirectPath() const { return ringBufferIsDeviceLocal_; }

private:
    // No plain default constructor: `ringBuffer_` is a real
    // rx::rhi::Buffer, whose own default constructor is private to Buffer
    // (friends only, see buffer.h) -- Uploader is not a friend of Buffer
    // and does not need to be. create() builds every member's real value
    // up front and constructs this Uploader directly via this
    // parameterized constructor instead (mirroring
    // rx::rhi::MeshBuffers/Texture2D's own equivalent constructors, for
    // the same reason). The move constructor/assignment below are
    // hand-written rather than `= default` for the opposite reason: this
    // class also owns several *raw* Vulkan handles (pool_/cmd_/fence_),
    // which a defaulted move would copy without nulling the source --
    // exactly the double-destroy hazard every other RAII type in this
    // library's move operations are hand-written to avoid.
    Uploader(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd, VkFence fence,
             Buffer ringBuffer, VkDeviceSize ringBufferSize, bool ringBufferIsDeviceLocal)
        : device_(device),
          queue_(queue),
          pool_(pool),
          cmd_(cmd),
          fence_(fence),
          ringBuffer_(std::move(ringBuffer)),
          ringBufferSize_(ringBufferSize),
          ringBufferIsDeviceLocal_(ringBufferIsDeviceLocal) {}

    void beginRecordingIfNeeded();
    bool reserveRingSpace(VkDeviceSize size, VkDeviceSize& outOffset);
    void destroyAll();

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;

    Buffer ringBuffer_;
    VkDeviceSize ringBufferSize_ = 0;
    VkDeviceSize ringCursor_ = 0;
    bool ringBufferIsDeviceLocal_ = false;

    bool recording_ = false;
};

}  // namespace rx::rhi
