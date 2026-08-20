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
#include <rx_graph/barriers.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/storage_image.h>
#include <rx_rhi_vk/texture.h>
#include <volk.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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

    // WRITE-access companion to lastFrameFinalStages: the union of write
    // accesses the prior execute() left un-flushed on this entry. The
    // next call's first-use (UNDEFINED-discard) barrier must carry these
    // in srcAccessMask -- execution ordering alone does not make a prior
    // frame's final writes AVAILABLE, and a discard transition over
    // pending writes is a write-after-write hazard (surfaced by
    // synchronization validation's cross-submission tracking). Starts at
    // MEMORY_WRITE to mirror ALL_COMMANDS' conservatism.
    VkAccessFlags2 lastFrameFinalAccess = VK_ACCESS_2_MEMORY_WRITE_BIT;

    // Executor's own monotonic frame counter (one tick per execute() call)
    // as of the last time this entry was claimed by a realize() call OR
    // touched by an execute() call -- whichever happened more recently.
    // sweepStale() compares the CURRENT frame counter against this to
    // decide eviction; realize()'s acquireImage()/acquireBuffer() and
    // execute()'s per-pass bookkeeping both bump it.
    uint64_t lastUsedFrame = 0;
};

// [Task 2 (#38), gate ruling RC2] One pooled STORAGE image entry -- same
// shape/contract as PooledImage above, but keyed additionally by
// (mipLevels, arrayLayers, cube), the shape axes only a storage-image
// resource (Pass::addStorageImageOutput()) can ever declare more than 1 of.
// A plain ordinary transient (color/depth/history/texture-input-only
// resource, never more than 1 mip/1 layer -- see resources.h's
// PhysicalResource::mipLevels comment) still goes through PooledImage/
// Texture2D above unchanged; only a resource whose imageUsage carries
// VK_IMAGE_USAGE_STORAGE_BIT is ever pooled here instead (Executor::
// realize()'s own branch).
struct PooledStorageImage {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    bool cube = false;
    VkImageUsageFlags usage = 0;

    std::optional<rx::rhi::StorageImage> texture;

    // Same cross-frame carry-forward contract as PooledImage's own two
    // fields (executor.cpp's applyBarriers() first-use-of-frame override).
    VkPipelineStageFlags2 lastFrameFinalStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkAccessFlags2 lastFrameFinalAccess = VK_ACCESS_2_MEMORY_WRITE_BIT;

    uint64_t lastUsedFrame = 0;
};

// One pooled buffer entry -- same shape/contract as PooledImage above,
// keyed by (size, usage) instead [task-3-brief.md].
struct PooledBuffer {
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;

    std::optional<rx::rhi::Buffer> buffer;

    VkPipelineStageFlags2 lastFrameFinalStages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // See PooledImage::lastFrameFinalAccess -- same availability contract.
    VkAccessFlags2 lastFrameFinalAccess = VK_ACCESS_2_MEMORY_WRITE_BIT;
    uint64_t lastUsedFrame = 0;
};

// Phase 4 Task 1: one ping-pong SLOT of a pinned history resource -- one of
// the two physical images a single history-resource name is backed by
// [design contract point 2: "TWO pinned physical images per history
// resource (slot A/B)"].
//
// `barrierState` is the SAME per-resource invalidate/flush state machine
// barriers.h's own detail::ResourceBarrierState/applyAccess() already
// implement and buildBarriers() already drives for every ordinary
// PhysicalResource -- reused here verbatim rather than re-derived, per this
// repo's own "don't reinvent the wheel" rule (CLAUDE.md) and because the
// exact behavior needed (barrier only when the layout differs or a prior
// write/read demands one, WAW/WAR-correct, per-stage read coverage) is
// EXACTLY what that state machine already gets right. The one thing that
// makes a pinned slot's use of it different from buildBarriers()' own: a
// PinnedHistorySlot's ResourceBarrierState is constructed ONCE, when the
// slot is first created, and then lives for the pinned entry's ENTIRE
// lifetime (every subsequent Executor::execute() call feeds it more
// accesses without ever resetting it) -- never reset to a fresh
// UNDEFINED/everAccessed=false state at the start of a compile() walk the
// way buildBarriers() resets its own per-call vector. That persistence IS
// design contract point 4's "barrier state machine initialized from
// tracked last-frame layout instead of UNDEFINED", for free: there is
// simply no reset to undo it.
struct PinnedHistorySlot {
    std::optional<rx::rhi::Texture2D> texture;
    detail::ResourceBarrierState barrierState;

