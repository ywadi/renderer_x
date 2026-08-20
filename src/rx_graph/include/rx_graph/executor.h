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
#include <rx_graph/pass_signature.h>
#include <rx_graph/render_graph.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace rx::rhi {
class Device;
}  // namespace rx::rhi

// Phase 4 Task 7 [spec D4 amendment]: only forward-declared here -- executor.h
// itself never calls a Scheduler method, only names a reference in
// Executor::create()'s signature and stores a pointer to it internally
// (executor.cpp, which does #include <rx_task/scheduler.h> for real).
// Thread-affinity (D5, Phase 4): see docs/threading.md -- Executor::create()
// must be called from `scheduler`'s own main thread (the thread that called
// rx::task::Scheduler::create()), and `scheduler` must outlive the Executor
// created against it (every execute() call fans chunked passes out through
// it).
namespace rx::task {
class Scheduler;
}  // namespace rx::task

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

// Phase 4 Task 7 [spec D4 amendment, "the executor derives chunk count from
// the scheduler"]: THE canonical chunk-count derivation for every chunked
// pass this Executor runs -- a pure function of `workerCount`
// (rx::task::Scheduler::workerCount(), i.e. background workers ONLY,
// excluding the participating main thread -- see scheduler.h's own doc
// comment on that exclusion) so both Executor::execute() and a caller
// wanting to predict/assert it (samples/07_stress's headless counter gate)
// share one source of truth, exactly like Scheduler::autoGrainSize() is
// itself a public, independently-callable pure function for the same
// reason.
//
// `min(workerCount, kMaxChunksPerPass)`: deliberately NOT `workerCount + 1`
// (that quantity instead sizes Executor's own per-thread command-pool
// coverage, below -- pools must cover every index parallelFor()'s
// workerIndex could ever report, INCLUDING the main thread's index 0, but
// the CHUNK COUNT itself does not need to also cover the main thread's own
// slot: `workerCount` background workers is already the number of
// recording streams that can usefully run concurrently while the main
// thread is elsewhere; the main thread MAY still end up grabbing one of
// those `workerCount` chunks via Scheduler::parallelFor()'s own
// calling-thread-participates behavior, which costs nothing extra to
// support since pool coverage already accounts for it). This is also
// EXACTLY why `--threads 1` (samples/07_stress's single-thread A/B
// baseline -- Scheduler::create(1)) is a genuine, unconditionally serial
// recording path: workerCount() == 1 resolves to chunkCountForWorkerCount()
// == 1, i.e. exactly one chunk exists that frame, so there is nothing for a
// second thread to steal regardless of which thread ends up running it.
//
// kMaxChunksPerPass = 16: a sane ceiling independent of how many hardware
// threads a machine reports, so a very high core-count box does not fan a
// single pass out into dozens of secondary command buffers -- each
// carrying its own vkBeginCommandBuffer/vkEndCommandBuffer/
// vkCmdExecuteCommands overhead -- for a benefit that stops scaling well
// past it (the threading research doc's own vkguide.dev citation: "10-1000
// draw calls per secondary; 2-10 secondaries per frame optimal" -- 16 is
// deliberately a little above that upper bound, leaving headroom for a
// future multi-chunked-pass frame without needing to be revisited, not a
// value tuned to sit inside it).
constexpr uint32_t kMaxChunksPerPass = 16;
[[nodiscard]] uint32_t chunkCountForWorkerCount(uint32_t workerCount);

// Test/debug-only seam -- NOT part of the stable public contract, same
// carve-out convention as debugLastFrameFinalStages() above and
// rx::task::detail::debugLastDroppedIoTaskCount() (scheduler.h). Snapshots
// this Executor's chunked-recording counters as of the moment it is
// called -- intended to be read right after an execute() call whose pass
// graph included at least one chunked pass.
struct ExecutorChunkDebugStats {
    // How many chunks the MOST RECENTLY EXECUTED chunked pass (across
    // every execute() call this Executor has ever made) fanned out to --
    // i.e. chunkCountForWorkerCount(scheduler.workerCount()) as of that
    // pass's own execution, held here so a caller does not need its own
    // copy of `scheduler` just to recompute it. 0 if this Executor has
    // never executed a graph containing a chunked pass.
    uint32_t lastChunkCount = 0;

    // Cumulative real vkAllocateCommandBuffers() calls this Executor has
    // made for chunk secondary command buffers, over its WHOLE lifetime --
    // see executor.cpp's own comment on the per-(frame-slot, thread-index)
    // command-buffer arena for why this is expected to reach a steady
    // state (stop growing) after at most kFramesInFlight execute() calls
    // that all exercise the same chunked pass(es): every later frame reuses
    // an already-allocated buffer from the same slot instead of allocating
    // a new one.
    uint64_t totalPoolAllocations = 0;
};
[[nodiscard]] ExecutorChunkDebugStats debugChunkStats(const Executor& executor);

