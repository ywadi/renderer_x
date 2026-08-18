#include <doctest/doctest.h>
#include <rx_core/handle.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <utility>
#include <vector>

// [Phase 4 Task 10, spec D24(e), card #27, gate ruling 2026-08-18] Rows
// 13-15 of gate/matrix-issue27-memory-budget.md: the eviction INVARIANT
// contract this task owns (CONTRACT TEXT lives in memory_report.h's own
// "EVICTION CONTRACT" section -- read that first) plus the minimal
// synthetic evict->fallback->reclaim wiring test this task's own plan step
// calls for. Task 13 (Registry) and Task 19 (DrawListBuilder) implement
// the SAME clauses at real handle-resolve call sites; this file proves the
// mechanism composes over REAL rx::core::Handle<Tag>/HandlePool<Tag,T>
// (the D6 pattern) and a REAL rx::rhi::DeletionQueue -- no mock/stub of
// either -- with zero Vulkan device needed at all (pure CPU logic, exactly
// like deletion_queue_test.cpp's own first three pure-logic TEST_CASEs).

namespace {

struct FakeAssetTag {};
constexpr int kFallbackId = -1;

// A MINIMAL synthetic registry -- not rx::asset::Registry (Task 13's job),
// just enough machinery to exercise the three contract clauses end to end:
// a real HandlePool (generational staleness, clause 3's "one mechanism"),
// a parallel per-slot "resident" flag (the one genuinely-new bit clause 1/
// 2's deferred window needs -- see memory_report.h's own contract text for
// why this does not duplicate the generational mechanism), and a real
// DeletionQueue for the deferred reclaim (clause c).
class SyntheticEvictableRegistry {
public:
    rx::core::Handle<FakeAssetTag> acquire(int id) {
        auto handle = pool_.acquire(id);
        if (handle.index() >= resident_.size()) {
            resident_.resize(handle.index() + 1, true);
        }
        resident_[handle.index()] = true;
        return handle;
    }

    // Clause (1)/(2): resolve is defined for every handle state and makes
    // non-residency observable via the `resident` half of the returned
    // pair -- never a silent substitution, never UB.
    std::pair<int, bool> resolve(rx::core::Handle<FakeAssetTag> handle) {
        int* value = pool_.get(handle);  // nullptr on a generationally-stale/dangling handle -- the D6 mechanism,
                                          // reused as-is, unmodified (clause 3).
        if (value == nullptr) {
            return {kFallbackId, false};
        }
        if (handle.index() < resident_.size() && !resident_[handle.index()]) {
            return {kFallbackId, false};  // evicted, not yet reclaimed
        }
        return {*value, true};
    }

    // Clause (c): evict() immediately flips the resident bit (so (1)/(2)
    // hold starting this instant), then retires the REAL reclaim -- the
    // slot going back to the pool's free list, bumping its generation --
    // into `deletionQueue`, tagged with `frameIndex`. `destroyedCount` is
    // this test's stand-in for a real GPU-resource destructor (the D24(e)
    // contract text's "the underlying GPU resource's real destructor").
    void evict(rx::core::Handle<FakeAssetTag> handle, rx::rhi::DeletionQueue& deletionQueue, uint64_t frameIndex,
               int* destroyedCount) {
        if (handle.index() < resident_.size()) {
            resident_[handle.index()] = false;
        }
        deletionQueue.retire(
            [this, handle, destroyedCount] {
                ++(*destroyedCount);
                pool_.release(handle);
            },
            frameIndex);
    }

private:
    rx::core::HandlePool<FakeAssetTag, int> pool_;
    std::vector<bool> resident_;
};

}  // namespace

TEST_CASE("Eviction contract: resolve is residency-tolerant and observable across evict->fallback->reclaim, over "
          "a real HandlePool + a real DeletionQueue") {
    SyntheticEvictableRegistry registry;
    rx::rhi::DeletionQueue deletionQueue;
    int destroyedCount = 0;

    auto handle = registry.acquire(42);
    {
        auto [value, resident] = registry.resolve(handle);
        CHECK(value == 42);
        CHECK(resident);
    }

    // Clause (c) step 1: evict marks non-resident immediately but defers
    // the real reclaim behind the DeletionQueue.
    registry.evict(handle, deletionQueue, /*frameIndex=*/5, &destroyedCount);

    // Clauses (1)/(2): between eviction and reclaim, resolve is defined
    // and observably non-resident -- the fallback, never UB, never a
    // silent substitution with no signal.
    {
        auto [value, resident] = registry.resolve(handle);
        CHECK(value == kFallbackId);
        CHECK_FALSE(resident);
    }
    // Row 15's own wording: "not destroyed before the tagged frame's fence
    // signals".
    CHECK(destroyedCount == 0);

    deletionQueue.onFrameFenceSignaled(4);  // not yet due
    CHECK(destroyedCount == 0);
    {
        auto [value, resident] = registry.resolve(handle);
        CHECK(value == kFallbackId);
        CHECK_FALSE(resident);
    }

    deletionQueue.onFrameFenceSignaled(5);  // now due -- the real reclaim runs
    CHECK(destroyedCount == 1);

    // Clause (3): post-reclaim, the OLD handle is caught by the SAME
    // generational staleness check the D6 Handle<Tag>/HandlePool<Tag,T>
    // pattern already uses for an ordinary dangling handle -- no bespoke
    // "was this ever evicted" bookkeeping is consulted for this branch
    // (registry.resolve()'s `pool_.get(handle) == nullptr` path, exactly
    // the same code path an ordinary stale handle would hit).
    {
        auto [value, resident] = registry.resolve(handle);
        CHECK(value == kFallbackId);
        CHECK_FALSE(resident);
    }

    // Slot reuse: a brand-new handle for the reclaimed slot resolves
    // correctly to its own value, while the stale pre-eviction handle
    // continues to fail generationally -- handle-mediated references never
    // alias a reclaimed-and-reused slot (the concrete form clause (b)'s
    // "no raw index escapes" prohibition protects against).
    auto handle2 = registry.acquire(99);
    CHECK(handle2.index() == handle.index());
    CHECK(handle2.generation() != handle.generation());
    {
        auto [value, resident] = registry.resolve(handle2);
        CHECK(value == 99);
        CHECK(resident);
    }
    {
        auto [staleValue, staleResident] = registry.resolve(handle);
        CHECK(staleValue == kFallbackId);
        CHECK_FALSE(staleResident);
    }
}

TEST_CASE("Eviction contract: a resolve against a handle that was never evicted at all is unaffected by an "
          "unrelated slot's eviction (no cross-talk)") {
    SyntheticEvictableRegistry registry;
    rx::rhi::DeletionQueue deletionQueue;
    int destroyedCount = 0;

    auto keep = registry.acquire(7);
    auto evictMe = registry.acquire(8);

    registry.evict(evictMe, deletionQueue, /*frameIndex=*/1, &destroyedCount);
    deletionQueue.onFrameFenceSignaled(1);
    CHECK(destroyedCount == 1);

    auto [keptValue, keptResident] = registry.resolve(keep);
    CHECK(keptValue == 7);
    CHECK(keptResident);

    auto [evictedValue, evictedResident] = registry.resolve(evictMe);
    CHECK(evictedValue == -1);
    CHECK_FALSE(evictedResident);
}
