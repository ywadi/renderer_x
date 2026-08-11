#pragma once
#include <rx_graph/resources.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rx::graph {

// The device-side handle Task 3's executor hands to a pass's recorded
// callback (real command buffer, resolved physical resources, ...).
// Defined in executor.h (rx_graph/executor.h), not here: pass.h/
// render_graph.h/resources.h/barriers.h stay device-free headers (no
// VkCommandBuffer, no volk, no rx_rhi_vk -- see render_graph.h's own
// header-hygiene comment), so only a forward declaration lives here.
// Pass::setExecute only stores the callback; Pass::invokeExecute() below
// is what actually calls it, once Executor::execute() has a real
// PassContext to hand it -- neither this header nor render_graph.cpp ever
// needs PassContext's complete definition themselves, since a
// std::function<void(PassContext&)> can be stored and invoked through a
// reference to an incomplete type.
class PassContext;

// Forward-declared purely to name it in Pass's `friend class Executor;`
// grant below -- pass.h itself never needs Executor's complete definition
// (executor.h), only the ability to say which class may call the private
// invokeExecute() a real PassContext requires. [Fix round 1, Important
// finding: without this, any code holding a const RenderGraph& could reach
// a Pass via RenderGraph::passAt() and call invokeExecute() with a
// default-constructed (executor_ == nullptr) PassContext -- see
// PassContext's own comment in executor.h for the matching half of this
// fix.]
class Executor;

class RenderGraph;

// A declarative description of one graph pass's resource reads/writes,
// queue hint, and (from Task 3 on) its recorded work. Constructed
// exclusively by RenderGraph::addPass() -- there is no public constructor
// -- and returned by reference so declarations chain fluently:
//
//   graph.addPass("forward")
//       .addTextureInput("shadow_map")
//       .addColorOutput("hdr", hdrDesc);
//
// RenderGraph owns every Pass for the lifetime of the graph (until
// RenderGraph::reset()) and hands out only this reference, never a copy.
class Pass {
public:
    // Declares a color attachment this pass writes. `name` is the virtual
    // resource's name: the first pass to declare a given name (as any kind
    // of output) establishes that resource; every later reference to the
    // same name -- read or write, in any pass -- refers to it.
    Pass& addColorOutput(std::string_view name, const AttachmentDesc& desc);

    // Declares the (at most one, by convention -- see pass.h's class
    // comment for why this is not enforced in Task 1) depth/stencil
    // attachment this pass writes.
    Pass& setDepthStencilOutput(std::string_view name, const AttachmentDesc& desc);

    // Declares a sampled read of a resource some earlier-established pass
    // wrote as an attachment. `name` must already have a writer somewhere
    // in the graph -- see RenderGraph::compile()'s validation.
    Pass& addTextureInput(std::string_view name);

    // Phase 4 Task 1: declares a sampled read of history resource `name`'s
    // PREVIOUS FRAME's contents -- NOT this frame's own setHistoryOutput()
    // write to the same name (there is no way to read that through this
    // API at all; a pass wanting to see its own frame's output writes a
    // DIFFERENT, ordinary name via addColorOutput()/setDepthStencilOutput()
    // and lets some other pass addTextureInput() that one instead -- see
    // this class's own "reading your own frame's write" note under
    // setHistoryOutput() below). `name` must have exactly one
    // setHistoryOutput() declarer somewhere in the graph (any pass, not
    // necessarily this one, and not necessarily before this declaration --
    // same declaration-order independence as addTextureInput()); compile()
    // throws, naming both the reading pass and `name`, if no pass declares
    // it. Resolves like addTextureInput() (FRAGMENT_SHADER or
    // COMPUTE_SHADER by this pass's own Graphics-/Compute-class split,
    // SHADER_SAMPLED_READ, SHADER_READ_ONLY_OPTIMAL) -- see
    // render_graph.cpp's Pass::resolveAccess.
    //
    // Deliberately creates NO same-frame ordering dependency on `name`'s
    // setHistoryOutput() writer (RenderGraph::compile() never adds a
    // dependsOn edge for this declaration kind, unlike every other read
    // kind) -- the read and this frame's write are two DIFFERENT pinned
    // physical images (ping-pong slots), so there is no same-frame data
    // hazard between them to order against; the real, necessary
    // synchronization is a cross-frame one Executor::execute() derives
    // from each pinned slot's own tracked state (executor.cpp), not
    // anything RenderGraph::compile()'s intra-frame DAG could express or
    // needs to.
    Pass& addHistoryInput(std::string_view name);

