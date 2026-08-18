#include <doctest/doctest.h>
#include <rx_asset/texture_cache.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_platform/window.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// texture_cache_test.cpp -- GPU-backed coverage for rx::asset::TextureCache
// [Phase 4 Stage 1 Task 14, spec D10/D11/D24/D25, gate matrix-issue03 as
// amended by gate/rulings-2026-08-18.md #3]. Joins rx_asset_tests (the
// SAME binary geometry_pool_test.cpp/accounting_test.cpp/bda_test.cpp
// already build against a real headless Device/Allocator/Uploader) -- see
// this directory's own CMakeLists.txt comment. texture_decode_test.cpp
// (rx_asset_gltf_tests, device-free) covers the parse/transcode DECISION
// layer this class calls into; this file proves the GPU-facing half
// (upload, bindless registration, sampler cache, D24 eviction, D25
// ticket consumption, FG9 accounting) against REAL fixtures on a real
// device.

using namespace rx::asset;

namespace {

std::string fixturePath(const std::string& name) { return std::string(RX_ASSET_ROOT_DIR) + "/assets/test/textures/" + name; }

// TextureCache stores its Allocator&/Device&/Uploader&/BindlessTable&/
// DeletionQueue& constructor arguments BY REFERENCE (matching
// GeometryPool::create()'s own established lifetime discipline,
// geometry_pool.h's own comment) -- it must be built AFTER every one of
// those objects has settled into its FINAL, stable address, never against
// a local variable this function is about to std::move() out of. Building
// it INSIDE this function (against `*device`/`*allocator`'s own
// still-local addresses, then moving those same objects into the
// returned aggregate) was this test file's own first-draft bug, caught
// directly by a real SIGABRT ("Invalid physicalDevice") the first time this file
// ran -- see this repository's own task-14-report.md for the full
// account. Fixed by the two-step pattern below: makeFixture() settles
// every GPU object into the returned TcTestFixture FIRST; makeCache()
// builds the TextureCache afterward, against the fixture's OWN
// (now-stable) members, exactly mirroring how every TEST_CASE in this
// binary already calls GeometryPool::create(fixture->allocator, ...)
// AFTER makeFixture() returns, never inside it.
struct TcTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    rx::rhi::Uploader uploader;
    rx::rhi::BindlessTable bindless;
    rx::rhi::DeletionQueue deletionQueue;
    std::unique_ptr<TextureCache> cache;
};

std::optional<TcTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping TextureCache test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping TextureCache test");
        return std::nullopt;
    }
    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());
    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);
    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());
    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());
    auto uploader = rx::rhi::Uploader::create(*allocator, *device);
    REQUIRE(uploader.has_value());

    rx::rhi::BindlessTable::Capacities capacities{/*sampledImages=*/64, /*samplers=*/8, /*storageBuffers=*/1};
    auto bindless = rx::rhi::BindlessTable::create(device->physicalDevice(), device->device(), capacities);
    REQUIRE(bindless.has_value());

    rx::rhi::DeletionQueue deletionQueue;

    // `cache` stays null here -- see this struct's own comment.
    return TcTestFixture{std::move(*window),      std::move(*context),   std::move(*device),
                          std::move(*allocator),    std::move(*uploader),  std::move(*bindless),
                          std::move(deletionQueue), nullptr};
}

// Builds `fixture.cache` against the fixture's OWN (now-stable) members --
// every TEST_CASE below calls this immediately after makeFixture() and
// before touching `fixture->cache`.
void makeCache(TcTestFixture& fixture) {
    fixture.cache = TextureCache::create(fixture.allocator, fixture.device, fixture.uploader, fixture.bindless,
                                          fixture.deletionQueue);
    REQUIRE(fixture.cache != nullptr);
}

