#include <rx_graph/render_graph.h>

#include <rx_core/log.h>
#include <rx_graph/pass_signature.h>

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
//   5. (Task 2) Derive every sync2 barrier from steps 1-4's output
//      (barriers.h's buildBarriers(), ported from Granite's per-resource
//      invalidate/flush accounting -- see that header's own comment).
//
// Phase 4 Task 1 additions (history/persistent resources -- see pass.h's
// addHistoryInput()/setHistoryOutput() comments for the full contract):
// a bounds check ahead of step 1 (no pass may declare more than
// PassSignature::kMaxColorAttachments color outputs -- a carried Phase 3
// final-review finding, unrelated to history but landing in this same
// task); a namespace-mixing + exactly-one-writer validation pass, also
// ahead of step 1; step 2 gets a HistoryInput-specific twin that resolves
// against the history writer map instead of writersByName, deliberately
// WITHOUT adding a dependsOn edge; step 3's cull treats every
// setHistoryOutput()-declaring pass as an implicit root, like
// setSideEffect(); step 4's resource-resolution switch grows two more
// cases. Steps 1/3b/3c/5 (writersByName/dependsOn construction, the
// topological sort, cycle detection, and buildBarriers() itself) are
// completely unchanged -- a history resource's REAL cross-frame
// ping-pong/layout-carry synchronization is entirely Executor-side
// (executor.cpp), never something this device-free algorithm computes.
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

namespace {

// Phase 4 Task 1: is `format` a depth or depth/stencil format -- the same
// bucket executor.cpp's own aspectMaskForFormat() (and, before that,
// rx_rhi_vk/texture.cpp's identically-named function) already uses to
// decide DEPTH_BIT-vs-COLOR_BIT aspect masks. An independent, equally
// small local copy for the same reason executor.cpp's own copy documents
// itself as one (neither is exported past its own .cpp) -- this one drives
// Pass::resolveAccess's HistoryOutput color-vs-depth dispatch below, a
// device-free decision that must not pull in rx_rhi_vk (or even volk) just
// to answer "is this VkFormat enum value one of six depth ones", exactly
// like setDepthStencilOutput()'s own existing resolveAccess row never
// checks its format at all (a pure-stencil-only format is bucketed here as
// "depth-like" for the same reason that pre-existing row does: this task's
// scope does not extend to giving depth-vs-stencil-only formats separate
// layouts, a gap setDepthStencilOutput already has today).
bool isDepthOrStencilFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_S8_UINT:
            return true;
        default:
            return false;
    }
}

}  // namespace

