#pragma once
#include <rx_rhi_vk/buffer.h>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace rx::rhi {

class Device;
class Texture2D;

// [Phase 4 Task 11, spec D25 as amended by gate ruling RC4] A pollable
// handle to the GPU work a specific `Uploader::flush()` call submitted --
// `value` is the counter value this Uploader's own timeline semaphore
// reaches once that work (and, by construction, every earlier submission
// on the same queue -- see flush()'s own comment) has completed.
//
// `value == 0` is the reserved "already complete" sentinel: the timeline
// semaphore starts at counter value 0 and only ever counts up, so
// `currentValue >= 0` is unconditionally true without ever touching the
// semaphore -- exactly the ticket a `flush()` call with nothing new to
// submit (nothing recorded since the last flush, or a batch that only
// ever took the direct memcpy path -- see uploadToBuffer()'s own comment)
// returns, matching this class's pre-Task-11 no-op-flush behavior with no
// fence/semaphore machinery touched at all [matrix-issue28 rows 8/9].
//
// Comparable/copyable POD; safe to store, copy, and query long after the
// `flush()` call that produced it, independent of how many LATER flush()
// calls have since been issued (this is the whole point of the redesign --
// see the class comment on Uploader below).
struct UploadTicket {
    uint64_t value = 0;

    // Diagnostic-only: Uploader::ringWrapCount() at the moment this ticket
    // was issued. Carried over from this ticket's originally proposed
    // shape (issue #28 body: "fence + ring generation") even though
    // neither isComplete() nor wait() below ever consult it -- completion
    // is determined entirely by `value` against the timeline semaphore.
    // Production code has no need to read this; it exists so a test can
    // assert which "lap" of the ring a given upload landed in.
    //
    // NOT a safety/epoch mechanism [review fix round 1]: this field plays
    // NO role in detecting a stale ticket, no role in ring-reclamation
    // correctness, and nothing anywhere compares it against a "current"
    // generation the way a generational handle (rx::core::Handle<Tag>)
    // compares its own generation field. A ticket is valid/comparable for
    // as long as this Uploader itself is alive, full stop -- do not add
    // logic that assumes generation-checking exists here.
    uint32_t ringGeneration = 0;

    bool operator==(const UploadTicket&) const = default;
};

