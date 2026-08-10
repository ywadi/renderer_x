#pragma once
#include <volk.h>
#include <rx_core/handle.h>
#include <cstdint>
#include <optional>

namespace rx::rhi {

// Which of BindlessTable's three resource-class arrays a BindlessHandle
// indexes into. Needed because register*()/release() share one handle type
// (per the interface this component is built to) even though each resource
// class keeps its own independent index/generation sequence internally
// (see BindlessTable's private rx::core::HandlePool members) -- without
// this tag, release() would have no way to know which pool a given
// (index, generation) pair belongs to.
enum class BindlessResourceKind : uint8_t {
    SampledImage,
    Sampler,
    StorageBuffer,
};

namespace detail {

// Trivial per-resource-kind tags handed to rx::core::HandlePool/Handle so
// each resource class gets its own independent generation sequence (a
// sampled-image slot 3/generation 2 and a storage-buffer slot 3/generation
// 2 must never compare equal or be confused with each other). No payload
// data is actually needed per slot -- BindlessTable writes straight
// through to the GPU-visible VkDescriptorSet via vkUpdateDescriptorSets,
// so HandlePool here is used purely for its generational index-allocation
// bookkeeping, not as storage for the registered resource itself.
struct SampledImageSlotTag {};
struct SamplerSlotTag {};
struct StorageBufferSlotTag {};
struct EmptyPayload {};

}  // namespace detail

// A generational handle into one of BindlessTable's three bindless arrays.
// `index()` is the shader-visible array index -- the value to embed in a
// push constant / indirection buffer for `ResourceDescriptorHeap`-style
// indexing in the shader. `generation()` distinguishes a live registration
// from a stale handle pointing at an already-released-and-reused slot,
// mirroring rx::core::Handle<Tag>'s own scheme (which this type wraps
// internally, once per resource kind -- see BindlessTable).
//
// Default-constructed handles are invalid (`isValid() == false`,
// `generation() == 0`); every handle actually returned by a successful
// register*() call has `generation() >= 1`. register*() also returns an
// invalid handle (logging an error) if the requested resource class's
// capacity is already exhausted -- callers must check `isValid()` before
// using a handle's index for anything.
class BindlessHandle {
public:
    BindlessHandle() = default;

    uint32_t index() const { return index_; }
    uint32_t generation() const { return generation_; }
    BindlessResourceKind kind() const { return kind_; }
    bool isValid() const { return generation_ != 0; }

    bool operator==(const BindlessHandle& other) const {
        return kind_ == other.kind_ && index_ == other.index_ && generation_ == other.generation_;
    }
    bool operator!=(const BindlessHandle& other) const { return !(*this == other); }

private:
    friend class BindlessTable;

    BindlessHandle(BindlessResourceKind kind, uint32_t index, uint32_t generation)
        : kind_(kind), index_(index), generation_(generation) {}

    BindlessResourceKind kind_ = BindlessResourceKind::SampledImage;
    uint32_t index_ = 0;
    uint32_t generation_ = 0;
};

// BindlessTable -- the engine's single global bindless descriptor set
// (set 0), per the spec's Fixed decision #5 [R:B3]: one
// VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT set layout,
// one VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT pool, one allocated
// VkDescriptorSet, with three runtime-array bindings:
//   binding 0 (kSampledImageBinding)  -- VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
//   binding 1 (kSamplerBinding)       -- VK_DESCRIPTOR_TYPE_SAMPLER
//   binding 2 (kStorageBufferBinding) -- VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
// Every binding carries VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
// VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; binding 2, being the LAST
// binding in the set (the only position Vulkan allows this flag), also
// carries VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT, and the
// descriptor set is allocated with exactly `capacities.storageBuffers`
// actual descriptors for that binding via
// VkDescriptorSetVariableDescriptorCountAllocateInfo.
//
// This is a separate, self-owned set layout from PipelineLayoutBuilder's
// reflection-driven ones (Task 2) -- BindlessTable owns set 0's actual
// pool/set here; a shader's own reflected set-0 layout must match this
// table's layout by construction (same binding numbers/types/flags) for a
// pipeline built from that reflection to be usable with this table's
// VkDescriptorSet, which is what a later sample proves end to end.
//
// ---------------------------------------------------------------------
// RELEASE-SAFETY CONTRACT -- read before calling release():
// ---------------------------------------------------------------------
// release() only returns a slot's (index, generation) pair to this
// table's internal free list for a FUTURE register*() call to reuse -- it
// never itself issues a vkUpdateDescriptorSets call, and it never touches
// (let alone destroys) the underlying VkImageView/VkSampler/VkBuffer that
// slot was last written with. A released slot's descriptor binding keeps
// whatever it was last written with, completely unchanged, until
// something re-registers into that same index.
//
// This table only ever stores an index -> descriptor mapping; it never
// takes ownership of the VkImageView/VkSampler/VkBuffer handles passed to
// register*(). UPDATE_AFTER_BIND + PARTIALLY_BOUND together make it valid
// to rewrite a descriptor in a set that is already bound by a command
// buffer the driver has not finished executing -- ONLY for a descriptor
// slot NOT dynamically used by that pending command buffer. (Per the
// Vulkan spec's update-after-bind rules: a descriptor updated via
// UPDATE_AFTER_BIND must not be dynamically used by any command in a
// submission that has not yet completed execution, unless the update is
// one the spec separately classifies as unobservable to that command --
// PARTIALLY_BOUND/UPDATE_UNUSED_WHILE_PENDING relax "an unwritten/unused
// slot doesn't need a valid descriptor for a shader invocation that never
// actually indexes it"; neither relaxes rewriting a slot a still-pending
// command buffer DOES dynamically index.) Concretely: release()-then-
// immediately-register() into that same freed index (this table's free
// list is LIFO, so this is the deterministic case, not a rare one) is a
// spec violation, invisible to validation, if any command buffer
// recorded before the release might still be executing a draw that
// dynamically samples that exact slot. Callers doing release-then-
// re-register under a frames-in-flight loop must defer the REWRITE
// itself -- not just the eventual destruction of the old resource -- to
// a point they know no such pending command buffer exists; the same
// fence-confirmed point DeletionQueue already exists to provide is the
// natural mechanism (see samples/04_streaming for the worked example: it
// defers register*() itself into a DeletionQueue-retired callback, using
// the identical frame-fence discipline it uses for the destruction
// below).
//
// Separately: neither flag makes it safe to destroy the GPU resource a
// slot's descriptor still points at while a command buffer that might
// read that slot could still be in flight. Guaranteeing that (deferring
// the real resource destruction until the owning frame's fence has
// signaled) is explicitly out of scope for this type -- it is Task 4's
// DeletionQueue's job. Callers must not destroy a registered resource,
// nor treat release() as a synchronization point for it, until they know
// (via DeletionQueue or an equivalent fence wait) that no in-flight frame
// can still reference that slot.
class BindlessTable {
public:
    struct Capacities {
        uint32_t sampledImages = 0;
        uint32_t samplers = 0;
        uint32_t storageBuffers = 0;
    };

