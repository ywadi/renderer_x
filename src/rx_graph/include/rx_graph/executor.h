#pragma once
// Vulkan-Headers only, exactly like render_graph.h/pass.h/resources.h/
// barriers.h -- no volk.h, no vulkan/vulkan.h, no vk_mem_alloc.h anywhere in
// THIS header. VkCommandBuffer/VkImage/VkImageView/VkBuffer/VkFormat/
// VkExtent2D are all plain Vulkan *types* (handles are opaque pointers,
// carrying no function-pointer table themselves) -- exactly the same
// device-free-header discipline barriers.h's ImageBarrier/BufferBarrier
// already established for Task 2, just with real (caller-supplied or
// Executor-resolved) handles this time instead of a bare physicalIndex.
// Task 3 is the first thing in rx_graph that touches a real VkDevice, but
// "touches a real device" happens in executor.cpp/transient_pool.cpp, not
// in this public header: volk usage (and every raw vkFoo call) stays out
// of every rx_graph header, matching this task's own binding constraint.
#include <rx_graph/render_graph.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace rx::rhi {
class Device;
}  // namespace rx::rhi

namespace rx::graph {

class Executor;

namespace detail {

// Test/debug-only seam -- NOT part of the stable public contract, exactly
// the same carve-out convention barriers.h's own `detail::` namespace
// already establishes ("exposed only so barriers.cpp's own unit tests and
// Task 3's executor can drive the per-resource state machine directly").
// Returns whatever `executor` currently has tracked as `name`'s pooled
// entry's lastFrameFinalStages (Task 3 ambiguity resolution #2's cross-
// frame carry-forward value -- see transient_pool.h's PooledImage/
// PooledBuffer::lastFrameFinalStages) -- added [Fix round 1, Critical
// finding] because this codebase enables no Vulkan synchronization
// validation anywhere (see rx_rhi_vk/context.cpp), so no --validate run
// could ever catch a wrong value here on its own; a test needs to observe
// it directly instead. Throws std::out_of_range under the same conditions
// PassContext's own resolvers do (see that class's comment).
[[nodiscard]] VkPipelineStageFlags2 debugLastFrameFinalStages(const Executor& executor, std::string_view name);

}  // namespace detail

// The device-side handle Executor::execute() hands to a pass's recorded
// execute() callback (pass.h's Pass::setExecute), once that pass's turn
// comes up in CompiledGraph::executionOrder(). This is the complete
// definition pass.h's own forward declaration anticipates -- see that
// header's comment on class PassContext.
//
// `cmd`/`renderArea` are plain public data (not accessors) because every
// pass callback needs both unconditionally and there is nothing to
// validate or compute on read -- Executor::execute() fills them in fresh
// immediately before invoking Pass::invokeExecute() for each pass, and
// nothing about them is meaningful once that call returns.
//
// The four name-based resolvers below all resolve `name` against whatever
// Executor::realize() most recently bound that name to (a pooled transient
// resource, or -- for RenderGraph::setBackbufferSource()'s named resource
// -- the external image Executor::execute() was called with this frame;
// see executor.cpp's own comment on why the backbuffer is never pooled).
// `name` must be one of CompiledGraph::resources()' own names (i.e. a name
// some surviving pass in the currently-realized graph actually reads or
// writes) -- passing anything else throws std::out_of_range, matching
// CompiledGraph::passAccesses()'s own out-of-range contract for a raw pass
// index. Calling any of these before the owning Executor's first
// realize() (or with a graph reset() since) throws for the same reason:
// there is nothing bound to look up yet. Passing a valid name of the WRONG
// kind also throws std::out_of_range -- imageView()/image()/imageFormat()
// on a buffer-typed name, or buffer() on an image-typed one -- naming both
// the resource and its actual kind, rather than silently returning a
// meaningless VK_NULL_HANDLE/VK_FORMAT_UNDEFINED [Fix round 1 + its
// supplemental follow-up].
//
// Task 5 adds passSignature() here (material/pipeline binding metadata);
// not part of Task 3's scope.
//
// Not default-constructible, and constructible only by Executor [Fix
// round 1, Important finding]: an earlier revision left the implicit
// default constructor public, leaving `executor_` at its default
// (nullptr) -- combined with Pass::invokeExecute() also being public at
// the time, any code holding only a const RenderGraph& could construct a
// blank PassContext and invoke a pass's callback with it, crashing (null-
// pointer dereference through executor_) the moment that callback called
// any of the four resolvers below. Both halves of that gap are closed
// together: invokeExecute() is now private + friend-gated to Executor
// (pass.h), and this class's only constructor is private + friend-gated
// to Executor too, so a PassContext can never exist at all without a real,
// non-null Executor behind it.
class PassContext {
public:
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    // Extent of this pass's own color/depth attachment(s), in real pixels
    // -- meaningless (left at {0, 0}) for a pass that declares neither: see
    // Executor::execute()'s own comment on graphics- vs bare-pass
    // classification (a pass with no attachment output gets no dynamic-
    // rendering scope at all, and therefore has no "render area" to speak
    // of).
    VkExtent2D renderArea{0, 0};

