#include <rx_graph/render_graph.h>

#include <rx_core/log.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// Front-half compile algorithm (Task 1 brief; re-implemented against this
// project's own types, not ported from Granite's traverse_dependencies/
// depend_passes_recursive [render_graph.cpp] -- see the Phase 3 design doc,
// decision D1, for why this is a from-scratch reimplementation guided by
// that source rather than a vendored port).
//
// compile() runs in four passes over the declared graph:
//   1. Collect every resource's writer list, across every declared pass in
//      addPass() call order, recording write-after-write chains as it goes
//      (Impl::collectWriters).
//   2. Resolve every read declaration against that writer list -- built
//      from the *whole* declaration set first, so a reader may be declared
//      before or after its writer; only the writer *list itself* is
//      ordered by declaration index (Impl::resolveReadEdges). This is also
//      where "reading a resource nobody wrote" is caught.
//   3. Cull: reachability from the backbuffer resource's final writer plus
//      every side-effect pass, walking the producer edges built in steps
//      1-2 backward (Impl::cull), then a stable topological sort
//      (Kahn's algorithm, ties broken by ascending declaration index --
//      the Task 1 brief asks for "no reordering heuristics", so this is
//      the simplest sort that still yields a deterministic order among
//      independent passes) (Impl::order). If Kahn's algorithm cannot
//      emit every reachable pass (fix round 1: a circular dependency --
//      trivially constructible since a reader may legally be declared
//      before its writer, step 2's whole point -- otherwise compiled to a
//      silently empty/degenerate graph with no diagnostic), a DFS over
//      the same producer edges finds one concrete cycle and compile()
//      throws, naming its passes.
//   4. Rescan only the surviving passes, in that execution order, to
//      resolve every PhysicalResource (name -> merged desc/imageUsage,
//      first/last use as *positions* in the execution order) and every
//      surviving pass's resolved ResourceAccess list (Impl::resolveResources).
namespace rx::graph {

struct RenderGraph::Impl {
    std::deque<Pass> passes;  // stable references: addPass() hands out Pass&
    std::string backbufferName;
    bool hasBackbuffer = false;
    CompiledGraph compiled;
};

// hasAttachmentOutput()/resolveAccess()/isWriteKind() are private members
// of Pass (declared in pass.h), not free functions here, because
// AccessKind/Declaration are private nested types of Pass: only Pass's own
// members or its declared friend (RenderGraph) may name them at all, and a
// free function in this .cpp's own namespace is neither, even though it
// lives in the same translation unit.

bool Pass::hasAttachmentOutput() const {
    for (const Declaration& decl : declarations_) {
        if (decl.kind == AccessKind::ColorOutput || decl.kind == AccessKind::DepthStencilOutput) {
            return true;
        }
    }
    return false;
}

ResourceAccess Pass::resolveAccess(const Declaration& decl, uint32_t physicalIndex, bool computeClass) {
    ResourceAccess access{};
    access.physicalIndex = physicalIndex;
    switch (decl.kind) {
        case AccessKind::ColorOutput:
            access.stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            access.access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            access.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            break;
        case AccessKind::DepthStencilOutput:
            access.stages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            access.access =
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            access.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            break;
        case AccessKind::TextureInput:
            // Fix round 1, mapping extension: the Task 1 brief's own table
            // gives texture-input access one fixed FRAGMENT_SHADER stage
            // with no Compute-class row (unlike the storage-buffer rows
            // below, which do split on pass kind) -- a real gap the Task 1
            // review flagged, since compute shaders can legally sample a
            // texture (OpImageSample) too. Coordinator ruling: extend the
            // same Compute-class/Graphics-class split already applied to
            // storage buffers to this case as well; access/layout are
            // identical either way, only the stage changes.
            access.stages =
                computeClass ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            access.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            access.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            break;
        case AccessKind::StorageBufferOutput:
            access.stages = computeClass ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                          : (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
            access.access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            access.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            break;
        case AccessKind::StorageBufferInput:
            access.stages = computeClass ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                          : (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
            access.access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            access.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            break;
    }
    return access;
}

bool Pass::isWriteKind(AccessKind kind) {
    return kind == AccessKind::ColorOutput || kind == AccessKind::DepthStencilOutput ||
           kind == AccessKind::StorageBufferOutput;
}

RenderGraph::RenderGraph() : impl_(std::make_unique<Impl>()) {
    rx::core::log::init();
}

RenderGraph::~RenderGraph() = default;

Pass& RenderGraph::addPass(std::string_view name, QueueClass queue) {
    // Constructed here (a friend context of Pass) and moved into the
    // deque via its implicitly-generated (public, since Pass declares no
    // custom copy/move members) move constructor -- push_back never needs
    // friend access to Pass's private named-parameter constructor itself.
    Pass pass(std::string(name), queue);
    impl_->passes.push_back(std::move(pass));
    return impl_->passes.back();
}

void RenderGraph::setBackbufferSource(std::string_view name) {
    impl_->backbufferName = std::string(name);
    impl_->hasBackbuffer = true;
}

void RenderGraph::compile(const CompileInfo& info) {
    Impl& g = *impl_;

    if (!g.hasBackbuffer) {
        RX_LOG_ERROR("rx_graph: RenderGraph::compile() called without a backbuffer source");
        throw std::runtime_error(
            "rx_graph: RenderGraph::compile() called without a backbuffer source; call setBackbufferSource() first");
    }

    const auto passCount = static_cast<uint32_t>(g.passes.size());

    // ---- duplicate pass names -------------------------------------------
    {
        std::unordered_set<std::string_view> seenNames;
        for (const Pass& pass : g.passes) {
            if (!seenNames.insert(pass.name_).second) {
                RX_LOG_ERROR("rx_graph: duplicate pass name '{}'", pass.name_);
                throw std::runtime_error("rx_graph: duplicate pass name '" + pass.name_ + "'");
            }
        }
    }

    // ---- step 1: collect writers, across the WHOLE declaration set -----
    // Ascending declaration index per resource name is guaranteed by
    // iterating passes 0..passCount-1 in order and appending as writes are
    // found -- no sort needed. write-after-write chains (a later-declared
    // writer depends on the immediately preceding one for the same name)
    // are recorded here too, per this task's ambiguity resolution: "the
    // later-declared pass depends on the earlier writer".
    std::unordered_map<std::string, std::vector<uint32_t>> writersByName;
    std::vector<std::vector<uint32_t>> dependsOn(passCount);  // dependsOn[p] = producers p depends on

    for (uint32_t p = 0; p < passCount; ++p) {
        const Pass& pass = g.passes[p];
        for (const Pass::Declaration& decl : pass.declarations_) {
            if (!Pass::isWriteKind(decl.kind)) {
                continue;
            }
            std::vector<uint32_t>& writers = writersByName[decl.resourceName];
            // Guard against a self-edge when the same pass declares the
            // same resource as a write more than once (e.g. two
            // addColorOutput() calls for the same name within one pass) --
            // a pass trivially happens after itself, so there is nothing
            // to chain and no second writer-list entry to add.
            if (writers.empty() || writers.back() != p) {
                if (!writers.empty()) {
                    dependsOn[p].push_back(writers.back());
                }
                writers.push_back(p);
            }
        }
    }

    // ---- step 2: resolve reads against the complete writer lists -------
    // Order-independent by construction: every writer, regardless of
    // whether it was declared before or after the pass reading it, is
    // already in writersByName from step 1 above.
    for (uint32_t p = 0; p < passCount; ++p) {
        const Pass& pass = g.passes[p];
        for (const Pass::Declaration& decl : pass.declarations_) {
            if (decl.kind != Pass::AccessKind::TextureInput && decl.kind != Pass::AccessKind::StorageBufferInput) {
                continue;
            }
            auto it = writersByName.find(decl.resourceName);
            if (it == writersByName.end() || it->second.empty()) {
                RX_LOG_ERROR("rx_graph: pass '{}' reads resource '{}', which no pass writes", pass.name_,
                             decl.resourceName);
                throw std::runtime_error("rx_graph: pass '" + pass.name_ + "' reads resource '" + decl.resourceName +
                                          "', which no pass writes");
            }
            // A read depends on the resource's final declared writer: the
            // write-after-write chain built in step 1 already orders every
            // earlier writer strictly before that one, so depending on the
            // last entry transitively depends on the whole chain.
            uint32_t writer = it->second.back();
            if (writer != p) {
                dependsOn[p].push_back(writer);
            }
        }
    }

    // ---- backbuffer resource must have a writer -------------------------
    auto backbufferWriters = writersByName.find(g.backbufferName);
    if (backbufferWriters == writersByName.end() || backbufferWriters->second.empty()) {
        RX_LOG_ERROR("rx_graph: backbuffer source resource '{}' is never written by any pass", g.backbufferName);
        throw std::runtime_error("rx_graph: backbuffer source resource '" + g.backbufferName +
                                  "' is never written by any pass");
    }
    const uint32_t backbufferWriterPass = backbufferWriters->second.back();

    // ---- step 3a: cull -- reachability from the roots -------------------
    // Roots: the backbuffer's final writer, plus every side-effect pass
    // [Task 1 brief, compile algorithm step 2]. Walking dependsOn edges
    // backward from a root marks every pass it (transitively) depends on
    // as reachable; anything never reached is culled.
    std::vector<bool> reachable(passCount, false);
    uint32_t reachableCount = 0;
    std::vector<uint32_t> stack;
    auto markReachable = [&](uint32_t p) {
        if (!reachable[p]) {
            reachable[p] = true;
            ++reachableCount;
            stack.push_back(p);
        }
    };
    markReachable(backbufferWriterPass);
    for (uint32_t p = 0; p < passCount; ++p) {
        if (g.passes[p].sideEffect_) {
            markReachable(p);
        }
    }
    while (!stack.empty()) {
        uint32_t p = stack.back();
        stack.pop_back();
        for (uint32_t producer : dependsOn[p]) {
            markReachable(producer);
        }
    }

    // ---- step 3b: stable topological order of survivors -----------------
    // Kahn's algorithm: `ready` always holds every reachable pass whose
    // producers have all already been emitted, and a std::set keeps it
    // sorted by ascending raw pass index -- the brief's "preserve
    // declaration order among independents" tie-break, with no additional
    // reordering heuristic.
    std::vector<uint32_t> indegree(passCount, 0);
    std::vector<std::vector<uint32_t>> successors(passCount);
    for (uint32_t p = 0; p < passCount; ++p) {
        if (!reachable[p]) {
            continue;
        }
        for (uint32_t producer : dependsOn[p]) {
            // Every producer of a reachable pass is itself reachable: the
            // DFS above visits producer edges from every marked pass, so
            // this can never be false; asserting it structurally here (via
            // the loop condition below rather than a runtime check) keeps
            // `successors`/`indegree` scoped to the surviving subgraph only.
            ++indegree[p];
            successors[producer].push_back(p);
        }
    }
    std::set<uint32_t> ready;
    for (uint32_t p = 0; p < passCount; ++p) {
        if (reachable[p] && indegree[p] == 0) {
            ready.insert(p);
        }
    }
    std::vector<uint32_t> executionOrder;
    executionOrder.reserve(passCount);
    while (!ready.empty()) {
        uint32_t p = *ready.begin();
        ready.erase(ready.begin());
        executionOrder.push_back(p);
        for (uint32_t successor : successors[p]) {
            if (--indegree[successor] == 0) {
                ready.insert(successor);
            }
        }
    }

    // ---- step 3c: cycle detection (fix round 1) --------------------------
    // Kahn's algorithm above emits a reachable pass exactly when every one
    // of its dependencies has already been emitted; over an acyclic
    // reachable subgraph that invariant guarantees every reachable pass
    // eventually gets emitted. If it didn't -- fewer entries in
    // executionOrder than reachable passes -- some reachable passes are
    // stuck behind an unresolved dependency cycle. Without this check,
    // compile() previously fell straight through to step 4 with a
    // silently truncated (possibly empty) executionOrder and no
    // diagnostic anywhere -- exactly the "typo swaps which resource two
    // passes read/write" landmine flagged in the Task 1 review.
    //
    // A cycle can only involve reachable passes (dependsOn[p] for any
    // reachable p contains only reachable producers -- see the DFS
    // reachability walk above), so a plain DFS with a recursion-stack
    // marker restricted to the reachable subgraph, starting from any
    // unvisited reachable pass, is guaranteed to walk into one: when it
    // follows a producer edge back to a pass still on its own recursion
    // stack, that stack's suffix from the revisited pass onward *is* a
    // concrete cycle -- named in the exception, not just "some pass never
    // ran" (which could equally be a pass merely downstream of the cycle,
    // not a member of it).
    if (executionOrder.size() != reachableCount) {
        std::vector<uint8_t> visitState(passCount, 0);  // 0 = unvisited, 1 = on the current DFS path, 2 = done
        std::vector<uint32_t> path;
        uint32_t cycleStart = passCount;  // sentinel; set once a back-edge is found

        std::function<bool(uint32_t)> visit = [&](uint32_t p) -> bool {
            visitState[p] = 1;
            path.push_back(p);
            for (uint32_t producer : dependsOn[p]) {
                if (visitState[producer] == 1) {
                    cycleStart = producer;
                    return true;
                }
                if (visitState[producer] == 0 && visit(producer)) {
                    return true;
                }
            }
            visitState[p] = 2;
            path.pop_back();
            return false;
        };

        for (uint32_t p = 0; p < passCount && cycleStart == passCount; ++p) {
            if (reachable[p] && visitState[p] == 0) {
                visit(p);
            }
        }

        // cycleStart == passCount would mean the size mismatch above was
        // not actually caused by a cycle -- structurally impossible given
        // Kahn's algorithm's invariant, but if it ever happened this
        // falls through to a generic message rather than indexing
        // `path` with the sentinel.
        std::string cycleDescription;
        if (cycleStart != passCount) {
            auto cycleBegin = std::find(path.begin(), path.end(), cycleStart);
            for (auto it = cycleBegin; it != path.end(); ++it) {
                if (!cycleDescription.empty()) {
                    cycleDescription += " -> ";
                }
                cycleDescription += "'" + g.passes[*it].name_ + "'";
            }
            cycleDescription += " -> '" + g.passes[cycleStart].name_ + "'";
        } else {
            cycleDescription = "(unable to isolate a specific cycle -- this should not happen)";
        }

        RX_LOG_ERROR("rx_graph: dependency cycle detected among passes: {}", cycleDescription);
        throw std::runtime_error("rx_graph: dependency cycle detected among passes: " + cycleDescription);
    }

    // ---- step 4: resolve physical resources + per-pass accesses --------
    // Rescans only the survivors, in execution order, so a resource
    // touched exclusively by culled passes never gets a PhysicalResource
    // entry at all (culling test: "x" disappears entirely when its only
    // writer, A, is culled).
    CompiledGraph compiled;
    compiled.executionOrder_ = executionOrder;
    compiled.culled_.assign(passCount, true);
    for (uint32_t p : executionOrder) {
        compiled.culled_[p] = false;
    }
    compiled.passAccesses_.assign(passCount, {});

    std::unordered_map<std::string, uint32_t> physicalIndexByName;
    for (uint32_t pos = 0; pos < executionOrder.size(); ++pos) {
        const uint32_t rawIndex = executionOrder[pos];
        const Pass& pass = g.passes[rawIndex];
        const bool computeClass = !pass.hasAttachmentOutput();
        std::vector<ResourceAccess>& accesses = compiled.passAccesses_[rawIndex];
        accesses.reserve(pass.declarations_.size());

        for (const Pass::Declaration& decl : pass.declarations_) {
            const bool isBufferKind =
                decl.kind == Pass::AccessKind::StorageBufferOutput || decl.kind == Pass::AccessKind::StorageBufferInput;

            uint32_t physicalIndex;
            auto existing = physicalIndexByName.find(decl.resourceName);
            if (existing == physicalIndexByName.end()) {
                PhysicalResource resource;
                resource.name = decl.resourceName;
                resource.isBuffer = isBufferKind;
                resource.firstUsePass = pos;
                resource.lastUsePass = pos;
                resource.isBackbuffer = (decl.resourceName == g.backbufferName);
                physicalIndex = static_cast<uint32_t>(compiled.resources_.size());
                compiled.resources_.push_back(std::move(resource));
                physicalIndexByName.emplace(decl.resourceName, physicalIndex);
            } else {
                physicalIndex = existing->second;
                // `pos` only increases across this loop, so the running
                // last-use position is simply the current one.
                compiled.resources_[physicalIndex].lastUsePass = pos;
            }

            PhysicalResource& resource = compiled.resources_[physicalIndex];
            switch (decl.kind) {
                case Pass::AccessKind::ColorOutput:
                    resource.attachment = decl.attachment;
                    resource.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                    break;
                case Pass::AccessKind::DepthStencilOutput:
                    resource.attachment = decl.attachment;
                    resource.imageUsage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                    break;
                case Pass::AccessKind::TextureInput:
                    resource.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
                    break;
                case Pass::AccessKind::StorageBufferOutput:
                    resource.buffer = decl.buffer;
                    break;
                case Pass::AccessKind::StorageBufferInput:
                    break;
            }

            accesses.push_back(Pass::resolveAccess(decl, physicalIndex, computeClass));
        }
    }

    // ---- resolve SwapchainRelative sizes / the backbuffer's own shape --
    for (PhysicalResource& resource : compiled.resources_) {
        if (resource.isBuffer) {
            continue;
        }
        if (resource.isBackbuffer) {
            // The backbuffer's real shape is dictated by the swapchain,
            // never by whatever AttachmentDesc its writer happened to
            // declare -- overriding it here is what gives CompileInfo a
            // reason to exist in a device-free compile step.
            resource.attachment.format = info.swapchainFormat;
            resource.attachment.sizeClass = SizeClass::Absolute;
            resource.attachment.width = static_cast<float>(info.swapchainWidth);
            resource.attachment.height = static_cast<float>(info.swapchainHeight);
            resource.attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        } else if (resource.attachment.sizeClass == SizeClass::SwapchainRelative) {
            const float width = resource.attachment.width * static_cast<float>(info.swapchainWidth);
            const float height = resource.attachment.height * static_cast<float>(info.swapchainHeight);
            resource.attachment.sizeClass = SizeClass::Absolute;
            resource.attachment.width = width;
            resource.attachment.height = height;
        }
    }

    g.compiled = std::move(compiled);
}

const CompiledGraph& RenderGraph::compiled() const {
    return impl_->compiled;
}

void RenderGraph::reset() {
    impl_->passes.clear();
    impl_->backbufferName.clear();
    impl_->hasBackbuffer = false;
    impl_->compiled = CompiledGraph{};
}

Pass::Pass(std::string name, QueueClass queue) : name_(std::move(name)), queue_(queue) {}

Pass& Pass::addColorOutput(std::string_view name, const AttachmentDesc& desc) {
    declarations_.push_back(Declaration{AccessKind::ColorOutput, std::string(name), desc, BufferDesc{}});
    return *this;
}

Pass& Pass::setDepthStencilOutput(std::string_view name, const AttachmentDesc& desc) {
    declarations_.push_back(Declaration{AccessKind::DepthStencilOutput, std::string(name), desc, BufferDesc{}});
    return *this;
}

Pass& Pass::addTextureInput(std::string_view name) {
    declarations_.push_back(Declaration{AccessKind::TextureInput, std::string(name), AttachmentDesc{}, BufferDesc{}});
    return *this;
}

Pass& Pass::addStorageBufferOutput(std::string_view name, const BufferDesc& desc) {
    declarations_.push_back(Declaration{AccessKind::StorageBufferOutput, std::string(name), AttachmentDesc{}, desc});
    return *this;
}

Pass& Pass::addStorageBufferInput(std::string_view name) {
    declarations_.push_back(
        Declaration{AccessKind::StorageBufferInput, std::string(name), AttachmentDesc{}, BufferDesc{}});
    return *this;
}

Pass& Pass::setSideEffect() {
    sideEffect_ = true;
    return *this;
}

Pass& Pass::setExecute(std::function<void(PassContext&)> fn) {
    execute_ = std::move(fn);
    return *this;
}

std::string_view Pass::name() const {
    return name_;
}

QueueClass Pass::queueClass() const {
    return queue_;
}

}  // namespace rx::graph
