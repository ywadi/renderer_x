// Sample 01: triangle.
//
// Two modes, one pipeline, one draw:
//
//   (default / no args) Headless correctness gate. Builds the FULL
//   rx_rhi_vk stack -- window, validated instance, surface, logical device +
//   real swapchain (created and queried, never written to) -- then renders a
//   single hardcoded triangle into a DEDICATED OFFSCREEN VkImage and reads
//   the result back to the host for pixel assertions. Why offscreen and not
//   swapchainImages()[0]: the application does not own a presentable image
//   until vkAcquireNextImageKHR returns it -- rendering into an un-acquired
//   swapchain image is a Vulkan spec violation regardless of whether a given
//   driver tolerates it. Registered as ctest sample_01_triangle_headless;
//   exits 0/1.
//
//   --present. Opens a real, visible window and runs the canonical
//   frames-in-flight present loop (rx::rhi::FrameSync) against the actual
//   swapchain images -- the first and only place in this codebase that
//   legally writes to a swapchain image, and only ever the acquired index.
//   Survives window resizes via Device's NeedsRecreate/recreateSwapchain
//   path; closes cleanly on window-close (SDL_EVENT_QUIT). Also accepts
//   --vsync on|off (default on) [Phase 4 Task 6]: forwarded into
//   Device::setPresentMode() + recreateSwapchain() right after
//   Device::create(), before any per-swapchain-image resource is built --
//   see runPresent()'s own comment. Has no effect in headless mode, whose
//   swapchain is built once and never presented (the flag still parses
//   there, it just has nothing to apply to). Not part of
//   ctest (it's an interactive/manual-verification path -- see
//   MANUAL_VERIFICATION.md at the repo root); exits 0 on a clean window
//   close, 1 on any Vulkan failure or device loss.
//
//   --log-callback (either mode, on top). [spec Phase 4 design D23, seed
//   13] Installs a tiny adapter (sampleLogCallback() below) proving the
//   public log sink a consuming engine would use to route renderer
//   diagnostics into its own logging system. This sample deliberately
//   calls rx::core::log::forwardSink() directly rather than linking
//   rx_material and going through its rxSetLogCallback() ABI wrapper:
//   rx_material transitively links slang::slang and needs its runtime
//   libraries deployed next to the binary (see rx_material/CMakeLists.txt),
//   a disproportionate dependency to pull into the smallest, lightest
//   sample purely to demonstrate log forwarding -- sample 06_materials
//   already links rx_material for unrelated reasons and is out of scope
//   here (sibling task touches it). rx_api.h's rxSetLogCallback() is a
//   direct, uncasted pass-through to this exact same rx_core mechanism
//   (see api_impl.cpp's own comment on why no translation is needed), so
//   this demo exercises the identical delivery path a real ABI consumer
//   gets -- just reached one layer lower, without the extra link weight.
//   Gate-unaffected: sample_01_triangle_headless (ctest) never passes this
//   flag.
//
// Both modes build and draw through the exact same VkPipeline-construction
// code (createTrianglePipeline() below): dynamic rendering, dynamic
// viewport/scissor, no vertex input (the vertex shader generates positions
// from SV_VertexID), no cull, no blend, 1 sample, targeting
// device->swapchainFormat() either way (headless mode's offscreen image is
// deliberately created in that same format for exactly this reason).
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <rx_core/profile.h>
#include <rx_frame_loop/present_loop.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>

#include <SDL3/SDL.h>
#include <volk.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;
constexpr VkDeviceSize kPixelBytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;

// --present mode's window size. Arbitrary (any size works -- viewport/
// scissor are dynamic state, and the window is resizable at runtime), just
// big enough on screen to make the triangle easy to see.
constexpr uint32_t kPresentWidth = 800;
constexpr uint32_t kPresentHeight = 600;

// Center of the triangle's body (NDC apex (0,-0.5), base corners
// (+-0.5, 0.5) -- with a 256x256 viewport and no Y-flip, that maps to a
// triangle spanning roughly screen Y in [64,192], widest at the base) --
// must sample white. (10,10) is well outside the triangle on every side --
// must sample the black clear color.
constexpr uint32_t kCenterX = 128;
constexpr uint32_t kCenterY = 150;
constexpr uint32_t kCornerX = 10;
constexpr uint32_t kCornerY = 10;

