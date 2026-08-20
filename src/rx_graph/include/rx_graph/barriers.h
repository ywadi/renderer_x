#pragma once
// Vulkan-Headers only -- same header-hygiene rule as resources.h (no volk,
// no vulkan/vulkan.h, no rx_rhi_vk): Task 2's barrier derivation is
// device-free, exactly like Task 1's compile() [Task 2 brief].
#include <vulkan/vulkan_core.h>

// [Task 2, gate ruling RC2] Needed for Subresource (ImageBarrier::
// subresource below) -- still device-free (resources.h itself is
// Vulkan-Headers-only, same rule this file already follows) and creates no
// cycle: resources.h has no dependency back on this header.
#include <rx_graph/resources.h>

#include <array>
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

    // [Task 2, gate ruling RC2] The RESOLVED subresource range this barrier
    // applies to -- default-constructed ("the whole resource") for every
    // resource this task does not extend to carry more than one mip/layer
    // (every existing barrier before this task effectively meant "the
    // whole resource" anyway, since every PhysicalResource had exactly one
    // mip/layer -- this field makes that implicit meaning explicit and lets
    // Executor build a real, narrower VkImageSubresourceRange for a
    // storage-image resource with more than one). See resources.h's
    // Subresource for the field shapes.
    Subresource subresource;
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

// Not API-stable: ResourceBarrierState/BarrierTransition/applyAccess below
// are exposed only so barriers.cpp's own unit tests and Task 3's executor
// can drive the per-resource state machine directly -- buildBarriers() is
// the one brief-specified, stable entry point. [Task 2 fix round 1,
// Important finding: this seam is real (buildBarriers() is genuinely built
// out of it, not scaffolding), but it was placed alongside the brief's
// locked interface with nothing marking it as a different stability tier;
// this namespace is that mark.]
namespace detail {

// Running invalidate/flush accounting for one physical resource, reset to
// this default-constructed state at the start of every buildBarriers()
// call [Task 2 brief ambiguity resolution D4: transients are
// discard-per-frame, so every resource starts each compile walk at
// UNDEFINED layout with empty flush/invalidate state -- cross-frame
// carry-over is Task 3's concern, not compile's].
//
// Faithfully re-expresses Granite's actual per-resource state
// (render_graph.hpp's `PipelineEvent`: `pipeline_barrier_src_stages`,
// `to_flush_access`, `invalidated_in_stage[64]`, `layout` -- see
// render_graph.cpp's `physical_pass_handle_invalidate_barrier()`/
// `need_invalidate()`/`physical_pass_handle_flush_barrier()`), not just
// the brief's one-paragraph paraphrase of it [Task 2 fix round 1, Critical
// finding: the first port collapsed two Granite fields that must stay
// separate -- see each field's own comment below for which Granite field
// it corresponds to and why collapsing it was wrong]. Granite's
// queue/semaphore/event machinery (`wait_graphics_semaphore`,
// `locked_invalidation`, ...) is still not ported: rx_graph has one queue
// and no cross-frame carry-over in Task 2, so there is nothing for that
// machinery to do here -- see barriers.cpp's own comment for exactly what
// was dropped and why.
struct ResourceBarrierState {
    VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool everAccessed = false;

    // Granite's `pipeline_barrier_src_stages`: the stage of the last real
    // write. Persists across reads -- nothing but a *new write* ever
    // changes it, which is exactly why it survives long enough to seed a
    // later read's barrier even after that read's own resolving barrier
    // has already cleared `pendingFlushAccess` to 0 below. Losing this
    // persistence (the first port's bug) is what let a second reader in a
    // never-covered pipeline stage fall through with zero barriers.
    VkPipelineStageFlags2 lastWriteStages = 0;

    // Granite's `to_flush_access`: the last write's own access, still
    // "not yet made available to anything" until any subsequent barrier
    // (read or write) resolves it -- then unconditionally 0, exactly like
    // Granite's `event.to_flush_access = 0` at the end of every invalidate
    // call, regardless of whether that call was for a read or a write.
    bool hasPendingFlush = false;
    VkAccessFlags2 pendingFlushAccess = 0;

    // Granite's `invalidated_in_stage[64]`: genuinely per-pipeline-stage
    // -bit record of which access bits are already visible *at that
    // specific stage* -- indexed by stage-bit position (bit 7 is
    // `VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT`'s slot, etc.), matching
    // `need_invalidate()`'s own per-bit loop. A reader in stage S1 being
    // invalidated must never be mistaken for stage S2 also being
    // invalidated -- the first port's single aggregate pair did exactly
    // that.
    std::array<VkAccessFlags2, 64> invalidatedInStage{};