// ---------------------------------------------------------------------
// Quadrant/deep-mip readback pipeline -- mirrors geometry_pool_test.cpp's
// own buildDrawPipeline()/readback pattern (raw dynamic rendering, no
// rx_graph) and samples/03_bindless_mesh's own bindless-sampling shader
// shape (gTextures[]/gSamplers[]/gTransforms[] at set-0 bindings 0/1/2,
// matching BindlessTable's own fixed layout so its descriptorSetLayout()
// substitutes cleanly as reflect()'s externalSet0 -- see that sample's
// own header comment for the full rationale, reused verbatim here).
constexpr const char* kQuadShaderSource = R"(
struct PushConstants {
    uint textureIndex;
    uint samplerIndex;
    float lod;
};
[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 0)]]
Texture2D gTextures[];

[[vk::binding(1, 0)]]
SamplerState gSamplers[];

[[vk::binding(2, 0)]]
StructuredBuffer<float4x4> gTransforms[];

struct VSIn {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    VSOut o;
    o.position = float4(input.position, 1.0);
    o.uv = input.uv;
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    return gTextures[gPush.textureIndex].SampleLevel(gSamplers[gPush.samplerIndex], input.uv, gPush.lod);
}
)";
const std::vector<std::string> kQuadEntryPoints = {"vsMain", "fsMain"};

struct QuadVertex {
    float position[3];
    float uv[2];
};

struct QuadPipeline {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderStageFlags pushConstantStages = 0;
};

void destroyQuadPipeline(VkDevice device, QuadPipeline& p) {
    if (p.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, p.pipeline, nullptr);
    }
    if (p.fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, p.fragModule, nullptr);
    }
    if (p.vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, p.vertModule, nullptr);
    }
    p = QuadPipeline{};
}

std::optional<QuadPipeline> buildQuadPipeline(VkDevice device, rx::rhi::BindlessTable& bindless, VkFormat colorFormat) {
    QuadPipeline result;

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        return std::nullopt;
    }
    rx::shader::CompileResult compileResult = compiler->compileFromSource("TcQuadModule", kQuadShaderSource, kQuadEntryPoints);
    if (!compileResult.ok) {
        MESSAGE("texture_cache_test quad shader compile failed: ", compileResult.diagnostics);
        return std::nullopt;
    }
    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value() || layoutInfo->bindings.size() != 3 || layoutInfo->pushRanges.size() != 1) {
        return std::nullopt;
    }

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, *layoutInfo, bindless.descriptorSetLayout());
    if (!layoutBundle.has_value()) {
        return std::nullopt;
    }
    result.pushConstantStages = layoutInfo->pushRanges[0].stages;
    result.layoutBundle = std::move(*layoutBundle);

    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = blob.code.size() * sizeof(uint32_t);
        moduleInfo.pCode = blob.code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            destroyQuadPipeline(device, result);
            return std::nullopt;
        }
        if (blob.entryPointName == "vsMain") {
            result.vertModule = module;
        } else if (blob.entryPointName == "fsMain") {
            result.fragModule = module;
        } else {
            vkDestroyShaderModule(device, module, nullptr);
            destroyQuadPipeline(device, result);
            return std::nullopt;
        }
    }
    if (result.vertModule == VK_NULL_HANDLE || result.fragModule == VK_NULL_HANDLE) {
        destroyQuadPipeline(device, result);
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

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(QuadVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(QuadVertex, position);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = offsetof(QuadVertex, uv);

    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount = 1;
    vertexInputState.pVertexBindingDescriptions = &binding;
    vertexInputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInputState.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
    inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = VK_CULL_MODE_NONE;  // winding-agnostic -- see this file's own header comment
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
    pipelineInfo.layout = result.layoutBundle.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &result.pipeline) != VK_SUCCESS) {
        destroyQuadPipeline(device, result);
        return std::nullopt;
    }
    return result;
}

// Full-screen quad: clip-space corners fed directly (no transform), UV
// (0,0) at the Vulkan-Y-down top-left corner through UV (1,1) at the
// bottom-right -- see this file's own header comment for the quadrant-
// color-to-screen-corner mapping this produces against the "quadrant"
// fixture's own TL=red/TR=green/BL=blue/BR=yellow layout.
constexpr std::array<QuadVertex, 4> kQuadVertices{{
    {{-1.0F, -1.0F, 0.0F}, {0.0F, 0.0F}},  // A: top-left
    {{1.0F, -1.0F, 0.0F}, {1.0F, 0.0F}},   // B: top-right
    {{1.0F, 1.0F, 0.0F}, {1.0F, 1.0F}},    // C: bottom-right
    {{-1.0F, 1.0F, 0.0F}, {0.0F, 1.0F}},   // D: bottom-left
}};
constexpr std::array<uint32_t, 6> kQuadIndices{0, 1, 2, 0, 2, 3};