// Gets CPU-side bytes into GPU-visible/device-local Vulkan resources:
// uploadToBuffer() for a destination rx::rhi::Buffer, uploadToImage() for
// a Texture2D (optionally generating its full mip chain via
// Texture2D::recordMipChainBlit() in the same submission) [spec Fixed
// decision #7, R:C1].
//
// BATCHED, POLLABLE FLUSH [Phase 4 Task 11, spec D25 as amended by gate
// ruling RC4] -- read before assuming per-call semantics: every upload
// that goes through the staging ring (see below) only RECORDS a command
// (a vkCmdCopyBuffer/vkCmdCopyBufferToImage, plus any mip-chain blit) onto
// one of this Uploader's own internal command buffers; nothing touches the
// GPU until flush() submits everything recorded so far. Unlike this
// class's pre-Task-11 shape, flush() no longer BLOCKS the calling thread
// on that submission: it returns an UploadTicket immediately, and
// isComplete(ticket)/wait(ticket) below let a caller check or block on
// that specific batch's completion whenever it actually needs to (not
// necessarily right away, or ever, if it never needs to know). This lets
// several uploads share one submission (cheaper than one submit-and-wait
// per resource) while letting a caller overlap many frames' worth of
// uploads without ever stalling its own main thread inside flush() itself.
//
// TICKET PRIMITIVE: ONE TIMELINE SEMAPHORE, owned by this Uploader for its
// whole lifetime. Every work-submitting flush() call signals the NEXT
// monotonically increasing counter value and returns
// UploadTicket{thatValue, ringWrapCount()}; isComplete(ticket) is one
// vkGetSemaphoreCounterValue compare (current >= ticket.value); wait(ticket)
// is vkWaitSemaphores for that value (returns immediately if already
// reached). Chosen over a fence (pool or single-reused) specifically
// because a REUSED fence is structurally incompatible with a pollable
// ticket: resetting fence F to submit ticket N+1's work while a caller
// might still be querying/waiting on ticket N (which used the SAME fence
// object) either corrupts ticket N's reported status or is an outright
// Vulkan validation error (vkResetFences while a wait on that fence is
// still meaningful elsewhere) -- the exact hazard gate matrix-issue28's
// row 1 traces to this class's original single-`VkFence` design. A
// monotonic timeline-semaphore counter has no reset step at all (core
// Vulkan 1.2, no extension gating needed on this project's 1.3 baseline),
// eliminating the hazard by construction: two overlapping flush() calls
// with no wait on the first one in between is the NORMAL case, not a
// hazard -- see upload_test.cpp's dedicated overlapping-flush test, the
// single highest-priority correctness gate for this class.
//
// TWO tickets from two DIFFERENT flush() calls are ordered exactly like
// their counter values: ticket N+1's value is always > ticket N's, and
// because both are signaled by submissions to the SAME queue in the SAME
// order they were issued, waiting on the LATER ticket also guarantees the
// earlier one's work has completed (this is what lets ~Uploader() below
// wait on only the single most-recently-queued value at teardown, rather
// than tracking every outstanding ticket it ever handed out).
//
// COMMAND-BUFFER ROTATION: a direct consequence of the same "flush() no
// longer blocks" change -- reusing a single command buffer the way this
// class did before Task 11 (vkResetCommandBuffer on every new batch) is
// now the SAME class of hazard as the fence-reuse one above, just for the
// command buffer instead of the completion primitive: resetting/
// re-recording into a buffer whose previous submission may still be
// pending on the GPU is a hard Vulkan violation. This class now keeps a
// small, lazily-grown ROTATION of command buffers (all allocated from one
// pool) and picks any buffer whose last submission is confirmed complete
// (or allocates a fresh one if none is free) every time a new batch starts
// recording -- never blocking to reclaim one, matching the timeline
// semaphore's own "no artificial in-flight depth limit" character.
//
// DIRECT UPLOAD (BUFFERS ONLY) VS. STAGING RING [R:C1] -- CORRECTED THIS
// TASK'S REVIEW CYCLE, read before assuming the mechanism below: the
// ReBAR-aware "direct" fast path applies to the DESTINATION buffer's OWN
// allocation, not to this Uploader's internal staging ring. The first
// version of this class got this backwards -- it applied
// ALLOW_TRANSFER_INSTEAD_BIT to the ring buffer (which only ever has
// VK_BUFFER_USAGE_TRANSFER_SRC_BIT usage), and verified directly against
// vendored VMA 3.4.0 source that this is structurally inert: VMA's
// FindMemoryPreferences() only adds DEVICE_LOCAL to its preferred flags
// when the buffer's usage contains a real device-consuming bit
// (VmaBufferImageUsage::ContainsDeviceAccess(), which masks OUT the
// transfer bits) -- a TRANSFER_SRC-only buffer can never trigger that
// branch, on any hardware, confirmed live on this project's own
// ReBAR-capable desktop GPU (the direct path never engaged once). The
// fix: rx::rhi::Allocator::createDeviceLocalBuffer() (buffer.h) is what
// now requests ALLOW_TRANSFER_INSTEAD_BIT, applied to a buffer with its
// REAL device-consuming usage (VERTEX_BUFFER_BIT, INDEX_BUFFER_BIT, ...)
// -- exactly the resource this "direct" path is supposed to describe.
// uploadToBuffer() checks `dst.directPathCapable() && dst.mappedData() !=
// nullptr` per call: when true (ReBAR-enabled desktop GPU, or a
// unified-memory APU like Steam Deck, landed `dst`'s own memory in
// something BOTH DEVICE_LOCAL and HOST_VISIBLE), it memcpy's straight
// into `dst.mappedData()` and calls `dst.flush()` -- no staging copy, no
// command recorded, no ring buffer touched at all, and (per the ticket
// design above) no fence/semaphore machinery touched by the flush() call
// that later covers it either, if nothing else in that batch needed real
// GPU work [matrix-issue28 row 8]. Otherwise it falls back to exactly the
// staging-ring mechanism this class always had: memcpy into the ring,
// Buffer::flush() it (correct even on a hypothetical non-coherent
// HOST_VISIBLE memory type, not just the coherent-in-practice hardware
// this engine has been tested against), record a vkCmdCopyBuffer from the
// ring into `dst`. Which branch a given call took is logged once per
// outcome (RX_LOG_INFO, the first time each is observed -- not per call)
// and exposed for tests via everUsedDirectPath()/everUsedStagingPath().
//
// IMAGES ALWAYS STAGE -- this is the honest shape of [R:C1]'s guidance,
// not a cop-out: a VkImage cannot be written from the host at all without
// VK_EXT_host_image_copy, which is not in this project's baseline
// (Vulkan 1.3 core + the descriptor-indexing feature set [spec Fixed
// decision #5] -- no host-image-copy anywhere in that list). There is no
// "direct" branch for uploadToImage() to take, on any hardware, by
// construction of the Vulkan API itself -- every image upload goes
// through the staging ring, unconditionally, exactly as this class
// always did for images; its ticket is therefore never the trivially-
// complete sentinel [matrix-issue28 row 7].
//
// RING RECLAMATION KEYED TO TICKET COMPLETION, NOT TO HAVING BLOCKED
// [matrix-issue28 rows 5/6, the D3D12 fence-gated-ring-buffer pattern]:
// this Uploader tracks a FIFO queue of (ticket, ring-byte-range) entries,
// one per work-submitting flush() call. Reclaiming ring space (needed
// only when a write would wrap past the buffer's end) first pops every
// entry at the FRONT of that queue whose ticket is already complete
// (a pure poll, never blocking) and, only if the requested range still
// overlaps the oldest remaining entry, blocks on THAT SPECIFIC ticket
// (never a whole-ring flush-and-wait) before trying again. The ring
// buffer itself is fixed-size (set once at create(), never grown) --
// exhaustion under many in-flight tickets blocks on the oldest one
// covering the needed range, exactly the D3D12-precedented
// block-on-oldest policy, not silent growth.
//
// If a single ring-staged upload's write position would run past the
// ring buffer's end before the pending batch is flushed, this Uploader
// transparently flushes what is already recorded (submitting it, and
// tracking its ring range per the paragraph above -- not waiting on it)
// and then continues writing from offset 0 once enough of the ring's
// front is confirmed reclaimable -- callers never need to reason about
// ring-buffer capacity themselves unless a single upload's size exceeds
// the ring buffer's total capacity, which fails cleanly (logged, returns
// false) rather than silently corrupting anything. This never applies to
// a direct-path buffer upload, which never touches the ring at all.
//
// Thread-affinity (D5, Phase 4): uploadToBuffer()/uploadToImage()/
// uploadImageMips()/flush()/isComplete()/wait() are main-thread-only -- see
// docs/threading.md. uploadToBuffer()/uploadToImage()/uploadImageMips()/
// flush()/isComplete()/wait() all carry a dev-time RX_ASSERT_MAIN_THREAD
// guard [Phase 4 Task 7 fix round 1;
// extended to flush()/isComplete()/wait() in Task 11, since flush() can
// now be a legitimate standalone call -- e.g. on nothing recorded -- no
// longer reachable only via an already-guarded uploadTo*() call] that
// fails loudly on a chunk >= 1 violation instead of corrupting shared
// state silently. isComplete()/wait() poll/wait on this Uploader's own
// timeline semaphore, which the Vulkan spec itself permits from any
// thread with no external synchronization -- the guard here is this
// project's own D5 policy scoping, matching every other Uploader entry
// point's identical scoping, not a hazard these two introduce.
class Uploader {
public:
    Uploader(Uploader&&) noexcept;
    Uploader& operator=(Uploader&&) noexcept;
    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    // Auto-flushes any pending (recorded-but-not-yet-submitted) uploads
    // and then WAITS for every ticket this Uploader ever queued --
    // including ones from earlier flush() calls a caller never itself
    // waited/polled on -- before tearing down the timeline semaphore/
    // command pool/ring buffer, so a caller that never explicitly waited
    // on a ticket never silently loses work or destroys a resource the
    // GPU is still using. Waiting on only the single most-recently-queued
    // value is sufficient (see the class comment's paragraph on ticket
    // ordering above) -- it transitively covers every earlier ticket too.
    ~Uploader();