    // Phase 4 Task 1, PassContext::historyValid(): true once a REAL
    // Pass::setHistoryOutput()-declared write has landed in this slot at
    // least once -- set by Executor::execute() only when it processes an
    // actual HistoryOutput access against this slot, NEVER by the
    // black-clear-at-creation init below (that establishes DEFINED
    // content for a valid sampled read, but it is not "this frame's real
    // history contents" in the sense historyValid() promises). Latches
    // true forever once set -- a history resource's write slot does not
    // revert to "never written" once it has been.
    bool everWrittenByRealPass = false;
};

// Phase 4 Task 1: one history resource's full pinned entry -- both ping-pong
// slots plus the shared shape it was created/last resized against. Looked
// up by NAME (TransientPool::acquireHistory()), never by shape -- the
// opposite of PooledImage/PooledBuffer's opportunistic shape-based reuse
// above, since two DIFFERENT history resources of the same shape must never
// alias onto the same physical images the way two same-shaped transient
// resources happily do [design contract point 2: "never key-matched...
// never aliased"]. Never swept by TransientPool::sweepStale() either (see
// that method's own comment) -- retired only by TransientPool::retireAll()
// at Executor shutdown, or replaced wholesale by a future acquireHistory()
// call whose shape no longer matches (a resize; see that method's comment).
struct PinnedHistoryEntry {
    std::string name;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    VkImageUsageFlags usage = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    // Slot 0 / slot 1 -- Executor::execute() selects which is this frame's
    // write vs. read slot from its own frame counter's parity (frameCounter
    // % 2 writes, (frameCounter + 1) % 2 reads -- see that file's own
    // comment); this struct itself has no notion of "read" or "write", only
    // "slot 0" and "slot 1".
    std::array<PinnedHistorySlot, 2> slots;
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

    // [Task 2 (#38), gate ruling RC2] Same acquire-or-create contract as
    // acquireImage() above, for a rx::rhi::StorageImage keyed by (format,
    // extent, mipLevels, arrayLayers, cube, usage). Returns std::nullopt
    // (logged) only if creating a brand new entry's StorageImage fails.
    std::optional<uint32_t> acquireStorageImage(VkFormat format, VkExtent2D extent, uint32_t mipLevels,
                                                 uint32_t arrayLayers, bool cube, VkImageUsageFlags usage,
                                                 uint64_t currentFrame);

    // Phase 4 Task 1: returns the index (stable for the pool's lifetime --
    // see this struct's own class comment on why a pinned entry is never
    // swept the way acquireImage()'s entries are) of `name`'s pinned entry,
    // creating both slots fresh if `name` has never been acquired before,
    // or if its shape (`format`/`extent`/`usage`/`samples`) no longer
    // matches what was pinned for it previously (a resize -- the old
    // slots' rx::rhi::Texture2D are retired into `deletionQueue`, tagged
    // with `currentFrame`, exactly like sweepStale()'s own retirement, and
    // fresh ones created in their place; a resize necessarily discards
    // whatever history content existed, which is why `freshlyCreated`
    // comes back true for this case too, not just a true first-ever
    // creation).
    //
    // Looked up by `name` ALONE -- never by shape -- so two different
    // history resources sharing a shape never alias onto one pinned entry
    // [design contract point 2].
    //
    // `freshlyCreated`, written through the pointer, is how the caller
    // (Executor::realize()) knows whether it must record the one-time
    // black-clear-at-creation init submission for the returned entry's two
    // slots (see executor.cpp's initializePinnedHistoryEntry()) -- this
    // function only creates/replaces the Texture2Ds and seeds each fresh
    // slot's ResourceBarrierState/everWrittenByRealPass back to their
    // defaults; it never itself records a single vkCmd* call (no
    // VkCommandBuffer is available at this layer -- see executor.h's own
    // header-hygiene comment), matching every other TransientPool method's
    // "resource lifetime only, never a live command" division of labor
    // with executor.cpp.
    //
    // Returns std::nullopt (logged) only if creating a brand new Texture2D
    // fails.
    std::optional<uint32_t> acquireHistory(std::string_view name, VkFormat format, VkExtent2D extent,
                                            VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                                            uint64_t currentFrame, rx::rhi::DeletionQueue& deletionQueue,
                                            bool* freshlyCreated);

