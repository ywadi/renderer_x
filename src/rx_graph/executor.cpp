#include <rx_graph/executor.h>

#include "transient_pool.h"

#include <rx_core/log.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <volk.h>

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Task 3's executor: turns a compiled, device-free rx::graph::RenderGraph
// into real Vulkan work -- transient_pool.h's TransientPool for physical
// resource realization, sync2 vkCmdPipelineBarrier2 for every
// CompiledGraph::passBarriers()/finalBarriers() entry, and one
// vkCmdBeginRendering/vkCmdEndRendering scope per graphics pass [spec
// Phase 3 design, D2 -- fresh design, not a Granite renderpass-object
// port; see .superpowers/sdd/2026-08-10-phase3-render-graph-materials/
// research-rendergraph.md 1's table for why Granite's physical-execution
// layer specifically does not port here].
namespace rx::graph {

namespace {

// Depth/depth-stencil formats need DEPTH_BIT (and STENCIL_BIT for the
// combined ones) as an image barrier's subresource aspect mask instead of
// COLOR_BIT -- the same well-known Vulkan format table
// rx_rhi_vk/texture.cpp's own aspectMaskForFormat() already encodes for
// Texture2D's view creation (not exported from that .cpp, so this is an
// independent, equally small local copy rather than a barriers.h/.cpp
// change this task's scope doesn't call for).
VkImageAspectFlags aspectMaskForFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkExtent2D toExtent(const AttachmentDesc& attachment) {
    return VkExtent2D{static_cast<uint32_t>(attachment.width + 0.5F), static_cast<uint32_t>(attachment.height + 0.5F)};
}

// Every declared ResourceAccess a single pass makes against one physical
// resource, combined into that resource's union of declared pipeline
// stages/access bits -- an independent copy of barriers.cpp's own (private,
// anonymous-namespace) combineByResource()/CombinedAccess: this file cannot
// call that one directly (it is not exported past barriers.cpp, deliberately
// -- see barriers.h's own comment on why detail:: is the only exposed
// seam), and this task's scope does not call for changing barriers.h/.cpp
// to export it. Used both to track each pooled resource's own *last*
// touched stage this execute() call (see the finalStageThisExecute loop in
// Executor::execute()) and, for the access half, to synthesize the one
// barrier a pooled BUFFER's true first-ever-in-the-graph access needs that
// compile()'s own device-free buildBarriers() correctly never emits -- see
// synthesizeFirstUseBufferBarrierIfNeeded()'s own comment just below for
// why that gap is real, not hypothetical.
// Access bits that constitute WRITES for the pool's cross-frame
// availability tracking (lastFrameFinalAccess) -- read bits are
// meaningless in a srcAccessMask and are masked out at accumulation.
constexpr VkAccessFlags2 kPoolWriteAccessMask =
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
    VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

struct CombinedAccess {
    VkPipelineStageFlags2 stages = 0;
    VkAccessFlags2 access = 0;
};

std::vector<std::pair<uint32_t, CombinedAccess>> combineAccessesByResource(std::span<const ResourceAccess> accesses) {
    std::vector<std::pair<uint32_t, CombinedAccess>> combined;
    for (const ResourceAccess& access : accesses) {
        auto it = std::find_if(combined.begin(), combined.end(),
                                [&](const auto& entry) { return entry.first == access.physicalIndex; });
        if (it == combined.end()) {
            combined.emplace_back(access.physicalIndex, CombinedAccess{access.stages, access.access});
        } else {
            it->second.stages |= access.stages;
            it->second.access |= access.access;
        }
    }
    return combined;
}

}  // namespace

// One CompiledGraph::resources() entry's real, device-side binding, as of
// the most recent Executor::realize() call. Rebuilt from scratch by every
// realize() call (see Executor::realize()); PassContext's name-based
// resolvers (imageView()/image()/buffer()/imageFormat()) read straight out
// of whatever the CURRENT vector holds.
struct ResolvedResource {
    bool isBuffer = false;
    bool isBackbuffer = false;

    // Phase 4 Task 1: true for a Pass::addHistoryInput()/setHistoryOutput()
    // resource -- pinned, ping-ponged, never pooled the way an ordinary
    // image is. Mutually exclusive with isBuffer (history is images-only)
    // and isBackbuffer (see resources.h's PhysicalResource::isHistory
    // comment for why the two can never coincide).
    bool isHistory = false;

    // Index into Impl::pool's images_/buffers_ (per isBuffer), or --  when
    // isHistory -- into Impl::pool's pinned_ instead [Phase 4 Task 1].
    // Meaningless (left at UINT32_MAX) for the backbuffer, which this
    // Executor never pools [Task 3 ambiguity resolution #4].
    uint32_t poolIndex = UINT32_MAX;

    // Meaningless (left at their defaults) for isHistory == true: a
    // history resource has TWO real images (ping-pong slots), never one,
    // so there is no single VkImage/VkImageView a generic field here could
    // hold -- every history-aware code path (applyHistoryAccesses(),
    // Executor::resolveImageView()/resolveImage(), the attachment-view
    // resolution in Executor::execute()) resolves the correct slot's real
    // handle directly from Impl::pool.pinned(poolIndex) instead, keyed by
    // this frame's read/write role, every time it needs one.
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
};

struct Executor::Impl {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

    // Phase 4 Task 1: needed to build the one-time init-clear
    // rx::rhi::CommandContext a freshly (re)created pinned history entry's
    // two slots need (see initializePinnedHistoryEntry() below) -- every
    // other Executor operation records onto a caller-supplied
    // VkCommandBuffer (execute()'s own `cmd` parameter) and never needed a
    // real queue/queue-family of its own before this.
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;

    rx::rhi::Allocator allocator;
    detail::TransientPool pool;
    rx::rhi::DeletionQueue deletionQueue;
    bool debugUtilsAvailable = false;

    // Executor's own monotonic tick, one increment per execute() call --
    // the clock TransientPool's staleness sweep and this Impl's own
    // deletion-queue pacing (see Executor::execute()) both run against.
    // Phase 4 Task 1: also the ping-pong parity clock -- a history
    // resource's write slot for a given execute() call is
    // `frameCounter % 2`, its read slot `(frameCounter + 1) % 2` [design
    // contract point 2], read by every history-aware function below
    // directly off this same field, never a separate counter.
    uint64_t frameCounter = 0;

