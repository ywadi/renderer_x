#pragma once
#include <rx_graph/barriers.h>
#include <rx_graph/pass.h>
#include <rx_graph/resources.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace rx::graph {

// Swapchain-derived inputs RenderGraph::compile() needs to resolve
// SwapchainRelative attachment sizes and the backbuffer resource's own
// format/extent (see PhysicalResource::attachment's comment and
// RenderGraph::setBackbufferSource()). Device-free: these are plain values
// the caller already has from rx_rhi_vk's Device/swapchain, not queried
// here -- rx_graph never touches a VkDevice in Task 1.
struct CompileInfo {
    uint32_t swapchainWidth = 0;
    uint32_t swapchainHeight = 0;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;

    // Task 3 ambiguity resolution #1: VK_IMAGE_LAYOUT_PRESENT_SRC_KHR is
    // only meaningful for an image the presentation engine will actually
    // consume (a real swapchain image) -- an offscreen "backbuffer" (e.g.
    // Task 3's GPU test, which renders into a caller-created, never-
    // presented VkImage) has no presentation engine to hand off to at all,
    // so CompiledGraph::finalBarriers() needs a caller-selectable final
    // layout instead of Task 2's hardcoded PRESENT_SRC_KHR assumption.
    // Defaults to PRESENT_SRC_KHR (Task 2's original, unchanged behavior
    // for the production swapchain-presenting case); pass e.g.
    // VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL for an offscreen backbuffer a
    // caller intends to read back instead. RenderGraph::compile() applies
    // this by overwriting buildBarriers()'s own (still PRESENT_SRC_KHR-
    // producing) finalBarriers() image barrier's newLayout in place -- see
    // render_graph.cpp's compile(), immediately after its buildBarriers()
    // call; barriers.h/barriers.cpp's own hardcoded PRESENT_SRC_KHR is
    // deliberately left untouched by this change.
    VkImageLayout backbufferFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
};

// The device-free result of RenderGraph::compile(): which declared passes
// survive culling and in what order, every physical resource compile()
// resolved, each surviving pass's resolved resource accesses (Task 1
// brief), and -- from Task 2 on -- the sync2 barriers compile() derived
// from those accesses (passBarriers()/finalBarriers(), populated by
// compile()'s last phase; see barriers.h).
//
// Only RenderGraph::compile() ever populates one of these -- there is no
// public constructor beyond the implicit default. Before the owning
// RenderGraph's first compile() call (or after reset()), an unpopulated
// CompiledGraph reports empty spans from executionOrder()/resources(), but
// isCulled()/passAccesses() throw std::out_of_range for any `passIndex` at
// all -- there is no pass count yet to be in range of. See those two
// methods' own comments for the exact per-method contract.
class CompiledGraph {
public:
    // Raw pass indices (RenderGraph::addPass() call order, 0-based), in
    // submission order. Excludes every culled pass.
    [[nodiscard]] std::span<const uint32_t> executionOrder() const { return executionOrder_; }

    // `passIndex` is the raw declaration index (addPass() call order),
    // the same indexing isCulled()/passAccesses() and executionOrder()'s
    // *values* (not positions) share -- not a position within
    // executionOrder(). Out-of-range throws std::out_of_range.
    [[nodiscard]] bool isCulled(uint32_t passIndex) const { return culled_.at(passIndex); }

    [[nodiscard]] std::span<const PhysicalResource> resources() const { return resources_; }

    // Empty for a culled pass (its declarations never resolved to a
    // physical resource -- see render_graph.cpp). Out-of-range `passIndex`
    // throws std::out_of_range.
    [[nodiscard]] std::span<const ResourceAccess> passAccesses(uint32_t passIndex) const {
        return passAccesses_.at(passIndex);
    }

