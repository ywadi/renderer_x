#include <rx_graph/barriers.h>
#include <rx_graph/render_graph.h>

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

// Task 2's barrier build: per-resource invalidate/flush state machine
// (ported from Granite's RenderGraph::build_physical_barriers /
// physical_pass_handle_invalidate_barrier / need_invalidate /
// physical_pass_handle_flush_barrier -- see barriers.h's own comment on
// detail::ResourceBarrierState/detail::applyAccess for the exact
// re-derivation and what didn't carry over) applied once per physical
// resource, in CompiledGraph::executionOrder() order, over the
// resource-access data Task 1's compile() already resolved.
//
// What Granite's model has that this port deliberately still does not:
// its `PipelineEvent` also carries `wait_graphics_semaphore`/
// `wait_compute_semaphore` and `locked_invalidation`, used to satisfy a
// dependency via a queue semaphore instead of a same-queue pipeline
// barrier, and to guard against double-processing one barrier. rx_graph
// has exactly one queue and calls detail::applyAccess() exactly once per
// resource per execution-order position (never re-entrantly), so neither
// concern exists yet -- Task 3's executor, if it ever needs cross-queue
// hand-off, is where that would be re-added, not here.
namespace rx::graph {

namespace detail {

namespace {

// Access bits any declared ResourceAccess in this project can carry that
// represent a write [Task 1's Pass::resolveAccess table]. A write's
// pendingFlushAccess only needs to remember these -- not e.g.
// DEPTH_STENCIL_ATTACHMENT_READ_BIT, which a depth-stencil output also
// carries but which never needs flushing (nothing was made dirty by
// reading it) [Task 2 brief: "WAW: barrier with srcAccess = prior write
// access"].
constexpr VkAccessFlags2 kWriteAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

bool isWriteAccess(VkAccessFlags2 access) {
    return (access & kWriteAccessMask) != 0;
}

// Granite's `Util::for_each_bit64` over a stage mask, re-expressed as a
// plain loop rather than ported bit-trick for bit-trick's sake -- called
// at most a handful of times per declared access, never a hot path.
template <typename Fn>
void forEachStageBit(VkPipelineStageFlags2 stages, Fn&& fn) {
    for (uint32_t bit = 0; bit < 64; ++bit) {
        if ((stages & (VkPipelineStageFlags2{1} << bit)) != 0) {
            fn(bit);
        }
    }
}

// Granite's `need_invalidate()` (render_graph.cpp ~L1748-1756): true if
// *any* of the requested stage bits does not yet have every requested
// access bit recorded as invalidated (visible) for that specific stage.
// Checking per-stage-bit, not against one aggregate, is exactly the fix
// this task's Critical finding required: a reader in stage S1 being
// invalidated must never be read back as "stage S2 is invalidated too."
bool coveredByInvalidated(const ResourceBarrierState& state, VkPipelineStageFlags2 stages, VkAccessFlags2 access) {
    bool covered = true;
    forEachStageBit(stages, [&](uint32_t bit) {
        if ((state.invalidatedInStage[bit] & access) != access) {
            covered = false;
        }
    });
    return covered;
}

}  // namespace

std::optional<BarrierTransition> applyAccess(ResourceBarrierState& state, bool isBuffer, VkPipelineStageFlags2 stages,
                                              VkAccessFlags2 access, VkImageLayout layout) {
    const bool write = isWriteAccess(access);
    const bool layoutDiffers = !isBuffer && layout != state.currentLayout;
    // Granite's need_sync = layout_change || (to_flush_access != 0) ||
    // need_invalidate(...), applied uniformly to both reads and writes in
    // physical_pass_handle_invalidate_barrier(). The write-specific
    // "(write && everAccessed)" disjunct below stands in for Granite's own
    // gate on whether to actually *emit* a barrier once need_sync is true
    // (`need_pipeline_barrier = event.pipeline_barrier_src_stages != 0`,
    // i.e. "only if some prior write exists to sync against") -- Task 1's
    // topology guarantees the first access to any resource is always a
    // write, so `everAccessed` and "a prior write exists" coincide here,
    // and folding need_invalidate's coverage check into the write branch
    // too would (wrongly) demand a barrier for a resource's very first
    // write, which has nothing to be invalidated against yet.
    const bool needBarrier =
        layoutDiffers || (write && state.everAccessed) || (!write && !coveredByInvalidated(state, stages, access));

    std::optional<BarrierTransition> transition;
    if (needBarrier) {
        BarrierTransition t;
        if (write) {
            if (state.hasPendingFlush) {
                // WAW: the prior write is still unflushed -- make it
                // available before this write's own incoming dependency.
                t.srcStage = state.lastWriteStages;
                t.srcAccess = state.pendingFlushAccess;
            } else if (state.everAccessed) {
                // WAR: nothing is unflushed, but every reader since the
                // last write must still finish -- srcStage is their union
                // [Task 2 fix round 1, Critical finding: not Granite's own
                // `pipeline_barrier_src_stages` here, which names only the
                // *write's* stage and would not wait on any reader at all;
                // see detail::ResourceBarrierState::invalidatedStagesUnion's
                // comment].
                t.srcStage = state.invalidatedStagesUnion;
                t.srcAccess = VK_ACCESS_2_NONE;
            } else {
                // First use ever.
                t.srcStage = VK_PIPELINE_STAGE_2_NONE;
                t.srcAccess = VK_ACCESS_2_NONE;
            }
        } else {
            // Read: chains off the persisted last-write source (Granite's
            // `pipeline_barrier_src_stages`), which survives a resolved
            // flush -- so a later read in a stage the first resolving read
            // never covered still gets a correct barrier instead of
            // silently falling through with none.
            t.srcStage = state.lastWriteStages;
            t.srcAccess = state.hasPendingFlush ? state.pendingFlushAccess : VK_ACCESS_2_NONE;
        }
        t.dstStage = stages;
        t.dstAccess = access;
        if (!isBuffer) {
            t.oldLayout = state.currentLayout;
            t.newLayout = layout;
        }
        transition = t;
    }

    if (!isBuffer) {
        state.currentLayout = layout;
    }

    if (write) {
        // Granite: a fresh write means every previously-invalidated stage
        // is stale (it granted visibility to the *old* contents), and
        // `pipeline_barrier_src_stages`/`to_flush_access` both reset to
        // this write's own (stage, access) [physical_pass_handle_flush_
        // barrier()].
        state.invalidatedInStage.fill(0);
        state.invalidatedStagesUnion = 0;
        state.lastWriteStages = stages;
        state.pendingFlushAccess = access & kWriteAccessMask;
        state.hasPendingFlush = true;
    } else if (needBarrier) {
        // Granite: `event.to_flush_access = 0` unconditionally after any
        // invalidate call, and `invalidated_in_stage[bit] |= barrier.access`
        // only for the bits this specific barrier's stages cover.
        state.hasPendingFlush = false;
        state.pendingFlushAccess = 0;
        forEachStageBit(stages, [&](uint32_t bit) { state.invalidatedInStage[bit] |= access; });
        state.invalidatedStagesUnion |= stages;
    }
    // A read that needed no barrier changes nothing: its own stage was
    // already fully covered, so there is nothing new to record.

    state.everAccessed = true;
    return transition;
}

}  // namespace detail

namespace {

// [Task 2, gate ruling RC2] The unit buildBarriers() now tracks ONE
// detail::ResourceBarrierState per, not one per physical resource -- see
// this file's own "subresource overlap validation" comment in
// render_graph.cpp for why identical-or-disjoint declared ranges are what
// makes independent per-key tracking correct. Every resource that predates
// this task resolves every one of its accesses to the exact same single
// Subresource (its whole, single-mip/single-layer shape -- see
// resources.h's ResourceAccess::subresource comment), so it still gets
// exactly one ResourceKey, byte-identical in effect to this file's former
// physicalIndex-only keying.
struct ResourceKey {
    uint32_t physicalIndex;
    Subresource subresource;