    // Rebuilt by every realize() call; indexed identically to whatever
    // CompiledGraph::resources() looked like as of that call.
    std::vector<ResolvedResource> resources;
    std::unordered_map<std::string, uint32_t> nameToIndex;

    Impl(VkDevice deviceIn, VkPhysicalDevice physicalDeviceIn, VkQueue graphicsQueueIn, uint32_t graphicsQueueFamilyIn,
         rx::rhi::Allocator allocatorIn)
        : device(deviceIn),
          physicalDevice(physicalDeviceIn),
          graphicsQueue(graphicsQueueIn),
          graphicsQueueFamily(graphicsQueueFamilyIn),
          allocator(std::move(allocatorIn)),
          pool(physicalDeviceIn, deviceIn, allocator) {}
};

namespace {

void beginDebugLabel(Executor::Impl& impl, VkCommandBuffer cmd, std::string_view name) {
    if (!impl.debugUtilsAvailable) {
        return;
    }
    // VkDebugUtilsLabelEXT::pLabelName must stay valid only for the
    // duration of this call -- vkCmdBeginDebugUtilsLabelEXT copies/consumes
    // it immediately, so a local std::string on this function's own stack
    // is sufficient (no need to keep it alive past the call).
    const std::string label(name);
    VkDebugUtilsLabelEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    info.pLabelName = label.c_str();
    vkCmdBeginDebugUtilsLabelEXT(cmd, &info);
}

void endDebugLabel(Executor::Impl& impl, VkCommandBuffer cmd) {
    if (!impl.debugUtilsAvailable) {
        return;
    }
    vkCmdEndDebugUtilsLabelEXT(cmd);
}

// Applies every barrier in `barriers` (one CompiledGraph::passBarriers()
// position, or finalBarriers()) via a single vkCmdPipelineBarrier2 call --
// a no-op if `barriers` is empty. `firstBarrierSeen`, shared across every
// call this same execute() makes, is Task 3 ambiguity resolution #2's own
// mechanism: the FIRST barrier this execute() call applies against a given
// (pooled, non-backbuffer) physicalIndex has its compile-time srcStage
// (always VK_PIPELINE_STAGE_2_NONE for a resource's first access within
// ANY single compile walk -- device-free compile() has no notion of "a
// previous frame" at all) overridden with that pooled entry's own tracked
// TransientPool::PooledImage/PooledBuffer::lastFrameFinalStages -- the
// real stage the GPU may still be finishing from the LAST execute() call
// that touched this same physical allocation. Every later barrier against
// the same physicalIndex, THIS call, is left exactly as compile() derived
// it (already correctly chained within this one topological walk).
void applyBarriers(Executor::Impl& impl, const CompiledGraph& compiled, VkCommandBuffer cmd,
                    const PassBarriers& barriers, std::vector<bool>& firstBarrierSeen) {
    if (barriers.imageBarriers.empty() && barriers.bufferBarriers.empty()) {
        return;
    }

    std::vector<VkImageMemoryBarrier2> vkImageBarriers;
    vkImageBarriers.reserve(barriers.imageBarriers.size());
    for (const ImageBarrier& b : barriers.imageBarriers) {
        const PhysicalResource& physical = compiled.resources()[b.physicalIndex];
        const ResolvedResource& resolved = impl.resources.at(b.physicalIndex);

        // Phase 4 Task 1: a history resource never has a single real
        // VkImage this compile-time-derived barrier could apply to (its
        // `resolved.image` is left at its default, VK_NULL_HANDLE -- see
        // ResolvedResource's own comment) -- applyHistoryAccesses() (called
        // separately, from Executor::execute()) computes and applies its
        // OWN real barriers for it directly, per-slot, from the unmerged
        // per-declaration access list. Skipped here entirely, including
        // `firstBarrierSeen` bookkeeping, which nothing history-related
        // ever consults.
        if (resolved.isHistory) {
            continue;
        }

        VkPipelineStageFlags2 srcStage = b.srcStage;
        VkAccessFlags2 srcAccess = b.srcAccess;
        if (!resolved.isBackbuffer && !firstBarrierSeen[b.physicalIndex]) {
            srcStage = impl.pool.image(resolved.poolIndex).lastFrameFinalStages;
            // Make the prior frame's final writes AVAILABLE before this
            // frame's discard transition (execution ordering alone does
            // not flush caches -- see PooledImage::lastFrameFinalAccess).
            srcAccess = impl.pool.image(resolved.poolIndex).lastFrameFinalAccess;
        } else if (resolved.isBackbuffer && !firstBarrierSeen[b.physicalIndex] &&
                   b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            // Backbuffer acquire chaining: when the backbuffer is a freshly
            // acquired swapchain image, its first (UNDEFINED-discard) layout
            // transition must be ordered AFTER the presentation engine's
            // acquire read. A semaphore wait only orders the stages named in
            // pWaitDstStageMask -- the contract with callers (all samples)
            // is that the submission waits the acquire semaphore at
            // COLOR_ATTACHMENT_OUTPUT, so this transition's srcStage must be
            // that same stage, not the compile-time NONE (which races the
            // acquire under synchronization validation's present-engine
            // tracking, layers >= ~1.3.240). For offscreen backbuffers
            // (tests) the extra execution-only dependency is trivially
            // satisfied -- srcAccess stays 0 either way.
            srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        }
        firstBarrierSeen[b.physicalIndex] = true;

        VkImageMemoryBarrier2 vkBarrier{};
        vkBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        vkBarrier.srcStageMask = srcStage;
        vkBarrier.srcAccessMask = srcAccess;
        vkBarrier.dstStageMask = b.dstStage;
        vkBarrier.dstAccessMask = b.dstAccess;
        vkBarrier.oldLayout = b.oldLayout;
        vkBarrier.newLayout = b.newLayout;
        vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBarrier.image = resolved.image;
        vkBarrier.subresourceRange.aspectMask = aspectMaskForFormat(physical.attachment.format);
        vkBarrier.subresourceRange.baseMipLevel = 0;
        vkBarrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        vkBarrier.subresourceRange.baseArrayLayer = 0;
        vkBarrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        vkImageBarriers.push_back(vkBarrier);
    }

    std::vector<VkBufferMemoryBarrier2> vkBufferBarriers;
    vkBufferBarriers.reserve(barriers.bufferBarriers.size());
    for (const BufferBarrier& b : barriers.bufferBarriers) {
        const ResolvedResource& resolved = impl.resources.at(b.physicalIndex);

        VkPipelineStageFlags2 srcStage = b.srcStage;
        VkAccessFlags2 srcAccess = b.srcAccess;
        if (!resolved.isBackbuffer && !firstBarrierSeen[b.physicalIndex]) {
            srcStage = impl.pool.buffer(resolved.poolIndex).lastFrameFinalStages;
            // Same availability contract as the image path above.
            srcAccess = impl.pool.buffer(resolved.poolIndex).lastFrameFinalAccess;
        }
        firstBarrierSeen[b.physicalIndex] = true;

        VkBufferMemoryBarrier2 vkBarrier{};
        vkBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        vkBarrier.srcStageMask = srcStage;
        vkBarrier.srcAccessMask = srcAccess;
        vkBarrier.dstStageMask = b.dstStage;
        vkBarrier.dstAccessMask = b.dstAccess;
        vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBarrier.buffer = resolved.buffer;
        vkBarrier.offset = 0;
        vkBarrier.size = VK_WHOLE_SIZE;
        vkBufferBarriers.push_back(vkBarrier);
    }

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(vkImageBarriers.size());
    dep.pImageMemoryBarriers = vkImageBarriers.empty() ? nullptr : vkImageBarriers.data();
    dep.bufferMemoryBarrierCount = static_cast<uint32_t>(vkBufferBarriers.size());
    dep.pBufferMemoryBarriers = vkBufferBarriers.empty() ? nullptr : vkBufferBarriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Task 2's buildBarriers() correctly emits NO barrier for a resource's
// very first access within one device-free compile walk: detail::
// applyAccess()'s "first use ever" branch (barriers.cpp) sets srcStage/
// srcAccess to NONE and -- for a BUFFER specifically, which has no layout
// to transition -- `needBarrier` itself is false (layoutDiffers is
// unconditionally false for a buffer, and `write && everAccessed` is false
// on a resource's very first write), so no ImageBarrier/BufferBarrier
// entry is produced at all. That is exactly correct for a compile walk
// with no notion of "a previous frame" -- nothing has touched this
// resource yet, so there is nothing to synchronize against.
//
// An IMAGE resource's first-ever access never has this gap: its layout
// always differs from the fresh VK_IMAGE_LAYOUT_UNDEFINED state
// (layoutDiffers == true unconditionally), so buildBarriers() always DOES
// emit a real ImageBarrier there, one applyBarriers() above can override
// the srcStage of. A pooled BUFFER reused across execute() calls has
// exactly the same cross-frame hazard an image does (the GPU may still be
// finishing the LAST execute() call's work on this same physical
// allocation) but no barrier for applyBarriers() to override at all,
// since compile() never produced one. This function closes that gap:
// called once per pass, immediately after applyBarriers() has run for it,
// for every BUFFER resource whose PhysicalResource::firstUsePass is THIS
// pass's position and which still has no barrier recorded in
// `firstBarrierSeen` (i.e. compile() genuinely emitted none for it, the
// buffer-first-use case above -- an already-barriered resource, or one
// whose true first use is a different pass, is left untouched). Never
// applies to the backbuffer (always an image, never pooled). See
// test_execute_gpu.cpp's dedicated "pooled storage buffer... across two
// consecutive execute() calls" case for the regression this covers.
void synthesizeFirstUseBufferBarrierIfNeeded(Executor::Impl& impl, const CompiledGraph& compiled, VkCommandBuffer cmd,
                                              std::span<const ResourceAccess> accesses, size_t pos,
                                              std::vector<bool>& firstBarrierSeen) {
    const std::span<const PhysicalResource> resources = compiled.resources();
    for (const auto& [physIdx, combined] : combineAccessesByResource(accesses)) {
        const PhysicalResource& physical = resources[physIdx];
        if (!physical.isBuffer || firstBarrierSeen[physIdx] || physical.firstUsePass != pos) {
            continue;
        }

        const ResolvedResource& resolved = impl.resources.at(physIdx);

        VkBufferMemoryBarrier2 vkBarrier{};
        vkBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        vkBarrier.srcStageMask = impl.pool.buffer(resolved.poolIndex).lastFrameFinalStages;
        // Prior-frame final writes must be made available here too -- the
        // synthesized barrier exists precisely because this is a pooled
        // buffer's true first access of the frame.
        vkBarrier.srcAccessMask = impl.pool.buffer(resolved.poolIndex).lastFrameFinalAccess;
        vkBarrier.dstStageMask = combined.stages;
        vkBarrier.dstAccessMask = combined.access;
        vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBarrier.buffer = resolved.buffer;
        vkBarrier.offset = 0;
        vkBarrier.size = VK_WHOLE_SIZE;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &vkBarrier;
        vkCmdPipelineBarrier2(cmd, &dep);

        firstBarrierSeen[physIdx] = true;
    }
}

// Phase 4 Task 1: history resources bypass compile()'s own device-free
// barrier derivation (allBarriers[pos]) ENTIRELY -- see
// transient_pool.h's PinnedHistorySlot class comment for why: a single
// physicalIndex's compile-time barrier, when a pass declares BOTH an
// addHistoryInput() read and (the same or a different pass, same frame)
// a setHistoryOutput() write of the SAME name, would have to describe TWO
// DIFFERENT real images (this frame's read slot and this frame's write
// slot) with only one (barriers.cpp's own combineByResource()-merged)
// oldLayout/newLayout pair -- structurally incapable of it. This function
// instead walks `accesses` (compiled.passAccesses(), the UNMERGED
// per-declaration list, never allBarriers[pos]) directly and, for every
// access against a history physicalIndex, resolves ITS OWN real slot --
// write slot for a COLOR_ATTACHMENT_OPTIMAL/DEPTH_ATTACHMENT_OPTIMAL
// access, read slot for a SHADER_READ_ONLY_OPTIMAL one, exhaustive and
// unambiguous exactly like the colorPhysIdx/depthPhysIdx classification
// just below in Executor::execute() -- and computes/applies its own real
// vkCmdPipelineBarrier2, chained off THAT SLOT's own persisted
// detail::ResourceBarrierState (never reset between execute() calls,
// unlike buildBarriers()' per-compile-walk state -- see
// PinnedHistorySlot's own comment for why that persistence alone realizes
// design contract point 4's "barrier state machine initialized from
// tracked last-frame layout instead of UNDEFINED"). Also latches
// PinnedHistorySlot::everWrittenByRealPass for a write access -- the value
// PassContext::historyValid() reads.
void applyHistoryAccesses(Executor::Impl& impl, const CompiledGraph& compiled, VkCommandBuffer cmd,
                           std::span<const ResourceAccess> accesses) {
    const std::span<const PhysicalResource> resources = compiled.resources();
    const auto writeSlot = static_cast<uint32_t>(impl.frameCounter % 2);
    const auto readSlot = static_cast<uint32_t>((impl.frameCounter + 1) % 2);

    for (const ResourceAccess& access : accesses) {
        const PhysicalResource& physical = resources[access.physicalIndex];
        if (!physical.isHistory) {
            continue;
        }

        const bool isWrite = access.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
                              access.layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        const uint32_t slotIndex = isWrite ? writeSlot : readSlot;

        const ResolvedResource& resolved = impl.resources.at(access.physicalIndex);
        detail::PinnedHistoryEntry& entry = impl.pool.pinned(resolved.poolIndex);
        detail::PinnedHistorySlot& slot = entry.slots[slotIndex];

        auto transition =
            detail::applyAccess(slot.barrierState, /*isBuffer=*/false, access.stages, access.access, access.layout);
        if (transition.has_value()) {
            VkImageMemoryBarrier2 vkBarrier{};
            vkBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            vkBarrier.srcStageMask = transition->srcStage;
            vkBarrier.srcAccessMask = transition->srcAccess;
            vkBarrier.dstStageMask = transition->dstStage;
            vkBarrier.dstAccessMask = transition->dstAccess;
            vkBarrier.oldLayout = transition->oldLayout;
            vkBarrier.newLayout = transition->newLayout;
            vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkBarrier.image = slot.texture->image();
            vkBarrier.subresourceRange.aspectMask = aspectMaskForFormat(physical.attachment.format);
            vkBarrier.subresourceRange.baseMipLevel = 0;
            vkBarrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            vkBarrier.subresourceRange.baseArrayLayer = 0;
            vkBarrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &vkBarrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        if (isWrite) {
            slot.everWrittenByRealPass = true;
        }
    }
}

// Phase 4 Task 1: the VkImageView Executor::execute()'s own dynamic-
// rendering attachment-building loop (below) should bind for `resolved` --
// this frame's WRITE slot for a history resource (Impl::frameCounter % 2;
// see applyHistoryAccesses()'s matching write-slot derivation, which has
// already transitioned that exact slot to COLOR_ATTACHMENT_OPTIMAL/
// DEPTH_ATTACHMENT_OPTIMAL by the time this runs), or simply `resolved.view`
// unchanged for any ordinary (non-history) resource.
VkImageView resolveAttachmentView(Executor::Impl& impl, const ResolvedResource& resolved) {
    if (resolved.isHistory) {
        const auto writeSlot = static_cast<uint32_t>(impl.frameCounter % 2);
        return impl.pool.pinned(resolved.poolIndex).slots[writeSlot].texture->view();
    }
    return resolved.view;
}

// Phase 4 Task 1, design contract point 3: the "small init submission"
// half of the brief's two offered choices for getting a pinned slot's
// contents into a DEFINED (black/1.0-cleared) state before its first-ever
// use as a sampled read -- chosen over "clear-on-first-write-load" because
// that alternative cannot cover the actual hazard at all: under the
// ping-pong parity this task uses (write slot = frameCounter % 2, read
// slot = (frameCounter + 1) % 2 -- see Impl::frameCounter's own comment),
// the VERY FIRST execute() call that ever touches a history resource
// reads a slot that will not be WRITTEN (by any real pass) until the frame
// AFTER NEXT -- there is no "first write" for a LOAD_OP to piggyback on
// before that first read happens. An explicit clear, run once here via its
// own one-shot rx::rhi::CommandContext submission (the same
// vkQueueWaitIdle-per-call tool this codebase already uses for setup/test
// work -- rx_rhi_vk/command.h's own doc comment), is unconditionally
// correct for both roles a freshly (re)created slot might be used in
// first, and only ever runs once per pinned entry's lifetime (or once per
// resize) -- see TransientPool::acquireHistory()'s `freshlyCreated` output
// parameter, this function's only caller.
//
// Does NOT go through detail::applyAccess() for the clear's own barrier --
// unlike every other caller of that function, this one is not a real
// declared ResourceAccess: barriers.cpp's own isWriteAccess()/
// kWriteAccessMask (its write-classification helper, scoped deliberately
// to "every write access any declared ResourceAccess in this project can
// carry" per that mask's own comment) does not recognize
// VK_ACCESS_2_TRANSFER_WRITE_BIT as a write at all, so feeding it through
// applyAccess() would mis-classify this as a READ and leave
// lastWriteStages/pendingFlushAccess/hasPendingFlush understated (0/0/
// false) instead of reflecting the real transfer write that just
// happened. Harmless in THIS specific call site purely by accident of
// timing (rx::rhi::CommandContext::runOnce() already vkQueueWaitIdle()s
// before returning, so nothing ever needs to synchronize against this
// clear across a submission boundary regardless of what state is left
// behind) -- but relying on that coincidence instead of recording the
// truth would be a latent bug waiting for a future caller that submits
// this asynchronously instead. The transition itself is fully known
// anyway (a freshly-created slot's ResourceBarrierState is always
// UNDEFINED/everAccessed=false, so this is unconditionally "first use
// ever": srcStage/srcAccess = NONE, oldLayout = UNDEFINED) -- hand-writing
// it and then hand-seeding the resulting state as a real write is both
// more precise and no more code than routing around the mask gap.
void initializePinnedHistoryEntry(Executor::Impl& impl, uint32_t pinnedIndex) {
    detail::PinnedHistoryEntry& entry = impl.pool.pinned(pinnedIndex);

    auto cmdCtx = rx::rhi::CommandContext::create(impl.device, impl.graphicsQueue, impl.graphicsQueueFamily);
    if (!cmdCtx.has_value()) {
        RX_LOG_ERROR("rx_graph: Executor::realize: failed to create a CommandContext to init-clear history "
                     "resource '{}'",
                     entry.name);
        return;
    }

    const VkImageAspectFlags aspect = aspectMaskForFormat(entry.format);
    const bool isDepthOrStencil = aspect != VK_IMAGE_ASPECT_COLOR_BIT;

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        for (detail::PinnedHistorySlot& slot : entry.slots) {
            VkImageMemoryBarrier2 vkBarrier{};
            vkBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            vkBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            vkBarrier.srcAccessMask = VK_ACCESS_2_NONE;
            vkBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            vkBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            vkBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            vkBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkBarrier.image = slot.texture->image();
            vkBarrier.subresourceRange.aspectMask = aspect;
            vkBarrier.subresourceRange.baseMipLevel = 0;
            vkBarrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            vkBarrier.subresourceRange.baseArrayLayer = 0;
            vkBarrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &vkBarrier;
            vkCmdPipelineBarrier2(cmd, &dep);

            // Hand-seed the state as if a real detail::applyAccess() write
            // had just run (see this function's own comment above for why
            // it is not literally called here) -- everything a subsequent
            // real access's own detail::applyAccess() call needs to chain
            // correctly off this clear.
            slot.barrierState.currentLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            slot.barrierState.everAccessed = true;
            slot.barrierState.lastWriteStages = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            slot.barrierState.pendingFlushAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            slot.barrierState.hasPendingFlush = true;

            VkImageSubresourceRange range{};
            range.aspectMask = aspect;
            range.baseMipLevel = 0;
            range.levelCount = VK_REMAINING_MIP_LEVELS;
            range.baseArrayLayer = 0;
            range.layerCount = VK_REMAINING_ARRAY_LAYERS;
            if (isDepthOrStencil) {
                VkClearDepthStencilValue clearValue{1.0F, 0};
                vkCmdClearDepthStencilImage(cmd, slot.texture->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             &clearValue, 1, &range);
            } else {
                VkClearColorValue clearValue{{0.0F, 0.0F, 0.0F, 1.0F}};
                vkCmdClearColorImage(cmd, slot.texture->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1,
                                      &range);
            }
        }
    });
}

}  // namespace

std::unique_ptr<Executor> Executor::create(rx::rhi::Device& device) {
    auto allocator = rx::rhi::Allocator::createRaw(device.physicalDevice(), device.device(), device.instance());
    if (!allocator.has_value()) {
        RX_LOG_ERROR("rx_graph: Executor::create: rx::rhi::Allocator::createRaw failed");
        return nullptr;
    }

    auto impl = std::make_unique<Impl>(device.device(), device.physicalDevice(), device.graphicsQueue(),
                                        device.graphicsQueueFamily(), std::move(*allocator));

    // Task 3 ambiguity resolution #7: query VK_EXT_debug_utils availability
    // once here, via the plain volk-global-function-pointer-null-check
    // this whole codebase already relies on everywhere it calls a raw
    // vkFoo (context.cpp/device.cpp track NO explicit enabled-extension
    // list at all -- confirmed by reading both; there is nothing to read
    // "capabilities" off of beyond volk's own loaded tables). volkLoadDevice()
    // (already run inside rx::rhi::Device::create(), before any Executor
    // can exist) leaves this device-level function pointer non-null if and
    // only if VK_EXT_debug_utils was actually enabled/available -- exactly
    // the same null-checked-before-use discipline every debug-label call
    // site below follows.
    impl->debugUtilsAvailable = (vkCmdBeginDebugUtilsLabelEXT != nullptr && vkCmdEndDebugUtilsLabelEXT != nullptr);

    return std::unique_ptr<Executor>(new Executor(std::move(impl)));
}

Executor::Executor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Executor::~Executor() {
    if (!impl_) {
        return;
    }
    // "waits idle via existing Device teardown conventions" [task-3-brief.md]
    // -- the same vkDeviceWaitIdle-before-destroying-anything discipline
    // FrameSync's/DeletionQueue's own destructor contracts already
    // document as load-bearing elsewhere in this engine. Proves no
    // in-flight command buffer can still reference anything this
    // destructor is about to tear down, before retireAll()/flushAll() run
    // unconditionally (both otherwise-unsafe on a queue that might still
    // be executing).
    vkDeviceWaitIdle(impl_->device);
    impl_->pool.retireAll(impl_->frameCounter, impl_->deletionQueue);
    impl_->deletionQueue.flushAll();
}

void Executor::realize(const RenderGraph& graph) {
    Impl& impl = *impl_;
    const CompiledGraph& compiled = graph.compiled();
    const std::span<const PhysicalResource> resources = compiled.resources();

    impl.pool.beginRealizeBatch();

    std::vector<ResolvedResource> resolved(resources.size());
    std::unordered_map<std::string, uint32_t> nameToIndex;
    nameToIndex.reserve(resources.size());

    for (uint32_t i = 0; i < resources.size(); ++i) {
        const PhysicalResource& physical = resources[i];
        nameToIndex.emplace(physical.name, i);

        ResolvedResource& entry = resolved[i];
        entry.isBuffer = physical.isBuffer;
        entry.isBackbuffer = physical.isBackbuffer;
        entry.isHistory = physical.isHistory;

        if (physical.isBackbuffer) {
            // Never pooled [Task 3 ambiguity resolution #4] -- image/view/
            // extent are filled in fresh by every execute() call instead;
            // format is already known now (compile() resolves it from
            // CompileInfo::swapchainFormat regardless of what its writer
            // pass declared -- see render_graph.cpp).
            entry.format = physical.attachment.format;
            continue;
        }

        if (physical.isHistory) {
            // Phase 4 Task 1: pinned, ping-ponged, looked up by NAME (never
            // shape) -- see TransientPool::acquireHistory()'s own comment.
            // `.image`/`.view` are deliberately left at their defaults;
            // every history-aware code path resolves the correct real slot
            // straight from Impl::pool.pinned(poolIndex) instead (see
            // ResolvedResource's own comment).
            const VkExtent2D extent = toExtent(physical.attachment);
            bool freshlyCreated = false;
            auto pinnedIndex =
                impl.pool.acquireHistory(physical.name, physical.attachment.format, extent, physical.imageUsage,
                                          physical.attachment.samples, impl.frameCounter, impl.deletionQueue,
                                          &freshlyCreated);
            if (!pinnedIndex.has_value()) {
                RX_LOG_ERROR("rx_graph: Executor::realize: failed to acquire a pinned history entry for resource '{}'",
                             physical.name);
                continue;
            }
            entry.poolIndex = *pinnedIndex;
            entry.format = physical.attachment.format;
            entry.extent = extent;
            if (freshlyCreated) {
                initializePinnedHistoryEntry(impl, *pinnedIndex);
            }
            continue;
        }

        if (physical.isBuffer) {
            auto poolIndex =
                impl.pool.acquireBuffer(physical.buffer.size, physical.buffer.usage, impl.frameCounter);
            if (!poolIndex.has_value()) {
                RX_LOG_ERROR("rx_graph: Executor::realize: failed to acquire a pooled buffer for resource '{}'",
                             physical.name);
                continue;
            }
            entry.poolIndex = *poolIndex;
            entry.buffer = impl.pool.buffer(*poolIndex).buffer->handle();
        } else {
            const VkExtent2D extent = toExtent(physical.attachment);
            auto poolIndex = impl.pool.acquireImage(physical.attachment.format, extent, physical.imageUsage,
                                                     physical.attachment.samples, impl.frameCounter);
            if (!poolIndex.has_value()) {
                RX_LOG_ERROR("rx_graph: Executor::realize: failed to acquire a pooled image for resource '{}'",
                             physical.name);
                continue;
            }
            entry.poolIndex = *poolIndex;
            const rx::rhi::Texture2D& texture = *impl.pool.image(*poolIndex).texture;
            entry.image = texture.image();
            entry.view = texture.view();
            entry.format = texture.format();
            entry.extent = texture.extent();
        }
    }

    impl.resources = std::move(resolved);
    impl.nameToIndex = std::move(nameToIndex);
}

void Executor::execute(const RenderGraph& graph, VkCommandBuffer cmd, VkImage backbufferImage,
                        VkImageView backbufferView, VkExtent2D backbufferExtent) {
    Impl& impl = *impl_;
    const CompiledGraph& compiled = graph.compiled();
    const std::span<const PhysicalResource> resources = compiled.resources();

    ++impl.frameCounter;

    // Evict any pooled entry a past realize() released and nothing has
    // reclaimed for kStaleAfterExecutes consecutive execute() calls, then
    // let the deletion queue actually run whatever this (or a past) sweep
    // already tagged as safe -- see transient_pool.h's own comment on
    // sweepStale()'s frame tagging, and Task 3 ambiguity resolution #2 for
    // the same-queue/frames-in-flight reasoning that makes self-pacing
    // this queue (rather than needing an externally-confirmed fence
    // signal) correct as long as callers drive execute() at a pace bounded
    // by real frame-in-flight fencing (e.g. rx::rhi::FrameSync) -- trivially
    // true for a caller that submits and waits synchronously between
    // calls, as Task 3's own GPU test does, since that is strictly MORE
    // conservative than the bound this reasoning requires.
    impl.pool.sweepStale(impl.frameCounter, impl.deletionQueue);
    if (impl.frameCounter > detail::kStaleAfterExecutes) {
        impl.deletionQueue.onFrameFenceSignaled(impl.frameCounter - detail::kStaleAfterExecutes - 1);
    }

    // Bind the backbuffer's ResolvedResource fresh every call [Task 3
    // ambiguity resolution #4].
    for (uint32_t i = 0; i < resources.size(); ++i) {
        if (resources[i].isBackbuffer) {
            ResolvedResource& entry = impl.resources.at(i);
            entry.image = backbufferImage;
            entry.view = backbufferView;
            entry.extent = backbufferExtent;
            break;
        }
    }

    std::vector<bool> firstBarrierSeen(resources.size(), false);
    std::vector<bool> attachmentEverWritten(resources.size(), false);
    std::unordered_map<uint32_t, VkPipelineStageFlags2> finalStageThisExecute;
    std::unordered_map<uint32_t, VkAccessFlags2> finalAccessThisExecute;

    const std::span<const uint32_t> order = compiled.executionOrder();
    const std::span<const PassBarriers> allBarriers = compiled.passBarriers();

    for (size_t pos = 0; pos < order.size(); ++pos) {
        const uint32_t rawIndex = order[pos];
        const Pass& pass = graph.passAt(rawIndex);
        const std::span<const ResourceAccess> accesses = compiled.passAccesses(rawIndex);

        beginDebugLabel(impl, cmd, pass.name());

        applyBarriers(impl, compiled, cmd, allBarriers[pos], firstBarrierSeen);
        synthesizeFirstUseBufferBarrierIfNeeded(impl, compiled, cmd, accesses, pos, firstBarrierSeen);
        applyHistoryAccesses(impl, compiled, cmd, accesses);

        // Classify this pass's own declared accesses into color/depth
        // attachment outputs [spec Phase 3 design, D2] -- every other
        // declared AccessKind (TextureInput, StorageBufferOutput/Input)
        // resolves to a layout that is neither COLOR_ATTACHMENT_OPTIMAL
        // nor DEPTH_ATTACHMENT_OPTIMAL (see pass.h's Pass::resolveAccess
        // table), so this is an exhaustive, unambiguous classification,
        // not a heuristic.
        std::vector<uint32_t> colorPhysIdx;
        std::optional<uint32_t> depthPhysIdx;
        for (const ResourceAccess& access : accesses) {
            if (access.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                if (std::find(colorPhysIdx.begin(), colorPhysIdx.end(), access.physicalIndex) ==
                    colorPhysIdx.end()) {
                    colorPhysIdx.push_back(access.physicalIndex);
                }
            } else if (access.layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
                depthPhysIdx = access.physicalIndex;
            }
        }

        const bool isGraphicsPass = !colorPhysIdx.empty() || depthPhysIdx.has_value();

        VkExtent2D renderArea{0, 0};
        std::vector<VkRenderingAttachmentInfo> colorAttachments;
        VkRenderingAttachmentInfo depthAttachment{};

        if (isGraphicsPass) {
            colorAttachments.reserve(colorPhysIdx.size());
            for (uint32_t physIdx : colorPhysIdx) {
                const ResolvedResource& resolvedRes = impl.resources.at(physIdx);

                VkRenderingAttachmentInfo info{};
                info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                info.imageView = resolveAttachmentView(impl, resolvedRes);
                info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                // Task 3 ambiguity resolution #3: first write-use of an
                // attachment in a frame clears; later uses load; always
                // store. Clear values are fixed this phase (per-pass
                // configurable clear values are future work).
                info.loadOp = attachmentEverWritten[physIdx] ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
                info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                info.clearValue.color = VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}};
                attachmentEverWritten[physIdx] = true;

                colorAttachments.push_back(info);
                renderArea = resolvedRes.extent;
            }

            if (depthPhysIdx.has_value()) {
                const ResolvedResource& resolvedRes = impl.resources.at(*depthPhysIdx);

                depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                depthAttachment.imageView = resolveAttachmentView(impl, resolvedRes);
                depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthAttachment.loadOp =
                    attachmentEverWritten[*depthPhysIdx] ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                depthAttachment.clearValue.depthStencil = VkClearDepthStencilValue{1.0F, 0};
                attachmentEverWritten[*depthPhysIdx] = true;

                if (colorAttachments.empty()) {
                    renderArea = resolvedRes.extent;
                }
            }

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = VkRect2D{{0, 0}, renderArea};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
            renderingInfo.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
            renderingInfo.pDepthAttachment = depthPhysIdx.has_value() ? &depthAttachment : nullptr;
            vkCmdBeginRendering(cmd, &renderingInfo);
        }

