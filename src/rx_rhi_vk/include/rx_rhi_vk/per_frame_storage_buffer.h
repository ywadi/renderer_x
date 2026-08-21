#pragma once
// rx_rhi_vk/per_frame_storage_buffer.h -- rx::rhi::PerFrameStorageBuffer, the
// N-buffered (frames-in-flight), bindless-registered, host-visible storage
// buffer pattern [Phase 5 Task 5, ticket #41 row 2; promoted from
// samples/08_gltf_viewer's and samples/09_scene's own independently
// hand-rolled `drawDataBuffers[]`/`drawDataBufferHandles[]` fields -- 09
// duplicated the SAME pattern a second time internally, for its own
// `shadowDrawDataBuffers[]`].
//
// WHY THIS LIVES HERE, IN rx_rhi_vk: this class composes ONLY rx_rhi_vk
// types (Buffer, BindlessTable's handle type, FrameSync::kFramesInFlight)
// -- it has zero scene-specific content (it never names
// `rx::material::DrawDataGpu` or any other payload shape; a caller supplies
// raw bytes). `FrameSync` -- the pattern's natural sibling -- already lives
// at this layer; housing an RHI-primitive composition in `rx_scene` because
// its first two real call sites happen to be scene-shaped would put the
// idiom at the wrong layer (matrix-p5t05 Open Questions #2).
//
// THE PATTERN, restated from the fix this promotes (Phase 4 exit-review
// I1): a single buffer rewritten every frame races frame N-1's still-
// possibly-in-flight GPU reads of it -- a caller's own
// `vkWaitForFences(currentFence)` only proves frame N-2 (the frame-in-flight
// SLOT's own last user) is done; frame N-1 always used the OTHER slot. ONE
// physical buffer PER frame-in-flight slot, written only into the CURRENT
// slot after that slot's own fence wait, is what makes that proof
// sufficient -- mirroring `rx::material::MaterialSystem`'s own `ParamArena`
// frames-in-flight discipline.
//
// Thread-affinity (D5): create()/write()/release() are main-thread-only,
// matching every other bindless-registering rx_rhi_vk type in this
// codebase (docs/threading.md) -- this class owns no synchronization of
// its own.
#include <volk.h>

#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/frame_sync.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace rx::rhi {

class PerFrameStorageBuffer;

namespace detail {

// Test-only seam -- NOT part of the stable public contract, mirroring
// `rx::material::detail::debugFrameBufferData()`'s own carve-out
// convention (instance.h) for the identical reason: a revert-
// discrimination test needs to read back the RAW bytes `write()` wrote
// into a given frame-in-flight slot's own physical buffer, to prove
// cross-slot isolation directly (I1's own regression class -- a single
// shared buffer reintroduced here would let one slot's write silently
// clobber another's) -- there is no other way to observe this from
// outside this class's own implementation. Returns nullptr if `slot` is
// out of range.
[[nodiscard]] const void* debugSlotBufferData(const PerFrameStorageBuffer& buffer, uint32_t slot);

}  // namespace detail

class PerFrameStorageBuffer {
public:
    PerFrameStorageBuffer(PerFrameStorageBuffer&&) noexcept = default;
    PerFrameStorageBuffer& operator=(PerFrameStorageBuffer&&) noexcept = default;
    PerFrameStorageBuffer(const PerFrameStorageBuffer&) = delete;
    PerFrameStorageBuffer& operator=(const PerFrameStorageBuffer&) = delete;

    // TEARDOWN CONTRACT (read before this object is destroyed/reset):
    // release() below MUST be called first -- exactly like every other
    // bindless-registering type in this codebase (e.g.
    // `MaterialSystem::releaseTexture()`), returning each slot's
    // `BindlessTable` registration to its owning table's free list is a
    // caller-driven, synchronization-aware operation
    // (`BindlessTable::release()`'s own RELEASE-SAFETY CONTRACT: safe only
    // once no pending command buffer can still dynamically index this
    // slot) this destructor cannot safely perform on its own -- it holds
    // no `BindlessTable&` at all (see this header's own top comment: this
    // class stores `BindlessHandle` VALUES, not a table reference). This
    // destructor only destroys the `Buffer`s themselves (via their own
    // RAII), which is always safe once the caller has already brought the
    // device to a real idle point, the same discipline every other
    // GPU-object-owning type in this codebase documents.
    ~PerFrameStorageBuffer() = default;

    // Allocates `framesInFlight` independent host-visible storage buffers
    // (`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`, `bytesPerSlot` bytes each) and
    // registers each one into `bindless` as a bindless storage buffer.
    // `initialData`, if non-null, is memcpy'd + flushed into EVERY slot at
    // creation (the same "prime every slot identically before ever being
    // read by a real draw" precedent both promoted call sites already
    // established) -- exactly `bytesPerSlot` bytes are read from it.
    // Returns std::nullopt (logged) if `bytesPerSlot == 0`, if
    // `framesInFlight == 0`, or if any slot's buffer allocation or
    // bindless registration fails (every slot successfully created before
    // the failure is torn down/released before returning, so a caller
    // checking `has_value()` never leaks one).
    static std::optional<PerFrameStorageBuffer> create(Allocator& allocator, BindlessTable& bindless,
                                                          VkDeviceSize bytesPerSlot,
                                                          const void* initialData = nullptr,
                                                          uint32_t framesInFlight = FrameSync::kFramesInFlight);

    // Writes `bytes` (<= the `bytesPerSlot` passed to create()) into slot
    // `frameSlot`'s own physical buffer and flushes the whole buffer
    // (`Buffer::flush()`'s own default range -- correct and cheap even on
    // an already-coherent memory type, matching every other host-visible
    // write in this codebase). CALLER CONTRACT [Phase 4 exit-review I1]:
    // only call once the caller has confirmed, via its own fence wait,
    // that frame-in-flight slot `frameSlot`'s prior GPU work (the last
    // frame that used THIS SAME physical slot) is complete -- this class
    // performs no synchronization of its own. Returns false (logged,
    // memcpy skipped) if `frameSlot >= framesInFlight()` or `bytes` exceeds
    // this instance's own `bytesPerSlot`.
    bool write(uint32_t frameSlot, const void* data, VkDeviceSize bytes);

    // Bindless STORAGE_BUFFER index for slot `frameSlot` -- the value a
    // draw's own push-constant/globals-shaped field embeds to address this
    // slot's buffer from a shader. Returns an invalid (index() == 0,
    // generation-carrying) handle's index only if `frameSlot` is
    // out-of-range; every slot successfully created by create() always has
    // a valid handle.
    [[nodiscard]] uint32_t bindlessIndex(uint32_t frameSlot) const;

    [[nodiscard]] uint32_t framesInFlight() const { return static_cast<uint32_t>(buffers_.size()); }

    // Releases every slot's BindlessTable registration -- see this class's
    // own TEARDOWN CONTRACT above; MUST be called before this object is
    // destroyed/reset. Idempotent: a slot whose handle is already invalid
    // (either never valid, or already released by a prior call) is
    // skipped, so calling this more than once is safe and a no-op the
    // second time.
    void release(BindlessTable& bindless);

private:
    friend const void* detail::debugSlotBufferData(const PerFrameStorageBuffer&, uint32_t);

    PerFrameStorageBuffer() = default;

    std::vector<std::optional<Buffer>> buffers_;
    std::vector<BindlessHandle> handles_;
    VkDeviceSize bytesPerSlot_ = 0;
};

}  // namespace rx::rhi