    [[nodiscard]] PooledImage& image(uint32_t index) { return images_.at(index); }
    [[nodiscard]] const PooledImage& image(uint32_t index) const { return images_.at(index); }
    [[nodiscard]] PooledBuffer& buffer(uint32_t index) { return buffers_.at(index); }
    [[nodiscard]] const PooledBuffer& buffer(uint32_t index) const { return buffers_.at(index); }
    [[nodiscard]] PooledStorageImage& storageImage(uint32_t index) { return storageImages_.at(index); }
    [[nodiscard]] const PooledStorageImage& storageImage(uint32_t index) const { return storageImages_.at(index); }
    [[nodiscard]] PinnedHistoryEntry& pinned(uint32_t index) { return pinned_.at(index); }
    [[nodiscard]] const PinnedHistoryEntry& pinned(uint32_t index) const { return pinned_.at(index); }

    // Bumps `lastUsedFrame` to `currentFrame` for the given already-
    // acquired entry -- called once per execute() call, for every pooled
    // entry that call's realize()-established mapping actually references
    // (see executor.cpp), so an entry realize() bound once and execute()
    // keeps reusing every frame never looks "unused" to sweepStale() below.
    void touchImage(uint32_t index, uint64_t currentFrame);
    void touchBuffer(uint32_t index, uint64_t currentFrame);
    void touchStorageImage(uint32_t index, uint64_t currentFrame);

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
    //
    // Phase 4 Task 1: deliberately never touches pinned_ [design contract
    // point 2: "never swept while the graph holds them"] -- a pinned
    // entry's `lastUsedFrame`-equivalent staleness tracking simply does not
    // exist; once created, a history resource is retired only by
    // retireAll() (shutdown) or replaced wholesale by a later
    // acquireHistory() call for the SAME name whose shape no longer
    // matches (a resize, handled inside acquireHistory() itself). This is
    // the simplest, most conservative choice that still satisfies "never
    // recycled/aliased" [seed-15 rationale]: an unclaimed pinned entry
    // simply stays parked (a handful of images at most, in any Phase 4
    // scene) rather than needing its own staleness clock and re-init
    // machinery for a case no current caller exercises.
    void sweepStale(uint64_t currentFrame, rx::rhi::DeletionQueue& deletionQueue);

    // Shutdown path: retires EVERY still-live entry -- including every
    // pinned history entry's two slots -- into `deletionQueue`
    // unconditionally, regardless of lastUsedFrame. Called from Executor's
    // destructor, immediately after it has already brought the device to
    // a real idle point (see executor.cpp), so `deletionQueue`'s own
    // flushAll() immediately after this call is safe to run
    // unconditionally too.
    void retireAll(uint64_t currentFrame, rx::rhi::DeletionQueue& deletionQueue);

private:
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    rx::rhi::Allocator* allocator_ = nullptr;

    std::vector<PooledImage> images_;
    std::vector<PooledBuffer> buffers_;
    // [Task 2 (#38), gate ruling RC2] Separate from images_ -- see
    // PooledStorageImage's own comment for why a storage-image resource
    // needs its own pool keyed on additional shape axes (mip/array/cube)
    // ordinary transients never vary.
    std::vector<PooledStorageImage> storageImages_;

    // Phase 4 Task 1: pinned history entries, keyed by name (linear scan in
    // acquireHistory() -- a graph has at most a handful of history
    // resources, nowhere near enough to justify a name->index map on top
    // of images_/buffers_' own precedent of a plain linear scan for the
    // same reason). No claimed-this-batch tracking (imageClaimedThisBatch_'s
    // own twin) -- name-keyed lookup cannot alias two different
    // PhysicalResource entries onto one pinned entry the way shape-keyed
    // lookup could, so there is nothing for a per-batch marker to guard
    // against here.
    std::vector<PinnedHistoryEntry> pinned_;

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
    std::vector<bool> storageImageClaimedThisBatch_;
};

}  // namespace rx::graph::detail