        // Task 5 (rx_material): mirror this pass's own attachment shape
        // into a device-free PassSignature -- the exact same
        // colorPhysIdx/depthPhysIdx classification isGraphicsPass above
        // already computed, just re-expressed as formats/sample-count
        // instead of physical-resource indices. Left default-constructed
        // (colorCount == 0, depthFormat == VK_FORMAT_UNDEFINED) for a bare
        // pass (isGraphicsPass == false) -- there is no attachment shape
        // to report, matching PassContext::passSignature()'s own doc
        // comment. Sample count is read off PhysicalResource::attachment
        // (compile()'s own resolved copy, per resources.h), not
        // ResolvedResource (which carries no sample-count field at all,
        // since it exists to answer "what real image/buffer backs this
        // resource", not "what shape did the pass declare it as") --
        // taken from whichever attachment this pass actually has (color
        // preferred, matching the renderArea fallback logic just above);
        // every attachment in one dynamic-rendering scope shares one
        // sample count in practice, so any single one is representative.
        PassSignature signature;
        if (isGraphicsPass) {
            signature.colorCount =
                static_cast<uint32_t>(std::min(colorPhysIdx.size(), signature.colorFormats.size()));
            for (uint32_t i = 0; i < signature.colorCount; ++i) {
                signature.colorFormats[i] = impl.resources.at(colorPhysIdx[i]).format;
            }
            if (depthPhysIdx.has_value()) {
                signature.depthFormat = impl.resources.at(*depthPhysIdx).format;
            }
            if (!colorPhysIdx.empty()) {
                signature.samples = compiled.resources()[colorPhysIdx[0]].attachment.samples;
            } else if (depthPhysIdx.has_value()) {
                signature.samples = compiled.resources()[*depthPhysIdx].attachment.samples;
            }
        }