    // The union of every stage bit with a nonzero `invalidatedInStage`
    // entry -- i.e. every stage that has *read* this resource since the
    // last write. Not a Granite field: Granite's own WAR/WAW barriers
    // chain off `pipeline_barrier_src_stages` (the last *write's* stage)
    // unconditionally, which -- traced against the actual source for this
    // fix round -- would let a write-after-multiple-different-stage-reads
    // barrier name only the original write's stage as its srcStage, doing
    // nothing to actually wait for those reads. The coordinator's
    // ambiguity resolution for WAR ("srcStage = the prior read stages")
    // and this fix round's required test both call for the *reads'*
    // stages instead, unioned across every reader since the last write --
    // this field is what makes that possible without rescanning all 64
    // `invalidatedInStage` entries on every write-after-read.
    VkPipelineStageFlags2 invalidatedStagesUnion = 0;
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
// execution order, and advances it -- the per-resource invalidate/flush
// accounting, re-derived against Granite's actual per-stage model [Task 2
// fix round 1]:
//   - a barrier is needed if the layout differs; or the access is a write
//     and any prior access exists (WAW/WAR need an execution dependency);
//     or the access is a read not yet covered by `invalidatedInStage` for
//     every one of its own declared stage bits (checked unconditionally,
//     not gated on `hasPendingFlush` -- a resolved flush does not mean
//     every stage has visibility, only that nothing is left to *flush*).
//   - a read's barrier (when needed) chains srcStage off `lastWriteStages`
//     (the persisted last-write stage, correct whether or not that write
//     has already been flushed to some *other* stage) and srcAccess off
//     `pendingFlushAccess` (0 if already flushed). A write's barrier
//     chains off `pendingFlushAccess`/`lastWriteStages` for WAW (something
//     is still unflushed), or off `invalidatedStagesUnion` with
//     srcAccess=0 for WAR (nothing is unflushed, but every reader since
//     the last write must still finish first).
//   - a write always ends by resetting `invalidatedInStage`/
//     `invalidatedStagesUnion` to empty and setting a fresh
//     `lastWriteStages`/`pendingFlushAccess`. A read that needed a barrier
//     ends by clearing `pendingFlushAccess` and folding (stage, access)
//     into `invalidatedInStage`/`invalidatedStagesUnion` for its own
//     stage bits only. A read that needed no barrier changes nothing.
// Returns the barrier to insert immediately before this access, or
// std::nullopt if none is needed (e.g. two consecutive reads in the same,
// already-covered stage with no intervening write, or a resource's very
// first write with no layout to transition from).
//
// Exposed here, not just as a buildBarriers() implementation detail,
// because it is also the smallest unit the WAW/WAR/multi-stage-read
// accounting rules can be verified against directly: RenderGraph::
// compile()'s own name-based dependency resolution (Task 1) always binds
// every reader of a declared resource name to that name's *final* declared
// writer, so a read can never itself be ordered ahead of a later write of
// the very same name through the public RenderGraph API -- see
// test_barriers.cpp's "war-execution-only" case (and this task's report)
// for the empirical proof. That is a genuine, deliberate Task 1
// simplification (one name is one physical resource for a compiled
// graph's lifetime, with no versioning), not a Task 2 defect to route
// around by contorting a pass graph -- so applyAccess() exists to let
// rules like this be exercised in isolation, against a synthetic access
// sequence, independent of whether any current RenderGraph pass topology
// can produce one end to end.
std::optional<BarrierTransition> applyAccess(ResourceBarrierState& state, bool isBuffer, VkPipelineStageFlags2 stages,
                                              VkAccessFlags2 access, VkImageLayout layout);

}  // namespace detail

// Derives every sync2 barrier RenderGraph::compile() needs to insert, from
// a CompiledGraph whose culling/ordering/resource-resolution phase (Task 1)
// has already run -- compile()'s last phase, after step 4 in
// render_graph.cpp populates executionOrder()/resources()/passAccesses().
// Device-free: reads `graph` purely through its public accessors, applies
// detail::applyAccess() once per physical resource touched at each
// execution-order position (merging every ResourceAccess a single pass
// declares against one physical resource first -- see barriers.cpp's
// combineByResource() for the same-pass-reads-and-writes-the-same-resource
// ambiguity resolution), and writes the backbuffer's PRESENT_SRC_KHR
// transition into `outFinalBarriers` once the walk reaches its last write.
//
// The returned vector has exactly one entry per position in
// `graph.executionOrder()` (not per raw pass index, unlike
// CompiledGraph::passAccesses()) -- entry `i` is the barriers to insert
// immediately before executionOrder()[i]'s pass runs. Called exactly once,
// from RenderGraph::compile(), which stores the result on its own
// CompiledGraph (passBarriers_/finalBarriers_) -- see render_graph.h.
std::vector<PassBarriers> buildBarriers(const CompiledGraph& graph, PassBarriers& outFinalBarriers);

}  // namespace rx::graph
