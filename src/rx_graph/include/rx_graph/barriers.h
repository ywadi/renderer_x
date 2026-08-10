#pragma once
// Vulkan-Headers only -- same header-hygiene rule as resources.h (no volk,
// no vulkan/vulkan.h, no rx_rhi_vk): Task 2's barrier derivation is
// device-free, exactly like Task 1's compile() [Task 2 brief].
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace rx::graph {

class CompiledGraph;  // forward declaration only -- buildBarriers() reads a
                       // CompiledGraph purely through its own public
                       // accessors (executionOrder()/resources()/
                       // passAccesses()); barriers.h never needs the full
                       // class definition, only barriers.cpp does (via
                       // render_graph.h), avoiding a header cycle with
                       // render_graph.h (which includes this header for
                       // PassBarriers).

// 1:1 payload for VkImageMemoryBarrier2, minus the real VkImage handle
// (device-free here -- Task 3's executor is what resolves `physicalIndex`
// to a live image and fills a real VkImageMemoryBarrier2 from these
// fields).
struct ImageBarrier {
    uint32_t physicalIndex;
    VkPipelineStageFlags2 srcStage;
    VkAccessFlags2 srcAccess;
    VkPipelineStageFlags2 dstStage;
    VkAccessFlags2 dstAccess;
    VkImageLayout oldLayout;
    VkImageLayout newLayout;
};

// 1:1 payload for VkBufferMemoryBarrier2, minus the real VkBuffer handle.
// Storage buffers never have layouts [Task 2 brief ambiguity resolution],
// so there is no oldLayout/newLayout pair here the way ImageBarrier has.
struct BufferBarrier {
    uint32_t physicalIndex;
    VkPipelineStageFlags2 srcStage;
    VkAccessFlags2 srcAccess;
    VkPipelineStageFlags2 dstStage;
    VkAccessFlags2 dstAccess;
};

// Every barrier to insert immediately before one pass (or, for
// CompiledGraph::finalBarriers(), immediately after the last surviving
// pass, before present).
struct PassBarriers {
    std::vector<ImageBarrier> imageBarriers;
    std::vector<BufferBarrier> bufferBarriers;
};

// Running invalidate/flush accounting for one physical resource, reset to
// this default-constructed state at the start of every buildBarriers()
// call [Task 2 brief ambiguity resolution D4: transients are
// discard-per-frame, so every resource starts each compile walk at
// UNDEFINED layout with empty flush/invalidate state -- cross-frame
// carry-over is Task 3's concern, not compile's].
//
// Ported from Granite's per-resource ResourceState
// (render_graph.cpp, RenderGraph::build_physical_barriers:
// initial_layout/final_layout/invalidated_types/flushed_types) --
// re-expressed as a single running accumulator per resource rather than
// Granite's per-physical-pass snapshot, since rx_graph has no subpass
// merging to reset state between (dynamic rendering has no equivalent
// concept -- see the Phase 3 render-graph research doc, §1/§5). Not
// copied: the field names, the single-flush-pair-instead-of-a-64-entry
// per-stage-bit array, and every rule below are this task's own
// re-derivation against rx_graph's types.
struct ResourceBarrierState {
    VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool everAccessed = false;

    // The most recent write's (stage, access) that no barrier has yet made
    // available to anything -- empty once a barrier resolves it.
    bool hasPendingFlush = false;
    VkPipelineStageFlags2 pendingFlushStages = 0;
    VkAccessFlags2 pendingFlushAccess = 0;

    // Which (stage, access) combinations are already known-visible,
    // accumulated by the barrier that last resolved pendingFlush; cleared
    // by the next write [Task 2 brief algorithm].
    VkPipelineStageFlags2 invalidatedStages = 0;
    VkAccessFlags2 invalidatedAccess = 0;
};

// The (src, dst, layout) fields one inserted barrier needs, independent of
// which physical resource or buffer-vs-image it applies to -- applyAccess()
// returns this; buildBarriers() attaches `physicalIndex` and picks
// ImageBarrier vs BufferBarrier from PhysicalResource::isBuffer.
// oldLayout/newLayout stay VK_IMAGE_LAYOUT_UNDEFINED for a buffer access,
// matching ResourceAccess::layout's own "meaningless for buffers" contract.
struct BarrierTransition {
    VkPipelineStageFlags2 srcStage = 0;
    VkAccessFlags2 srcAccess = 0;
    VkPipelineStageFlags2 dstStage = 0;
    VkAccessFlags2 dstAccess = 0;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Applies one declared (stages, access, layout) access to `state`, in
// execution order, and advances it -- the exact per-resource invalidate/
// flush accounting [Task 2 brief algorithm]:
//   - a barrier is needed if the layout differs; or the access is a write
//     and any prior access exists (WAW/WAR need an execution dependency;
//     WAR's srcAccess is 0); or the access is a read not yet covered by
//     `invalidated*` while a write is still pending flush.
//   - a write always ends by setting pendingFlush to its own (stage,
//     access) and clearing invalidated; a read that needed a barrier ends
//     by clearing pendingFlush and folding (stage, access) into
//     invalidated; a read that needed no barrier changes nothing.
// Returns the barrier to insert immediately before this access, or
// std::nullopt if none is needed (e.g. two consecutive reads with no
// intervening write, or a resource's very first write with no layout to
// transition from).
//
// Exposed at namespace scope, not just as a buildBarriers() implementation
// detail, because it is also the smallest unit the WAW/WAR accounting
// rules can be verified against directly: RenderGraph::compile()'s own
// name-based dependency resolution (Task 1) always binds every reader of a
// declared resource name to that name's *final* declared writer, so a
// read can never itself be ordered ahead of a later write of the very same
// name through the public RenderGraph API -- see test_barriers.cpp's
// "war-execution-only" case (and this task's report) for the empirical
// proof. That is a genuine, deliberate Task 1 simplification (one name is
// one physical resource for a compiled graph's lifetime, with no
// versioning), not a Task 2 defect to route around by contorting a pass
// graph -- so applyAccess() exists to let the WAR rule itself be exercised
// in isolation, against a synthetic access sequence, independent of
// whether any current RenderGraph pass topology can produce one end to
// end.
std::optional<BarrierTransition> applyAccess(ResourceBarrierState& state, bool isBuffer, VkPipelineStageFlags2 stages,
                                              VkAccessFlags2 access, VkImageLayout layout);

// Derives every sync2 barrier RenderGraph::compile() needs to insert, from
// a CompiledGraph whose culling/ordering/resource-resolution phase (Task 1)
// has already run -- compile()'s last phase, after step 4 in
// render_graph.cpp populates executionOrder()/resources()/passAccesses().
// Device-free: reads `graph` purely through its public accessors, applies
// applyAccess() once per physical resource touched at each execution-order
// position (merging every ResourceAccess a single pass declares against
// one physical resource first -- see barriers.cpp's combineByResource() for
// the same-pass-reads-and-writes-the-same-resource ambiguity resolution),
// and writes the backbuffer's PRESENT_SRC_KHR transition into
// `outFinalBarriers` once the walk reaches its last write.
//
// The returned vector has exactly one entry per position in
// `graph.executionOrder()` (not per raw pass index, unlike
// CompiledGraph::passAccesses()) -- entry `i` is the barriers to insert
// immediately before executionOrder()[i]'s pass runs. Called exactly once,
// from RenderGraph::compile(), which stores the result on its own
// CompiledGraph (passBarriers_/finalBarriers_) -- see render_graph.h.
std::vector<PassBarriers> buildBarriers(const CompiledGraph& graph, PassBarriers& outFinalBarriers);

}  // namespace rx::graph
