#pragma once
#include <volk.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace rx::rhi {

// Per-frame-in-flight pool sizing for DescriptorArena -- generous for Phase
// 3's material-instance binding load (one VkDescriptorSet + one
// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER descriptor per bindInstance() call,
// rx_material's own only consumer of this class): 512 sets / 512 UBO
// descriptors per frame-in-flight slot -- see rx_material's
// kMaxInstancesPerFrame (instance.h) for the documented Phase 3 usage
// ceiling this is sized against. A caller that exhausts this in one frame
// gets a clean, logged VK_NULL_HANDLE from DescriptorArena::allocate()
// rather than a driver-dependent failure.
//
// Declared at namespace scope, not nested inside DescriptorArena itself,
// despite being used nowhere else -- a nested class with default member
// initializers cannot be used (via `= {}`) as the default argument of an
// enclosing class's OWN member function declared inside that same class
// body (verified directly: both this project's zig-cross clang and its
// native compiler reject "default member initializer ... needed within
// definition of enclosing class ... outside of member functions" for
// exactly that nesting). Hoisting it out is the minimal fix -- the
// ergonomic `DescriptorArena::Capacities` call-site spelling is preserved
// below via a plain alias.
struct DescriptorArenaCapacities {
    uint32_t maxSets = 512;
    uint32_t uniformBuffers = 512;
};

// DescriptorArena -- a reusable, frames-in-flight-scoped VkDescriptorSet
// allocator [Phase 3 Task 7 brief]: one VkDescriptorPool per frame-in-flight
// slot, reset (never individually freed) via beginFrame(), and a single
// allocate(VkDescriptorSetLayout) that hands out one VkDescriptorSet from
// whichever slot beginFrame() most recently reset. This is the GPU-object
// counterpart to rx_material's own per-frame CPU-side parameter arena
// (src/rx_material/include/rx_material/instance.h's ParamArena): every
// material instance bound in a frame gets one fresh VkDescriptorSet from
// THIS arena and one fresh byte range from THAT one, and both are reclaimed
// together the next time this same frame-in-flight slot comes back around.
//
// DEVICE-FREE-WHERE-POSSIBLE: create() needs only a VkDevice -- no
// VkPhysicalDevice, no VkInstance, no window/surface -- unlike
// rx::rhi::BindlessTable, which additionally needs a VkPhysicalDevice to
// pre-check update-after-bind limits (bindless.h). This arena's pools are
// plain, non-update-after-bind pools with no such per-device ceiling to
// check, so there is nothing else this class needs beyond the device
// itself.
//
// RESET, NOT FREE -- every pool this class owns is created WITHOUT
// VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT: beginFrame() calls
// vkResetDescriptorPool(), which implicitly invalidates every
// VkDescriptorSet previously allocated from that slot's pool in one call --
// there is no per-set free() on this class, mirroring
// rx::rhi::BindlessTable's own "destroying the pool implicitly frees
// everything" precedent (bindless.cpp's destroyPoolAndLayout() comment)
// applied to reset instead of destroy.
//
// SAFETY CONTRACT -- read before calling beginFrame(): exactly the same
// contract rx::rhi::FrameSync's own per-frame-in-flight objects document
// (frame_sync.h) and rx::rhi::DeletionQueue's own "pure bookkeeping, caller
// drives real synchronization" split (deletion_queue.h): beginFrame(N) is
// only safe to call once the caller has confirmed (via its own fence wait --
// e.g. vkWaitForFences on whatever FrameSync-equivalent fence covers slot
// `N % framesInFlight()`) that no in-flight command buffer can still be
// using a descriptor set this reset is about to invalidate. This class owns
// no VkFence itself and never checks this on the caller's behalf.
class DescriptorArena {
public:
    using Capacities = DescriptorArenaCapacities;

    DescriptorArena(DescriptorArena&&) noexcept;
    DescriptorArena& operator=(DescriptorArena&&) noexcept;
    DescriptorArena(const DescriptorArena&) = delete;
    DescriptorArena& operator=(const DescriptorArena&) = delete;
    ~DescriptorArena();

    // Builds `framesInFlight` independent VkDescriptorPools (each sized per
    // `capacities`) -- kept as a runtime parameter rather than a hardcoded
    // constant so this stays a genuinely reusable RHI piece, not one
    // hardwired to any one caller's own frame count (every real caller in
    // this codebase passes rx::rhi::FrameSync::kFramesInFlight, currently
    // 2, but this class does not itself depend on frame_sync.h). Returns
    // std::nullopt (logged) if `framesInFlight == 0`, if either capacity
    // field is 0, or on any vkCreateDescriptorPool failure (cleaning up
    // every pool already created in that same call first).
    static std::optional<DescriptorArena> create(VkDevice device, uint32_t framesInFlight,
                                                  Capacities capacities = {});

    // Resets frame slot `frameIndex % framesInFlight()`'s pool
    // (vkResetDescriptorPool) -- see the class-level SAFETY CONTRACT above.
    // Every VkDescriptorSet previously allocated from that slot becomes
    // invalid the moment this returns. Logs (RX_LOG_ERROR) and otherwise
    // no-ops on the rare underlying VkResult failure.
    void beginFrame(uint32_t frameIndex);

    // Allocates one VkDescriptorSet of `layout` from the CURRENT slot's
    // pool (whichever beginFrame() most recently reset -- slot 0 if
    // allocate() is called before the first beginFrame()). Returns
    // VK_NULL_HANDLE (logged) if that slot's pool cannot satisfy the
    // request (e.g. maxSets already exhausted this reset cycle).
    [[nodiscard]] VkDescriptorSet allocate(VkDescriptorSetLayout layout);

    [[nodiscard]] uint32_t framesInFlight() const { return static_cast<uint32_t>(pools_.size()); }

private:
    DescriptorArena() = default;

    void destroyAll();

    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> pools_;
    uint32_t currentFrame_ = 0;
};

}  // namespace rx::rhi