constexpr const char* kVertFilename = "triangle.vert.spv";
constexpr const char* kFragFilename = "triangle.frag.spv";

// Resolves a precompiled SPIR-V file's path next to this executable at
// runtime (same SDL_GetBasePath() mechanism 02_hotreload's
// resolveShaderPath()/03_bindless_mesh's resolveTexturePath() use), so a
// redistributed copy of this sample's build-output directory (binary + the
// two .spv files deployed next to it by CMakeLists.txt's POST_BUILD copy
// step) works identically outside the build tree -- this sample ships no
// Slang runtime libraries (its shaders are precompiled offline by slangc at
// build time, never compiled in-process [R:D2]), but it still needs these
// two files on disk somewhere findable at run time, and a hardcoded
// absolute build-tree path (the old behavior, still available below as a
// fallback) is not that.
//
// Falls back to `buildTreeFallback` -- the compile-time absolute path
// (RX_TRIANGLE_VERT_SPV/RX_TRIANGLE_FRAG_SPV, still valid when running
// in-place from the build tree, e.g. under a debugger with a stale deploy
// step) -- only if the exe-relative copy can't be found; the redistributed
// zip's only copy is the exe-relative one, and this is the path that
// actually gets exercised there.
std::string resolveSpvPath(const char* filename, const char* buildTreeFallback) {
    const char* basePath = SDL_GetBasePath();
    if (basePath != nullptr) {
        std::string candidate = std::string(basePath) + filename;
        std::ifstream probe(candidate, std::ios::binary);
        if (probe.good()) {
            return candidate;
        }
    }
    return buildTreeFallback;
}

std::vector<char> readFile(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }
    std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0);
    std::vector<char> buffer(static_cast<size_t>(size));
    file.read(buffer.data(), size);
    if (!file) {
        return {};
    }
    return buffer;
}

VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code) {
    // SPIR-V is a stream of uint32_t words; codeSize/pCode below require the
    // byte buffer to be 4-byte-aligned in length.
    if (code.empty() || code.size() % 4 != 0) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

std::optional<uint32_t> findMemoryTypeIndex(const VkPhysicalDeviceMemoryProperties& memProps, uint32_t typeBits,
                                             VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1U << i)) != 0U && (memProps.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    return std::nullopt;
}

// The one pipeline (plus the shader modules and layout it depends on) both
// modes render with. Raw Vulkan objects, not RAII -- destroyed explicitly
// via destroyTrianglePipeline() before the owning VkDevice goes away, same
// discipline as every other raw handle in this file.
struct TrianglePipeline {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

void destroyTrianglePipeline(VkDevice device, TrianglePipeline& p) {
    if (p.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, p.pipeline, nullptr);
    }
    if (p.layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, p.layout, nullptr);
    }
    if (p.fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, p.fragModule, nullptr);
    }
    if (p.vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, p.vertModule, nullptr);
    }
    p = TrianglePipeline{};
}