    static constexpr uint32_t kSampledImageBinding = 0;
    static constexpr uint32_t kSamplerBinding = 1;
    static constexpr uint32_t kStorageBufferBinding = 2;

    BindlessTable(BindlessTable&&) noexcept;
    BindlessTable& operator=(BindlessTable&&) noexcept;
    BindlessTable(const BindlessTable&) = delete;
    BindlessTable& operator=(const BindlessTable&) = delete;
    ~BindlessTable();

    // Builds the set layout + update-after-bind pool + descriptor set
    // described above.
    //
    // `physicalDevice` is used ONLY to defensively pre-check each
    // requested capacity against this device's real
    // maxDescriptorSetUpdateAfterBind{SampledImages,Samplers,
    // StorageBuffers} limits (queried via vkGetPhysicalDeviceProperties2's
    // VkPhysicalDeviceVulkan12Properties) before issuing a single Vulkan
    // call -- exceeding one is rejected here as a clean, logged
    // std::nullopt, never left to whatever a driver/validation layer does
    // when handed a descriptor set layout/pool request beyond its own
    // advertised limit. This is a deliberate elaboration of the
    // VkDevice-only shorthand in this component's brief: the capacity
    // failure path this class is required to support cannot be
    // implemented as "clean error, no crash" without a physical-device
    // handle to check limits against (see task-3-report.md).
    //
    // Also rejects (nullopt, logged) if any capacity is 0 -- a
    // VkDescriptorPoolSize/VkDescriptorSetLayoutBinding with
    // descriptorCount == 0 is not a valid way to express "this resource
    // class is unused" for this table's fixed three-binding layout.
    static std::optional<BindlessTable> create(VkPhysicalDevice physicalDevice, VkDevice device,
                                                 Capacities capacities);

    VkDescriptorSetLayout descriptorSetLayout() const { return setLayout_; }
    VkDescriptorSet descriptorSet() const { return set_; }

    // Registers `view` (already expected to be in `layout`, e.g.
    // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) at a free index in
    // binding 0 and writes it immediately via vkUpdateDescriptorSets.
    // Returns an invalid handle (and logs an error) if this table's
    // sampled-image capacity is already fully occupied.
    BindlessHandle registerSampledImage(VkImageView view, VkImageLayout layout);

    // Registers `sampler` at a free index in binding 1, written
    // immediately. Returns an invalid handle (and logs an error) if this
    // table's sampler capacity is already fully occupied.
    BindlessHandle registerSampler(VkSampler sampler);

    // Registers `range` bytes of `buffer` starting at `offset` at a free
    // index in binding 2, written immediately. Returns an invalid handle
    // (and logs an error) if this table's storage-buffer capacity is
    // already fully occupied.
    BindlessHandle registerStorageBuffer(VkBuffer buffer, VkDeviceSize range, VkDeviceSize offset = 0);

    // Returns `handle`'s slot to the relevant resource class's free list
    // for future reuse. See the RELEASE-SAFETY CONTRACT above -- this does
    // NOT touch the descriptor's current GPU-visible contents, and does
    // NOT destroy or otherwise affect the resource `handle` was registered
    // with. A no-op (logs nothing) if `handle` is invalid or already
    // stale/released.
    void release(BindlessHandle handle);

private:
    BindlessTable() = default;

    void destroyAll();

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    Capacities capacities_;

    rx::core::HandlePool<detail::SampledImageSlotTag, detail::EmptyPayload> sampledImages_;
    rx::core::HandlePool<detail::SamplerSlotTag, detail::EmptyPayload> samplers_;
    rx::core::HandlePool<detail::StorageBufferSlotTag, detail::EmptyPayload> storageBuffers_;
};

}  // namespace rx::rhi
