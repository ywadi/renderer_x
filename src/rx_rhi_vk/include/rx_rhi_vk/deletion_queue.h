#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace rx::rhi {

class DeletionQueue;

namespace detail {

// [Phase 4 Task 23, gate ruling #29] Test-only seam -- NOT part of the
// stable public contract, mirroring rx::graph::detail::debugChunkStats()/
// rx::scene::detail::capacitiesForTesting()'s own carve-out convention.
// Returns `queue`'s own private Item-vector `.capacity()` as of the moment
// it is called -- pendingCount() above only exposes `.size()`, which the
// zero-alloc regression proof for onFrameFenceSignaled()'s in-place
// compaction needs to distinguish from actual reallocation.
[[nodiscard]] size_t itemCapacityForTesting(const DeletionQueue& queue);

// [Phase 4 Task 23] `queue`'s own private Item-vector `.data()` pointer, as
// an opaque `const void*` (identity comparison only -- the pointed-to
// `Item` type is private to DeletionQueue, so no caller outside this class
// could dereference it anyway). A REAL, EMPIRICALLY VERIFIED limitation of
// `itemCapacityForTesting()` alone this task's own revert-probe found: for
// a queue whose pending-item COUNT is identical every call (this test's own
// steady-state shape), a buggy "build a fresh vector every call" version
// reserves/ends up at the exact SAME final capacity every time too (a
// from-empty `reserve(n)`/vector-of-size-n allocates EXACTLY capacity n,
// with no extra headroom to differ by) -- capacity alone cannot distinguish
// "genuinely reused storage" from "freshly reconstructed to an identical
// final size," so pointer identity is checked ALONGSIDE capacity as a
// second, independent signal (matching rx_scene::DrawListBuilder's own
// zero-alloc test precedent, draw_list_test.cpp, which checks both for the
// identical reason -- "capacity equality and .data() pointer identity are
// both checked... but neither is sufficient on its own"). Still not
// airtight on its own either (a freed-then-immediately-reallocated
// same-size block can legitimately land on the same address under some
// allocators/workloads) -- see this task's own report for the full,
// honest accounting of what this combined signal can and cannot prove
// without the global operator-new interposition the gate ruling bars for
// this binary's own volk/validation-layer linkage.
[[nodiscard]] const void* itemDataForTesting(const DeletionQueue& queue);

}  // namespace detail

// Fence-gated deferred-destruction queue [spec Fixed decision #9]. A
// resource retired while it might still be referenced by an in-flight
// command buffer must not actually be destroyed until the GPU has
// finished with it -- retire() tags a destructor closure with the frame
// index the resource was last used in; onFrameFenceSignaled(frameIndex)
// runs (and permanently removes) every retired destructor whose tagged
// frame index is <= the frame index the caller has just confirmed
// completed on the GPU.
//
// This class deliberately owns NO Vulkan state at all -- no VkDevice, no
// VkFence, nothing. It is a pure bookkeeping queue: the caller (a frame
// loop, or a headless test driving fences by hand) is the one that waits
// on real fences and decides when a given frame index is actually safe;
// DeletionQueue's only job is running the right closures at the right
// time, each exactly once. `frameIndex` here is meant to be a
// monotonically increasing counter that never repeats for the lifetime of
// whatever owns this queue -- rx::rhi::FrameSync::frameNumber() (added
// this task for exactly this purpose) is the intended source in
// production code, NOT FrameSync::currentFrameIndex(), which cycles mod
// FrameSync::framesInFlight() and therefore cannot disambiguate "frame 5"
// from "frame 7" once both land on the same frame-in-flight slot.
//
// Typical frame-loop usage:
//   deletionQueue.retire([tex = std::make_shared<Texture2D>(std::move(oldTexture))] {},
//                         frameSync.frameNumber());
//   ... later, once this frame's fence has been waited on ...
//   deletionQueue.onFrameFenceSignaled(frameSync.frameNumber());
//
// (The lambda's body can be empty when the destructor is really just "let
// this moved-in RAII object's own destructor run" -- retire() still calls
// it, which is harmless, and dropping the moved-in capture when the
// std::function itself is erased is what actually destroys the
// underlying Vulkan object either way.)
//
// STD::FUNCTION COPY-CONSTRUCTIBILITY PITFALL -- read before capturing a
// move-only resource (rx::rhi::Buffer, Texture2D, MeshBuffers, Uploader,
// ...) directly by move: `std::function<void()>`'s type-erased storage
// requires its target callable to be COPY-constructible, even though
// DeletionQueue itself never actually copies the std::function objects it
// stores. A lambda that captures a move-only object by move (e.g.
// `[buf = std::move(someBuffer)] {}`) has an implicitly-deleted copy
// constructor, which fails to compile the moment it is handed to
// `std::function<void()>` -- not a DeletionQueue bug, a real, standard
// `std::function` requirement. The standard fix -- used throughout this
// engine's own tests -- is to wrap the moved-in resource in a
// `std::shared_ptr` first (`std::make_shared<Buffer>(std::move(buffer))`),
// which IS copyable (the shared_ptr, not the Buffer inside it), and whose
// own destructor still runs the underlying Vulkan teardown exactly once
// when the last reference (here, always exactly one: the retired
// std::function itself) is dropped.
// Thread-affinity (D5, Phase 4): retire()/onFrameFenceSignaled()/flushAll()
// are main-thread-only -- see docs/threading.md. retire() (the
// enqueue-style mutator a chunked pass could plausibly reach mid-frame)
// carries a dev-time RX_ASSERT_MAIN_THREAD guard [Phase 4 Task 7 fix round
// 1] that fails loudly on a chunk >= 1 violation instead of corrupting
// items_ silently.
class DeletionQueue {
public:
    DeletionQueue() = default;
    DeletionQueue(DeletionQueue&&) noexcept = default;
    DeletionQueue& operator=(DeletionQueue&&) noexcept = default;
    DeletionQueue(const DeletionQueue&) = delete;
    DeletionQueue& operator=(const DeletionQueue&) = delete;

