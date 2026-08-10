#pragma once
// Private implementation detail of rx_graph's Executor -- NOT installed
// under include/rx_graph/, and NOT reachable from any public rx_graph
// header (executor.h forward-declares only rx::rhi::Device; this header is
// included by executor.cpp/transient_pool.cpp alone). Unlike every public
// rx_graph header, this one freely depends on rx_rhi_vk (Texture2D/Buffer/
// Allocator) and volk -- Task 3's own binding constraint ("no volk in
// public headers") is about headers a consumer of rx_graph's public API
// might include, which this one structurally cannot be.
//
// Reuses rx::rhi::Texture2D/rx::rhi::Buffer/rx::rhi::Allocator wholesale
// instead of calling vmaCreateImage/vmaCreateBuffer directly -- this
// project's own standing rule against rebuilding what already exists
// [repo CLAUDE.md, "Do not reinvent the wheel"] applies exactly as much to
// rx_graph's own sibling library as it would to a third-party one.
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/texture.h>
#include <volk.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace rx::rhi {
class DeletionQueue;
}  // namespace rx::rhi

namespace rx::graph::detail {

// How many consecutive execute() calls (Executor's own frame counter, one
// tick per execute() call -- see executor.cpp) a pooled entry may go
// unclaimed by any realize() before it is swept: FrameSync::kFramesInFlight
// (2) + 1, per task-3-brief.md's own transient-pool ambiguity resolution.
// The "+1" is deliberate slack, not an off-by-one: an entry freed by a
// realize() call that ran between two execute() calls should survive at
// least one full frames-in-flight cycle before eviction, since a
// same-shape resource could plausibly be reclaimed by a *later* realize()
// (e.g. a window resize that bounces back to a previous size) without
// this pool ever needing to reallocate.
constexpr uint64_t kStaleAfterExecutes = 2 + 1;

// One pooled image entry. `texture` is empty (std::nullopt) exactly when
// this entry is either not yet created (a freshly-grown vector slot -- see
// TransientPool::images_) or has been retired via
// TransientPool::sweepStale() (its real rx::rhi::Texture2D moved into a
// DeletionQueue-retired closure, leaving this slot vacant and immediately
// reusable by a future acquireImage() call for a *different* key).
// Deliberately std::optional rather than a default-constructed
// placeholder: rx::rhi::Texture2D's own default constructor is private
// (friends only), so an "empty but valid" Texture2D is not constructible
// from here at all -- std::optional sidesteps that without needing
// Texture2D to grant this file friendship.
struct PooledImage {
    // Key rx_graph/executor.cpp's acquireImage() matches against --
    // task-3-brief.md: "keyed by (format, extent, usage, samples)".
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    VkImageUsageFlags usage = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    std::optional<rx::rhi::Texture2D> texture;

    // Task 3 ambiguity resolution #2: the pipeline stage(s) this entry's
    // contents were last left "in flight" by, as of the end of the last
    // execute() call that actually touched it -- VK_PIPELINE_STAGE_2_
    // ALL_COMMANDS_BIT on an entry that has never been touched by any
    // execute() yet (its very first use, whether that's a brand new
    // allocation or a reclaimed-but-previously-untouched-this-realize
    // entry). Executor::execute() reads this to override the srcStage of
    // the FIRST barrier that touches this entry's physicalIndex each call
    // (compile()'s own device-free barrier derivation always says
    // srcStage NONE there, correct only for a resource's very first use
    // within ONE compile walk, never across the frame boundary a pooled
    // entry's real lifetime spans) and OVERWRITES it with this entry's own
    // final dstStage from THIS call, at the end of execute() -- so the
    // NEXT call's first-use override chains correctly off what this call
    // actually left the resource doing.
    VkPipelineStageFlags2 lastFrameFinalStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    // Executor's own monotonic frame counter (one tick per execute() call)
    // as of the last time this entry was claimed by a realize() call OR
    // touched by an execute() call -- whichever happened more recently.
    // sweepStale() compares the CURRENT frame counter against this to
    // decide eviction; realize()'s acquireImage()/acquireBuffer() and
    // execute()'s per-pass bookkeeping both bump it.
    uint64_t lastUsedFrame = 0;
};

// One pooled buffer entry -- same shape/contract as PooledImage above,
// keyed by (size, usage) instead [task-3-brief.md].
struct PooledBuffer {
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;

    std::optional<rx::rhi::Buffer> buffer;