    static constexpr VkDeviceSize kDefaultRingBufferSize = 16u * 1024u * 1024u;  // 16 MiB

    // Builds this Uploader's internal command pool (command buffers are
    // allocated from it lazily, on demand -- see the class comment's
    // "COMMAND-BUFFER ROTATION" paragraph) and timeline semaphore against
    // `device`'s graphics queue, and its staging ring buffer (`usage`
    // VK_BUFFER_USAGE_TRANSFER_SRC_BIT only -- this buffer is always a
    // copy source, never a real destination) of `ringBufferSize` bytes
    // via `allocator`. Returns std::nullopt (logged) on any failure.
    static std::optional<Uploader> create(Allocator& allocator, Device& device,
                                           VkDeviceSize ringBufferSize = kDefaultRingBufferSize);

    // Copies `size` bytes from `data` into `dst` at `dstOffset`. Takes
    // `dst` by reference (not a bare VkBuffer) specifically so this
    // method can inspect `dst.directPathCapable()`/`dst.mappedData()` to
    // decide the direct-vs-staging branch described in the class comment
    // above -- a bare handle carries neither. `dst` is assumed to already
    // be sized/allocated by the caller (e.g. via
    // Allocator::createDeviceLocalBuffer()) with
    // VK_BUFFER_USAGE_TRANSFER_DST_BIT (needed for the staging branch;
    // harmless/unused for the direct branch). Returns false (logged) only
    // if the staging branch is taken AND `size` exceeds this Uploader's
    // total ring-buffer capacity outright -- every other case (including
    // a same-batch auto-flush to reclaim ring space, and every direct-path
    // call regardless of size) succeeds. A `size` of 0 is a no-op that
    // returns true.
    bool uploadToBuffer(Buffer& dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size);

