// Sample 01: triangle correctness gate.
//
// Builds the FULL rx_rhi_vk stack -- window, validated instance, surface,
// logical device + real swapchain (created and queried, never written to)
// -- then renders a single hardcoded triangle into a DEDICATED OFFSCREEN
// VkImage and reads the result back to the host for pixel assertions.
//
// Why offscreen and not swapchainImages()[0]: the application does not own
// a presentable image until vkAcquireNextImageKHR returns it -- rendering
// into an un-acquired swapchain image is a Vulkan spec violation regardless
// of whether a given driver tolerates it. This sample still exercises
// Device::create's full swapchain path (so the next task's --present mode
// reuses the exact same pipeline/draw code against a real acquired image);
// it just never touches swapchainImages() itself. See task-5-brief.md.
//
// Headless mode (argv ignored today) is the only mode this task builds:
// create everything, draw once, read back two pixels, assert, exit 0/1.
// A later task extends this same main() with a --present code path that
// opens a window and runs a real present loop; the argc/argv parameters
// are already accepted (and currently unused) so that extension does not
// need to change this function's signature.
#include <rx_core/log.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>

#include <volk.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <vector>

namespace {

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;
constexpr VkDeviceSize kPixelBytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;

// Center of the triangle's body (NDC apex (0,-0.5), base corners
// (+-0.5, 0.5) -- with a 256x256 viewport and no Y-flip, that maps to a
// triangle spanning roughly screen Y in [64,192], widest at the base) --
// must sample white. (10,10) is well outside the triangle on every side --
// must sample the black clear color.
constexpr uint32_t kCenterX = 128;
constexpr uint32_t kCenterY = 150;
constexpr uint32_t kCornerX = 10;
constexpr uint32_t kCornerY = 10;

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

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    rx::core::log::init();

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
    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
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
    // pipeline stays valid for the next task's real swapchain rendering.
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
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    auto destroyRawResources = [&]() {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(vkDevice, pipeline, nullptr);
        }
        if (pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vkDevice, pipelineLayout, nullptr);
        }
        if (fragModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(vkDevice, fragModule, nullptr);
        }
        if (vertModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(vkDevice, vertModule, nullptr);
        }
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

    // --- Pipeline: shader modules from the compiled triangle demo -------
    auto vertCode = readFile(RX_TRIANGLE_VERT_SPV);
    auto fragCode = readFile(RX_TRIANGLE_FRAG_SPV);
    if (vertCode.empty() || fragCode.empty()) {
        RX_LOG_ERROR("failed to read compiled triangle shader SPIR-V from {} / {}", RX_TRIANGLE_VERT_SPV,
                      RX_TRIANGLE_FRAG_SPV);
        destroyRawResources();
        return 1;
    }

    vertModule = createShaderModule(vkDevice, vertCode);
    fragModule = createShaderModule(vkDevice, fragCode);
    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        RX_LOG_ERROR("vkCreateShaderModule failed for the triangle demo shaders");
        destroyRawResources();
        return 1;
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(vkDevice, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        RX_LOG_ERROR("vkCreatePipelineLayout failed");
        destroyRawResources();
        return 1;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // No vertex input: the shader generates positions from SV_VertexID.
    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
    inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport/scissor are dynamic state, set at record time (see below);
    // only their counts matter here.
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
    renderingCreateInfo.pColorAttachmentFormats = &targetFormat;

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
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;  // dynamic rendering
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(vkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        RX_LOG_ERROR("vkCreateGraphicsPipelines failed");
        destroyRawResources();
        return 1;
    }

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

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRendering(cmd);

        rx::rhi::transitionImage(cmd, offscreenImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    });

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
    if (context->hasValidationErrors()) {
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
