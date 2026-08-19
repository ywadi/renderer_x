#include <rx_graph/executor.h>

#include "transient_pool.h"

#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_rhi_vk/tracy_gpu.h>
#include <rx_task/scheduler.h>
#include <volk.h>

#include <algorithm>
#include <array>
#include <atomic>
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

// [D29, gate ruling RC2] The one place both of executor.cpp's depth
// clear-value sites derive their VkClearDepthStencilValue from --
// Standard (D13's pre-Stage-2 default, and every non-main-camera pass
// today: shadow maps stay standard-Z per D13 itself) clears to 1.0 (the
// far plane, in the LESS-family convention); Reversed (D13's main-camera
// convention from Stage 2 on) clears to 0.0 (the far plane, in the
// GREATER_OR_EQUAL-family convention) -- both leave stencil at 0
// (unused by any depth-only attachment this engine builds today).
VkClearDepthStencilValue depthClearValueFor(DepthConvention convention) {
    return convention == DepthConvention::Reversed ? VkClearDepthStencilValue{0.0F, 0} : VkClearDepthStencilValue{1.0F, 0};
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

// Phase 4 Task 7 [spec D4 amendment; R:threading "Per-Thread, Per-Frame
// Command Pools"]: one worker (or the participating main) thread's secondary
// command-buffer arena for ONE frame-in-flight slot. `pool` is created
// lazily (VK_NULL_HANDLE until the first chunk this (frameSlot, threadIndex)
// pair ever needs to record); `buffers` grows on demand the first time a
// given frame needs MORE simultaneous secondaries from this pool than any
// past frame at this same slot ever did, and is otherwise fully reused,
// frame after frame, via a whole-pool vkResetCommandPool() at the start of
// each frame that reuses this slot (resetChunkPoolsForFrameSlot(), below) --
// never a per-buffer reset (this pool is created WITHOUT
// VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT), matching rx::rhi::
// FrameSync's own primary-command-buffer pools (frame_sync.cpp) and the
// "bulk reset; no per-buffer fences needed" pattern R:threading documents.
struct ChunkCommandPool {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> buffers;
    uint32_t usedThisFrame = 0;
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

    // Tracy GPU-zone context [Phase 4 Stage 0 Task 3, spec D3] -- created
    // once per Executor (i.e. per Device, since Executor::create() is the
    // sole owner of exactly one of these) in Executor::create() below, and
    // destroyed in ~Executor(). nullptr when RX_TRACY/TRACY_ENABLE is off
    // (createGpuProfileContext() itself degrades to a constant nullptr
    // return in that build -- see tracy_gpu.h/.cpp) or if context creation
    // failed -- every use of it below is null-safe either way.
    rx::rhi::GpuProfileContext gpuProfileCtx = nullptr;

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

    // Phase 4 Task 7 [spec D4 amendment]: non-owning -- `scheduler` must
    // outlive this Executor (executor.h's create() doc comment). Every
    // chunked pass's fan-out goes through this pointer; never null once
    // construction succeeds (create() always supplies a real reference).
    rx::task::Scheduler* scheduler = nullptr;

    // Indexed [frameSlot][threadIndex] -- see ChunkCommandPool's own
    // comment above and chunkFrameSlotForThisExecute()'s comment below for
    // exactly what each index means. Sized to scheduler->workerCount() + 1
    // entries per frame slot at construction time (below) -- the "+1"
    // covers the participating main thread's own index 0 alongside every
    // background worker's [scheduler.h: parallelFor()'s workerIndex
    // includes the calling thread] -- and never resized again (Scheduler::
    // workerCount() is fixed for a Scheduler's whole lifetime).
    std::array<std::vector<ChunkCommandPool>, rx::rhi::FrameSync::kFramesInFlight> chunkPools;

    // Phase 4 Task 7 debug/stats seam backing detail::debugChunkStats() --
    // see executor.h's own comment on that function/struct.
    uint32_t lastChunkCount = 0;
    std::atomic<uint64_t> totalChunkPoolAllocations{0};

    Impl(VkDevice deviceIn, VkPhysicalDevice physicalDeviceIn, VkQueue graphicsQueueIn, uint32_t graphicsQueueFamilyIn,
         rx::rhi::Allocator allocatorIn, rx::task::Scheduler& schedulerIn)
        : device(deviceIn),
          physicalDevice(physicalDeviceIn),
          graphicsQueue(graphicsQueueIn),
          graphicsQueueFamily(graphicsQueueFamilyIn),
          allocator(std::move(allocatorIn)),
          pool(physicalDeviceIn, deviceIn, allocator),
          scheduler(&schedulerIn) {
        for (std::vector<ChunkCommandPool>& perThread : chunkPools) {
            perThread.resize(static_cast<size_t>(scheduler->workerCount()) + 1);
        }
    }
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
void initializePinnedHistoryEntry(Executor::Impl& impl, uint32_t pinnedIndex, DepthConvention depthConvention) {
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
                // [D29, gate ruling RC2] Per-pass, not a hardcoded 1.0 --
                // see depthClearValueFor()'s own comment.
                const VkClearDepthStencilValue clearValue = depthClearValueFor(depthConvention);
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

// Phase 4 Task 7 [spec D4 amendment]: resets every ChunkCommandPool this
// Executor has ever lazily created for frame-in-flight slot `frameSlot` --
// called once, unconditionally, near the top of every Executor::execute()
// call, before any pass (chunked or otherwise) runs this frame. A whole-pool
// vkResetCommandPool() recycles every secondary command buffer ever
// allocated from that pool back to Vulkan's INITIAL state in one call
// [R:threading: "bulk reset; no per-buffer fences needed"] -- `usedThisFrame`
// resetting to 0 alongside it is what lets acquireChunkCommandBuffer() below
// treat `buffers` as a reusable arena rather than re-allocating every frame.
// A pool that was never actually created (still VK_NULL_HANDLE -- no chunked
// pass has ever landed on this exact (frameSlot, threadIndex) pair) is
// simply skipped; resetting a pool that was never used costs a driver call
// for nothing.
void resetChunkPoolsForFrameSlot(Executor::Impl& impl, uint32_t frameSlot) {
    for (ChunkCommandPool& cp : impl.chunkPools[frameSlot]) {
        if (cp.pool != VK_NULL_HANDLE) {
            vkResetCommandPool(impl.device, cp.pool, 0);
        }
        cp.usedThisFrame = 0;
    }
}

// Returns a secondary VkCommandBuffer, in Vulkan's INITIAL state (ready for
// vkBeginCommandBuffer), from `threadIndex`'s own arena for `frameSlot` --
// VK_NULL_HANDLE (logged) on any Vulkan failure, which callers must treat as
// "this chunk recorded nothing" rather than crash. Reuses an
// already-allocated buffer left over from a past frame's use of this exact
// (frameSlot, threadIndex) pair when one is available (resetChunkPoolsForFrameSlot()
// above already reset it to INITIAL state this frame); allocates a genuinely
// new one -- counted via impl.totalChunkPoolAllocations, the
// detail::debugChunkStats() seam's own budget counter -- only the first time
// a given (frameSlot, threadIndex) pair needs MORE simultaneous secondaries
// than any past frame at that same slot ever did (in this project's own
// samples, at most one chunked pass per frame, so this budget stabilizes
// after the first kFramesInFlight execute() calls).
//
// Never touched by more than one thread at a time BY CONSTRUCTION --
// `threadIndex` is the identity of whichever single physical OS thread is
// calling this (docs/threading.md's own "never touched by more than one
// thread at a time" contract for exactly this arena: a VkCommandPool is not
// internally synchronized, and no two of this Scheduler's threads ever share
// a workerIndex), so no lock guards `cp` itself. impl.totalChunkPoolAllocations
// is the one piece of shared state this function CAN touch from multiple
// threads concurrently (two different threadIndex's own first-allocation-
// this-frame moments can race each other) -- a std::atomic<uint64_t>, per
// Impl's own field comment, precisely because of that.
VkCommandBuffer acquireChunkCommandBuffer(Executor::Impl& impl, uint32_t frameSlot, uint32_t threadIndex) {
    ChunkCommandPool& cp = impl.chunkPools[frameSlot].at(threadIndex);

    if (cp.pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // TRANSIENT, deliberately NOT RESET_COMMAND_BUFFER_BIT -- see
        // ChunkCommandPool's own comment for why (whole-pool reset only).
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = impl.graphicsQueueFamily;
        const VkResult result = vkCreateCommandPool(impl.device, &poolInfo, nullptr, &cp.pool);
        if (result != VK_SUCCESS) {
            RX_LOG_ERROR("rx_graph: Executor: vkCreateCommandPool failed for chunk pool (frameSlot={}, "
                         "threadIndex={}): VkResult={}",
                         frameSlot, threadIndex, static_cast<int>(result));
            return VK_NULL_HANDLE;
        }
    }

    if (cp.usedThisFrame < cp.buffers.size()) {
        return cp.buffers[cp.usedThisFrame++];
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = cp.pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    const VkResult result = vkAllocateCommandBuffers(impl.device, &allocInfo, &cmd);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("rx_graph: Executor: vkAllocateCommandBuffers failed for chunk secondary (frameSlot={}, "
                     "threadIndex={}): VkResult={}",
                     frameSlot, threadIndex, static_cast<int>(result));
        return VK_NULL_HANDLE;
    }
    cp.buffers.push_back(cmd);
    cp.usedThisFrame++;
    impl.totalChunkPoolAllocations.fetch_add(1, std::memory_order_relaxed);
    return cmd;
}

// Phase 4 Task 7: backs PassContext::chunkCommandBuffer() -- see that
// method's own doc comment (executor.h) for the full contract. thread_local
// rather than a field on PassContext itself: a chunked pass's PassContext is
// ONE object shared by every chunk's own (possibly concurrent) invocation of
// Pass::invokeExecuteChunked(), so any per-chunk mutable state on PassContext
// itself would race; each thread instead only ever reads/writes its OWN slot
// here.
thread_local VkCommandBuffer tChunkCommandBuffer = VK_NULL_HANDLE;

// RAII guard around one chunk callback invocation: sets the calling thread's
// tChunkCommandBuffer slot for the callback's duration, restoring whatever
// it held before on scope exit (including via an exception unwinding through
// the callback) -- "restore the previous value" rather than "always clear to
// VK_NULL_HANDLE" defends a hypothetical nested chunk invocation (none of
// this project's own callers nest them, but this makes the guarantee
// unconditional rather than "true in practice"). This is also what makes
// PassContext::chunkCommandBuffer() correctly throw when called from a
// WHOLE-PASS callback even on a thread that previously ran a chunk earlier
// in the SAME execute() call: this scope's destructor already restored that
// thread's slot back to VK_NULL_HANDLE (or whatever it was before) the
// moment the earlier chunk's own callback returned, so no stale handle from
// a past chunk can ever leak into a later, unrelated callback.
class ChunkCommandBufferScope {
public:
    explicit ChunkCommandBufferScope(VkCommandBuffer cmd) : previous_(tChunkCommandBuffer) {
        tChunkCommandBuffer = cmd;
    }
    ~ChunkCommandBufferScope() { tChunkCommandBuffer = previous_; }
    ChunkCommandBufferScope(const ChunkCommandBufferScope&) = delete;
    ChunkCommandBufferScope& operator=(const ChunkCommandBufferScope&) = delete;

private:
    VkCommandBuffer previous_;
};

}  // namespace

std::unique_ptr<Executor> Executor::create(rx::rhi::Device& device, rx::task::Scheduler& scheduler) {
    auto allocator = rx::rhi::Allocator::createRaw(device.physicalDevice(), device.device(), device.instance());
    if (!allocator.has_value()) {
        RX_LOG_ERROR("rx_graph: Executor::create: rx::rhi::Allocator::createRaw failed");
        return nullptr;
    }

    auto impl = std::make_unique<Impl>(device.device(), device.physicalDevice(), device.graphicsQueue(),
                                        device.graphicsQueueFamily(), std::move(*allocator), scheduler);

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

    // Tracy GPU-zone context: created once per Executor/Device [Phase 4
    // Stage 0 Task 3, spec D3] -- see tracy_gpu.h's own comment for exactly
    // which of TracyVkContextCalibrated/TracyVkContext this resolves to,
    // and why null-safe unconditionally (nullptr on RX_TRACY=OFF or on any
    // creation failure, never fatal to Executor::create() itself -- a
    // renderer must not fail to start because a profiler capability could
    // not be wired up).
    impl->gpuProfileCtx = rx::rhi::createGpuProfileContext(device);

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
    // Null-safe unconditionally (RX_TRACY off, or any context-creation
    // failure) -- see tracy_gpu.h's own comment on RX_GPU_CONTEXT_DESTROY.
    RX_GPU_CONTEXT_DESTROY(impl_->gpuProfileCtx);
    // Phase 4 Task 7: destroying each lazily-created chunk pool implicitly
    // frees every secondary VkCommandBuffer ever allocated from it -- same
    // "destroying the pool is enough, no separate vkFreeCommandBuffers"
    // pattern rx::rhi::CommandContext::destroyAll() already uses. Safe here,
    // after vkDeviceWaitIdle() above, for the same reason every other
    // teardown call in this destructor is: nothing on the device can still
    // be executing (or about to reference) any of these buffers.
    for (std::vector<ChunkCommandPool>& perThread : impl_->chunkPools) {
        for (ChunkCommandPool& cp : perThread) {
            if (cp.pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(impl_->device, cp.pool, nullptr);
            }
        }
    }
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
                // [D29, gate ruling RC2] `physical.attachment.depthConvention`
                // is in scope here (this is realize(), not
                // initializePinnedHistoryEntry() itself) precisely because
                // that function needs to know which clear value to use for
                // a freshly-created history depth attachment too -- see its
                // own comment.
                initializePinnedHistoryEntry(impl, *pinnedIndex, physical.attachment.depthConvention);
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
    // Whole-execute() CPU zone [Phase 4 Stage 0 Task 3, spec D3: "Executor::
    // execute (whole)"] -- spans this entire call, i.e. every pass this
    // frame, on top of the per-pass zones declared inside the loop below.
    RX_ZONE;
    Impl& impl = *impl_;
    const CompiledGraph& compiled = graph.compiled();
    const std::span<const PhysicalResource> resources = compiled.resources();

    ++impl.frameCounter;

    // Phase 4 Task 7 [spec D4 amendment]: this call's own frame-in-flight
    // slot for the CHUNK secondary-buffer arenas (ChunkCommandPool, above) --
    // an independent 2-cycle from history's own ping-pong parity below (both
    // derive from frameCounter, but there is no requirement they share a
    // phase; each only needs to be internally consistent with itself across
    // calls). Reset UNCONDITIONALLY, before any pass runs this frame, exactly
    // once -- a chunked pass may be the very first pass in the graph.
    const uint32_t chunkFrameSlot = static_cast<uint32_t>(impl.frameCounter % rx::rhi::FrameSync::kFramesInFlight);
    resetChunkPoolsForFrameSlot(impl, chunkFrameSlot);

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

        // Per-pass CPU zone, named dynamically from the pass's own
        // std::string-backed name [Phase 4 Stage 0 Task 3, spec D3: "per-
        // pass named zones (use the pass's name string ... Tracy zones
        // want static strings; use ZoneScopedN for fixed + ZoneText/
        // ZoneName for the dynamic pass name")]. A pass name is set once,
        // at RenderGraph::addPass() time, and never changes for the
        // lifetime of the graph -- but it is still a runtime std::string,
        // never a compile-time literal, so it cannot be handed to
        // ZoneScopedN's own `name` parameter directly (Tracy retains only
        // the pointer for that one, assuming static storage duration).
        // RX_ZONE_NAMED below gives the zone a fixed, always-valid display
        // name ("graph_pass"); RX_ZONE_DYNAMIC_NAME then attaches this
        // call's real pass name via Tracy's documented per-call dynamic-
        // name mechanism (see rx_core/profile.h's own citation of the
        // Tracy manual for exactly this macro).
        RX_ZONE_NAMED("graph_pass");
        RX_ZONE_DYNAMIC_NAME(pass.name().data(), pass.name().size());

        // Matching per-pass GPU zone [spec D3: "TracyVkZone around
        // Executor's per-pass command recording"] -- spans from just
        // before this pass's own barriers through its
        // vkCmdBeginRendering/execute-callback/vkCmdEndRendering (i.e. the
        // same span the CPU zone above covers), timestamped on `cmd`
        // itself. Uses the "transient" GPU-zone idiom (TracyVkZoneTransient,
        // via RX_GPU_ZONE_DYNAMIC -- see tracy_gpu.h's own citation of the
        // Tracy manual's "Transient GPU zones" section) rather than plain
        // TracyVkZone specifically because -- exactly like the CPU zone
        // above -- a render-graph pass's name is a runtime std::string, not
        // a compile-time literal; TracyVkZone's own `name` parameter must be
        // the latter. `pass.name().data()` is safe to hand to Tracy's
        // internal `strlen()` call unterminated-string-free: Pass::name()
        // (render_graph.cpp) always returns the WHOLE backing std::string
        // (never a substring), so `.data()` is null-terminated by the same
        // guarantee std::string::c_str() gives. Null-safe unconditionally
        // (RX_GPU_ZONE_DYNAMIC compiles away entirely when TRACY_ENABLE is
        // off; Tracy's own VkCtxScope no-ops when constructed with a null
        // ctx when it is on -- verified directly against the vendored
        // v0.14.0 source before writing this).
        RX_GPU_ZONE_DYNAMIC(impl.gpuProfileCtx, rxGpuPassZone, cmd, pass.name().data());

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
        // Phase 4 Task 7: hoisted out of the `if (isGraphicsPass)` block
        // below (it used to be a block-local) so a CHUNKED graphics pass can
        // still reach it after the block closes -- see recordChunkedPass()'s
        // own comment for why its vkCmdBeginRendering() call (with
        // VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT set into this
        // same struct's `flags`) happens AFTER every chunk has already
        // finished recording, not here.
        VkRenderingInfo renderingInfo{};

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
                // [D29, gate ruling RC2] Per-pass, not a process-wide 1.0
                // constant -- see depthClearValueFor()'s own comment.
                depthAttachment.clearValue.depthStencil =
                    depthClearValueFor(compiled.resources()[*depthPhysIdx].attachment.depthConvention);
                attachmentEverWritten[*depthPhysIdx] = true;

                if (colorAttachments.empty()) {
                    renderArea = resolvedRes.extent;
                }
            }

            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = VkRect2D{{0, 0}, renderArea};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
            renderingInfo.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
            renderingInfo.pDepthAttachment = depthPhysIdx.has_value() ? &depthAttachment : nullptr;
            // Phase 4 Task 7: only the WHOLE-PASS path begins rendering
            // here, inline, with the (implicit, flags == 0) default
            // "contents recorded directly into the primary" mode -- a
            // CHUNKED pass instead defers this exact call to
            // recordChunkedPass() below, with
            // VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT set,
            // after every chunk's own secondary has finished recording (see
            // that function's own comment for why recording order and
            // primary-command-stream order are independent here).
            if (!pass.hasChunkedExecute()) {
                vkCmdBeginRendering(cmd, &renderingInfo);
            }
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
        ctx.renderArea = renderArea;
        ctx.passSignature_ = signature;

        // Phase 4 Task 7 [spec D4 amendment, "Parallelism is the engine
        // default, not a mode"]: a chunked pass fans its recording out in
        // parallel (recordChunkedPass()); a whole-pass one runs exactly as
        // every pass did before this task -- byte-identical, since this
        // branch reaches the SAME two statements (ctx.cmd = cmd;
        // pass.invokeExecute(ctx);) with nothing else changed around them.
        if (pass.hasChunkedExecute()) {
            recordChunkedPass(pass, ctx, cmd, isGraphicsPass, renderingInfo, signature, chunkFrameSlot);
        } else {
            ctx.cmd = cmd;
            pass.invokeExecute(ctx);
            if (isGraphicsPass) {
                vkCmdEndRendering(cmd);
            }
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

    // Collect this frame's GPU timestamp queries [spec D3: "TracyVkCollect
    // once per frame (hook where FrameSync completes a frame)"] -- hooked
    // HERE, at the very end of this same execute() call, rather than
    // reaching back into rx::rhi::FrameSync itself: FrameSync (frame_sync.h)
    // has no notion of Tracy or of this Executor's GPU context at all (by
    // design -- see profile.h/tracy_gpu.h's own "no Tracy in any OTHER
    // public header" rule), while Executor::execute() already runs exactly
    // once per real frame, on the exact command buffer FrameSync's own
    // frame-in-flight slot owns, and is the sole owner of gpuProfileCtx --
    // making this the one point in the whole call graph that already has
    // every piece Collect() needs with no new coupling. `cmd` is guaranteed
    // outside a render pass instance here (every vkCmdEndRendering this
    // call could have issued already ran, inside the per-pass loop above;
    // the finalBarriers() call just above never opens one) -- exactly
    // Tracy's own documented requirement for TracyVkCollect.
    RX_GPU_COLLECT(impl.gpuProfileCtx, cmd);
}

// Phase 4 Task 7 [spec D4 amendment; R:threading "Secondary Command Buffers
// + Dynamic Rendering"]: records `pass`'s chunked callback in parallel
// across impl_->scheduler's workers (background workers AND the
// participating main thread -- scheduler.h), then stitches every chunk's own
// recorded secondary into `cmd` via ONE vkCmdExecuteCommands() call, in
// chunk-index order.
//
// RECORDING ORDER VS. PRIMARY COMMAND-STREAM ORDER: for a GRAPHICS pass
// (isGraphicsPass), every chunk finishes recording its OWN, independent
// secondary command buffer BEFORE this function ever calls
// vkCmdBeginRendering() on `cmd` at all. That is equivalent to (not merely
// "close enough to") recording every chunk strictly inside the primary's
// rendering scope: a secondary is an independent command-buffer object, so
// the wall-clock order in which its contents are FILLED IN has no bearing on
// GPU-visible execution order, which is governed entirely by `cmd`'s own
// recorded command sequence -- vkCmdBeginRendering(cmd,
// ...SECONDARY_COMMAND_BUFFERS_BIT...) then vkCmdExecuteCommands(cmd, ...)
// then vkCmdEndRendering(cmd), exactly as if every chunk had been recorded
// strictly between the begin and the execute calls. Doing it in this order
// (fan out and finish every secondary FIRST, touch the primary only after)
// needs no restructuring of `renderingInfo`'s construction above and keeps
// the primary's own command stream exactly two calls longer than the
// whole-pass path (vkCmdBeginRendering + vkCmdExecuteCommands +
// vkCmdEndRendering vs. vkCmdBeginRendering + <inline recording> +
// vkCmdEndRendering) regardless of chunkCount.
//
// For a BARE/compute-class chunked pass (!isGraphicsPass) [task-7-brief.md:
// "Chunked COMPUTE passes: no rendering scope -- secondaries with plain
// inheritance, same fan-out"]: `renderingInfo` is never touched at all, and
// each secondary's own VkCommandBufferInheritanceInfo::pNext stays null
// (plain inheritance, no VkCommandBufferInheritanceRenderingInfo chained in)
// -- see the per-chunk lambda below.
//
// CHUNK 0 IS GUARANTEED SYNCHRONOUS, ON THIS CALL'S OWN THREAD -- discovered
// necessary while migrating samples/06_materials' own forward pass (not
// merely convenient): rx::material::MaterialSystem::bindInstance() (which a
// per-object material draw must call to resolve its pipeline and stream its
// parameter UBO) is main-thread-only, no exception, and a chunked callback
// has NO thread-affinity guarantee otherwise -- Scheduler::parallelFor()'s
// own doc comment is explicit that the calling thread merely "participates",
// racing every background worker for each chunk, including chunk 0.
// Without a genuine guarantee, a pass with unavoidably-sequential,
// main-thread-only per-frame setup (docs/threading.md's "Main-thread-only"
// list) would have no safe way to use setExecuteChunked() at all. This
// executor closes that gap once, here, rather than leaving every such pass
// to reinvent its own workaround: chunk 0's own invocation of `pass`'s
// callback always runs directly on the thread that called
// Executor::execute() (which is itself contractually this Executor's
// Scheduler's own main thread -- Executor::create()'s doc comment), to
// completion, BEFORE chunks [1, chunkCount) are ever handed to
// parallelFor() -- i.e. chunk 0 is never concurrent with any other chunk.
// A pass with no such constraint (samples/05_multipass's forward pass;
// samples/07_stress's) is unaffected: every chunk's own draw-recording work
// is equally safe on any thread, so this ordering costs nothing but
// (chunkCount > 1 ? one synchronous chunk's worth of otherwise-parallel
// work : nothing) -- negligible next to real per-frame draw volumes, and,
// for chunkCount == 1 (samples/07_stress's `--threads 1` A/B baseline)
// EXACTLY the fully-serial recording path that baseline needs, with no
// parallelFor()/enkiTS task-submission overhead at all.
void Executor::recordChunkedPass(const Pass& pass, PassContext& ctx, VkCommandBuffer cmd, bool isGraphicsPass,
                                  VkRenderingInfo& renderingInfo, const PassSignature& signature,
                                  uint32_t frameSlot) {
    Impl& impl = *impl_;

    const uint32_t chunkCount = detail::chunkCountForWorkerCount(impl.scheduler->workerCount());
    impl.lastChunkCount = chunkCount;
    std::vector<VkCommandBuffer> chunkBuffers(chunkCount, VK_NULL_HANDLE);

    // Records chunk `chunkIndex`'s own secondary command buffer, acquiring
    // it from `workerIndex`'s own (frameSlot, threadIndex) arena -- shared
    // verbatim by the guaranteed-synchronous chunk 0 call and every
    // parallelFor()-dispatched chunk [1, chunkCount) below, so the two
    // paths cannot silently drift apart.
    auto recordOneChunk = [&](uint32_t chunkIndex, uint32_t workerIndex) {
        // Per-chunk CPU zone [task-7-brief.md item 3: "RX_ZONE per
        // chunk-record (worker side)"] -- recorded on whichever thread
        // actually runs this chunk (always the calling thread for chunk 0 --
        // see this function's own comment above).
        RX_ZONE_NAMED("graph_pass_chunk_record");
        RX_ZONE_DYNAMIC_NAME(pass.name().data(), pass.name().size());

        const VkCommandBuffer secondary = acquireChunkCommandBuffer(impl, frameSlot, workerIndex);
        if (secondary == VK_NULL_HANDLE) {
            // Already logged by acquireChunkCommandBuffer() -- this chunk
            // simply contributes nothing (chunkBuffers[chunkIndex] stays
            // VK_NULL_HANDLE, filtered out below).
            return;
        }

        VkCommandBufferInheritanceRenderingInfo inheritRendering{};
        VkCommandBufferInheritanceInfo inherit{};
        inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
        // renderPass/framebuffer stay VK_NULL_HANDLE (dynamic rendering --
        // R:threading: "renderPass = NULL with dynamic rendering").
        // occlusionQueryEnable/queryFlags/pipelineStatistics stay at their
        // zero defaults -- no pass in this codebase uses queries.

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pInheritanceInfo = &inherit;

        if (isGraphicsPass) {
            // VkCommandBufferInheritanceRenderingInfo must match the
            // primary's own vkCmdBeginRendering() shape for this pass
            // exactly [R:threading] -- `signature` already IS that shape
            // (Executor::execute() derives it from the identical
            // colorPhysIdx/depthPhysIdx classification renderingInfo itself
            // was built from, just above this call). Unused color slots
            // stay VK_FORMAT_UNDEFINED (PassSignature's own zero-init
            // padding, per pass_signature.h); stencil is always
            // VK_FORMAT_UNDEFINED here too -- this engine never binds a
            // separate stencil attachment (pStencilAttachment is never set
            // on renderingInfo either).
            inheritRendering.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
            inheritRendering.colorAttachmentCount = signature.colorCount;
            inheritRendering.pColorAttachmentFormats = signature.colorFormats.data();
            inheritRendering.depthAttachmentFormat = signature.depthFormat;
            inheritRendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
            inheritRendering.rasterizationSamples = signature.samples;
            inherit.pNext = &inheritRendering;
            beginInfo.flags =
                VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        } else {
            // Plain inheritance -- inherit.pNext stays null.
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        }

        const VkResult beginResult = vkBeginCommandBuffer(secondary, &beginInfo);
        if (beginResult != VK_SUCCESS) {
            RX_LOG_ERROR(
                "rx_graph: Executor: vkBeginCommandBuffer failed for chunk {}/{} of pass '{}': VkResult={}",
                chunkIndex, chunkCount, std::string(pass.name()), static_cast<int>(beginResult));
            return;
        }

        {
            // Scoped so PassContext::chunkCommandBuffer() resolves to
            // `secondary` for exactly the duration of this one chunk's own
            // callback invocation, on exactly this thread -- see
            // ChunkCommandBufferScope's own comment.
            ChunkCommandBufferScope scope(secondary);
            pass.invokeExecuteChunked(ctx, chunkIndex, chunkCount);
        }

        vkEndCommandBuffer(secondary);
        chunkBuffers[chunkIndex] = secondary;
    };

    // The fan-out itself gets its own zone [task-7-brief.md item 3],
    // distinct from the per-chunk RX_ZONE recordOneChunk() declares above --
    // this one spans chunk 0's own synchronous call PLUS the whole blocking
    // parallelFor() call for the rest, on whichever thread reaches this
    // point (contractually this Executor's Scheduler's own main thread --
    // see this function's own comment above).
    RX_ZONE_NAMED("graph_pass_chunk_fanout");
    RX_ZONE_DYNAMIC_NAME(pass.name().data(), pass.name().size());

    // Chunk 0: always synchronous, always workerIndex 0 -- enkiTS's own
    // convention ("Thread 0 is main thread", verified directly against
    // TaskScheduler.h) makes 0 the correct, real identity for whichever
    // thread this call is running on, which Executor::create()'s own
    // contract already requires to be this Scheduler's main thread.
    recordOneChunk(0, /*workerIndex=*/0);

    if (chunkCount > 1) {
        impl.scheduler->parallelFor(chunkCount - 1, /*grainSize=*/1,
                                     [&](uint32_t begin, uint32_t /*end*/, uint32_t workerIndex) {
                                         // grainSize == 1 over [0, chunkCount
                                         // - 1): every partition is exactly
                                         // one item wide; `begin + 1` maps it
                                         // back into the real [1, chunkCount)
                                         // chunk-index space chunk 0 already
                                         // claimed above.
                                         recordOneChunk(begin + 1, workerIndex);
                                     });
    }

    // Filter out any chunk that failed to record at all (see the
    // VK_NULL_HANDLE early-returns above) -- vkCmdExecuteCommands() requires
    // commandBufferCount > 0 and every handle in pCommandBuffers to be
    // valid, so a defensive, logged degrade here is "this pass drew less
    // than it should have" (already logged, per-chunk, above), never a
    // crash or a validation error from handing it a null handle.
    std::vector<VkCommandBuffer> validChunkBuffers;
    validChunkBuffers.reserve(chunkBuffers.size());
    for (VkCommandBuffer buf : chunkBuffers) {
        if (buf != VK_NULL_HANDLE) {
            validChunkBuffers.push_back(buf);
        }
    }

    if (isGraphicsPass) {
        renderingInfo.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
        vkCmdBeginRendering(cmd, &renderingInfo);
    }
    if (!validChunkBuffers.empty()) {
        vkCmdExecuteCommands(cmd, static_cast<uint32_t>(validChunkBuffers.size()), validChunkBuffers.data());
    }
    if (isGraphicsPass) {
        vkCmdEndRendering(cmd);
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

// Phase 4 Task 7: backs PassContext::chunkCommandBuffer() -- see that
// method's own doc comment (executor.h) and tChunkCommandBuffer/
// ChunkCommandBufferScope's own comments above for the thread_local
// mechanism this reads.
VkCommandBuffer Executor::resolveChunkCommandBuffer() const {
    if (tChunkCommandBuffer == VK_NULL_HANDLE) {
        throw std::out_of_range(
            "rx_graph: PassContext::chunkCommandBuffer(): not currently inside a Pass::setExecuteChunked() "
            "callback (called from a whole-pass setExecute() callback, or from outside any pass callback at all)");
    }
    return tChunkCommandBuffer;
}

VkImageView PassContext::imageView(std::string_view name) const { return executor_->resolveImageView(name); }
VkImage PassContext::image(std::string_view name) const { return executor_->resolveImage(name); }
VkBuffer PassContext::buffer(std::string_view name) const { return executor_->resolveBuffer(name); }
VkFormat PassContext::imageFormat(std::string_view name) const { return executor_->resolveImageFormat(name); }
bool PassContext::historyValid(std::string_view name) const { return executor_->resolveHistoryValid(name); }
VkCommandBuffer PassContext::chunkCommandBuffer() const { return executor_->resolveChunkCommandBuffer(); }

namespace detail {

// Phase 4 Task 7: see executor.h's own extensive comment on this function
// for the full derivation rationale -- kept here as a one-line formula
// specifically so it stays trivially re-derivable by inspection, matching
// rx::task::Scheduler::autoGrainSize()'s own "pure, static, directly
// unit-testable" posture.
uint32_t chunkCountForWorkerCount(uint32_t workerCount) {
    return std::max<uint32_t>(1, std::min(workerCount, kMaxChunksPerPass));
}

ExecutorChunkDebugStats debugChunkStats(const Executor& executor) {
    const Executor::Impl& impl = *executor.impl_;
    ExecutorChunkDebugStats stats;
    stats.lastChunkCount = impl.lastChunkCount;
    stats.totalPoolAllocations = impl.totalChunkPoolAllocations.load(std::memory_order_relaxed);
    return stats;
}

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