    // Copies `pixelBytes` bytes from `pixels` (tightly packed, matching
    // `dst`'s extent/format texel layout) into mip level 0 of `dst`,
    // through the ring buffer, then transitions `dst` to
    // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL -- via
    // Texture2D::recordMipChainBlit() (generating the full mip chain in
    // the same recorded batch) when `generateMips` is true, or via a
    // single whole-image layout transition when it's false. Note this is
    // NOT additionally gated on `dst.mipLevels() > 1`: recordMipChainBlit()
    // already handles a single-mip `dst` correctly and cheaply via its
    // own early-return branch, so `generateMips` alone decides which of
    // the two branches runs. As with uploadToBuffer(), nothing reaches
    // the GPU until flush(). Returns false (logged) only if `pixelBytes`
    // exceeds this Uploader's total ring-buffer capacity outright. A
    // `pixelBytes` of 0 is a no-op that returns true.
    bool uploadToImage(Texture2D& dst, const void* pixels, VkDeviceSize pixelBytes, bool generateMips);

    // [Phase 4 Stage 1 Task 14, gate matrix-issue03 N4/gate ruling #3]
    // One caller-supplied mip level to upload verbatim -- `data`/`size`
    // is the level's own tightly-packed byte range (as reported by, e.g.,
    // a KTX2 container's own per-level image size -- rx::asset::
    // DecodedKtx2Texture::levels()), and `extent` is the level's TRUE
    // texel extent (NEVER rounded up to block granularity -- a
    // block-compressed format's sub-block mip TAIL level, e.g. 2x2 or
    // 1x1 under a 4x4-block format, reports its real 2 or 1 here; see
    // uploadImageMips()'s own comment for why this is still correct).
    struct ImageMipLevel {
        const void* data = nullptr;
        VkDeviceSize size = 0;
        uint32_t mipLevel = 0;
        VkExtent2D extent{0, 0};
    };