    // Phase 4 Task 1: declares `name` as a persistent (history) written
    // attachment -- color or depth/stencil, chosen from `desc.format`
    // exactly like addColorOutput() vs setDepthStencilOutput() would (see
    // render_graph.cpp's Pass::resolveAccess). Unlike every other output
    // declaration, this resource is backed by TWO pinned physical images
    // that ping-pong across frames (Executor's pinned pool,
    // transient_pool.h) rather than one discard-per-frame transient
    // allocation -- `name`'s contents, once written, survive into the
    // NEXT frame's addHistoryInput() read of the same name (a different
    // pass's declaration, or a future frame's), never this SAME frame's.
    //
    // At most one setHistoryOutput() declaration for a given `name` may
    // exist across the whole graph (compile() throws, naming `name` and
    // both offending passes, on a second one) -- unlike addColorOutput(),
    // which tolerates multiple writers of the same name via its
    // write-after-write chain, there is no such chain here: a ping-ponged
    // resource has exactly one writer's worth of "this frame's slot" to
    // fill. `name` also may not be used by any NON-history declaration
    // anywhere in the graph (addColorOutput/setDepthStencilOutput/
    // addTextureInput/addStorageBufferOutput/addStorageBufferInput) --
    // history names are their own namespace, and compile() rejects mixing
    // them with transient/buffer uses of the same name (naming `name`).
    //
    // A pass establishing `name` this way is never culled even if nothing
    // in THIS graph reads it (RenderGraph::compile()'s reachability walk
    // treats every setHistoryOutput()-declaring pass as an implicit root,
    // exactly like setSideEffect() -- see that method's own comment and
    // render_graph.cpp's cull step): its effect crosses the frame
    // boundary, so "nothing downstream reads it in this compiled graph" is
    // never a valid reason to discard it, unlike an ordinary dead-end
    // write.
    //
    // Per-frame load semantics stay VK_ATTACHMENT_LOAD_OP_CLEAR on this
    // pass's own first write each frame, exactly like any other attachment
    // output [Task 3 ambiguity resolution #3] -- a history resource's
    // pass is expected to fully rewrite its own write slot every frame it
    // runs (Phase 4 has no consumer needing partial/incremental history
    // updates), so there is no "preserve what was already in the write
    // slot" load-preserve mode in this phase. A future phase wanting that
    // (e.g. a pass that only updates part of a persistent buffer) would
    // need a dedicated load-preserve declaration this one deliberately
    // does not provide.
    Pass& setHistoryOutput(std::string_view name, const AttachmentDesc& desc);

    // Declares a storage buffer this pass writes (read-write, per the Task
    // 1 brief's stage/access table: a storage buffer *output* carries both
    // SHADER_STORAGE_WRITE and SHADER_STORAGE_READ access).
    Pass& addStorageBufferOutput(std::string_view name, const BufferDesc& desc);

    // Declares a storage buffer read of a resource some earlier-established
    // pass wrote via addStorageBufferOutput.
    Pass& addStorageBufferInput(std::string_view name);

    // Exempts this pass from culling: RenderGraph::compile() keeps it (and
    // everything it transitively depends on) even if nothing reads any
    // resource it writes -- e.g. a pass whose only job is a GPU readback or
    // a debug overlay with no downstream consumer.
    Pass& setSideEffect();

    // Stores the callback Task 3's executor will invoke with a real
    // PassContext once this pass's turn comes in CompiledGraph::
    // executionOrder(). Task 1 only stores it -- nothing here or in
    // RenderGraph::compile() calls it; PassContext is forward-declared
    // above precisely because no complete definition exists yet.
    //
    // Phase 4 Task 7 [spec D4 amendment, "Parallelism is the engine
    // default, not a mode"]: mutually exclusive with setExecuteChunked()
    // below -- a pass provides EITHER a whole-pass callback (this one; the
    // hand-written simple case the engine cannot itself split, since it
    // does not own the code inside it) OR a chunked one, never both. Throws
    // std::logic_error if this pass already has a setExecuteChunked()
    // callback stored -- a silent "last call wins" would let a caller
    // accidentally record two conflicting execution strategies for the
    // same pass with no diagnostic at all.
    Pass& setExecute(std::function<void(PassContext&)> fn);