// [Phase 4 Task 23, gate ruling #29] Test/debug-only seam -- NOT part of
// the stable public contract, same carve-out convention as
// debugLastFrameFinalStages()/debugChunkStats() above. Snapshots the
// `.capacity()` AND `.data()` pointer of every Impl-persistent scratch/
// tracking buffer Task 23 converted Executor::execute()'s own former
// per-call heap allocations into (executor.cpp) -- the bound zero-alloc-
// test methodology for this ticket (capacity-snapshot via a test-only
// accessor, NOT global operator-new interposition, which
// rx_graph_gpu_tests' own Vulkan/validation-layer/Tracy-rpmalloc linkage
// makes unsafe to install process-wide). `chunkBuffersScratch`/
// `validChunkBuffersScratch` are frame-slot indexed
// (rx::rhi::FrameSync::kFramesInFlight entries each), matching
// Executor::Impl's own [frameSlot] scratch shape.
//
// BOTH capacity and pointer identity are captured -- an EMPIRICALLY
// VERIFIED necessity, not belt-and-suspenders caution (this task's own
// revert-probe against the sibling DeletionQueue fix, see the task report):
// for a buffer whose final element COUNT is identical every call (this
// task's own "unchanged graph" steady-state premise), a buggy "freshly
// reconstruct every call" version can reach the exact same STABLE capacity
// every time too (a from-empty allocation of a fixed size N has no
// geometric-growth headroom to differ by), so capacity alone is
// insufficient; `.data()` pointer identity is the second, independent
// signal (mirroring rx_scene::DrawListBuilder's own zero-alloc test
// precedent, draw_list_test.cpp, which checks both for the identical
// reason). `*Data` fields are `const void*` (identity comparison only).
struct ExecutorAllocationCapacitiesForTesting {
    size_t firstBarrierSeen = 0;
    size_t attachmentEverWritten = 0;
    size_t touchedThisExecute = 0;
    size_t finalStageThisExecute = 0;
    size_t finalAccessThisExecute = 0;
    size_t colorPhysIdxScratch = 0;
    size_t colorAttachmentsScratch = 0;
    size_t vkImageBarriersScratch = 0;
    size_t vkBufferBarriersScratch = 0;
    size_t combinedAccessScratch = 0;
    size_t debugLabelScratch = 0;
    std::array<size_t, 2> chunkBuffersScratch{};
    std::array<size_t, 2> validChunkBuffersScratch{};