    // Uploads a full set of caller-supplied mip levels DIRECTLY into
    // `dst` -- one VkBufferImageCopy region per entry in `levels`, through
    // this Uploader's own staging ring exactly like uploadToImage() above
    // -- and transitions `dst` to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
    // NEVER calls Texture2D::recordMipChainBlit() (no mip GENERATION on
    // this path at all -- every level's real data comes from `levels`
    // itself): `dst` must have been created via
    // Texture2D::createForPresuppliedMips(), not create().
    //
    // BLOCK-COMPRESSED ROW PITCH / SUB-BLOCK MIP TAILS [gate matrix-
    // issue03 N4, the mip-chain matrix row's own "classic off-by-one"]:
    // every VkBufferImageCopy region here leaves `bufferRowLength`/
    // `bufferImageHeight` at 0 ("tightly packed" per each entry's own
    // `extent`) rather than computing an explicit block-row pitch by
    // hand. This is deliberately NOT a simplification that happens to
    // work -- it is the Vulkan spec's OWN documented block-copy rule
    // (`vkCmdCopyBufferToImage`'s "Buffer and Image Layouts" section): a
    // 0 rowLength/imageHeight is defined to mean "computed from
    // imageExtent, per-block, rounded UP to whole blocks" -- which is
    // EXACTLY the KTX2 per-level byte layout libktx itself already
    // produces (verified directly: `ktxTexture_GetImageSize()`'s own
    // block-rounded byte count matches this formula bit-for-bit for
    // every level, including sub-block tails, where `extent` reports the
    // true 2x2/1x1 texel size while the region's OWN copied byte count is
    // exactly one whole compressed block regardless). No block-width/
    // height-aware pitch arithmetic is written by this engine at all --
    // Vulkan's own copy semantics already do it correctly for every
    // level in one uniform code path, compressed or not.
    //
    // Levels need not be sorted or cover every level of `dst`'s own
    // mipLevels() (not exercised by any Task 14 test, but not
    // disallowed).
    //
    // OVERSIZED LEVELS (bigger than this Uploader's own ring-buffer
    // capacity) [texture-path round, item B -- the 16MB staging-cap fix;
    // e.g. a 4096x4096 RGBA8 mip 0 is 64MB against the 16MB default ring]:
    // handled via CHUNKED STAGING TRIPS, not rejected. A level whose
    // `size` exceeds ringCapacity() is split into consecutive, contiguous
    // row-groups -- each its own independent reserve/memcpy/
    // vkCmdCopyBufferToImage trip through the SAME fixed-size ring (never
    // growing it, never a separate transient allocation) -- so the final
    // image ends up byte-identical to one theoretical giant copy, just
    // assembled from several bounded ones. This composes transparently
    // with flush()/isComplete()/wait(): every chunk's copy is recorded
    // like any other Uploader command, covered by whatever ticket the
    // NEXT flush() call returns -- a caller awaiting that ticket (or
    // relying on the sync loadFromBytes()-style flush()+wait()
    // convenience) is guaranteed every chunk has completed before
    // treating the texture as ready, exactly D25's ticket-covers-
    // everything-recorded-so-far semantics, no separate per-chunk
    // completion tracking needed. A level this large can still fail
    // (logged, returns false, BEFORE any GPU work is recorded for the
    // whole call -- same "no partial-batch corruption" contract as
    // uploadToImage()/uploadToBuffer() above) if it cannot be safely
    // chunked at all: `levels` is empty, or an oversized level's `size`
    // does not divide evenly into whole `extent.height` rows (every
    // uncompressed RGBA8 level, and every block-compressed level actually
    // large enough to need chunking, divides evenly in practice -- see
    // upload.cpp's own levelNeedsChunking() comment), or even a single row
    // does not fit in the ring outright (a pathologically wide level).
    bool uploadImageMips(Texture2D& dst, std::span<const ImageMipLevel> levels);