    // Phase 4 Task 7 [spec D4 amendment; docs/threading.md "Parallelism is
    // the default, not a mode"]: stores the CHUNKED callback Executor::
    // execute() fans out across rx::task::Scheduler workers -- see that
    // header's own class comment for the worker-allowed thread-affinity
    // note this callback is held to (D5). Providing this callback IS the
    // parallel path: there is no separate opt-in flag and no
    // caller-chosen chunk count (the executor derives `chunkCount` from
    // the scheduler it was created with -- see executor.h's
    // detail::chunkCountForWorkerCount(), the documented derivation `fn`
    // can rely on being handed as its own `chunkCount` argument every
    // single time this pass runs, for the lifetime of one Executor).
    //
    // `fn` is called once per chunk index in [0, chunkCount). CHUNK 0 IS
    // GUARANTEED to run synchronously, on the SAME thread that called
    // Executor::execute() (this Executor's Scheduler's own main thread --
    // Executor::create()'s doc comment), to completion, BEFORE any other
    // chunk begins -- never concurrent with chunk 1..chunkCount-1 or with
    // itself. Chunks [1, chunkCount) fan out across the scheduler's
    // participating background workers (Scheduler::parallelFor()), POSSIBLY
    // CONCURRENTLY with each other (never with chunk 0, which has already
    // finished by then).
    //
    // This makes chunk 0 the one safe place a chunked pass may call a
    // main-thread-only API it cannot avoid needing once per frame
    // (BindlessTable registration, Uploader submissions,
    // MaterialSystem::loadMaterial()/getPipeline()/bindInstance(),
    // DeletionQueue -- docs/threading.md's "Main-thread-only" list;
    // discovered load-bearing, not merely convenient, while migrating
    // samples/06_materials' own forward pass: MaterialSystem::bindInstance()
    // resolves a material's pipeline AND streams its per-frame parameter
    // UBO in one call, main-thread-only with no split "resolve on main,
    // record anywhere" API to fall back on). Every OTHER chunk -- including
    // chunk 0 whenever a pass has no such constraint at all, e.g.
    // samples/05_multipass's and samples/07_stress's own forward passes --
    // must still touch no shared, unsynchronized state other than through
    // its own PassContext& (whose chunkCommandBuffer() below is exactly one
    // real VkCommandBuffer per chunk, safe to record into without any lock)
    // and must never call a main-thread-only API from anywhere but chunk 0.
    //
    // `fn` records exclusively through PassContext::chunkCommandBuffer()
    // (a secondary command buffer, one per chunk this frame) -- NOT
    // PassContext::cmd, which stays at its default VK_NULL_HANDLE for a
    // chunked pass's invocation (Executor::execute() never assigns it in
    // this path): a secondary command buffer inherits neither the
    // primary's bound pipeline/descriptor sets nor its dynamic state
    // (viewport/scissor -- core Vulkan secondary-buffer semantics, not an
    // engine restriction), so `fn` must set up everything its own chunk's
    // draws need from scratch, every invocation, exactly like a whole-pass
    // callback already does once per execute() call today.
    //
    // Mutually exclusive with setExecute() above -- throws std::logic_error
    // if this pass already has a whole-pass callback stored.
    Pass& setExecuteChunked(std::function<void(PassContext&, uint32_t chunkIndex, uint32_t chunkCount)> fn);

    [[nodiscard]] std::string_view name() const;
    [[nodiscard]] QueueClass queueClass() const;

private:
    friend class RenderGraph;
    friend class Executor;

    // Task 3: invokes this pass's recorded execute() callback (setExecute()
    // above) with a real PassContext, if one was ever stored -- a no-op
    // for a pass that never called setExecute() (e.g. a pass whose only
    // job is the sync Executor already emits from CompiledGraph::
    // passBarriers()/the dynamic-rendering clear it triggers, with nothing
    // further to record). Implemented in render_graph.cpp alongside Pass's
    // other methods.
    //
    // Private + friend-gated to Executor alone [Fix round 1, Important
    // finding]: a real PassContext is only ever safely constructible by
    // Executor (see executor.h's matching fix), so nothing outside this
    // library's own Executor should be able to invoke a pass's callback at
    // all -- a caller holding only a const RenderGraph& (RenderGraph::
    // passAt() is public and returns const Pass&) must not be able to
    // reach this. Declared here rather than free-standing in executor.cpp
    // because execute_ is itself a private member.
    void invokeExecute(PassContext& ctx) const;

    // Phase 4 Task 7: true if setExecuteChunked() (not setExecute()) was
    // called for this pass -- Executor::execute() branches its whole
    // per-pass recording strategy on this (parallel fan-out + secondary
    // command buffers vs. today's single whole-pass callback on the
    // primary). False for a pass with neither callback stored (a bare
    // barrier-only pass) exactly like invokeExecute() already no-ops for
    // that case.
    [[nodiscard]] bool hasChunkedExecute() const { return static_cast<bool>(executeChunked_); }