    PassContext(const PassContext&) = delete;
    PassContext& operator=(const PassContext&) = delete;
    PassContext(PassContext&&) = delete;
    PassContext& operator=(PassContext&&) = delete;

    [[nodiscard]] VkImageView imageView(std::string_view name) const;
    [[nodiscard]] VkImage image(std::string_view name) const;
    [[nodiscard]] VkBuffer buffer(std::string_view name) const;
    [[nodiscard]] VkFormat imageFormat(std::string_view name) const;

private:
    friend class Executor;

    explicit PassContext(const Executor& executor) : executor_(&executor) {}

    const Executor* executor_;
};

// Owns every physical resource backing a compiled rx::graph::RenderGraph on
// a real device -- a pooled transient allocation per non-backbuffer image/
// buffer resource (transient_pool.h/.cpp, this library's private
// implementation detail, not a public rx_graph header) -- plus the sync2
// barrier emission and dynamic-rendering scoping that turn
// CompiledGraph::passBarriers()/passAccesses()/finalBarriers() into real
// vkCmdPipelineBarrier2/vkCmdBeginRendering calls. The RenderGraph/
// CompiledGraph types themselves stay exactly as device-free as Tasks 1-2
// left them -- Executor is an additional, separate object built ON TOP of
// a RenderGraph, never a change to what RenderGraph itself is [render_graph.h's
// own header comment: "Task 3's executor is the first thing in rx_graph
// that touches a real device," not the graph class].
//
// Not copyable or movable (owns a live VkDevice-scoped transient pool and
// deletion queue that a shallow copy/move could not safely duplicate or
// hand off) -- always held via the std::unique_ptr create() returns.
class Executor {
public:
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    Executor(Executor&&) = delete;
    Executor& operator=(Executor&&) = delete;

    // Builds an Executor against `device`'s already-created VkDevice/
    // VkPhysicalDevice/VkInstance/graphics queue (an internal
    // rx::rhi::Allocator, transient pool, and deletion queue -- see
    // executor.cpp). Returns nullptr (logged via RX_LOG_ERROR) on any
    // underlying failure (e.g. rx::rhi::Allocator::createRaw()).
    static std::unique_ptr<Executor> create(rx::rhi::Device& device);

    // Realizes (or re-realizes) every non-backbuffer PhysicalResource
    // `graph.compiled()` describes against this Executor's transient pool
    // -- one rx::rhi::Texture2D per image resource, one rx::rhi::Buffer per
    // buffer resource, keyed by (format, extent, usage, samples)/(size,
    // usage) respectively (transient_pool.h). Idempotent for an unchanged
    // compiled shape: calling this again with nothing structurally
    // different re-binds the exact same pooled entries. Safe (and
    // expected) to call again after any RenderGraph::compile() that
    // changed a resource's shape (e.g. a swapchain resize changing a
    // SwapchainRelative attachment's real extent) -- this Executor's
    // previous bindings are released back to the pool (not destroyed
    // outright; a matching future realize() can reclaim them, and an
    // unmatched one is eventually swept by the transient pool's own
    // frames-in-flight-bounded eviction -- see transient_pool.h) and
    // rebuilt from scratch against the new shapes. Must be called at
    // least once, after `graph`'s first compile(), before the first
    // execute() call against it.
    void realize(const RenderGraph& graph);