    // Submits every uploadTo*() call recorded since the last flush() (or
    // since create()) and returns a pollable UploadTicket -- see the class
    // comment above for the full ticket design. Does NOT block: a caller
    // that needs the old always-blocking behavior calls wait() on the
    // returned ticket explicitly (see MeshBuffers::create() for exactly
    // this pattern). If nothing was recorded since the last flush() (no
    // uploadTo*() call happened, or every call in this batch took the
    // direct memcpy path with zero real GPU work queued), returns the
    // trivially-complete UploadTicket{0, ringWrapCount()} without
    // touching the timeline semaphore or submitting anything at all --
    // matching this class's pre-Task-11 no-op-flush() behavior exactly.
    UploadTicket flush();

    // Pure poll, no blocking: true iff this Uploader's timeline semaphore
    // has reached (or passed) `ticket.value`. A `ticket.value` of 0
    // (the trivially-complete sentinel) is always true without querying
    // the semaphore at all. On the rare vkGetSemaphoreCounterValue
    // failure, logs a named RX_LOG_ERROR (never silently treated as
    // either state) and conservatively returns false.
    bool isComplete(UploadTicket ticket) const;

    // Blocks the calling thread until this Uploader's timeline semaphore
    // reaches `ticket.value` (vkWaitSemaphores, UINT64_MAX timeout) --
    // the direct analog of this class's pre-Task-11 always-blocking
    // flush(). Returns immediately if `ticket` is already complete
    // (including the ticket.value == 0 sentinel, which never touches the
    // semaphore at all -- see isComplete()'s own comment).
    void wait(UploadTicket ticket) const;

    // [CI-red fix, matrix-issue28 follow-up] The raw VkSemaphore backing
    // every UploadTicket this Uploader ever hands out -- for a caller
    // building its OWN VkSubmitInfo/VkSubmitInfo2 that needs a real
    // GPU-side (submission-visible) dependency on a specific ticket's
    // value, via VkTimelineSemaphoreSubmitInfo's pWaitSemaphoreValues,
    // rather than relying solely on wait()/isComplete()'s HOST-side
    // polling. A host wait's visibility scope covers only HOST accesses to
    // the written memory (Vulkan spec); it does not, by itself, establish
    // the availability/visibility chain a LATER, separate device queue
    // submission needs to safely read what this Uploader wrote --
    // Vulkan-ValidationLayers' own synchronization-validation design docs
    // describe this exact host-vs-device-submission gap as outside its
    // tracked feature set. See command.h's CommandContext::runOnce()
    // `waitValue` parameter and upload_test.cpp's readBackBuffer() for the
    // pattern this accessor exists for. Thread-affinity (D5): safe from
    // any thread -- a plain read of an immutable-after-create() handle,
    // same as ringCapacity()/everUsedDirectPath() below.
    VkSemaphore timelineSemaphore() const { return timelineSemaphore_; }

    // How many times this Uploader's ring buffer has wrapped back to
    // offset 0 since create() -- diagnostic/test accessor (see
    // UploadTicket::ringGeneration's own comment); production code has no
    // need for it.
    uint32_t ringWrapCount() const { return ringGeneration_; }

    // How many times reserveRingSpace() had to block (wait() on a
    // specific in-flight ticket) to reclaim ring space, across this
    // Uploader's whole lifetime -- test/diagnostic accessor proving the
    // D3D12-pattern partial reclaim in the class comment above actually
    // engages (a stress test asserts this stays below the number of
    // flush() calls issued, per matrix-issue28 row 5). Production code
    // has no need for it.
    uint32_t blockingRingWaitCount() const { return blockingRingWaitCount_; }

    // This Uploader's fixed ring-buffer capacity, exactly as passed to
    // create() -- never changes for this Uploader's lifetime (this class
    // never silently grows the ring; see the class comment's ring-
    // reclamation paragraph). Test/diagnostic accessor.
    VkDeviceSize ringCapacity() const { return ringBufferSize_; }