struct QuadrantPixels {
    std::array<uint8_t, 4> topLeft;
    std::array<uint8_t, 4> topRight;
    std::array<uint8_t, 4> bottomLeft;
    std::array<uint8_t, 4> bottomRight;
};

// Renders the full-screen quad sampling `bindlessTextureIndex`/
// `bindlessSamplerIndex` at explicit LOD `lod`, into a `extent`x`extent`
// UNORM offscreen target, and returns the 4 corner pixels (well inside
// each quadrant -- 1/4 and 3/4 fractions, matching this file's own
// simpler quadrant geometry -- no bilinear-edge derivation needed since
// SampleLevel against a POINT- or LINEAR-filtered single/explicit-LOD
// sample of a flat-quadrant image is exact well within either quadrant
// half regardless of filter mode).
std::optional<QuadrantPixels> renderAndReadbackQuadrants(TcTestFixture& fixture, uint32_t bindlessTextureIndex,
                                                           uint32_t bindlessSamplerIndex, float lod,
                                                           uint32_t extent = 64) {
    VkDevice device = fixture.device.device();
    constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    auto pipeline = buildQuadPipeline(device, fixture.bindless, kColorFormat);
    if (!pipeline.has_value()) {
        return std::nullopt;
    }

    auto vertexBuffer = fixture.allocator.createHostVisibleBuffer(sizeof(kQuadVertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto indexBuffer = fixture.allocator.createHostVisibleBuffer(sizeof(kQuadIndices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!vertexBuffer.has_value() || !indexBuffer.has_value()) {
        destroyQuadPipeline(device, *pipeline);
        return std::nullopt;
    }
    std::memcpy(vertexBuffer->mappedData(), kQuadVertices.data(), sizeof(kQuadVertices));
    vertexBuffer->flush();
    std::memcpy(indexBuffer->mappedData(), kQuadIndices.data(), sizeof(kQuadIndices));
    indexBuffer->flush();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kColorFormat;
    imageInfo.extent = {extent, extent, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        destroyQuadPipeline(device, *pipeline);
        return std::nullopt;
    }
    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, image, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(fixture.device.physicalDevice(), &memProps);
    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == UINT32_MAX) {
        vkDestroyImage(device, image, nullptr);
        destroyQuadPipeline(device, *pipeline);
        return std::nullopt;
    }
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        destroyQuadPipeline(device, *pipeline);
        return std::nullopt;
    }
    vkBindImageMemory(device, image, imageMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kColorFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView imageView = VK_NULL_HANDLE;
    vkCreateImageView(device, &viewInfo, nullptr, &imageView);

    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(extent) * extent * 4;
    std::vector<uint8_t> pixels(pixelBytes);

    {
        auto cmdCtx = rx::rhi::CommandContext::create(device, fixture.device.graphicsQueue(), fixture.device.graphicsQueueFamily());
        if (!cmdCtx.has_value()) {
            vkDestroyImageView(device, imageView, nullptr);
            vkDestroyImage(device, image, nullptr);
            vkFreeMemory(device, imageMemory, nullptr);
            destroyQuadPipeline(device, *pipeline);
            return std::nullopt;
        }
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = imageView;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}};

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = VkRect2D{{0, 0}, {extent, extent}};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            vkCmdBeginRendering(cmd, &renderingInfo);

            VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent), static_cast<float>(extent), 0.0F, 1.0F};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            VkRect2D scissor{{0, 0}, {extent, extent}};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
            VkDescriptorSet bindlessSet = fixture.bindless.descriptorSet();
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layoutBundle.layout, 0, 1, &bindlessSet, 0, nullptr);

            struct { uint32_t textureIndex; uint32_t samplerIndex; float lod; } push{bindlessTextureIndex, bindlessSamplerIndex, lod};
            vkCmdPushConstants(cmd, pipeline->layoutBundle.layout, pipeline->pushConstantStages, 0, sizeof(push), &push);

            VkBuffer vb = vertexBuffer->handle();
            VkDeviceSize vbOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOffset);
            vkCmdBindIndexBuffer(cmd, indexBuffer->handle(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, static_cast<uint32_t>(kQuadIndices.size()), 1, 0, 0, 0);

            vkCmdEndRendering(cmd);
            rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        });

        auto readback = fixture.allocator.createHostVisibleBuffer(pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (!readback.has_value()) {
            vkDestroyImageView(device, imageView, nullptr);
            vkDestroyImage(device, image, nullptr);
            vkFreeMemory(device, imageMemory, nullptr);
            destroyQuadPipeline(device, *pipeline);
            return std::nullopt;
        }
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {extent, extent, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
        });
        readback->invalidate();
        std::memcpy(pixels.data(), readback->mappedData(), pixels.size());
    }

    vkDeviceWaitIdle(device);
    destroyQuadPipeline(device, *pipeline);
    vkDestroyImageView(device, imageView, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkFreeMemory(device, imageMemory, nullptr);

    auto pixelAt = [&](uint32_t x, uint32_t y) -> std::array<uint8_t, 4> {
        size_t o = (static_cast<size_t>(y) * extent + x) * 4;
        return {pixels[o], pixels[o + 1], pixels[o + 2], pixels[o + 3]};
    };
    const uint32_t q1 = extent / 4;
    const uint32_t q3 = extent - q1 - 1;
    QuadrantPixels result;
    result.topLeft = pixelAt(q1, q1);
    result.topRight = pixelAt(q3, q1);
    result.bottomLeft = pixelAt(q1, q3);
    result.bottomRight = pixelAt(q3, q3);
    return result;
}

