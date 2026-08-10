#include <rx_graph/executor.h>

#include "transient_pool.h"

#include <rx_core/log.h>
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

    // Index into Impl::pool's images_/buffers_ (per isBuffer) -- meaningless
    // (left at UINT32_MAX) for the backbuffer, which this Executor never
    // pools [Task 3 ambiguity resolution #4].
    uint32_t poolIndex = UINT32_MAX;

    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
};

struct Executor::Impl {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    rx::rhi::Allocator allocator;
    detail::TransientPool pool;
    rx::rhi::DeletionQueue deletionQueue;
    bool debugUtilsAvailable = false;

    // Executor's own monotonic tick, one increment per execute() call --
    // the clock TransientPool's staleness sweep and this Impl's own
    // deletion-queue pacing (see Executor::execute()) both run against.
    uint64_t frameCounter = 0;

    // Rebuilt by every realize() call; indexed identically to whatever
    // CompiledGraph::resources() looked like as of that call.
    std::vector<ResolvedResource> resources;
    std::unordered_map<std::string, uint32_t> nameToIndex;

    Impl(VkDevice deviceIn, VkPhysicalDevice physicalDeviceIn, rx::rhi::Allocator allocatorIn)
        : device(deviceIn),
          physicalDevice(physicalDeviceIn),
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

        VkPipelineStageFlags2 srcStage = b.srcStage;
        if (!resolved.isBackbuffer && !firstBarrierSeen[b.physicalIndex]) {
            srcStage = impl.pool.image(resolved.poolIndex).lastFrameFinalStages;
        }
        firstBarrierSeen[b.physicalIndex] = true;

        VkImageMemoryBarrier2 vkBarrier{};
        vkBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        vkBarrier.srcStageMask = srcStage;
        vkBarrier.srcAccessMask = b.srcAccess;
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
        if (!resolved.isBackbuffer && !firstBarrierSeen[b.physicalIndex]) {
            srcStage = impl.pool.buffer(resolved.poolIndex).lastFrameFinalStages;
        }
        firstBarrierSeen[b.physicalIndex] = true;

        VkBufferMemoryBarrier2 vkBarrier{};
        vkBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        vkBarrier.srcStageMask = srcStage;
        vkBarrier.srcAccessMask = b.srcAccess;
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
        vkBarrier.srcAccessMask = VK_ACCESS_2_NONE;
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

}  // namespace

std::unique_ptr<Executor> Executor::create(rx::rhi::Device& device) {
    auto allocator = rx::rhi::Allocator::createRaw(device.physicalDevice(), device.device(), device.instance());
    if (!allocator.has_value()) {
        RX_LOG_ERROR("rx_graph: Executor::create: rx::rhi::Allocator::createRaw failed");
        return nullptr;
    }

    auto impl = std::make_unique<Impl>(device.device(), device.physicalDevice(), std::move(*allocator));

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

        if (physical.isBackbuffer) {
            // Never pooled [Task 3 ambiguity resolution #4] -- image/view/
            // extent are filled in fresh by every execute() call instead;
            // format is already known now (compile() resolves it from
            // CompileInfo::swapchainFormat regardless of what its writer
            // pass declared -- see render_graph.cpp).
            entry.format = physical.attachment.format;
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

    const std::span<const uint32_t> order = compiled.executionOrder();
    const std::span<const PassBarriers> allBarriers = compiled.passBarriers();

    for (size_t pos = 0; pos < order.size(); ++pos) {
        const uint32_t rawIndex = order[pos];
        const Pass& pass = graph.passAt(rawIndex);
        const std::span<const ResourceAccess> accesses = compiled.passAccesses(rawIndex);

        beginDebugLabel(impl, cmd, pass.name());

        applyBarriers(impl, compiled, cmd, allBarriers[pos], firstBarrierSeen);
        synthesizeFirstUseBufferBarrierIfNeeded(impl, compiled, cmd, accesses, pos, firstBarrierSeen);

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
                info.imageView = resolvedRes.view;
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
                depthAttachment.imageView = resolvedRes.view;
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
        for (const auto& [physIdx, combined] : combineAccessesByResource(accesses)) {
            if (!impl.resources.at(physIdx).isBackbuffer) {
                finalStageThisExecute[physIdx] |= combined.stages;
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
        if (resources[physIdx].isBuffer) {
            impl.pool.buffer(resolvedRes.poolIndex).lastFrameFinalStages = stage;
            impl.pool.touchBuffer(resolvedRes.poolIndex, impl.frameCounter);
        } else {
            impl.pool.image(resolvedRes.poolIndex).lastFrameFinalStages = stage;
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
    return resolved.view;
}

VkImage Executor::resolveImage(std::string_view name) const {
    const uint32_t idx = lookupResolvedIndex(*impl_, name);
    const ResolvedResource& resolved = impl_->resources.at(idx);
    requireKind(name, resolved, /*expectBuffer=*/false, "image");
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
    return resolved.format;
}

VkImageView PassContext::imageView(std::string_view name) const { return executor_->resolveImageView(name); }
VkImage PassContext::image(std::string_view name) const { return executor_->resolveImage(name); }
VkBuffer PassContext::buffer(std::string_view name) const { return executor_->resolveBuffer(name); }
VkFormat PassContext::imageFormat(std::string_view name) const { return executor_->resolveImageFormat(name); }

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