    // firstBarrierSeen/attachmentEverWritten/touchedThisExecute have no
    // pointer-identity companion field: all three are std::vector<bool>,
    // the bit-packed standard-library specialization with NO `.data()`
    // member at all (not merely inconvenient -- genuinely absent from the
    // standard interface) -- capacity is the only signal available for
    // them, an accepted, disclosed gap in coverage for exactly these three
    // fields (the matrix's own binding acceptance criterion for sites 1-2
    // names `std::vector<bool>` explicitly, so changing their element type
    // just to gain a `.data()` pointer is not this task's call to make).
    const void* finalStageThisExecuteData = nullptr;
    const void* finalAccessThisExecuteData = nullptr;
    const void* colorPhysIdxScratchData = nullptr;
    const void* colorAttachmentsScratchData = nullptr;
    const void* vkImageBarriersScratchData = nullptr;
    const void* vkBufferBarriersScratchData = nullptr;
    const void* combinedAccessScratchData = nullptr;
    const void* debugLabelScratchData = nullptr;
    std::array<const void*, 2> chunkBuffersScratchData{};
    std::array<const void*, 2> validChunkBuffersScratchData{};
};
[[nodiscard]] ExecutorAllocationCapacitiesForTesting allocationCapacitiesForTesting(const Executor& executor);

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

    // For a history-resource `name` (Pass::addHistoryInput()/
    // setHistoryOutput()), imageView()/image() resolve to THIS FRAME'S
    // READ SLOT -- the previous frame's write, i.e. exactly what a pass
    // declaring addHistoryInput() actually wants to sample. A pass
    // establishing `name` via setHistoryOutput() alone never needs to call
    // these for it at all: this frame's WRITE slot is bound automatically
    // as the pass's own color/depth attachment (Executor::execute(), same
    // as any other addColorOutput()/setDepthStencilOutput() resource) --
    // there is no name-based accessor for "the write slot" because nothing
    // in this codebase needs one; a pass callback interacts with its own
    // attachment purely through the dynamic-rendering scope it runs
    // inside, never by re-resolving its own output by name.
    [[nodiscard]] VkImageView imageView(std::string_view name) const;
    [[nodiscard]] VkImage image(std::string_view name) const;
    [[nodiscard]] VkBuffer buffer(std::string_view name) const;
    [[nodiscard]] VkFormat imageFormat(std::string_view name) const;

    // [Task 2 (#38), gate ruling RC2] Resolves to a VkImageView scoped
    // EXACTLY to the subresource (mip level + array-layer range) THIS
    // PASS declared against `name` via Pass::addStorageImageOutput()/
    // addStorageImageInput()'s own `subresource` argument -- as opposed to
    // imageView() above, which always resolves to a view covering the
    // resource's WHOLE mip/layer range regardless of what any one pass
    // declared. A storage-image binding always needs a real,
    // subresource-scoped VkImageView (Vulkan has no "bind the whole
    // resource, index a mip/layer range yourself" mode a RWTexture2D/
    // RWTexture2DArray could use the way a StructuredBuffer's own
    // byte-offset indexing lets buffer()'s whole-buffer handle work) --
    // this is why storage images need a SEPARATE resolver from
    // imageView(), not an overload of it: the two can legitimately return
    // DIFFERENT views for the SAME `name` (imageView() the whole resource,
    // this one THIS pass's own narrower declared range).
    //
    // Throws std::out_of_range if `name` was not declared as a
    // StorageImageOutput/StorageImageInput by THIS pass (a name declared
    // by a DIFFERENT pass, or declared here only as a TextureInput/
    // StorageBufferOutput/etc., is "not resolvable" from this call's own
    // narrow contract -- use imageView()/buffer() for those instead).
    [[nodiscard]] VkImageView storageImageView(std::string_view name) const;

    // Phase 4 Task 7 [spec D4 amendment]: the secondary command buffer THIS
    // CHUNK records into -- valid only from inside a Pass::setExecuteChunked()
    // callback (`cmd` above stays VK_NULL_HANDLE for that callback kind; use
    // this instead). Backed by a thread_local slot Executor::execute() sets
    // for the duration of each individual chunk invocation (executor.cpp) --
    // safe to call from any thread a chunked callback runs on, including
    // concurrently from several chunks' own threads at once, since each
    // thread only ever reads its OWN slot.
    //
    // Throws std::out_of_range if called from a whole-pass
    // (Pass::setExecute()) callback -- established resolver error style
    // (rx_graph: PassContext::<method>(): ..., same exception TYPE every
    // resolver above already throws, e.g. requireKind()'s "wrong kind of
    // resource" case in executor.cpp): a caller of this class only ever
    // needs to catch the one exception type it throws, whether the
    // complaint is "name not resolvable", "wrong resource kind", or (this
    // one) "wrong calling context".
    [[nodiscard]] VkCommandBuffer chunkCommandBuffer() const;

    // Phase 4 Task 1: false until `name`'s history resource has completed
    // one full ping-pong cycle -- i.e. until THIS frame's read slot has
    // actually been the target of a real Pass::setHistoryOutput() write in
    // some PAST execute() call, not merely the black-clear-at-creation
    // init every pinned slot gets before its first-ever use (see
    // transient_pool.h's PinnedHistorySlot::everWrittenByRealPass, the
    // exact value this resolves to). A pass reading addHistoryInput(name)
    // is expected to branch on this -- e.g. skip a temporal blend entirely
    // on its own first-ever frame, since the "previous frame" it would
    // blend against does not exist yet and this call only guarantees
    // DEFINED (black/1.0-cleared), not MEANINGFUL, content for that case.
    // `name` must be a history-resource name among CompiledGraph::
    // resources() -- throws std::out_of_range otherwise, matching every
    // other resolver's own "not resolvable" contract (a name that exists
    // but is NOT a history resource throws too, naming it, exactly like
    // buffer()/imageView() do for a wrong-kind name).
    [[nodiscard]] bool historyValid(std::string_view name) const;

    // Task 5 (rx_material): this pass's own attachment shape, exactly as
    // Executor::execute() classified it this call (colorAttachments/
    // depthPhysIdx/sample count -- see that function's own
    // isGraphicsPass/colorPhysIdx/depthPhysIdx locals, which this mirrors
    // into the device-free rx::graph::PassSignature value a material's
    // pipeline-variant cache key needs). Default-constructed
    // (colorCount == 0, depthFormat == VK_FORMAT_UNDEFINED,
    // samples == VK_SAMPLE_COUNT_1_BIT) for a pass with no attachment
    // output at all (a "bare" pass, per Executor::execute()'s own
    // graphics-vs-bare classification) -- there is no attachment shape to
    // report, and rx_material's MaterialSystem::getPipeline() rejects that
    // degenerate signature outright rather than trying to build a graphics
    // pipeline from it [Task 5 ambiguity resolution]. Returned by value
    // (not reference): PassSignature is a small, trivially-copyable value
    // type (see pass_signature.h), and every real caller (getPipeline()'s
    // request struct) needs its own copy to key a cache lookup with
    // anyway.
    [[nodiscard]] PassSignature passSignature() const { return passSignature_; }