        PassContext ctx(*this);
        ctx.cmd = cmd;
        ctx.renderArea = renderArea;
        ctx.passSignature_ = signature;
        pass.invokeExecute(ctx);

        if (isGraphicsPass) {
            vkCmdEndRendering(cmd);
        }

        endDebugLabel(impl, cmd);

        // [Fix round 1, Critical finding]: UNION every pass's stage into
        // this resource's running total for the whole execute() call --
        // NOT an overwrite. PhysicalResource::lastUsePass names only the
        // single highest-position touching pass, but a resource can
        // legitimately have several passes tied for "last" in the sense
        // that matters here: several readers of the same write, at
        // different pipeline stages, with no intervening write between
        // any of them (e.g. a shadow map sampled by both a lighting pass
        // and a debug-overlay pass). Every one of those readers' work must
        // still be waited on by next call's first-use-of-frame override
        // (see applyBarriers()) -- an overwrite silently drops every
        // reader but the last one processed, which reproduced as a real,
        // empirically-verified write-after-read hazard (see
        // task-3-review.md's Critical finding and task-3-report.md's fix-
        // round section). Carrying the union forward is always safe (only
        // ever adds extra, conservative wait scope) -- a subsequent WRITE
        // within the SAME execute() call already correctly waits on every
        // reader via Task 2's own invalidatedStagesUnion-based WAR barrier,
        // so there is no case where accumulating instead of resetting on a
        // write causes an actual over-synchronization problem worth
        // special-casing.
        // Phase 4 Task 1: history physicalIndex entries are deliberately
        // excluded here -- their poolIndex addresses Impl::pool's pinned_
        // vector, not images_/buffers_ (the two the loop just below this
        // one indexes into), and their cross-frame carry-forward is
        // already handled per-slot, inline, by applyHistoryAccesses()
        // above (PinnedHistorySlot::barrierState persists across
        // execute() calls on its own -- there is nothing left for this
        // generic mechanism to do for them at all).
        for (const auto& [physIdx, combined] : combineAccessesByResource(accesses)) {
            const ResolvedResource& combinedRes = impl.resources.at(physIdx);
            if (!combinedRes.isBackbuffer && !combinedRes.isHistory) {
                finalStageThisExecute[physIdx] |= combined.stages;
                finalAccessThisExecute[physIdx] |= combined.access & kPoolWriteAccessMask;
            }
        }
    }

    applyBarriers(impl, compiled, cmd, compiled.finalBarriers(), firstBarrierSeen);

    // Task 3 ambiguity resolution #2: record what stage each pooled
    // resource this call actually touched was left at, so the NEXT
    // execute() call's first-use-of-frame override (applyBarriers() above)
    // chains off the real value instead of staying pinned at
    // ALL_COMMANDS forever.
    for (const auto& [physIdx, stage] : finalStageThisExecute) {
        const ResolvedResource& resolvedRes = impl.resources.at(physIdx);
        const VkAccessFlags2 access = finalAccessThisExecute[physIdx];
        if (resources[physIdx].isBuffer) {
            impl.pool.buffer(resolvedRes.poolIndex).lastFrameFinalStages = stage;
            impl.pool.buffer(resolvedRes.poolIndex).lastFrameFinalAccess = access;
            impl.pool.touchBuffer(resolvedRes.poolIndex, impl.frameCounter);
        } else {
            impl.pool.image(resolvedRes.poolIndex).lastFrameFinalStages = stage;
            impl.pool.image(resolvedRes.poolIndex).lastFrameFinalAccess = access;
            impl.pool.touchImage(resolvedRes.poolIndex, impl.frameCounter);
        }
    }
}