    bool operator==(const ResourceKey&) const = default;
};

// One pass's declared accesses, merged per (physical resource, subresource)
// key. Almost always a 1:1 pass with a single ResourceAccess per key; the
// merge only does real work for Task 2's ambiguity resolution "a resource
// both read and written by the same pass (e.g. depth output also declared
// sampled -- invalid combination) -- ... derive barriers from the union
// access at that pass; do not add new validation in this task": stages
// and access simply union, and a write's layout wins over a read's when a
// pass declares both for the same resource (the write is what actually
// establishes the pass's real image layout; Task 1 does not reject this
// combination, so this is "don't crash on it", not new validation).
struct CombinedAccess {
    VkPipelineStageFlags2 stages = 0;
    VkAccessFlags2 access = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

std::vector<std::pair<ResourceKey, CombinedAccess>> combineByResource(std::span<const ResourceAccess> accesses) {
    std::vector<std::pair<ResourceKey, CombinedAccess>> combined;
    for (const ResourceAccess& access : accesses) {
        const ResourceKey key{access.physicalIndex, access.subresource};
        auto it = std::find_if(combined.begin(), combined.end(), [&](const auto& entry) { return entry.first == key; });
        if (it == combined.end()) {
            combined.push_back({key, CombinedAccess{access.stages, access.access, access.layout}});
            continue;
        }
        it->second.stages |= access.stages;
        it->second.access |= access.access;
        if (detail::isWriteAccess(access.access) || it->second.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            it->second.layout = access.layout;
        }
    }
    return combined;
}

}  // namespace

std::vector<PassBarriers> buildBarriers(const CompiledGraph& graph, PassBarriers& outFinalBarriers) {
    const std::span<const PhysicalResource> resources = graph.resources();

    // [Task 2, RC2] Grown dynamically, keyed by (physicalIndex,
    // subresource) -- a linear scan is plenty at the tens-of-entries scale
    // a real compiled graph has (the same "plainest correct structure"
    // precedent this file's own sibling TransientPool::pinned_ already
    // follows, transient_pool.h). Per-frame: starts empty [Task 2 brief D4].
    std::vector<std::pair<ResourceKey, detail::ResourceBarrierState>> state;
    auto stateFor = [&state](const ResourceKey& key) -> detail::ResourceBarrierState& {
        for (auto& entry : state) {
            if (entry.first == key) {
                return entry.second;
            }
        }
        state.push_back({key, detail::ResourceBarrierState{}});
        return state.back().second;
    };

    const std::span<const uint32_t> order = graph.executionOrder();
    std::vector<PassBarriers> result(order.size());

    for (size_t pos = 0; pos < order.size(); ++pos) {
        PassBarriers& passBarriers = result[pos];
        for (const auto& [key, combined] : combineByResource(graph.passAccesses(order[pos]))) {
            const PhysicalResource& resource = resources[key.physicalIndex];
            detail::ResourceBarrierState& s = stateFor(key);
            std::optional<detail::BarrierTransition> transition =
                detail::applyAccess(s, resource.isBuffer, combined.stages, combined.access, combined.layout);
            if (!transition) {
                continue;
            }
            if (resource.isBuffer) {
                passBarriers.bufferBarriers.push_back(BufferBarrier{key.physicalIndex, transition->srcStage,
                                                                      transition->srcAccess, transition->dstStage,
                                                                      transition->dstAccess});
            } else {
                passBarriers.imageBarriers.push_back(ImageBarrier{
                    key.physicalIndex, transition->srcStage, transition->srcAccess, transition->dstStage,
                    transition->dstAccess, transition->oldLayout, transition->newLayout, key.subresource});
            }
        }
    }

    // ---- finalBarriers(): the backbuffer's transition to PRESENT_SRC_KHR,
    // src = its last write's (stage, access) [Task 2 brief ambiguity
    // resolution]. That is exactly the resource's pendingFlush at the end
    // of the walk: nothing reads the backbuffer after its final writer
    // runs, so nothing ever resolves/clears it first. The backbuffer is
    // always established via addColorOutput()/setDepthStencilOutput()
    // (never addStorageImageOutput()), so it is always single-mip/
    // single-layer -- every one of its accesses resolved to the same one
    // Subresource{0, 1, 0, 1} key throughout the walk above [Task 2, RC2].
    for (uint32_t i = 0; i < resources.size(); ++i) {
        if (!resources[i].isBackbuffer) {
            continue;
        }
        const ResourceKey key{i, Subresource{0, 1, 0, 1}};
        const detail::ResourceBarrierState& s = stateFor(key);
        outFinalBarriers.imageBarriers.push_back(ImageBarrier{i, s.lastWriteStages, s.pendingFlushAccess,
                                                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, s.currentLayout,
                                                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, key.subresource});
        break;
    }

    return result;
}

}  // namespace rx::graph
