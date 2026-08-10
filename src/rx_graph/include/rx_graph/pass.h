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
    Pass& setExecute(std::function<void(PassContext&)> fn);

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
    };

    struct Declaration {
        AccessKind kind;
        std::string resourceName;
        AttachmentDesc attachment;  // meaningful for ColorOutput/DepthStencilOutput
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
    // used for this classification instead.
    [[nodiscard]] bool hasAttachmentOutput() const;

    // Resolves one declaration's VkPipelineStageFlags2/VkAccessFlags2/
    // VkImageLayout triple, per the Task 1 brief's compile algorithm step
    // 1. `computeClass` selects between the Compute-class and
    // Graphics-class storage-buffer stage rows of that table; irrelevant
    // to every other declaration kind, which has exactly one fixed row.
    static ResourceAccess resolveAccess(const Declaration& decl, uint32_t physicalIndex, bool computeClass);

    // True for a declaration kind that establishes/updates a resource
    // (color/depth-stencil/storage-buffer output); false for a read
    // (texture input, storage buffer input).
    static bool isWriteKind(AccessKind kind);

    std::string name_;
    QueueClass queue_ = QueueClass::Graphics;
    bool sideEffect_ = false;
    std::function<void(PassContext&)> execute_;
    std::vector<Declaration> declarations_;
};

}  // namespace rx::graph
