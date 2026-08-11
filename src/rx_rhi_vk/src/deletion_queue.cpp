#include <rx_rhi_vk/deletion_queue.h>
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <utility>

namespace rx::rhi {

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

    // Two-pass rather than an erase-remove-with-side-effects idiom: the
    // predicate in std::remove_if/std::erase_if is not guaranteed to be
    // invoked exactly once per element, which would make "call the
    // destructor as a side effect of the predicate" unsafe. Building a
    // fresh `remaining` vector, running due destructors in their original
    // retire() order along the way, keeps both guarantees ("runs each due
    // destructor exactly once" and "preserves order") simple and correct.
    std::vector<Item> remaining;
    remaining.reserve(items_.size());
    for (auto& item : items_) {
        if (item.frameIndex <= completedFrameIndex) {
            if (item.destructor) {
                item.destructor();
            }
        } else {
            remaining.push_back(std::move(item));
        }
    }
    items_ = std::move(remaining);
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