namespace {

uint32_t lookupResolvedIndex(const Executor::Impl& impl, std::string_view name) {
    auto it = impl.nameToIndex.find(std::string(name));
    if (it == impl.nameToIndex.end()) {
        throw std::out_of_range("rx_graph: PassContext: no realized resource named '" + std::string(name) + "'");
    }
    return it->second;
}

// [Fix round 1, Minor finding + its supplemental follow-up]: a resolved
// resource's image-only fields (view/image/format) are never set to
// anything but their defaults (VK_NULL_HANDLE/VK_FORMAT_UNDEFINED) for a
// buffer resource, and vice versa for `buffer` on an image resource --
// returning one of those silently for a legitimately-registered but
// wrong-kind name is a meaningless value, not an error a caller could ever
// act on correctly. Every one of PassContext's four resolvers throws the
// same way every other "not resolvable" case documented on that class
// does (naming both the resource and its actual kind) rather than
// special-casing any one path to succeed with garbage.
void requireKind(std::string_view name, const ResolvedResource& resolved, bool expectBuffer,
                  std::string_view resolverName) {
    if (resolved.isBuffer != expectBuffer) {
        throw std::out_of_range("rx_graph: PassContext::" + std::string(resolverName) + "(): '" + std::string(name) +
                                 "' is a " + (resolved.isBuffer ? "buffer" : "image") + " resource, not " +
                                 (expectBuffer ? "a buffer" : "an image"));
    }
}

}  // namespace