    // True once at least one uploadToBuffer() call has taken the direct
    // (memcpy-straight-into-`dst`) branch / the staging-ring branch,
    // respectively, at any point in this Uploader's lifetime. Test/
    // diagnostic accessors -- production code has no need for them
    // (uploadToBuffer()'s return value already reports success/failure
    // uniformly regardless of which branch ran).
    bool everUsedDirectPath() const { return everUsedDirectPath_; }
    bool everUsedStagingPath() const { return everUsedStagingPath_; }

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
    // class also owns several *raw* Vulkan handles (pool_/timelineSemaphore_/
    // every VkCommandBuffer inside cmdSlots_), which a defaulted move
    // would copy without nulling the source -- exactly the double-destroy
    // hazard every other RAII type in this library's move operations are
    // hand-written to avoid.
    Uploader(VkDevice device, VkQueue queue, VkCommandPool pool, VkSemaphore timelineSemaphore, Buffer ringBuffer,
             VkDeviceSize ringBufferSize)
        : device_(device),
          queue_(queue),
          pool_(pool),
          timelineSemaphore_(timelineSemaphore),
          ringBuffer_(std::move(ringBuffer)),
          ringBufferSize_(ringBufferSize) {}

    // [Phase 4 Task 11] One rotating recorded-command-buffer slot -- see
    // the class comment's "COMMAND-BUFFER ROTATION" paragraph for why a
    // single reused buffer is no longer safe once flush() stops blocking.
    struct CmdSlot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        UploadTicket lastTicket{};
        bool everSubmitted = false;
    };

    // [Phase 4 Task 11] One entry in the ring-reclamation FIFO -- see the
    // class comment's "RING RECLAMATION" paragraph. `[start, end)` is the
    // half-open byte range, within one "lap" of the ring, that `ticket`'s
    // flush() call submitted staging/image copies out of.
    struct PendingRegion {
        UploadTicket ticket;
        VkDeviceSize start = 0;
        VkDeviceSize end = 0;
    };

    static constexpr size_t kInvalidSlot = static_cast<size_t>(-1);

    void beginRecordingIfNeeded();
    bool reserveRingSpace(VkDeviceSize size, VkDeviceSize& outOffset);
    void reclaimCompletedRegions();
    // VK_NULL_HANDLE (not an out-of-bounds index) when beginRecordingIfNeeded()
    // itself failed to allocate/begin a slot -- the caller (uploadToBuffer()/
    // uploadToImage()) still unconditionally records into whatever this
    // returns on that rare path, exactly like this class's pre-Task-11
    // behavior against its single `cmd_` member; a VK_NULL_HANDLE command
    // buffer fails loudly (validation error) rather than corrupting memory.
    VkCommandBuffer activeCmd() const {
        return activeSlot_ == kInvalidSlot ? VK_NULL_HANDLE : cmdSlots_[activeSlot_].cmd;
    }
    void destroyAll();

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkSemaphore timelineSemaphore_ = VK_NULL_HANDLE;
    uint64_t lastQueuedValue_ = 0;

    std::vector<CmdSlot> cmdSlots_;
    size_t activeSlot_ = kInvalidSlot;

    Buffer ringBuffer_;
    VkDeviceSize ringBufferSize_ = 0;
    VkDeviceSize ringCursor_ = 0;
    // Boundary between "already pushed into pendingRegions_ by a previous
    // flush()" and "written since then, not yet tracked" -- see flush()'s
    // own comment.
    VkDeviceSize lastFlushRingCursor_ = 0;
    std::deque<PendingRegion> pendingRegions_;
    uint32_t ringGeneration_ = 0;
    uint32_t blockingRingWaitCount_ = 0;

    bool recording_ = false;

    bool everUsedDirectPath_ = false;
    bool everUsedStagingPath_ = false;
    bool loggedDirectPathOnce_ = false;
    bool loggedStagingPathOnce_ = false;
};

}  // namespace rx::rhi