// Builds the triangle demo's shader modules, empty pipeline layout, and
// dynamic-rendering VkPipeline targeting `colorFormat` -- shared verbatim
// between headless mode's offscreen render target and --present mode's real
// swapchain images (see the file-level comment for why one pipeline is
// valid for both). Dynamic viewport/scissor means this same VkPipeline
// stays valid across a --present window resize with no recreation needed.
// Returns nullopt (logged) on any failure, having already destroyed
// whatever partial state it created -- callers never need to call
// destroyTrianglePipeline() themselves on a nullopt return.
std::optional<TrianglePipeline> createTrianglePipeline(VkDevice device, VkFormat colorFormat) {
    TrianglePipeline result;

    const std::string vertPath = resolveSpvPath(kVertFilename, RX_TRIANGLE_VERT_SPV);
    const std::string fragPath = resolveSpvPath(kFragFilename, RX_TRIANGLE_FRAG_SPV);
    auto vertCode = readFile(vertPath.c_str());
    auto fragCode = readFile(fragPath.c_str());
    if (vertCode.empty() || fragCode.empty()) {
        RX_LOG_ERROR("failed to read compiled triangle shader SPIR-V from {} / {}", vertPath, fragPath);
        return std::nullopt;
    }

    result.vertModule = createShaderModule(device, vertCode);
    result.fragModule = createShaderModule(device, fragCode);
    if (result.vertModule == VK_NULL_HANDLE || result.fragModule == VK_NULL_HANDLE) {
        RX_LOG_ERROR("vkCreateShaderModule failed for the triangle demo shaders");
        destroyTrianglePipeline(device, result);
        return std::nullopt;
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &result.layout) != VK_SUCCESS) {
        RX_LOG_ERROR("vkCreatePipelineLayout failed");
        destroyTrianglePipeline(device, result);
        return std::nullopt;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = result.vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = result.fragModule;
    stages[1].pName = "main";

    // No vertex input: the shader generates positions from SV_VertexID.
    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
    inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport/scissor are dynamic state, set at record time; only their
    // counts matter here.
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = VK_CULL_MODE_NONE;
    rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationState.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                      VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlendState{};
    colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = &blendAttachment;

    std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineRenderingCreateInfo renderingCreateInfo{};
    renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCreateInfo.colorAttachmentCount = 1;
    renderingCreateInfo.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingCreateInfo;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInputState;
    pipelineInfo.pInputAssemblyState = &inputAssemblyState;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizationState;
    pipelineInfo.pMultisampleState = &multisampleState;
    pipelineInfo.pColorBlendState = &colorBlendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = result.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;  // dynamic rendering
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &result.pipeline) !=
        VK_SUCCESS) {
        RX_LOG_ERROR("vkCreateGraphicsPipelines failed");
        destroyTrianglePipeline(device, result);
        return std::nullopt;
    }

    return result;
}