VkImageView Executor::resolveImageView(std::string_view name) const {
    const uint32_t idx = lookupResolvedIndex(*impl_, name);
    const ResolvedResource& resolved = impl_->resources.at(idx);
    requireKind(name, resolved, /*expectBuffer=*/false, "imageView");
    if (resolved.isHistory) {
        // Phase 4 Task 1: always THIS FRAME'S READ SLOT -- see executor.h's
        // updated doc comment on this resolver for why there is no
        // equivalent "give me the write slot" path at all.
        const auto readSlot = static_cast<uint32_t>((impl_->frameCounter + 1) % 2);
        return impl_->pool.pinned(resolved.poolIndex).slots[readSlot].texture->view();
    }
    return resolved.view;
}

VkImage Executor::resolveImage(std::string_view name) const {
    const uint32_t idx = lookupResolvedIndex(*impl_, name);
    const ResolvedResource& resolved = impl_->resources.at(idx);
    requireKind(name, resolved, /*expectBuffer=*/false, "image");
    if (resolved.isHistory) {
        const auto readSlot = static_cast<uint32_t>((impl_->frameCounter + 1) % 2);
        return impl_->pool.pinned(resolved.poolIndex).slots[readSlot].texture->image();
    }
    return resolved.image;
}

VkBuffer Executor::resolveBuffer(std::string_view name) const {
    const uint32_t idx = lookupResolvedIndex(*impl_, name);
    const ResolvedResource& resolved = impl_->resources.at(idx);
    requireKind(name, resolved, /*expectBuffer=*/true, "buffer");
    return resolved.buffer;
}