    // Destructor contract (load-bearing, not a style note, mirroring
    // FrameSync's own destructor contract in frame_sync.h): destroying a
    // DeletionQueue with anything still pending runs every remaining
    // captured RAII destructor as an ordinary side effect of this
    // object's std::vector<Item> member being destroyed -- WITHOUT ever
    // calling the destructor closures themselves and WITHOUT any device-
    // idle guarantee. That is almost never what a caller actually wants
    // (it is exactly the premature-destroy-while-in-flight bug this whole
    // class exists to prevent). Callers must call flushAll() (which does
    // provide a device-idle opportunity via `waitIdleFirst`) before
    // letting a non-empty DeletionQueue go out of scope. This destructor
    // cannot enforce that (it has no VkDevice to wait idle on), but it
    // does the next best thing: logs a loud RX_LOG_ERROR naming how many
    // destructors were never flushed, so the mistake is never silent.
    ~DeletionQueue();

    // Queues `destructor` to run once frame `frameIndex` has completed on
    // the GPU (per this class's own contract above -- see
    // onFrameFenceSignaled()). `destructor` may be empty-bodied if its
    // only purpose is to own a moved-in RAII capture (see the class
    // comment above).
    void retire(std::function<void()> destructor, uint64_t frameIndex);

    // Runs (in the order they were retired) and permanently removes every
    // retired destructor tagged with frameIndex <= `completedFrameIndex`.
    // Call this once per frame-loop iteration, immediately after
    // confirming (via vkWaitForFences/vkGetFenceStatus, outside this
    // class) that the fence for `completedFrameIndex` has actually
    // signaled -- this function trusts the caller's claim entirely and
    // never itself touches any Vulkan fence. Safe to call with nothing
    // pending, or with nothing yet due (no-op either way).
    void onFrameFenceSignaled(uint64_t completedFrameIndex);

    // Shutdown path: runs every retired destructor unconditionally,
    // regardless of its tagged frame index, then clears the queue.
    // `waitIdleFirst`, if non-empty, is invoked before running anything --
    // pass e.g. `[device] { vkDeviceWaitIdle(device); }` so this queue
    // never runs a destructor while the GPU could still be using the
    // resource it frees. Safe to call on an already-empty queue (still
    // invokes `waitIdleFirst` if provided, then no-ops).
    void flushAll(const std::function<void()>& waitIdleFirst = {});

    // Number of retired destructors not yet run. Test/diagnostic
    // accessor -- production frame-loop code has no need for it.
    size_t pendingCount() const { return items_.size(); }

private:
    friend size_t detail::itemCapacityForTesting(const DeletionQueue&);
    friend const void* detail::itemDataForTesting(const DeletionQueue&);

    struct Item {
        uint64_t frameIndex = 0;
        std::function<void()> destructor;
    };

    std::vector<Item> items_;
};

}  // namespace rx::rhi