// --- Headless mode: offscreen render + pixel readback ----------------------
int runHeadless(bool enableValidation) {
    auto window = rx::platform::Window::create("rx_triangle_sample", static_cast<int>(kWidth),
                                                 static_cast<int>(kHeight), /*visible=*/false);
    if (!window.has_value()) {
        RX_LOG_ERROR("Window::create failed: no display backend available");
        return 1;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        RX_LOG_ERROR("video driver reports no Vulkan surface extensions (e.g. dummy driver)");
        return 1;
    }

    // Exactly one Context is built in this process -- see the WARNING on
    // Context::create() in context.h about vk-bootstrap's process-wide
    // cached instance function pointers; a single windowed, validated
    // Context sidesteps that hazard entirely.
    auto context = rx::rhi::Context::create(extensions, enableValidation);
    if (!context.has_value()) {
        RX_LOG_ERROR("Context::create failed");
        return 1;
    }

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    if (surface == VK_NULL_HANDLE) {
        RX_LOG_ERROR("createVulkanSurface failed");
        return 1;
    }

    // Device::create takes ownership of `surface` unconditionally, on both
    // success and failure -- see rx_rhi_vk/device.h. Builds and queries a
    // real swapchain against the window's surface; this sample never writes
    // to any of swapchainImages(), only reads swapchainFormat() below.
    auto device = rx::rhi::Device::create(*context, surface);
    if (!device.has_value()) {
        RX_LOG_ERROR("Device::create failed");
        return 1;
    }

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    if (!allocator.has_value()) {
        RX_LOG_ERROR("Allocator::create failed");
        return 1;
    }

    auto cmdCtx =
        rx::rhi::CommandContext::create(device->device(), device->graphicsQueue(), device->graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        RX_LOG_ERROR("CommandContext::create failed");
        return 1;
    }

    const VkDevice vkDevice = device->device();
    // The offscreen target uses the swapchain's own format so this exact
    // pipeline stays valid for --present mode's real swapchain rendering.
    // The only colors this sample ever asserts on are pure white
    // ((255,255,255) triangle fill) and pure black ((0,0,0) clear): both
    // are fixed points of the sRGB transfer function and byte-identical
    // regardless of channel order, so neither an RGBA/BGRA swap nor a
    // UNORM/SRGB choice in whatever format the surface actually offers can
    // break the readback assertions below.
    const VkFormat targetFormat = device->swapchainFormat();

    // ---------------------------------------------------------------------
    // Raw (non-RAII) Vulkan objects. destroyRawResources() below tears all
    // of these down explicitly before `device` (and, beneath it, the
    // logical VkDevice) goes out of scope -- see the RAII ordering
    // discipline established in clear_color_test.cpp (task-3-brief.md).
    // ---------------------------------------------------------------------
    VkImage offscreenImage = VK_NULL_HANDLE;
    VkDeviceMemory offscreenMemory = VK_NULL_HANDLE;
    VkImageView offscreenView = VK_NULL_HANDLE;
    TrianglePipeline trianglePipeline;

    auto destroyRawResources = [&]() {
        destroyTrianglePipeline(vkDevice, trianglePipeline);
        if (offscreenView != VK_NULL_HANDLE) {
            vkDestroyImageView(vkDevice, offscreenView, nullptr);
        }
        if (offscreenImage != VK_NULL_HANDLE) {
            vkDestroyImage(vkDevice, offscreenImage, nullptr);
        }
        if (offscreenMemory != VK_NULL_HANDLE) {
            vkFreeMemory(vkDevice, offscreenMemory, nullptr);
        }
    };

    // --- Offscreen render target: 256x256, device-local -----------------
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = targetFormat;
    imageInfo.extent = {kWidth, kHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(vkDevice, &imageInfo, nullptr, &offscreenImage) != VK_SUCCESS) {
        RX_LOG_ERROR("vkCreateImage(offscreen target) failed");
        destroyRawResources();
        return 1;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(vkDevice, offscreenImage, &memReq);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(device->physicalDevice(), &memProps);

    auto memoryTypeIndex = findMemoryTypeIndex(memProps, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memoryTypeIndex.has_value()) {
        RX_LOG_ERROR("no device-local memory type found for the offscreen target");
        destroyRawResources();
        return 1;
    }

    VkMemoryAllocateInfo memAllocInfo{};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReq.size;
    memAllocInfo.memoryTypeIndex = *memoryTypeIndex;

    if (vkAllocateMemory(vkDevice, &memAllocInfo, nullptr, &offscreenMemory) != VK_SUCCESS) {
        RX_LOG_ERROR("vkAllocateMemory(offscreen target) failed");
        destroyRawResources();
        return 1;
    }
    if (vkBindImageMemory(vkDevice, offscreenImage, offscreenMemory, 0) != VK_SUCCESS) {
        RX_LOG_ERROR("vkBindImageMemory(offscreen target) failed");
        destroyRawResources();
        return 1;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = offscreenImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = targetFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &offscreenView) != VK_SUCCESS) {
        RX_LOG_ERROR("vkCreateImageView(offscreen target) failed");
        destroyRawResources();
        return 1;
    }

    // --- Pipeline: shared with --present mode (see createTrianglePipeline) --
    auto pipelineResult = createTrianglePipeline(vkDevice, targetFormat);
    if (!pipelineResult.has_value()) {
        destroyRawResources();
        return 1;
    }
    trianglePipeline = std::move(*pipelineResult);

    // --- Draw: transition, clear black, draw 3 verts, transition --------
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        rx::rhi::transitionImage(cmd, offscreenImage, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = offscreenView;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = VkRect2D{{0, 0}, {kWidth, kHeight}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport viewport{0.0F, 0.0F, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0F, 1.0F};
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, trianglePipeline.pipeline);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRendering(cmd);

        rx::rhi::transitionImage(cmd, offscreenImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    });
    // RX_FRAME_MARK once per rendered frame [Phase 4 Stage 0 Task 3, spec
    // D3] -- headless-mode path: this sample's headless mode renders
    // exactly one frame (the runOnce() call just above), so this is the
    // single frame boundary to mark.
    RX_FRAME_MARK;

    // --- Readback ---------------------------------------------------------
    auto readback = allocator->createHostVisibleBuffer(kPixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!readback.has_value()) {
        RX_LOG_ERROR("createHostVisibleBuffer(readback) failed");
        destroyRawResources();
        return 1;
    }

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {kWidth, kHeight, 1};

        vkCmdCopyImageToBuffer(cmd, offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                                &region);
    });

    // runOnce() already vkQueueWaitIdle()'d before returning. Buffer/
    // Allocator (Task 2) expose no vmaFlush/InvalidateAllocation surface, so
    // this read relies on the same precondition clear_color_test.cpp
    // verifies explicitly: every HOST_VISIBLE memory type this physical
    // device exposes is also HOST_COHERENT (true for every desktop Vulkan
    // driver in practice) -- check it here too rather than assume it
    // silently, since a violation would mean reading stale data below.
    VkPhysicalDeviceMemoryProperties hostMemProps{};
    vkGetPhysicalDeviceMemoryProperties(device->physicalDevice(), &hostMemProps);
    for (uint32_t i = 0; i < hostMemProps.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = hostMemProps.memoryTypes[i].propertyFlags;
        const bool hostVisible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U;
        const bool hostCoherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
        if (hostVisible && !hostCoherent) {
            RX_LOG_ERROR("host-visible memory type {} is not host-coherent; readback would need an explicit invalidate",
                         i);
            destroyRawResources();
            return 1;
        }
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(kPixelBytes));
    std::memcpy(pixels.data(), readback->mappedData(), pixels.size());

    // Raw Vulkan objects are done being used now -- tear them down while
    // `device` (and its VkDevice) is still alive, before any of the RAII
    // objects (readback buffer, cmdCtx, allocator, device, context, window)
    // unwind at the end of this function.
    destroyRawResources();

    auto pixelAt = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return pixels.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
    };

    const uint8_t* center = pixelAt(kCenterX, kCenterY);
    const uint8_t* corner = pixelAt(kCornerX, kCornerY);

    bool pass = true;
    for (int c = 0; c < 3; ++c) {
        if (center[c] <= 200) {
            RX_LOG_ERROR("center pixel ({},{}) channel {} = {} (expected > 200)", kCenterX, kCenterY, c, center[c]);
            pass = false;
        }
    }
    for (int c = 0; c < 3; ++c) {
        if (corner[c] >= 20) {
            RX_LOG_ERROR("corner pixel ({},{}) channel {} = {} (expected < 20)", kCornerX, kCornerY, c, corner[c]);
            pass = false;
        }
    }
    if (enableValidation && context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during this run");
        pass = false;
    }

    if (pass) {
        RX_LOG_INFO("triangle readback PASSED");
        return 0;
    }
    RX_LOG_ERROR("triangle readback FAILED");
    return 1;
}