    // Records one frame's worth of work for `graph` onto `cmd` (an
    // already-allocated command buffer the caller has already begun
    // recording into -- Executor never itself calls
    // vkBeginCommandBuffer/vkEndCommandBuffer, nor submits or waits on
    // anything, exactly like rx::rhi::CommandContext::runOnce()'s own
    // `record` callback contract): for every surviving pass in
    // CompiledGraph::executionOrder(), in order -- one vkCmdPipelineBarrier2
    // for that position's CompiledGraph::passBarriers() entry (see
    // executor.cpp for the first-use-of-frame srcStage override this
    // applies to a pooled resource's very first barrier each call), an
    // optional vkCmdBeginRendering/vkCmdEndRendering scope when the pass
    // declares at least one color/depth attachment output (color:
    // VK_ATTACHMENT_LOAD_OP_CLEAR to {0,0,0,1} on that resource's first
    // write this call, VK_ATTACHMENT_LOAD_OP_LOAD after; depth: same rule,
    // clearing to 1.0f; always VK_ATTACHMENT_STORE_OP_STORE -- Task 3
    // ambiguity resolution #3, fixed for this phase; per-pass configurable
    // clear values are future work), and the pass's own recorded
    // execute() callback (Pass::invokeExecute(), given a real PassContext)
    // -- then one final vkCmdPipelineBarrier2 for
    // CompiledGraph::finalBarriers() once every pass has run.
    //
    // `graph` must already have been realize()d (directly, or via a prior
    // compile() + realize() call whose resource shapes this call's own
    // compile() didn't subsequently change) before this is called.
    // `backbufferImage`/`backbufferView`/`backbufferExtent` are the
    // caller-owned target for whichever resource
    // RenderGraph::setBackbufferSource() named -- never pooled by this
    // Executor, supplied fresh on every call (Task 3 ambiguity resolution
    // #4): a real swapchain image acquired this frame, or (as in Task 3's
    // GPU test) a caller-created offscreen image intended for readback
    // instead of presentation.
    void execute(const RenderGraph& graph, VkCommandBuffer cmd, VkImage backbufferImage, VkImageView backbufferView,
                 VkExtent2D backbufferExtent);

    // Forward-declared publicly (the struct body is defined, and only
    // ever meaningfully used, inside executor.cpp) purely so a handful of
    // free helper functions in executor.cpp's own anonymous namespace --
    // applyBarriers()/beginDebugLabel()/endDebugLabel(), none of which are
    // part of Executor's own interface and none of which any caller
    // outside this .cpp can ever actually name a real Impl instance to
    // call them with -- can take an `Impl&` parameter at all. C++ access
    // control is per-translation-unit-blind (a free function gets none of
    // a class's own member access just by living in the same .cpp file as
    // its out-of-line member definitions), so `Impl` itself must be
    // nameable from outside Executor's member functions for that pattern
    // to compile; nothing about making the mere NAME public exposes any
    // actual state -- create()/the constructor/impl_ below stay private,
    // so no caller can ever construct, copy, or reach a real Impl through
    // Executor's own public surface.
    struct Impl;

private:
    friend class PassContext;
    friend VkPipelineStageFlags2 detail::debugLastFrameFinalStages(const Executor&, std::string_view);

    explicit Executor(std::unique_ptr<Impl> impl);

    [[nodiscard]] VkImageView resolveImageView(std::string_view name) const;
    [[nodiscard]] VkImage resolveImage(std::string_view name) const;
    [[nodiscard]] VkBuffer resolveBuffer(std::string_view name) const;
    [[nodiscard]] VkFormat resolveImageFormat(std::string_view name) const;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rx::graph