bool approxEqual(const std::array<uint8_t, 4>& actual, std::array<uint8_t, 3> expectedRgb, int tolerance) {
    return std::abs(static_cast<int>(actual[0]) - expectedRgb[0]) <= tolerance &&
           std::abs(static_cast<int>(actual[1]) - expectedRgb[1]) <= tolerance &&
           std::abs(static_cast<int>(actual[2]) - expectedRgb[2]) <= tolerance;
}

}  // namespace

// ===== create() + D11 fallback textures =====================================

TEST_CASE("TextureCache::create builds real, distinct fallback/utility textures: checkerboard + role-appropriate "
          "white/flat-normal/neutral-MR, never a null/invalid handle") {
    auto fixture = makeFixture("rx_asset_tc_create");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    TextureHandle checker = fixture->cache->checkerboardHandle();
    CHECK(checker.isValid());
    const TextureRecord& checkerRecord = fixture->cache->resolve(checker);
    CHECK(checkerRecord.isFallback);
    CHECK(checkerRecord.resident);
    CHECK(checkerRecord.width == 4);
    CHECK(checkerRecord.height == 4);

    TextureHandle white = fixture->cache->fallbackHandle(TextureRole::BaseColor);
    TextureHandle emissiveWhite = fixture->cache->fallbackHandle(TextureRole::Emissive);
    TextureHandle genericWhite = fixture->cache->fallbackHandle(TextureRole::GenericData);
    CHECK(white.isValid());
    // [D11] "1x1 white... shared across baseColor/emissive/genericData" --
    // the SAME underlying texture, not three near-identical uploads.
    CHECK(white == emissiveWhite);
    CHECK(white == genericWhite);

    TextureHandle flatNormal = fixture->cache->fallbackHandle(TextureRole::Normal);
    CHECK(flatNormal.isValid());
    CHECK_FALSE(flatNormal == white);
    CHECK(fixture->cache->resolve(flatNormal).role == TextureRole::Normal);

    TextureHandle neutralMrMr = fixture->cache->fallbackHandle(TextureRole::MetallicRoughness);
    TextureHandle neutralMrOcc = fixture->cache->fallbackHandle(TextureRole::Occlusion);
    CHECK(neutralMrMr.isValid());
    CHECK(neutralMrMr == neutralMrOcc);  // [D11] shared "neutral-MR"
    CHECK_FALSE(neutralMrMr == white);
    CHECK_FALSE(neutralMrMr == flatNormal);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== In-memory load (bytes never touch disk) + role/format assertions ===

TEST_CASE("TextureCache::loadFromBytes: an in-memory KTX2 byte span (never touching disk inside the call) "
          "loads baseColor UASTC to the role's exact BC7_SRGB format when the device supports it, else the "
          "role-correct RGBA32 fallback [matrix's own lavapipe-support-verify-in-task wording]") {
    auto fixture = makeFixture("rx_asset_tc_inmemory");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    std::ifstream file(fixturePath("basecolor_uastc.ktx2"), std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    auto size = file.tellg();
    file.seekg(0);
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    TextureHandle handle = fixture->cache->loadFromBytes(std::span<const std::byte>(bytes), TextureRole::BaseColor, "in-memory-basecolor");
    REQUIRE(handle.isValid());
    CHECK_FALSE(handle == fixture->cache->checkerboardHandle());

    const TextureRecord& record = fixture->cache->resolve(handle);
    CHECK_FALSE(record.isFallback);
    CHECK(record.width == 4);
    CHECK(record.height == 4);
    CHECK(record.mipLevels == 1);
    CHECK(record.role == TextureRole::BaseColor);
    // Exact-vs-fallback: whichever the REAL device actually supports --
    // asserted against BOTH possibilities, not assumed.
    CHECK((record.format == VK_FORMAT_BC7_SRGB_BLOCK || record.format == VK_FORMAT_R8G8B8A8_SRGB));

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache::load(path, role): the filesystem convenience overload wraps loadFromBytes and "
          "produces the SAME result the in-memory path does") {
    auto fixture = makeFixture("rx_asset_tc_pathload");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("basecolor_uastc.ktx2"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());
    CHECK_FALSE(handle == fixture->cache->checkerboardHandle());
    CHECK(fixture->cache->resolve(handle).width == 4);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache: ETC1S and UASTC+zstd fixtures both load to a real, non-fallback texture") {
    auto fixture = makeFixture("rx_asset_tc_etc1s_zstd");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle etc1s = fixture->cache->load(fixturePath("basecolor_etc1s.ktx2"), TextureRole::BaseColor);
    TextureHandle zstd = fixture->cache->load(fixturePath("basecolor_uastc_zstd.ktx2"), TextureRole::BaseColor);
    CHECK(etc1s.isValid());
    CHECK(zstd.isValid());
    CHECK_FALSE(etc1s == fixture->cache->checkerboardHandle());
    CHECK_FALSE(zstd == fixture->cache->checkerboardHandle());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache: a non-multiple-of-4 base-dimension KTX2 (6x5) loads correctly with its TRUE extent") {
    auto fixture = makeFixture("rx_asset_tc_nonmult4");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("nonmult4.ktx2"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());
    const TextureRecord& record = fixture->cache->resolve(handle);
    CHECK(record.width == 6);
    CHECK(record.height == 5);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache: mips-absent KTX2 loads exactly 1 mip level (no runtime mip generation for "
          "compressed formats, D10)") {
    auto fixture = makeFixture("rx_asset_tc_mipsabsent");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("mips_absent.ktx2"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());
    CHECK(fixture->cache->resolve(handle).mipLevels == 1);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache: a full mip chain (16x16 -> 1x1) loads all 5 levels") {
    auto fixture = makeFixture("rx_asset_tc_withmips");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("basecolor_withmips_uastc.ktx2"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());
    const TextureRecord& record = fixture->cache->resolve(handle);
    CHECK(record.width == 16);
    CHECK(record.height == 16);
    CHECK(record.mipLevels == 5);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== D11 fallback routing for every failure mode ==========================

TEST_CASE("TextureCache: cubemap KTX2 -> checkerboard fallback (never a silently-wrong 2D slice)") {
    auto fixture = makeFixture("rx_asset_tc_cubemap");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("cubemap.ktx2"), TextureRole::BaseColor);
    CHECK(handle == fixture->cache->checkerboardHandle());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache: corrupt KTX2 -> checkerboard fallback, no crash") {
    auto fixture = makeFixture("rx_asset_tc_corrupt");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("corrupt.ktx2"), TextureRole::BaseColor);
    CHECK(handle == fixture->cache->checkerboardHandle());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache: a nonexistent path -> checkerboard fallback (byte source miss), no crash") {
    auto fixture = makeFixture("rx_asset_tc_missing");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("does_not_exist.ktx2"), TextureRole::BaseColor);
    CHECK(handle == fixture->cache->checkerboardHandle());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache: corrupt PNG bytes (stb decode failure) -> checkerboard fallback, no crash") {
    auto fixture = makeFixture("rx_asset_tc_corruptpng");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("corrupt.png"), TextureRole::BaseColor);
    CHECK(handle == fixture->cache->checkerboardHandle());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== stb PNG/JPG fallback path ============================================

TEST_CASE("TextureCache: a real PNG loads via stb to a real, non-fallback texture, mip level 0 only") {
    auto fixture = makeFixture("rx_asset_tc_png");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("quadrant.png"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());
    const TextureRecord& record = fixture->cache->resolve(handle);
    CHECK_FALSE(record.isFallback);
    CHECK(record.width == 8);
    CHECK(record.height == 8);
    CHECK(record.mipLevels == 1);
    CHECK(record.format == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache: a real JPEG loads via stb to a real, non-fallback texture") {
    auto fixture = makeFixture("rx_asset_tc_jpg");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("quadrant.jpg"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());
    CHECK_FALSE(fixture->cache->resolve(handle).isFallback);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Sampler cache (G6): dedup + negative test ============================

TEST_CASE("TextureCache::getOrCreateSampler: two identical glTF samplers dedup to ONE VkSampler; distinct "
          "state yields distinct samplers [G6 dedup + negative test]") {
    auto fixture = makeFixture("rx_asset_tc_samplercache");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    SamplerDesc a;  // glTF defaults (REPEAT/REPEAT, unspecified filters)
    SamplerDesc b;  // identical values, a SEPARATE struct instance
    CHECK(fixture->cache->samplerCountForTesting() == 0);

    VkSampler samplerA = fixture->cache->getOrCreateSampler(a);
    REQUIRE(samplerA != VK_NULL_HANDLE);
    CHECK(fixture->cache->samplerCountForTesting() == 1);

    VkSampler samplerB = fixture->cache->getOrCreateSampler(b);
    CHECK(samplerB == samplerA);
    CHECK(fixture->cache->samplerCountForTesting() == 1);  // dedup -- no new sampler created

    SamplerDesc c;
    c.wrapS = 33071;  // CLAMP_TO_EDGE -- distinct state
    VkSampler samplerC = fixture->cache->getOrCreateSampler(c);
    REQUIRE(samplerC != VK_NULL_HANDLE);
    CHECK_FALSE(samplerC == samplerA);
    CHECK(fixture->cache->samplerCountForTesting() == 2);  // negative test -- distinct state, distinct sampler

    SamplerDesc d;
    d.minFilter = 9728;  // NEAREST -- another distinct axis
    VkSampler samplerD = fixture->cache->getOrCreateSampler(d);
    CHECK_FALSE(samplerD == samplerA);
    CHECK_FALSE(samplerD == samplerC);
    CHECK(fixture->cache->samplerCountForTesting() == 3);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== D24 eviction: evict -> fallback -> reload -> real ====================

TEST_CASE("TextureCache D24: evictForTesting immediately makes resolve() report the checkerboard fallback "
          "(non-resident, observable); reclaim runs only once the tagged frame's fence signals; a later "
          "load() produces a fresh, independent real handle") {
    auto fixture = makeFixture("rx_asset_tc_eviction");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    TextureHandle handle = fixture->cache->load(fixturePath("basecolor_uastc.ktx2"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());
    REQUIRE_FALSE(fixture->cache->resolve(handle).isFallback);
    size_t liveBefore = fixture->cache->liveTextureCountForTesting();

    fixture->cache->evictForTesting(handle, /*frameIndex=*/5);

    // Immediately non-resident -- resolve() substitutes the checkerboard
    // record (D24 clauses 1/2), even though the underlying GPU resource
    // has not actually been destroyed yet (fence not signaled).
    const TextureRecord& evicted = fixture->cache->resolve(handle);
    CHECK(evicted.isFallback);
    CHECK(evicted.width == 4);  // the checkerboard's own dims -- proves this IS the checkerboard, not a stale read
    CHECK(fixture->cache->liveTextureCountForTesting() == liveBefore);  // reclaim not yet due

    fixture->deletionQueue.onFrameFenceSignaled(4);  // not yet due
    CHECK(fixture->cache->liveTextureCountForTesting() == liveBefore);
    CHECK(fixture->cache->resolve(handle).isFallback);

    fixture->deletionQueue.onFrameFenceSignaled(5);  // now due -- real reclaim runs
    CHECK(fixture->cache->liveTextureCountForTesting() == liveBefore - 1);

    // Reload -> real, a genuinely NEW, independent handle (never a stale
    // resolve for the pre-eviction one).
    TextureHandle reloaded = fixture->cache->load(fixturePath("basecolor_uastc.ktx2"), TextureRole::BaseColor);
    REQUIRE(reloaded.isValid());
    CHECK_FALSE(reloaded == handle);
    CHECK_FALSE(fixture->cache->resolve(reloaded).isFallback);
    // The OLD handle still resolves to the fallback (dead/reclaimed slot
    // -- the same generational staleness check D6's HandlePool already
    // provides, per D24 clause 3).
    CHECK(fixture->cache->resolve(handle).isFallback);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== FG9 accounting: bytes-by-role balances to zero across load/evict ====

TEST_CASE("TextureCache::stats: bytes-by-role/count increases on load() and balances back down across "
          "evict+reclaim [Task 10 accounting-test pattern]") {
    auto fixture = makeFixture("rx_asset_tc_accounting");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    TextureCacheStats before = fixture->cache->stats();
    uint32_t roleIdx = static_cast<uint32_t>(TextureRole::BaseColor);

    TextureHandle handle = fixture->cache->load(fixturePath("basecolor_withmips_uastc.ktx2"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());

    TextureCacheStats afterLoad = fixture->cache->stats();
    CHECK(afterLoad.byRole[roleIdx].count == before.byRole[roleIdx].count + 1);
    CHECK(afterLoad.byRole[roleIdx].bytes > before.byRole[roleIdx].bytes);
    CHECK(afterLoad.totalCount == before.totalCount + 1);
    CHECK(afterLoad.totalBytes > before.totalBytes);

    fixture->cache->evictForTesting(handle, /*frameIndex=*/1);
    fixture->deletionQueue.onFrameFenceSignaled(1);

    TextureCacheStats afterReclaim = fixture->cache->stats();
    CHECK(afterReclaim.byRole[roleIdx].count == before.byRole[roleIdx].count);
    CHECK(afterReclaim.byRole[roleIdx].bytes == before.byRole[roleIdx].bytes);
    CHECK(afterReclaim.totalCount == before.totalCount);
    CHECK(afterReclaim.totalBytes == before.totalBytes);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Quadrant pixel GPU test [plan Task 14: "glTF-referenced KTX2 renders =
// through the cache"] ========================================================

TEST_CASE("TextureCache GPU: a loaded KTX2's 4 quadrant colors render correctly through a real bindless-"
          "sampled draw (BC7 transcode -> upload -> bindless registration -> sample, end to end)") {
    auto fixture = makeFixture("rx_asset_tc_quadrant");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    TextureHandle handle = fixture->cache->load(fixturePath("basecolor_uastc.ktx2"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());
    const TextureRecord& record = fixture->cache->resolve(handle);
    REQUIRE_FALSE(record.isFallback);

    SamplerDesc nearestDesc;
    nearestDesc.magFilter = 9728;  // NEAREST -- exact texel reads, no filtering ambiguity
    nearestDesc.minFilter = 9728;
    VkSampler sampler = fixture->cache->getOrCreateSampler(nearestDesc);
    REQUIRE(sampler != VK_NULL_HANDLE);
    rx::rhi::BindlessHandle samplerHandle = fixture->bindless.registerSampler(sampler);
    REQUIRE(samplerHandle.isValid());

    auto readback = renderAndReadbackQuadrants(*fixture, record.bindlessIndex, samplerHandle.index(), /*lod=*/0.0F);
    REQUIRE(readback.has_value());

    // Quadrant fixture layout [assets/test/textures/generate_fixtures.sh]:
    // TL=red, TR=green, BL=blue, BR=yellow. Primary-color channels (each
    // exactly 0 or 255) are invariant under sRGB<->linear conversion, so
    // an exact (zero-tolerance) match is the correct expectation even
    // though this texture samples through an sRGB-decoding format.
    CHECK(approxEqual(readback->topLeft, {255, 0, 0}, 2));
    CHECK(approxEqual(readback->topRight, {0, 255, 0}, 2));
    CHECK(approxEqual(readback->bottomLeft, {0, 0, 255}, 2));
    CHECK(approxEqual(readback->bottomRight, {255, 255, 0}, 2));

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Deep-mip readback / sub-block mip-tail GPU test =====================

TEST_CASE("TextureCache GPU: every mip level of a flat-color chain -- INCLUDING the 2x2 and 1x1 sub-block "
          "tail -- samples back the correct color, proving block-compressed multi-level upload (Uploader::"
          "uploadImageMips) landed each level's bytes at the right subresource with the right block-rounded "
          "region [gate matrix-issue03 N4, the 'classic off-by-one']") {
    auto fixture = makeFixture("rx_asset_tc_deepmip");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    // GenericData role (not BaseColor): this fixture's own container DFD
    // is linear (assign_oetf linear, generate_fixtures.sh), and
    // GenericData's role-derived format is BC7_UNORM (no sRGB decode) --
    // see that fixture's own generation comment for why this sidesteps
    // gamma-curve reasoning entirely for this specific test.
    TextureHandle handle = fixture->cache->load(fixturePath("flat_withmips_uastc.ktx2"), TextureRole::GenericData);
    REQUIRE(handle.isValid());
    const TextureRecord& record = fixture->cache->resolve(handle);
    REQUIRE_FALSE(record.isFallback);
    REQUIRE(record.mipLevels == 4);  // 8 -> 4 -> 2 -> 1

    SamplerDesc nearestDesc;
    nearestDesc.magFilter = 9728;
    nearestDesc.minFilter = 9984;  // NEAREST_MIPMAP_NEAREST -- honors explicit SampleLevel exactly, no blending across levels
    VkSampler sampler = fixture->cache->getOrCreateSampler(nearestDesc);
    REQUIRE(sampler != VK_NULL_HANDLE);
    rx::rhi::BindlessHandle samplerHandle = fixture->bindless.registerSampler(sampler);
    REQUIRE(samplerHandle.isValid());

    // flat_orange.png = #FF8000 = (255, 128, 0) -- every level of a FLAT
    // solid color's box-filtered mip chain is the SAME color (no cross-
    // region blending ambiguity, unlike the quadrant pattern).
    for (uint32_t level = 0; level < record.mipLevels; ++level) {
        CAPTURE(level);
        auto readback = renderAndReadbackQuadrants(*fixture, record.bindlessIndex, samplerHandle.index(), static_cast<float>(level));
        REQUIRE(readback.has_value());
        // A flat image's own 4 "quadrants" are all the same pixel color.
        CHECK(approxEqual(readback->topLeft, {255, 128, 0}, 6));
        CHECK(approxEqual(readback->bottomRight, {255, 128, 0}, 6));
    }

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