// --- --present mode: real window, real swapchain, shared present loop -----
//
// [Phase 5 Task 5, ticket #41] This is the first and only place in this
// codebase that legally writes to a swapchain image -- and only ever the
// acquired index. The acquire/status-handle/recreate/submit/present/advance
// MECHANICS this comment used to document in full here now live ONCE,
// engine-side, in rx::frame_loop::PresentLoop (src/rx_frame_loop/include/
// rx_frame_loop/present_loop.h) -- every later sample's own "see
// samples/01_triangle/main.cpp's runPresent() for the full... rationale"
// cross-reference now points there instead. What remains sample-local here
// is exactly this sample's own identity: window/device/pipeline setup, the
// SDL event pump, and the frame-body callback that records this sample's
// one triangle draw.
struct Args {
    bool validate = false;
    rx::rhi::PresentMode vsyncMode = rx::rhi::PresentMode::VsyncOn;
    bool fullscreen = false;
};

int runPresent(const Args& args) {
    auto window = rx::platform::Window::create("rx_triangle_sample (--present)", static_cast<int>(kPresentWidth),
                                                 static_cast<int>(kPresentHeight), /*visible=*/true);
    if (!window.has_value()) {
        RX_LOG_ERROR("Window::create failed: no display backend available");
        return 1;
    }

    // --fullscreen [Phase 4 Task 17, FG7]: applied immediately after window
    // creation, before Device::create() below builds the initial swapchain
    // -- Device::create() always sizes that first swapchain off whatever
    // the window's CURRENT extent already is, so entering fullscreen here
    // (rather than forcing a second recreateSwapchain() call the way
    // --vsync below has to) reuses that existing "build against the live
    // window size" behavior instead of adding a second recreation path
    // [gate ruling #25 row 5: exactly one recreation call site].
    if (args.fullscreen) {
        if (!window->setFullscreen(true)) {
            RX_LOG_ERROR("Window::setFullscreen(true) failed while applying --fullscreen");
            return 1;
        }
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        RX_LOG_ERROR("video driver reports no Vulkan surface extensions (e.g. dummy driver)");
        return 1;
    }

    auto context = rx::rhi::Context::create(extensions, args.validate);
    if (!context.has_value()) {
        RX_LOG_ERROR("Context::create failed");
        return 1;
    }

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    if (surface == VK_NULL_HANDLE) {
        RX_LOG_ERROR("createVulkanSurface failed");
        return 1;
    }

    // Device::create takes ownership of `surface` unconditionally -- see
    // rx_rhi_vk/device.h. `surface` remains valid and usable (just not
    // ours to destroy) for the recreateSwapchain() calls below.
    auto device = rx::rhi::Device::create(*context, surface);
    if (!device.has_value()) {
        RX_LOG_ERROR("Device::create failed");
        return 1;
    }
    const VkDevice vkDevice = device->device();

    // --vsync [Phase 4 Task 6]: Device::create() always builds its
    // swapchain with an explicit FIFO default (PresentMode::VsyncOn) --
    // see device.cpp's own comment at the creation site. setPresentMode()
    // only records what the caller wants; recreateSwapchain() is what
    // actually applies it -- reused here, once, before PresentLoop::create()
    // builds any per-swapchain-image resource, so nothing downstream is
    // built against a swapchain generation that is about to be replaced.
    if (args.vsyncMode == rx::rhi::PresentMode::VsyncOff) {
        device->setPresentMode(args.vsyncMode);
        if (!device->recreateSwapchain(surface)) {
            RX_LOG_ERROR("Device::recreateSwapchain failed while applying --vsync off");
            return 1;
        }
    }
    RX_LOG_INFO("--present: present mode in use: {}", rx::rhi::presentModeName(device->presentMode()));

    // Pipeline: shared with headless mode (see createTrianglePipeline).
    auto pipelineResult = createTrianglePipeline(vkDevice, device->swapchainFormat());
    if (!pipelineResult.has_value()) {
        return 1;
    }
    TrianglePipeline trianglePipeline = std::move(*pipelineResult);

    // [Phase 5 Task 5] PresentLoop owns FrameSync + the per-swapchain-image
    // VkImageViews internally -- no RenderGraph for this sample (raw
    // dynamic rendering, exactly as before), so CreateInfo::graph/executor
    // stay null.
    auto loop = rx::frame_loop::PresentLoop::create(
        rx::frame_loop::PresentLoop::CreateInfo{&*device, surface, &*window});
    if (!loop.has_value()) {
        RX_LOG_ERROR("PresentLoop::create failed");
        destroyTrianglePipeline(vkDevice, trianglePipeline);
        return 1;
    }

    RX_LOG_INFO("--present: window open ({}x{}); close the window to exit", kPresentWidth, kPresentHeight);

    bool ok = true;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        const auto result = loop->runFrame([&](const rx::frame_loop::FrameContext& ctx) {
            rx::rhi::transitionImage(ctx.cmd, ctx.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = ctx.view;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}};

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = VkRect2D{{0, 0}, ctx.extent};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;

            vkCmdBeginRendering(ctx.cmd, &renderingInfo);

            VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.extent.width),
                                 static_cast<float>(ctx.extent.height), 0.0F, 1.0F};
            vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);

            VkRect2D scissor{{0, 0}, ctx.extent};
            vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, trianglePipeline.pipeline);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);

            vkCmdEndRendering(ctx.cmd);

            rx::rhi::transitionImage(ctx.cmd, ctx.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        });
        if (result == rx::frame_loop::Result::Failed) {
            ok = false;
            break;
        }
        if (result == rx::frame_loop::Result::SurfaceLost) {
            running = false;
        }
        // Ok/Skipped: keep looping.
    }

    // [Issue #74, promoted] Shutdown: vkDeviceWaitIdle BEFORE destroying
    // PresentLoop (via its destructor below, which destroys FrameSync/the
    // per-image views) and the pipeline -- the only point any of these may
    // legally die. See frame_sync.h's destructor contract. A device lost
    // in the exact compound state shouldSkipTeardownAfterDeviceLoss()
    // documents skips the fine-grained teardown below entirely (it would
    // otherwise produce real, unfiltered "still in use" validation errors,
    // since nothing was actually drained).
    const VkResult waitIdleResult = vkDeviceWaitIdle(vkDevice);
    if (rx::frame_loop::shouldSkipTeardownAfterDeviceLoss(waitIdleResult, loop->isSurfaceLost())) {
        const bool hadValidationErrors = args.validate && context->hasValidationErrors();
        if (hadValidationErrors) {
            RX_LOG_ERROR("Vulkan validation layer reported errors during the present loop");
        }
        RX_LOG_INFO("VkDevice reports lost immediately after the present window's native handle was already "
                     "known gone -- skipping further Vulkan teardown and letting process exit reclaim GPU "
                     "resources directly [Issue #74]");
        ::spdlog::default_logger()->flush();
        std::_Exit((hadValidationErrors || !ok) ? 1 : 0);
    }

    loop.reset();
    destroyTrianglePipeline(vkDevice, trianglePipeline);

    if (args.validate && context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during the present loop");
        return 1;
    }
    if (!ok) {
        return 1;
    }
    RX_LOG_INFO("--present: window closed cleanly");
    return 0;
}