private:
    friend class Executor;

    explicit PassContext(const Executor& executor) : executor_(&executor) {}

    const Executor* executor_;

    // Set directly by Executor::execute() (a friend) immediately after
    // constructing this PassContext, alongside cmd/renderArea above -- see
    // that function's own comment for exactly where and how it is derived.
    PassSignature passSignature_;
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
    //
    // Phase 4 Task 7 [spec D4 amendment, "Parallelism is the engine
    // default, not a mode"]: `scheduler` is REQUIRED, not optional --
    // there is no null-scheduler / parallelism-disabled construction path,
    // matching every other engine-owned-work call site's own "self-scaling,
    // never toggled" posture (docs/threading.md). Every chunked pass this
    // Executor ever runs (Pass::setExecuteChunked()) fans out through
    // `scheduler`, so `scheduler` must outlive this Executor, and this
    // Executor's own execute()/realize()/destructor calls must all happen
    // on `scheduler`'s own main thread (the thread that called
    // rx::task::Scheduler::create() -- see that header's class comment;
    // this Executor never itself calls parallelFor() from any other
    // thread).
    static std::unique_ptr<Executor> create(rx::rhi::Device& device, rx::task::Scheduler& scheduler);

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
    // Thread-affinity (D5, Phase 4): main-thread-only, [guarded] -- see
    // docs/threading.md.
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
    //
    // CALLER-DISCIPLINE BOUND, UNENFORCED [audit finding F10]: every cross-
    // frame safety argument this method relies on -- the chunk-pool reset
    // above, secondary-buffer reuse, and the deletion queue's own pacing
    // (executor.cpp's own comment on sweepStale()/onFrameFenceSignaled()
    // has the full reasoning) -- assumes at most ONE execute() call per
    // real, fence-bounded frame (i.e. the caller does not submit and start
    // a second execute() before confirming, via its own fence wait, that
    // an earlier frame's GPU work sharing the same frame-in-flight slot has
    // completed). Every in-tree caller already satisfies this (samples via
    // `FrameSync`'s fence-wait-then-record cycle; GPU tests via a
    // synchronous submit-and-wait between calls, which is strictly more
    // conservative than the bound requires) -- but nothing here asserts it,
    // so a future caller driving two graphs per real frame would reset
    // pools the GPU could still be reading.
    // Thread-affinity (D5, Phase 4): main-thread-only, [guarded] -- see
    // docs/threading.md.
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
    friend detail::ExecutorChunkDebugStats detail::debugChunkStats(const Executor&);
    friend detail::ExecutorAllocationCapacitiesForTesting detail::allocationCapacitiesForTesting(const Executor&);

    explicit Executor(std::unique_ptr<Impl> impl);

    [[nodiscard]] VkImageView resolveImageView(std::string_view name) const;
    [[nodiscard]] VkImage resolveImage(std::string_view name) const;
    [[nodiscard]] VkBuffer resolveBuffer(std::string_view name) const;
    [[nodiscard]] VkFormat resolveImageFormat(std::string_view name) const;
    // [Task 2 (#38), gate ruling RC2] Backs PassContext::storageImageView()
    // -- see that method's own doc comment.
    [[nodiscard]] VkImageView resolveStorageImageView(std::string_view name) const;
    [[nodiscard]] bool resolveHistoryValid(std::string_view name) const;
    // Phase 4 Task 7: backs PassContext::chunkCommandBuffer() -- see that
    // method's own doc comment.
    [[nodiscard]] VkCommandBuffer resolveChunkCommandBuffer() const;

    // Phase 4 Task 7: factored out of execute() purely for readability --
    // still a genuine Executor MEMBER function (not a free function in
    // executor.cpp's anonymous namespace) specifically so it keeps the
    // friend-granted access to Pass::hasChunkedExecute()/
    // invokeExecuteChunked() that a free function in the same .cpp would
    // NOT have: C++ access control is per-CLASS, not per-translation-unit
    // (see this header's own comment on why `struct Impl` above is public,
    // for the same underlying reason executor.cpp's other free helpers
    // need it). Fans `pass`'s chunked callback out across impl_->scheduler,
    // one call per chunk index, then stitches every chunk's own recorded
    // secondary into `cmd` via a single vkCmdExecuteCommands() call in
    // chunk-index order -- see executor.cpp's own comment on this method
    // for the full contract (dynamic-rendering wrapping for a graphics
    // pass, plain inheritance for a bare/compute one).
    void recordChunkedPass(const Pass& pass, PassContext& ctx, VkCommandBuffer cmd, bool isGraphicsPass,
                            VkRenderingInfo& renderingInfo, const PassSignature& signature, uint32_t frameSlot);

    std::unique_ptr<Impl> impl_;
};

}  // namespace rx::graph