VkFormat Executor::resolveImageFormat(std::string_view name) const {
    const uint32_t idx = lookupResolvedIndex(*impl_, name);
    const ResolvedResource& resolved = impl_->resources.at(idx);
    requireKind(name, resolved, /*expectBuffer=*/false, "imageFormat");
    // Both ping-pong slots of a history resource always share one shape
    // [design contract point 2] -- resolved.format (set once in
    // Executor::realize(), unconditionally, regardless of isHistory) is
    // already correct with no per-slot redirect needed.
    return resolved.format;
}

bool Executor::resolveHistoryValid(std::string_view name) const {
    const uint32_t idx = lookupResolvedIndex(*impl_, name);
    const ResolvedResource& resolved = impl_->resources.at(idx);
    if (!resolved.isHistory) {
        throw std::out_of_range("rx_graph: PassContext::historyValid(): '" + std::string(name) +
                                 "' is not a history resource");
    }
    const auto readSlot = static_cast<uint32_t>((impl_->frameCounter + 1) % 2);
    return impl_->pool.pinned(resolved.poolIndex).slots[readSlot].everWrittenByRealPass;
}

VkImageView PassContext::imageView(std::string_view name) const { return executor_->resolveImageView(name); }
VkImage PassContext::image(std::string_view name) const { return executor_->resolveImage(name); }
VkBuffer PassContext::buffer(std::string_view name) const { return executor_->resolveBuffer(name); }
VkFormat PassContext::imageFormat(std::string_view name) const { return executor_->resolveImageFormat(name); }
bool PassContext::historyValid(std::string_view name) const { return executor_->resolveHistoryValid(name); }

namespace detail {

VkPipelineStageFlags2 debugLastFrameFinalStages(const Executor& executor, std::string_view name) {
    const Executor::Impl& impl = *executor.impl_;
    const uint32_t idx = lookupResolvedIndex(impl, name);
    const ResolvedResource& resolved = impl.resources.at(idx);
    if (resolved.isBuffer) {
        return impl.pool.buffer(resolved.poolIndex).lastFrameFinalStages;
    }
    return impl.pool.image(resolved.poolIndex).lastFrameFinalStages;
}

}  // namespace detail

}  // namespace rx::graph
