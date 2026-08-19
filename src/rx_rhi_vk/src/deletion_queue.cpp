#include <rx_rhi_vk/deletion_queue.h>
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <utility>

namespace rx::rhi {

namespace detail {
size_t itemCapacityForTesting(const DeletionQueue& queue) { return queue.items_.capacity(); }
const void* itemDataForTesting(const DeletionQueue& queue) { return queue.items_.data(); }
}  // namespace detail

DeletionQueue::~DeletionQueue() {
    if (!items_.empty()) {
        RX_LOG_ERROR(
            "DeletionQueue destroyed with {} destructor(s) never flushed -- call flushAll() before destruction; "
            "their captured resources are being destroyed now, with no device-idle guarantee",
            items_.size());
    }
}

void DeletionQueue::retire(std::function<void()> destructor, uint64_t frameIndex) {
    // [Phase 4 Task 7 fix round 1] Only the enqueue-style mutator is
    // guarded here -- onFrameFenceSignaled()/flushAll() below are the
    // per-frame drain/shutdown paths, out of this fix round's explicit
    // scope (docs/threading.md's own main-thread-only note for this class
    // covers all three; this guard specifically targets the one mutator a
    // chunk >= 1 caller could plausibly reach mid-frame).
    RX_ASSERT_MAIN_THREAD("DeletionQueue::retire");
    items_.push_back(Item{frameIndex, std::move(destructor)});
}

void DeletionQueue::onFrameFenceSignaled(uint64_t completedFrameIndex) {
    if (items_.empty()) {
        return;
    }

    // In-place compaction [Phase 4 Task 23, gate ruling #29 -- "SCOPE
    // GROWS": this non-empty-path fresh-vector allocation was a genuine,
    // repeatable per-call heap allocation whenever anything is retired,
    // folded into Task 23's zero-alloc scope even though this file lives
    // outside rx_graph/executor.cpp (Executor::execute() calls directly
    // into this function every call -- see executor.cpp's own
    // sweepStale()/onFrameFenceSignaled() comment)] -- replaces the old
    // "build a fresh `remaining` vector every call" two-pass with the
    // classic erase-remove idiom, its per-element side effect run BY HAND
    // rather than folded into the predicate: std::remove_if/std::erase_if's
    // own predicate is not guaranteed to be invoked exactly once per
    // element, which would make "run the destructor as a side effect of
    // the predicate" unsafe (the same reasoning the old comment here
    // already gave for avoiding that idiom -- still correct, just paired
    // with a different, allocation-free compaction mechanism now).
    // `writeIndex` tracks where the next SURVIVING item belongs;
    // `items_[readIndex]`'s destructor still runs exactly once, in
    // original retire() order, for every DUE item, and `items_`'s own
    // storage/capacity is reused in place -- never replaced -- so a queue
    // that always has something pending (the realistic "steady state" for
    // any scene with in-flight deferred resource retirement) makes this
    // function genuinely zero-alloc after its first call ever needed to
    // grow `items_` to its peak size.
    size_t writeIndex = 0;
    for (size_t readIndex = 0; readIndex < items_.size(); ++readIndex) {
        Item& item = items_[readIndex];
        if (item.frameIndex <= completedFrameIndex) {
            if (item.destructor) {
                item.destructor();
            }
        } else {
            if (writeIndex != readIndex) {
                items_[writeIndex] = std::move(item);
            }
            ++writeIndex;
        }
    }
    // Only ever shrinks `items_`'s SIZE (every survivor already occupies
    // [0, writeIndex) after the loop above) -- never its capacity, so this
    // is the same "no allocation on the shrink path" guarantee
    // std::vector::resize() always gives when the new size is <= the
    // current size.
    items_.resize(writeIndex);
}

void DeletionQueue::flushAll(const std::function<void()>& waitIdleFirst) {
    if (waitIdleFirst) {
        waitIdleFirst();
    }
    for (auto& item : items_) {
        if (item.destructor) {
            item.destructor();
        }
    }
    items_.clear();
}

}  // namespace rx::rhi