    // Task 2: barriers to insert immediately before executionOrder()[pos]'s
    // pass. Same length as executionOrder() -- indexed by *position*, not
    // raw pass index (unlike passAccesses(), which is raw-index-keyed,
    // because a culled pass has no position to be indexed by at all).
    // Populated by RenderGraph::compile()'s last phase (barriers.h's
    // buildBarriers()); empty before the owning RenderGraph's first
    // compile() call or after reset(), exactly like executionOrder().
    [[nodiscard]] std::span<const PassBarriers> passBarriers() const { return passBarriers_; }

    // Task 2: the backbuffer's transition to VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    // after the last surviving pass runs -- not "immediately before a
    // pass" like passBarriers()'s entries, so it is not itself part of
    // that span. Default-constructed (empty) before the first compile().
    [[nodiscard]] const PassBarriers& finalBarriers() const { return finalBarriers_; }

private:
    friend class RenderGraph;

    std::vector<uint32_t> executionOrder_;
    std::vector<bool> culled_;                              // indexed by raw pass index
    std::vector<PhysicalResource> resources_;
    std::vector<std::vector<ResourceAccess>> passAccesses_;  // indexed by raw pass index
    std::vector<PassBarriers> passBarriers_;                 // indexed by executionOrder() position
    PassBarriers finalBarriers_;
};

// Owns every Pass declared against it and, after compile(), the derived
// CompiledGraph. Device-free in Task 1: compile() performs culling,
// topological ordering, and physical-resource lifetime analysis purely
// over the declared name/kind graph -- no VkDevice or rx_rhi_vk handle
// anywhere in this header or render_graph.cpp. Task 2 adds barrier
// derivation (still device-free); Task 3's executor is the first thing in
// rx_graph that touches a real device.
//
// Not copyable or movable: the only special members declared are the
// default constructor and destructor (both defined out-of-line in
// render_graph.cpp, where Impl is a complete type), so the compiler does
// not implicitly generate copy or move operations for a class with a
// user-declared destructor -- deliberate, since every Pass& RenderGraph
// hands out points into this object's own storage.
class RenderGraph {
public:
    RenderGraph();
    ~RenderGraph();

    // Declares a new pass. `name` must be unique across the graph's
    // lifetime (until reset()) -- compile() rejects duplicates. The
    // returned reference stays valid until reset() or the RenderGraph's
    // own destruction.
    Pass& addPass(std::string_view name, QueueClass queue = QueueClass::Graphics);

    // Names the resource presented at frame end. Must be called before
    // compile(); the named resource must have at least one writer among
    // the graph's declared passes.
    void setBackbufferSource(std::string_view name);

    // Culls, orders, and resolves resource lifetimes over every pass
    // declared so far (Task 1 brief's compile algorithm). Throws
    // std::runtime_error, naming the offending pass/resource, on: no
    // backbuffer source set; a duplicate pass name; a read of a resource
    // no pass writes; or a backbuffer source no pass writes.
    void compile(const CompileInfo& info);

    [[nodiscard]] const CompiledGraph& compiled() const;

    // Task 3: the one piece of per-pass data CompiledGraph deliberately
    // never carries forward on its own (see CompiledGraph's class comment
    // above -- it only ever resolves *resource* data, never the Pass
    // object itself) -- needed so Executor::execute() can invoke a
    // surviving pass's recorded execute() callback (pass.h's
    // Pass::setExecute) and read its name() for debug labels, given only
    // the raw pass index CompiledGraph::executionOrder() yields. `rawIndex`
    // uses the exact same indexing as isCulled()/passAccesses() (addPass()
    // call order); out-of-range throws std::out_of_range, matching those
    // two methods' own contract. Purely additive: does not change
    // compile()'s device-free algorithm, ABI-break any existing accessor,
    // or give a caller any way to mutate a Pass after the fact (returns a
    // const reference) -- Task 3 is this method's first caller.
    [[nodiscard]] const Pass& passAt(uint32_t rawIndex) const;

    // Clears every declared pass, the backbuffer source, and any prior
    // compile() result, so the graph can be re-declared from scratch
    // (e.g. for the next frame, or when a sample's pass topology changes).
    // Every Pass& previously handed out by addPass() is invalidated.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rx::graph
