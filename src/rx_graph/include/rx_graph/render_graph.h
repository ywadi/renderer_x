#pragma once
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
};

// The device-free result of RenderGraph::compile(): which declared passes
// survive culling and in what order, every physical resource compile()
// resolved, and each surviving pass's resolved resource accesses. Task 2
// adds per-order-position barrier lists (passBarriers()); this task's
// CompiledGraph carries exactly the culling/ordering/lifetime data the
// Task 1 brief specifies.
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

private:
    friend class RenderGraph;

    std::vector<uint32_t> executionOrder_;
    std::vector<bool> culled_;                              // indexed by raw pass index
    std::vector<PhysicalResource> resources_;
    std::vector<std::vector<ResourceAccess>> passAccesses_;  // indexed by raw pass index
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