    VkPipelineStageFlags2 lastFrameFinalStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    uint64_t lastUsedFrame = 0;
};

// Executor's private transient-resource pool: acquire-or-create image/
// buffer entries keyed by their physical descriptor, reused across
// execute() calls (a realize()d Executor keeps referencing the same
// entries call after call -- see executor.cpp's ResolvedResource) and
// across separate realize() calls once a prior binding is released (a
// resize, or an entirely different RenderGraph topology sharing the same
// shape) -- entries left unclaimed by any realize() for
// kStaleAfterExecutes consecutive execute() calls are torn down via
// rx::rhi::DeletionQueue rather than kept forever.
//
// Move-only; not copyable (owns live VMA-backed Vulkan resources through
// its rx::rhi::Texture2D/Buffer members).
class TransientPool {
public:
    TransientPool(TransientPool&&) noexcept = default;
    TransientPool& operator=(TransientPool&&) noexcept = default;
    TransientPool(const TransientPool&) = delete;
    TransientPool& operator=(const TransientPool&) = delete;
    ~TransientPool() = default;

    TransientPool(VkPhysicalDevice physicalDevice, VkDevice device, rx::rhi::Allocator& allocator)
        : physicalDevice_(physicalDevice), device_(device), allocator_(&allocator) {}

    // Begins one realize() call's acquire batch: clears the "already
    // claimed in this batch" marker every acquire*() call below checks, so
    // two acquire calls within the SAME realize() invocation for the SAME
    // key never alias onto one physical resource (two logically distinct
    // PhysicalResource entries sharing a descriptor must each get their
    // own allocation) while still allowing a LATER, separate realize()
    // call to reclaim whatever this batch didn't.
    void beginRealizeBatch();

    // Returns the index (stable until a future sweepStale() vacates it) of
    // an image entry matching `format`/`extent`/`usage`/`samples`, not
    // already claimed within this batch -- reusing the first idle match
    // found, or creating (and appending) a fresh rx::rhi::Texture2D if
    // none exists. Marks the returned entry claimed-this-batch and bumps
    // its lastUsedFrame to `currentFrame`. Returns std::nullopt (logged)
    // only if creating a brand new entry's Texture2D fails.
    std::optional<uint32_t> acquireImage(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                                          VkSampleCountFlagBits samples, uint64_t currentFrame);

    // Same contract as acquireImage(), for a buffer keyed by (size, usage).
    std::optional<uint32_t> acquireBuffer(VkDeviceSize size, VkBufferUsageFlags usage, uint64_t currentFrame);

    [[nodiscard]] PooledImage& image(uint32_t index) { return images_.at(index); }
    [[nodiscard]] const PooledImage& image(uint32_t index) const { return images_.at(index); }
    [[nodiscard]] PooledBuffer& buffer(uint32_t index) { return buffers_.at(index); }
    [[nodiscard]] const PooledBuffer& buffer(uint32_t index) const { return buffers_.at(index); }

    // Bumps `lastUsedFrame` to `currentFrame` for the given already-
    // acquired entry -- called once per execute() call, for every pooled
    // entry that call's realize()-established mapping actually references
    // (see executor.cpp), so an entry realize() bound once and execute()
    // keeps reusing every frame never looks "unused" to sweepStale() below.
    void touchImage(uint32_t index, uint64_t currentFrame);
    void touchBuffer(uint32_t index, uint64_t currentFrame);

    // Retires (via `deletionQueue`, tagged with each entry's own
    // lastUsedFrame -- already provably safe to destroy once that frame
    // number's submission has completed, per this same frames-in-flight
    // bound) every entry whose `currentFrame - lastUsedFrame >=
    // kStaleAfterExecutes` (exactly "unused for kStaleAfterExecutes
    // consecutive execute() calls" [Fix round 1, Minor finding -- an
    // earlier revision used `>`, which held an idle entry alive one call
    // longer than the ruling's literal wording]). Called once per
    // execute() call, before that call's own barrier/rendering work -- see
    // executor.cpp.
    void sweepStale(uint64_t currentFrame, rx::rhi::DeletionQueue& deletionQueue);

    // Shutdown path: retires EVERY still-live entry into `deletionQueue`
    // unconditionally, regardless of lastUsedFrame -- called from
    // Executor's destructor, immediately after it has already brought the
    // device to a real idle point (see executor.cpp), so `deletionQueue`'s
    // own flushAll() immediately after this call is safe to run
    // unconditionally too.
    void retireAll(uint64_t currentFrame, rx::rhi::DeletionQueue& deletionQueue);

private:
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    rx::rhi::Allocator* allocator_ = nullptr;

    std::vector<PooledImage> images_;
    std::vector<PooledBuffer> buffers_;

    // Per-batch "already handed out this realize() call" markers, sized to
    // match images_/buffers_ and cleared by beginRealizeBatch(). Plain
    // std::vector<bool> rather than a set: acquire calls per realize() are
    // a handful (one per PhysicalResource), and this is checked/set at
    // most once per candidate entry per acquire call -- a linear scan
    // over a small vector is simpler and fast enough, matching this
    // project's general preference for the plainest correct data
    // structure over cleverness with no measured benefit.
    std::vector<bool> imageClaimedThisBatch_;
    std::vector<bool> bufferClaimedThisBatch_;
};

}  // namespace rx::graph::detail