// --log-callback adapter [spec Phase 4 design D23, seed 13] -- stands in
// for a consuming engine's own logging system: real integration code
// would route these three fields into that engine's own log sink instead
// of stdout. `userData` is unused here (nullptr installed below) since
// this demo has no per-installation state to carry.
void sampleLogCallback(int32_t severity, const char* category, const char* message, void* /*userData*/) {
    static constexpr const char* kSeverityNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    const char* severityName =
        (severity >= 0 && severity < static_cast<int32_t>(std::size(kSeverityNames))) ? kSeverityNames[severity]
                                                                                        : "UNKNOWN";
    bool hasCategory = category != nullptr && category[0] != '\0';
    std::fprintf(stdout, "[log-callback] [%s]%s%s %s\n", severityName, hasCategory ? " " : "",
                 hasCategory ? category : "", message != nullptr ? message : "");
}

}  // namespace

int main(int argc, char** argv) {
    rx::core::log::init();

    bool presentMode = false;
    bool enableValidation = false;
    // --vsync on|off, default on [Phase 4 Task 6] -- forwarded to
    // runPresent() only. Headless mode's swapchain is built once via
    // Device::create() and never presented (see this file's header
    // comment), so there is nothing for a present-mode choice to affect
    // there; the flag is still parsed like any other (no error on it) but
    // simply has no effect in that path.
    rx::rhi::PresentMode vsyncMode = rx::rhi::PresentMode::VsyncOn;
    bool logCallback = false;
    // --fullscreen [Phase 4 Task 17, FG7] -- forwarded to runPresent() only,
    // same rationale as --vsync above (headless mode never shows a window).
    bool fullscreen = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--present") {
            presentMode = true;
        } else if (std::string_view(argv[i]) == "--validate") {
            enableValidation = true;
        } else if (std::string_view(argv[i]) == "--vsync" && i + 1 < argc) {
            std::string_view value = argv[++i];
            if (value == "off") {
                vsyncMode = rx::rhi::PresentMode::VsyncOff;
            } else if (value == "on") {
                vsyncMode = rx::rhi::PresentMode::VsyncOn;
            } else {
                RX_LOG_ERROR("--vsync expects 'on' or 'off', got '{}' -- defaulting to on", value);
            }
        } else if (std::string_view(argv[i]) == "--fullscreen") {
            fullscreen = true;
        } else if (std::string_view(argv[i]) == "--log-callback") {
            logCallback = true;
        }
    }

    if (logCallback) {
        // Called from main(), never from inside sampleLogCallback() itself,
        // so this can never hit the one documented rejection case (calling
        // set()/rxSetLogCallback() from inside the installed callback's own
        // invocation -- rx_core/log_forward_sink.h's own comment on set())
        // -- always succeeds here.
        bool installed = rx::core::log::forwardSink()->set(&sampleLogCallback, nullptr);
        (void)installed;
    }

    if (presentMode) {
        Args args;
        args.validate = enableValidation;
        args.vsyncMode = vsyncMode;
        args.fullscreen = fullscreen;
        return runPresent(args);
    }
    return runHeadless(enableValidation);
}