bool Pass::hasAttachmentOutput() const {
    for (const Declaration& decl : declarations_) {
        // Phase 4 Task 1: a persistent (history) output IS an attachment
        // output -- see setHistoryOutput()'s own comment (pass.h) and this
        // method's updated doc comment.
        if (decl.kind == AccessKind::ColorOutput || decl.kind == AccessKind::DepthStencilOutput ||
            decl.kind == AccessKind::HistoryOutput) {
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
        case AccessKind::HistoryInput:
            // Phase 4 Task 1: identical resolution to TextureInput above --
            // a sampled read is a sampled read regardless of whether the
            // resource it targets happens to be persistent. The only
            // difference between the two kinds is upstream of this
            // function entirely (which name-resolution path -- writersByName
            // vs historyOutputWritersByName -- and which physical images Executor::
            // execute() actually binds), never the resolved stage/access/
            // layout triple itself.
            access.stages =
                computeClass ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            access.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            access.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            break;
        case AccessKind::HistoryOutput:
            // Phase 4 Task 1: color or depth/stencil, chosen by
            // decl.attachment.format -- exactly the same two rows
            // ColorOutput/DepthStencilOutput use above, just selected
            // dynamically instead of by which builder method was called
            // (setHistoryOutput() is the one output declaration whose
            // format alone decides its attachment kind; see pass.h's own
            // comment on why there is no separate "setHistoryDepthOutput").
            if (isDepthOrStencilFormat(decl.attachment.format)) {
                access.stages =
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                access.access =
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                access.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            } else {
                access.stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                access.access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                access.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            break;
    }
    return access;
}

bool Pass::isWriteKind(AccessKind kind) {
    // Phase 4 Task 1: HistoryOutput is deliberately NOT included here --
    // see this method's own doc comment (pass.h) for exactly why a
    // resource kind that unquestionably IS a write must still stay out of
    // the writersByName/dependsOn edge graph this predicate feeds.
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

    // ---- bounds check (carried from the Phase 3 final review): no single
    // pass may declare more than PassSignature::kMaxColorAttachments color
    // outputs -- a HistoryOutput declaration that resolves to a color
    // format (isDepthOrStencilFormat() false) counts exactly like an
    // ordinary addColorOutput() for this purpose, since it is one, per
    // Pass::hasAttachmentOutput()/resolveAccess above; a depth/stencil one
    // (either kind) never counts here at all, matching
    // setDepthStencilOutput()'s own long-standing (never bounded, "at most
    // one, by convention" per pass.h's class comment) treatment. Checked
    // per pass over the WHOLE declaration set, before culling -- this is a
    // structural pass-shape violation, not something whether the pass
    // survives culling should affect. -----------------------------------
    for (const Pass& pass : g.passes) {
        uint32_t colorCount = 0;
        for (const Pass::Declaration& decl : pass.declarations_) {
            if (decl.kind == Pass::AccessKind::ColorOutput ||
                (decl.kind == Pass::AccessKind::HistoryOutput && !isDepthOrStencilFormat(decl.attachment.format))) {
                ++colorCount;
            }
        }
        if (colorCount > PassSignature::kMaxColorAttachments) {
            RX_LOG_ERROR("rx_graph: pass '{}' declares {} color outputs, more than the maximum of {}", pass.name_,
                         colorCount, PassSignature::kMaxColorAttachments);
            throw std::runtime_error("rx_graph: pass '" + pass.name_ + "' declares " + std::to_string(colorCount) +
                                      " color outputs, more than the maximum of " +
                                      std::to_string(PassSignature::kMaxColorAttachments));
        }
    }

    // ---- history resources: namespace-mixing + exactly-one-writer checks,
    // and the writer lookup addHistoryInput() resolves against (Phase 4
    // Task 1). Also structural, checked over the WHOLE declaration set
    // before culling -- a history resource's writer is never subject to
    // culling in the first place (see step 3a below), so there is no
    // "wait until culling settles" reason to defer this either. ----------
    std::unordered_set<std::string> historyNames;
    std::unordered_set<std::string> nonHistoryNames;
    std::unordered_map<std::string, std::vector<uint32_t>> historyOutputWritersByName;
    for (uint32_t p = 0; p < passCount; ++p) {
        for (const Pass::Declaration& decl : g.passes[p].declarations_) {
            const bool isHistoryKind =
                decl.kind == Pass::AccessKind::HistoryInput || decl.kind == Pass::AccessKind::HistoryOutput;
            (isHistoryKind ? historyNames : nonHistoryNames).insert(decl.resourceName);
            if (decl.kind == Pass::AccessKind::HistoryOutput) {
                historyOutputWritersByName[decl.resourceName].push_back(p);
            }
        }
    }
    for (const std::string& name : historyNames) {
        if (nonHistoryNames.contains(name)) {
            RX_LOG_ERROR("rx_graph: resource '{}' is used as both a history resource and a transient/buffer "
                         "resource -- history names are their own namespace",
                         name);
            throw std::runtime_error("rx_graph: resource '" + name +
                                      "' is used as both a history resource and a transient/buffer resource -- "
                                      "history names are their own namespace");
        }
    }
    for (const auto& [name, writers] : historyOutputWritersByName) {
        if (writers.size() > 1) {
            std::string passNames;
            for (uint32_t p : writers) {
                if (!passNames.empty()) {
                    passNames += ", ";
                }
                passNames += "'" + g.passes[p].name_ + "'";
            }
            RX_LOG_ERROR("rx_graph: history resource '{}' has more than one setHistoryOutput() pass: {}", name,
                         passNames);
            throw std::runtime_error("rx_graph: history resource '" + name +
                                      "' has more than one setHistoryOutput() pass: " + passNames);
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

    // ---- step 2b: resolve HistoryInput reads against historyOutputWritersByName
    // instead of writersByName (Phase 4 Task 1) -- deliberately separate
    // from the loop above: a HistoryInput read must NOT add a dependsOn
    // edge onto its resource's setHistoryOutput() writer (see
    // addHistoryInput()'s own comment, pass.h, for why that dependency
    // would be wrong, not just redundant), but it still needs the SAME
    // "reading something nobody declares" validation every other read
    // kind gets, so a typo'd or never-established history name is caught
    // here exactly as loudly.
    for (uint32_t p = 0; p < passCount; ++p) {
        const Pass& pass = g.passes[p];
        for (const Pass::Declaration& decl : pass.declarations_) {
            if (decl.kind != Pass::AccessKind::HistoryInput) {
                continue;
            }
            auto it = historyOutputWritersByName.find(decl.resourceName);
            if (it == historyOutputWritersByName.end() || it->second.empty()) {
                RX_LOG_ERROR("rx_graph: pass '{}' reads history resource '{}', which no pass declares via "
                             "setHistoryOutput()",
                             pass.name_, decl.resourceName);
                throw std::runtime_error("rx_graph: pass '" + pass.name_ + "' reads history resource '" +
                                          decl.resourceName + "', which no pass declares via setHistoryOutput()");
            }
            // No dependsOn edge added -- intentional, see this block's own
            // comment above.
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
        // Phase 4 Task 1: a setHistoryOutput() declaration is an implicit
        // side effect -- its write persists into next frame's ping-pong
        // slot regardless of whether anything in THIS compiled graph
        // reads it (see setHistoryOutput()'s own comment, pass.h). Without
        // this, a history-only writer with no in-graph reader would be
        // wrongly culled as dead code the very first time Phase 4 adds a
        // pass that writes history and nothing else in the same frame.
        for (const Pass::Declaration& decl : g.passes[p].declarations_) {
            if (decl.kind == Pass::AccessKind::HistoryOutput) {
                markReachable(p);
                break;
            }
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
    // Unreachable cyclic subgraphs are automatically culled as dead code
    // by step 3a above (a pass with no dependency path from backbuffer or
    // side-effect passes never enters the reachable set). This check only
    // detects and rejects cycles that involve reachable passes -- a real
    // bug that must be reported. Reachable cycles are always fatal.
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
                case Pass::AccessKind::HistoryInput:
                    // Phase 4 Task 1: a history resource is ALWAYS sampled
                    // eventually (that is its entire point -- see
                    // setHistoryOutput()'s own unconditional
                    // VK_IMAGE_USAGE_SAMPLED_BIT union just below, which
                    // covers the case no addHistoryInput() declaration
                    // exists in THIS compiled graph at all); still unioned
                    // here too so a graph that DOES declare one is in no
                    // way relying on that other union instead.
                    resource.isHistory = true;
                    resource.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
                    break;
                case Pass::AccessKind::HistoryOutput:
                    resource.attachment = decl.attachment;
                    resource.isHistory = true;
                    // Always sampled (see the HistoryInput case's comment
                    // above) in addition to its color-or-depth attachment
                    // usage, chosen by format exactly like
                    // Pass::resolveAccess's own HistoryOutput row.
                    resource.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
                    resource.imageUsage |= isDepthOrStencilFormat(decl.attachment.format)
                                               ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                               : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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

    // ---- Task 2's last phase: derive every sync2 barrier from the
    // executionOrder()/resources()/passAccesses() data just resolved above
    // (barriers.h's buildBarriers()) -----------------------------------
    compiled.passBarriers_ = buildBarriers(compiled, compiled.finalBarriers_);

    // ---- Task 3 ambiguity resolution #1: apply CompileInfo::
    // backbufferFinalLayout. buildBarriers() itself still always produces
    // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR (barriers.h/barriers.cpp are
    // untouched by this task) -- compile() overwrites that one field here
    // instead, immediately after the call above. finalBarriers_ always has
    // exactly one imageBarrier (the backbuffer's) once compile() has
    // gotten this far: the backbuffer-writer validation earlier in this
    // function already guarantees exactly one PhysicalResource has
    // isBackbuffer == true, and buildBarriers() always emits exactly one
    // entry for it (barriers.cpp's own loop `break`s after the first
    // match) -- the emptiness check below is defensive, not expected to
    // ever trigger.
    if (!compiled.finalBarriers_.imageBarriers.empty()) {
        compiled.finalBarriers_.imageBarriers.front().newLayout = info.backbufferFinalLayout;
    }

    g.compiled = std::move(compiled);
}

const CompiledGraph& RenderGraph::compiled() const {
    return impl_->compiled;
}

const Pass& RenderGraph::passAt(uint32_t rawIndex) const {
    return impl_->passes.at(rawIndex);
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

Pass& Pass::addHistoryInput(std::string_view name) {
    declarations_.push_back(Declaration{AccessKind::HistoryInput, std::string(name), AttachmentDesc{}, BufferDesc{}});
    return *this;
}

Pass& Pass::setHistoryOutput(std::string_view name, const AttachmentDesc& desc) {
    declarations_.push_back(Declaration{AccessKind::HistoryOutput, std::string(name), desc, BufferDesc{}});
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

void Pass::invokeExecute(PassContext& ctx) const {
    if (execute_) {
        execute_(ctx);
    }
}

}  // namespace rx::graph