    // The chunked twin of invokeExecute() above -- same friend-gating
    // rationale, same no-op-if-never-set posture (never actually reached
    // with executeChunked_ unset: Executor::execute() only takes this path
    // when hasChunkedExecute() is already true).
    void invokeExecuteChunked(PassContext& ctx, uint32_t chunkIndex, uint32_t chunkCount) const;

    // Which builder method produced one Declaration -- drives both the
    // stage/access/layout resolution in RenderGraph::compile() (Task 1
    // brief, compile algorithm step 1) and the imageUsage union stored on
    // the resource's PhysicalResource entry.
    enum class AccessKind : uint8_t {
        ColorOutput,
        DepthStencilOutput,
        TextureInput,
        StorageBufferOutput,
        StorageBufferInput,
        // Phase 4 Task 1: see addHistoryInput()/setHistoryOutput() above.
        // Deliberately listed last (not interleaved next to the
        // TextureInput/ColorOutput/DepthStencilOutput kinds they otherwise
        // parallel) purely so every existing switch/if-chain over this
        // enum that was written before this task stays easy to diff
        // against -- no ordering significance beyond that.
        HistoryInput,
        HistoryOutput,
    };

    struct Declaration {
        AccessKind kind;
        std::string resourceName;
        AttachmentDesc attachment;  // meaningful for ColorOutput/DepthStencilOutput/HistoryOutput
        BufferDesc buffer;          // meaningful for StorageBufferOutput
    };

    Pass(std::string name, QueueClass queue);

    // These three helpers live here (private members of Pass), rather than
    // as free functions in render_graph.cpp, purely because of access
    // control: AccessKind and Declaration are private nested types, so
    // only Pass's own members (or its declared friend, RenderGraph) can
    // name them -- a free function in another translation-unit-local
    // namespace cannot, even inside the same .cpp file. RenderGraph::
    // compile() (the friend) is the only caller of all three.

    // True if this pass declares at least one color or depth/stencil
    // output -- RenderGraph::compile() uses this to classify a pass as
    // "Graphics-class" (this) vs "Compute-class" (neither): the Task 1
    // brief's storage-buffer stage table resolves differently for the two.
    // See resources.h's comment on QueueClass for why that hint is not
    // used for this classification instead. Phase 4 Task 1: a
    // setHistoryOutput() declaration counts as an attachment output here
    // too -- it IS one (a color or depth/stencil write, chosen by format;
    // see Pass::resolveAccess), just persistent instead of discarded.
    [[nodiscard]] bool hasAttachmentOutput() const;

    // Resolves one declaration's VkPipelineStageFlags2/VkAccessFlags2/
    // VkImageLayout triple, per the Task 1 brief's compile algorithm step
    // 1. `computeClass` selects between the Compute-class and
    // Graphics-class storage-buffer stage rows of that table; irrelevant
    // to every other declaration kind, which has exactly one fixed row.
    static ResourceAccess resolveAccess(const Declaration& decl, uint32_t physicalIndex, bool computeClass);

    // True for a declaration kind that establishes/updates a resource
    // (color/depth-stencil/storage-buffer output) AND that should
    // participate in RenderGraph::compile()'s cross-pass dependency-edge
    // graph (writersByName/dependsOn -- render_graph.cpp step 1); false
    // for a read (texture input, storage buffer input) AND, deliberately,
    // for HistoryOutput [Phase 4 Task 1]: a history resource's write
    // genuinely does establish/update it (hasAttachmentOutput() above and
    // Pass::resolveAccess both treat it as a real write), but it must NOT
    // feed the SAME-FRAME writersByName/dependsOn machinery that
    // addTextureInput()'s read-resolution walks -- a addHistoryInput()
    // read of the same name resolves against a separate,
    // dependency-edge-free lookup instead (render_graph.cpp's
    // historyOutputWritersByName), precisely so reading last frame's contents
    // never creates a same-frame ordering dependency on this frame's
    // writer (see addHistoryInput()'s own comment above for why that
    // dependency would be actively wrong, not just unnecessary).
    static bool isWriteKind(AccessKind kind);

    std::string name_;
    QueueClass queue_ = QueueClass::Graphics;
    bool sideEffect_ = false;
    std::function<void(PassContext&)> execute_;
    // Phase 4 Task 7 [spec D4 amendment].
    std::function<void(PassContext&, uint32_t, uint32_t)> executeChunked_;
    std::vector<Declaration> declarations_;
};

}  // namespace rx::graph
