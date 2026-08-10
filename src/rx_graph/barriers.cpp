#include <rx_graph/barriers.h>
#include <rx_graph/render_graph.h>

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

// Task 2's barrier build: per-resource invalidate/flush state machine
// (ported from Granite's RenderGraph::build_physical_barriers -- see
// barriers.h's own comment on ResourceBarrierState/applyAccess for the
// exact re-derivation and what didn't carry over) applied once per
// physical resource, in CompiledGraph::executionOrder() order, over the
// resource-access data Task 1's compile() already resolved.
namespace rx::graph {

namespace {

// Access bits any declared ResourceAccess in this project can carry that
// represent a write [Task 1's Pass::resolveAccess table]. A write pass's
// pendingFlush only needs to remember these -- not e.g.
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

bool coveredByInvalidated(const ResourceBarrierState& state, VkPipelineStageFlags2 stages, VkAccessFlags2 access) {
    return (state.invalidatedStages & stages) == stages && (state.invalidatedAccess & access) == access;
}

// One pass's declared accesses, merged per physical resource. Almost
// always a 1:1 pass with a single ResourceAccess per resource; the merge
// only does real work for Task 2's ambiguity resolution "a resource both
// read and written by the same pass (e.g. depth output also declared
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

std::vector<std::pair<uint32_t, CombinedAccess>> combineByResource(std::span<const ResourceAccess> accesses) {
    std::vector<std::pair<uint32_t, CombinedAccess>> combined;
    for (const ResourceAccess& access : accesses) {
        auto it = std::find_if(combined.begin(), combined.end(),
                                [&](const auto& entry) { return entry.first == access.physicalIndex; });
        if (it == combined.end()) {
            combined.push_back({access.physicalIndex, CombinedAccess{access.stages, access.access, access.layout}});
            continue;
        }
        it->second.stages |= access.stages;
        it->second.access |= access.access;
        if (isWriteAccess(access.access) || it->second.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            it->second.layout = access.layout;
        }
    }
    return combined;
}

}  // namespace

std::optional<BarrierTransition> applyAccess(ResourceBarrierState& state, bool isBuffer, VkPipelineStageFlags2 stages,
                                              VkAccessFlags2 access, VkImageLayout layout) {
    const bool write = isWriteAccess(access);
    const bool layoutDiffers = !isBuffer && layout != state.currentLayout;
    const bool needBarrier = layoutDiffers || (write && state.everAccessed) ||
                              (!write && state.hasPendingFlush && !coveredByInvalidated(state, stages, access));

    std::optional<BarrierTransition> transition;
    if (needBarrier) {
        BarrierTransition t;
        if (state.hasPendingFlush) {
            // WAW, or the first read/write to make a still-unflushed write
            // visible: the barrier must wait on (and make available) that
            // write.
            t.srcStage = state.pendingFlushStages;
            t.srcAccess = state.pendingFlushAccess;
        } else if (write && state.everAccessed) {
            // WAR: the prior access was a read with nothing left to flush
            // -- an execution-only dependency [Task 2 brief ambiguity
            // resolution].
            t.srcStage = state.invalidatedStages;
            t.srcAccess = VK_ACCESS_2_NONE;
        } else {
            // First use ever.
            t.srcStage = VK_PIPELINE_STAGE_2_NONE;
            t.srcAccess = VK_ACCESS_2_NONE;
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
        state.pendingFlushStages = stages;
        state.pendingFlushAccess = access & kWriteAccessMask;
        state.hasPendingFlush = true;
        state.invalidatedStages = 0;
        state.invalidatedAccess = 0;
    } else if (needBarrier) {
        state.hasPendingFlush = false;
        state.pendingFlushStages = 0;
        state.pendingFlushAccess = 0;
        state.invalidatedStages |= stages;
        state.invalidatedAccess |= access;
    }
    // A read that needed no barrier changes nothing: either nothing was
    // pending (multiple reads never need to synchronize against each
    // other), or its visibility was already covered.

    state.everAccessed = true;
    return transition;
}

std::vector<PassBarriers> buildBarriers(const CompiledGraph& graph, PassBarriers& outFinalBarriers) {
    const std::span<const PhysicalResource> resources = graph.resources();
    std::vector<ResourceBarrierState> state(resources.size());  // per-frame: starts empty every call [Task 2 brief D4]

    const std::span<const uint32_t> order = graph.executionOrder();
    std::vector<PassBarriers> result(order.size());

    for (size_t pos = 0; pos < order.size(); ++pos) {
        PassBarriers& passBarriers = result[pos];
        for (const auto& [physicalIndex, combined] : combineByResource(graph.passAccesses(order[pos]))) {
            const PhysicalResource& resource = resources[physicalIndex];
            std::optional<BarrierTransition> transition =
                applyAccess(state[physicalIndex], resource.isBuffer, combined.stages, combined.access, combined.layout);
            if (!transition) {
                continue;
            }
            if (resource.isBuffer) {
                passBarriers.bufferBarriers.push_back(BufferBarrier{
                    physicalIndex, transition->srcStage, transition->srcAccess, transition->dstStage, transition->dstAccess});
            } else {
                passBarriers.imageBarriers.push_back(ImageBarrier{physicalIndex, transition->srcStage, transition->srcAccess,
                                                                    transition->dstStage, transition->dstAccess,
                                                                    transition->oldLayout, transition->newLayout});
            }
        }
    }

    // ---- finalBarriers(): the backbuffer's transition to PRESENT_SRC_KHR,
    // src = its last write's (stage, access) [Task 2 brief ambiguity
    // resolution]. That is exactly the resource's pendingFlush at the end
    // of the walk: nothing reads the backbuffer after its final writer
    // runs, so nothing ever resolves/clears it first.
    for (uint32_t i = 0; i < resources.size(); ++i) {
        if (!resources[i].isBackbuffer) {
            continue;
        }
        const ResourceBarrierState& s = state[i];
        outFinalBarriers.imageBarriers.push_back(ImageBarrier{i, s.pendingFlushStages, s.pendingFlushAccess,
                                                                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, s.currentLayout,
                                                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR});
        break;
    }

    return result;
}

}  // namespace rx::graph
