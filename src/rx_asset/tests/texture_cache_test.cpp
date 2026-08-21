#include <doctest/doctest.h>
#include <rx_asset/geometry_pool.h>
#include <rx_asset/registry.h>
#include <rx_asset/texture_cache.h>
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_platform/window.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <rx_task/scheduler.h>
#include <spdlog/sinks/ostream_sink.h>
#include <glm/gtc/packing.hpp>
#include <glm/vec4.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
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

// [closure-sweep item 4] Task 13/14's glTF fixtures live directly under
// assets/test/ (not assets/test/textures/, the KTX2-only directory
// fixturePath() above points at) -- same directory
// import_gltf_basisu_test.cpp's own testAssetDir() uses, duplicated here
// rather than shared (this codebase's own established per-file fixture
// convention, e.g. LogCapture below).
std::string gltfFixturePath(const std::string& name) { return std::string(RX_ASSET_ROOT_DIR) + "/assets/test/" + name; }

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

std::optional<TcTestFixture> makeFixture(
    const char* title,
    rx::rhi::BindlessTable::Capacities capacities = {/*sampledImages=*/64, /*samplers=*/8, /*storageBuffers=*/1}) {
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

    // `capacities` defaults to the pre-existing hardcoded value every
    // TEST_CASE in this file relied on before this parameter existed --
    // byte-identical behavior for every caller that doesn't pass one
    // explicitly. [Fix round, Finding H1(b)] The capacity-exhaustion
    // regression TEST_CASE below is the one caller that does.
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

// Renders a quad sampling `bindlessTextureIndex`/`bindlessSamplerIndex` at
// explicit LOD `lod`, into a `extent`x`extent` UNORM offscreen target, and
// returns the 4 corner pixels (well inside each quadrant -- 1/4 and 3/4
// fractions, matching this file's own simpler quadrant geometry -- no
// bilinear-edge derivation needed since SampleLevel against a POINT- or
// LINEAR-filtered single/explicit-LOD sample of a flat-quadrant image is
// exact well within either quadrant half regardless of filter mode).
//
// `vertices` [Fix round 2, review finding: deduplicate] defaults to the
// fixed [0,1]-cornered `kQuadVertices` above -- EVERY pre-existing caller
// (this file's own established D10/D11/G6 test net) keeps compiling and
// behaving byte-identically without passing this parameter at all. The
// sampler-wrap regression TEST_CASE (below) is the one caller that passes
// a CUSTOM quad: DamagedHelmet's real defect (helmet-sampler-fix-brief.md)
// needs UV values OUTSIDE [0,1], which `kQuadVertices`'s fixed [0,1]
// corners can never produce. (Previously a full, deliberately near-
// identical duplicate of this function's own body under a second name,
// `renderCustomQuadAndReadbackQuadrants()` -- consolidated here per code
// review, since a defaulted parameter gives the same "zero risk to
// existing callers" guarantee the duplicate was written to provide,
// without carrying two ~90-line GPU-pipeline-setup bodies that would
// otherwise drift independently.)
std::optional<QuadrantPixels> renderAndReadbackQuadrants(TcTestFixture& fixture, uint32_t bindlessTextureIndex,
                                                           uint32_t bindlessSamplerIndex, float lod,
                                                           uint32_t extent = 64,
                                                           const std::array<QuadVertex, 4>& vertices = kQuadVertices) {
    VkDevice device = fixture.device.device();
    constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    auto pipeline = buildQuadPipeline(device, fixture.bindless, kColorFormat);
    if (!pipeline.has_value()) {
        return std::nullopt;
    }

    auto vertexBuffer = fixture.allocator.createHostVisibleBuffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto indexBuffer = fixture.allocator.createHostVisibleBuffer(sizeof(kQuadIndices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!vertexBuffer.has_value() || !indexBuffer.has_value()) {
        destroyQuadPipeline(device, *pipeline);
        return std::nullopt;
    }
    std::memcpy(vertexBuffer->mappedData(), vertices.data(), sizeof(vertices));
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

// [Phase 5 Task 6, ticket #42, gate matrix-p5t06-ktx2-cubemap-hdr row 11's
// own "direct face-indexed readback" alternative] A sampler-free
// readback of ONE subresource (a specific mip level + array layer) of a
// real, already-uploaded rx::rhi::Texture2D's underlying VkImage --
// vkCmdCopyImageToBuffer straight off the GPU image, mirroring the
// established texture_test.cpp/upload_test.cpp readback pattern (this
// file's own renderAndReadbackQuadrants() above uses a fragment-shader/
// sampler pipeline instead, which has no bindless-array slot for a
// TextureCube-typed binding -- this class's own fixed 3-binding bindless
// layout is 2D-only; see TextureCache::rawImageForTesting()'s own header
// comment). `bytesPerTexel` parametrizes over both the 4-byte RGBA8 cube
// fixture and the 8-byte R16G16B16A16_SFLOAT HDR fixture this task adds.
std::optional<std::vector<uint8_t>> readBackSubresource(TcTestFixture& fixture, VkImage image, VkExtent2D extent,
                                                           uint32_t mipLevel, uint32_t arrayLayer,
                                                           VkDeviceSize bytesPerTexel) {
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(extent.width) * extent.height * bytesPerTexel;
    auto readback = fixture.allocator.createHostVisibleBuffer(byteSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!readback.has_value()) {
        return std::nullopt;
    }
    auto cmdCtx = rx::rhi::CommandContext::create(fixture.device.device(), fixture.device.graphicsQueue(),
                                                    fixture.device.graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        return std::nullopt;
    }
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = mipLevel;
        region.imageSubresource.baseArrayLayer = arrayLayer;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
        rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    readback->invalidate();
    std::vector<uint8_t> result(byteSize);
    std::memcpy(result.data(), readback->mappedData(), byteSize);
    return result;
}

bool approxEqual(const std::array<uint8_t, 4>& actual, std::array<uint8_t, 3> expectedRgb, int tolerance) {
    return std::abs(static_cast<int>(actual[0]) - expectedRgb[0]) <= tolerance &&
           std::abs(static_cast<int>(actual[1]) - expectedRgb[1]) <= tolerance &&
           std::abs(static_cast<int>(actual[2]) - expectedRgb[2]) <= tolerance;
}

// [Fix round 1] Swaps spdlog's default logger for an ostream-capturing one
// for the scope of one TEST_CASE -- the same lightweight rx_core-only
// pattern src/rx_core/tests/log_test.cpp, texture_decode_test.cpp, and
// import_gltf_basisu_test.cpp all already establish (duplicated per-file,
// matching this codebase's own established precedent, not shared).
struct LogCapture {
    std::ostringstream stream;
    std::shared_ptr<spdlog::logger> previousDefault;

    LogCapture() {
        rx::core::log::init();
        previousDefault = spdlog::default_logger();
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream);
        auto testLogger = std::make_shared<spdlog::logger>("texture_cache_test", sink);
        testLogger->set_pattern("%v");
        spdlog::set_default_logger(testLogger);
    }
    ~LogCapture() { spdlog::set_default_logger(previousDefault); }

    std::string str() const { return stream.str(); }
    int count(const std::string& needle) const {
        const std::string s = str();
        int n = 0;
        size_t pos = 0;
        while ((pos = s.find(needle, pos)) != std::string::npos) {
            ++n;
            pos += needle.size();
        }
        return n;
    }
};

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

// [Phase 5 Task 6, ticket #42, gate matrix-p5t06-ktx2-cubemap-hdr row 2 --
// the DISCRIMINATION PROOF] Replaces the Phase 4 test that asserted this
// SAME fixture (cubemap.ktx2) resolved to the checkerboard -- exactly the
// assertion a no-op cube implementation could leave passing. Loads the
// SAME file and asserts the FLIP: a real, non-fallback, cube-flagged
// texture.
TEST_CASE("TextureCache: cubemap KTX2 now loads as a real, non-fallback cube texture [matrix row 2, "
          "discrimination proof]") {
    auto fixture = makeFixture("rx_asset_tc_cubemap");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("cubemap.ktx2"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());
    CHECK_FALSE(handle == fixture->cache->checkerboardHandle());
    const TextureRecord& record = fixture->cache->resolve(handle);
    CHECK_FALSE(record.isFallback);
    CHECK(record.isCube);
    CHECK(record.width == 4);   // ONE FACE's own extent (row 7's own contract)
    CHECK(record.height == 4);
    CHECK(record.mipLevels == 1);  // cubemap.ktx2 is deliberately single-level
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Phase 5 Task 6, gate matrix row 3] Flat 2D-array and cube-array KTX2
// stay explicitly rejected after the narrowed predicate -- their own
// GPU-level regression, mirroring this test's own pre-Task-6 shape
// (which the test just above replaced) but against the two NEW
// still-rejected fixtures instead of the now-supported cubemap.ktx2.
//
// [Review round, MINOR] LogCapture-asserted WARN text, matching this
// file's own established D11-rejection-path convention (the
// sRGB-mislabeled-normal WARN test and the log-once-dedup test just
// above both lock their exact message text this same way) -- the actual
// RX_LOG_WARN only fires here, at the TextureCache layer
// (applyDecodeResult()); the device-free DecodedKtx2Texture::
// parseAndTranscode() tests in texture_decode_test.cpp have no message
// text to capture at all (that layer only returns the Ktx2ParseError
// enum), so this is the one place this warning's own wording is
// regression-locked.
TEST_CASE("TextureCache: flat 2D-array and cube-array KTX2 both still -> checkerboard fallback (cubemap-only "
          "support, matrix row 3)") {
    auto fixture = makeFixture("rx_asset_tc_array_rejected");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    LogCapture capture;
    TextureHandle array2d = fixture->cache->load(fixturePath("array2d_rejected.ktx2"), TextureRole::BaseColor);
    TextureHandle cubeArray = fixture->cache->load(fixturePath("cubearray_rejected.ktx2"), TextureRole::BaseColor);
    CHECK(array2d == fixture->cache->checkerboardHandle());
    CHECK(cubeArray == fixture->cache->checkerboardHandle());

    // Both fixtures fire the SAME shared warning text (texture_decode.cpp's
    // decodeKtx2ForUpload(), "unsupported-layout" category) -- distinct
    // debugNames (the two different filenames) mean D11's per-(debugName,
    // category) dedup does NOT collapse them into one line, so this text
    // is expected exactly twice, once per fixture.
    CHECK(capture.count("is an array, cube-array, or non-2D KTX2 container") == 2);
    CHECK(capture.count("cubemap-only per the Phase 5 Task 6 ruling") == 2);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Phase 5 Task 6, gate matrix row 6/11] THE primary cube GPU acceptance
// test: uploads cubemap_mips.ktx2 (32x32 base, 6 mip levels, each face a
// distinct flat authored color) and directly reads back EVERY face at
// BOTH mip 0 and the deepest (1x1) mip level, asserting each matches its
// authored color EXACTLY -- proving mip data landed in the correct
// face's own subresource range, not just that SOME upload succeeded.
TEST_CASE("TextureCache: cubemap_mips.ktx2 GPU readback proves exact per-face, per-mip values [matrix row 6/11]") {
    auto fixture = makeFixture("rx_asset_tc_cubemap_mips");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("cubemap_mips.ktx2"), TextureRole::Environment);
    REQUIRE(handle.isValid());
    CHECK_FALSE(handle == fixture->cache->checkerboardHandle());
    const TextureRecord& record = fixture->cache->resolve(handle);
    CHECK(record.isCube);
    CHECK(record.width == 32);
    CHECK(record.height == 32);
    CHECK(record.mipLevels == 6);  // 32 -> 16 -> 8 -> 4 -> 2 -> 1

    VkImage image = fixture->cache->rawImageForTesting(handle);
    REQUIRE(image != VK_NULL_HANDLE);

    // Standard KTX2 cube face order (+X,-X,+Y,-Y,+Z,-Z), matching
    // generate_fixtures.sh's own toktx invocation order -- identical
    // authored colors to the device-free flip test (texture_decode_test.cpp),
    // here proven all the way through GPU upload + readback instead.
    struct FaceColor {
        uint32_t face;
        std::array<uint8_t, 4> rgba;
    };
    constexpr std::array<FaceColor, 6> kExpected{{
        {0, {0xFF, 0x00, 0x00, 0xFF}},  // +X red
        {1, {0x00, 0xFF, 0xFF, 0xFF}},  // -X cyan
        {2, {0x00, 0xFF, 0x00, 0xFF}},  // +Y green
        {3, {0xFF, 0x00, 0xFF, 0xFF}},  // -Y magenta
        {4, {0x00, 0x00, 0xFF, 0xFF}},  // +Z blue
        {5, {0xFF, 0xFF, 0x00, 0xFF}},  // -Z yellow
    }};

    for (const FaceColor& expected : kExpected) {
        for (uint32_t level : {0U, 5U}) {  // mip 0 (32x32) and the deepest 1x1 tail
            const uint32_t extent = 32U >> level;
            auto pixels = readBackSubresource(*fixture, image, VkExtent2D{extent, extent}, level, expected.face,
                                               /*bytesPerTexel=*/4);
            REQUIRE(pixels.has_value());
            // A flat authored color box-filters to itself at every mip
            // level (same reasoning as flat_withmips_uastc.ktx2's own
            // deep-mip test) -- every texel in this level must match
            // EXACTLY, not just the corners.
            for (size_t texel = 0; texel < static_cast<size_t>(extent) * extent; ++texel) {
                const uint8_t* px = pixels->data() + texel * 4;
                CAPTURE(expected.face);
                CAPTURE(level);
                CHECK(px[0] == expected.rgba[0]);
                CHECK(px[1] == expected.rgba[1]);
                CHECK(px[2] == expected.rgba[2]);
                CHECK(px[3] == expected.rgba[3]);
            }
        }
    }
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Equirect HDR (Radiance .hdr) input [Phase 5 Task 6, gate matrix
// row 9/13] ===================================================================

TEST_CASE("TextureCache: equirect .hdr loads as a real R16G16B16A16_SFLOAT texture -- a >1.0 texel survives "
          "GPU upload + readback exactly [matrix row 13, the ticket's own explicit acceptance bar]") {
    auto fixture = makeFixture("rx_asset_tc_hdr");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("equirect_test.hdr"), TextureRole::Environment);
    REQUIRE(handle.isValid());
    CHECK_FALSE(handle == fixture->cache->checkerboardHandle());
    const TextureRecord& record = fixture->cache->resolve(handle);
    CHECK_FALSE(record.isCube);
    CHECK(record.format == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(record.width == 2);
    CHECK(record.height == 2);

    VkImage image = fixture->cache->rawImageForTesting(handle);
    REQUIRE(image != VK_NULL_HANDLE);
    auto pixels = readBackSubresource(*fixture, image, VkExtent2D{2, 2}, /*mipLevel=*/0, /*arrayLayer=*/0,
                                       /*bytesPerTexel=*/8);
    REQUIRE(pixels.has_value());
    REQUIRE(pixels->size() == 2 * 2 * 8);

    auto texelAt = [&](size_t index) {
        uint64_t packed = 0;
        std::memcpy(&packed, pixels->data() + index * 8, sizeof(packed));
        return glm::unpackHalf4x16(packed);
    };
    // Same per-texel values as texture_decode_test.cpp's own device-free
    // decodeStbImageHdr() test, this time round-tripped all the way
    // through real GPU upload (glm::packHalf4x16 in decodeStbHdrForUpload())
    // and a real GPU readback -- proving no accidental UNORM clamp/8-bit
    // truncation anywhere in the path (row 13's own acceptance wording).
    glm::vec4 tl = texelAt(0);
    glm::vec4 br = texelAt(3);
    CHECK(tl.r == doctest::Approx(4.0F).epsilon(0.001));
    CHECK(tl.g == doctest::Approx(0.5F).epsilon(0.001));
    CHECK(tl.b == doctest::Approx(0.5F).epsilon(0.001));
    CHECK(tl.r > 1.0F);
    CHECK(br.r == doctest::Approx(2.0F).epsilon(0.001));
    CHECK(br.g == doctest::Approx(2.0F).epsilon(0.001));
    CHECK(br.b == doctest::Approx(2.0F).epsilon(0.001));

    TextureCacheStats stats = fixture->cache->stats();
    CHECK(stats.byRole[static_cast<size_t>(TextureRole::Environment)].count >= 1);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache: corrupt HDR bytes -> checkerboard fallback, no crash [mirrors the corrupt.ktx2/corrupt.png "
          "tests' identical D11 contract]") {
    auto fixture = makeFixture("rx_asset_tc_corrupthdr");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle handle = fixture->cache->load(fixturePath("corrupt.hdr"), TextureRole::Environment);
    CHECK(handle == fixture->cache->checkerboardHandle());
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache: TextureRole::Environment's own D11 fallback is a REAL, non-checkerboard, uniform "
          "mid-gray R16G16B16A16_SFLOAT texture [coordinator ruling T6, matrix Open Question 3]") {
    auto fixture = makeFixture("rx_asset_tc_env_fallback");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);
    TextureHandle fallback = fixture->cache->fallbackHandle(TextureRole::Environment);
    REQUIRE(fallback.isValid());
    CHECK_FALSE(fallback == fixture->cache->checkerboardHandle());
    const TextureRecord& record = fixture->cache->resolve(fallback);
    CHECK(record.isFallback);
    CHECK(record.format == VK_FORMAT_R16G16B16A16_SFLOAT);

    VkImage image = fixture->cache->rawImageForTesting(fallback);
    REQUIRE(image != VK_NULL_HANDLE);
    auto pixels = readBackSubresource(*fixture, image, VkExtent2D{record.width, record.height}, /*mipLevel=*/0,
                                       /*arrayLayer=*/0, /*bytesPerTexel=*/8);
    REQUIRE(pixels.has_value());
    uint64_t packed = 0;
    std::memcpy(&packed, pixels->data(), sizeof(packed));
    glm::vec4 decoded = glm::unpackHalf4x16(packed);
    // Mid-gray (0.5) -- deliberately NOT black (D11's "neutral ambient",
    // not "no light at all") and NOT sRGB 0.5 (Environment is always
    // linear, roleExpectsSrgb()==false).
    CHECK(decoded.r == doctest::Approx(0.5F).epsilon(0.001));
    CHECK(decoded.g == doctest::Approx(0.5F).epsilon(0.001));
    CHECK(decoded.b == doctest::Approx(0.5F).epsilon(0.001));
    CHECK(decoded.a == doctest::Approx(1.0F).epsilon(0.001));
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

TEST_CASE("TextureCache: a real PNG loads via stb to a real, non-fallback texture with a FULL runtime-generated "
          "mip chain [texture-path round, D10 Option A -- previously mip level 0 only]") {
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
    // 8x8 -> 4x4 -> 2x2 -> 1x1: 4 levels total, exactly Vulkan's own
    // floor(log2(max(w,h)))+1 formula -- see generateStbMipChain()'s own
    // header comment (texture_decode.h).
    CHECK(record.mipLevels == 4);
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

// ===== Bindless capacity exhaustion (fix round, Finding H1(b)) ==============
//
// An independent review of samples/09_scene fetched real Sponza content
// (~25 materials) against that sample's own too-small BindlessTable
// (sampledImages=64) and reproduced a genuine crash: registerRealTexture()
// (texture_cache.cpp) already calls Uploader::uploadImageMips() -- which
// RECORDS real GPU commands referencing the new Texture2D's own VkImage --
// BEFORE attempting BindlessTable::registerSampledImage(); when THAT call
// fails (capacity exhausted), the function returned immediately, letting
// the local Texture2D's destructor destroy a VkImage its own
// already-recorded (not yet flushed/awaited) upload commands still
// reference -- "UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-
// VkImage: ... bound VkImage ... was destroyed", then a segfault. No
// earlier test ever drove bindless sampled-image capacity to genuine
// exhaustion during a real registerRealTexture() call, so this path had
// never been exercised. This TEST_CASE reproduces the exact failure mode
// in isolation (no Sponza/network fetch needed): a fixture whose
// sampledImages capacity is EXACTLY the D11 fallback-texture count (5 as
// of Phase 5 Task 6's Environment mid-gray addition: checkerboard + white
// + flat-normal + neutral-MR + environment, see buildFallbackTextures()'s
// own comment) -- TextureCache::create() itself must still succeed (it
// consumes exactly 5, zero spare), but the very NEXT real texture load is
// guaranteed to hit BindlessTable::registerSampledImage()'s own
// capacity-exhaustion rejection deterministically, every run, on any
// device.
TEST_CASE("TextureCache: bindless sampled-image capacity exhaustion at registerRealTexture() -- checkerboard "
          "fallback, no crash, zero validation errors [fix round, Finding H1(b), independent review of #15]") {
    rx::rhi::BindlessTable::Capacities tightCapacities{/*sampledImages=*/5, /*samplers=*/8, /*storageBuffers=*/1};
    auto fixture = makeFixture("rx_asset_tc_capexhaust", tightCapacities);
    if (!fixture.has_value()) {
        return;
    }
    // TextureCache::create() -> buildFallbackTextures() consumes exactly 5
    // sampled-image slots (checkerboard + white + flat-normal +
    // neutral-MR + [Phase 5 Task 6] environment) -- this must still
    // succeed against a capacity of exactly 5, proving the fixture itself
    // is sized precisely, not accidentally too small to even construct a
    // cache.
    makeCache(*fixture);
    CHECK_FALSE(fixture->context.hasValidationErrors());

    // The 5th sampled-image registration attempt: capacity is already
    // fully, deterministically exhausted. Before the fix, this crashed
    // (destroy-while-upload-commands-still-reference-the-image); after
    // the fix, registerRealTexture() flushes+awaits the already-recorded
    // upload commands before the doomed Texture2D is destroyed, and
    // load() falls back to the checkerboard handle via its own
    // established failure-path convention (same as every other
    // load-failure TEST_CASE above).
    TextureHandle handle = fixture->cache->load(fixturePath("quadrant.png"), TextureRole::BaseColor);
    CHECK(handle == fixture->cache->checkerboardHandle());
    CHECK_FALSE(fixture->context.hasValidationErrors());

    // A SECOND exhausted-capacity load, immediately after the first --
    // proves the fix doesn't merely survive ONE failure by accident (e.g.
    // some residual GPU idle state from fixture construction happening to
    // paper over the hazard) but genuinely leaves the Uploader/BindlessTable
    // in a consistent, reusable state.
    TextureHandle secondHandle = fixture->cache->load(fixturePath("quadrant.jpg"), TextureRole::Normal);
    CHECK(secondHandle == fixture->cache->checkerboardHandle());
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

// ===== Fix round 1, reviewer IMPORTANT-2: non-Basis KTX2 end-to-end =======
// coverage (previously: raw_rgba8.ktx2 was never loaded through
// TextureCache in any test -- only through the device-free decode layer
// directly -- and srgb_mislabeled_normal.ktx2 is UASTC/Basis, so neither
// live criterion of the non-Basis matrix row was proven end to end).

TEST_CASE("TextureCache GPU [IMPORTANT-2a]: a non-Basis (raw, uncompressed) KTX2 uploads in its STORED format "
          "-- never transcoded, never relabeled -- and its quadrant colors render correctly through the SAME "
          "bindless-sampled draw path the Basis fixtures use, proving the non-Basis upload branch end to end "
          "(not just format bookkeeping)") {
    auto fixture = makeFixture("rx_asset_tc_nonbasis_e2e");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    TextureHandle handle = fixture->cache->load(fixturePath("raw_rgba8.ktx2"), TextureRole::BaseColor);
    REQUIRE(handle.isValid());
    const TextureRecord& record = fixture->cache->resolve(handle);
    REQUIRE_FALSE(record.isFallback);

    // "uploaded in stored format when supported" -- raw_rgba8.ktx2's own
    // container vkFormat (verified directly via ktxinfo at fixture-
    // generation time, generate_fixtures.sh's own comment on this
    // fixture) IS VK_FORMAT_R8G8B8A8_SRGB; role (BaseColor) agrees, so
    // there is no colorspace disagreement to additionally prove here --
    // IMPORTANT-2b below covers the disagreeing case.
    CHECK(record.format == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(record.width == 4);
    CHECK(record.height == 4);
    CHECK(record.mipLevels == 1);

    SamplerDesc nearestDesc;
    nearestDesc.magFilter = 9728;  // NEAREST -- exact texel reads
    nearestDesc.minFilter = 9728;
    VkSampler sampler = fixture->cache->getOrCreateSampler(nearestDesc);
    REQUIRE(sampler != VK_NULL_HANDLE);
    rx::rhi::BindlessHandle samplerHandle = fixture->bindless.registerSampler(sampler);
    REQUIRE(samplerHandle.isValid());

    auto readback = renderAndReadbackQuadrants(*fixture, record.bindlessIndex, samplerHandle.index(), /*lod=*/0.0F);
    REQUIRE(readback.has_value());

    // Same quadrant fixture pattern as basecolor_uastc.ktx2 (both encode
    // quadrant4x4.png): TL=red, TR=green, BL=blue, BR=yellow.
    CHECK(approxEqual(readback->topLeft, {255, 0, 0}, 2));
    CHECK(approxEqual(readback->topRight, {0, 255, 0}, 2));
    CHECK(approxEqual(readback->bottomLeft, {0, 0, 255}, 2));
    CHECK(approxEqual(readback->bottomRight, {255, 255, 0}, 2));

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("TextureCache GPU [IMPORTANT-2b]: a non-Basis KTX2 with a container-vs-role colorspace disagreement "
          "still fires the WARN, but its stored format is KEPT (no relabel) -- deliberately different from "
          "the Basis path's own free relabel (srgb_mislabeled_normal.ktx2's own test)") {
    auto fixture = makeFixture("rx_asset_tc_nonbasis_mislabel");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    LogCapture capture;
    // raw_srgb_mislabeled_normal.ktx2 [generate_fixtures.sh]: content is
    // linear normal-map data (normal_flat.png), container transfer
    // function force-assigned sRGB, NO --encode (non-Basis, KHR_DF_MODEL_
    // RGBSDA -- verified directly via ktxinfo at generation time, so
    // ktxTexture2_NeedsTranscoding() reports false for this fixture).
    TextureHandle handle = fixture->cache->load(fixturePath("raw_srgb_mislabeled_normal.ktx2"), TextureRole::Normal);
    REQUIRE(handle.isValid());
    const TextureRecord& record = fixture->cache->resolve(handle);
    REQUIRE_FALSE(record.isFallback);

    // The WARN fires -- role (Normal, linear) disagrees with the
    // container's own forced sRGB claim.
    CHECK(capture.count("disagrees with the role's") == 1);

    // NO RELABEL: the non-Basis path keeps the container's own stored
    // format verbatim (VK_FORMAT_R8G8B8A8_SRGB, verified directly via
    // ktxinfo at fixture-generation time) -- NEVER coerced to a UNORM
    // variant the way the Basis path would (D10/gate ruling #3's
    // "relabeling is scoped to the Basis-transcoded path only" scope
    // decision, texture_cache.cpp's own comment on loadKtx2Bytes).
    CHECK(record.format == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(record.width == 8);
    CHECK(record.height == 8);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Fix round 1, reviewer minor 4.5: direct one-log-per-asset dedup ====

TEST_CASE("TextureCache [minor 4.5]: loading the SAME failing asset twice logs the failure WARN/ERROR exactly "
          "ONCE, not once per call -- D11's own 'no per-frame spam' dedup criterion, proven directly against "
          "captured log output (not just the D24/accounting side effects other tests already exercise)") {
    auto fixture = makeFixture("rx_asset_tc_logonce");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    LogCapture capture;
    TextureHandle first = fixture->cache->load(fixturePath("corrupt.ktx2"), TextureRole::BaseColor);
    TextureHandle second = fixture->cache->load(fixturePath("corrupt.ktx2"), TextureRole::BaseColor);
    CHECK(first == fixture->cache->checkerboardHandle());
    CHECK(second == fixture->cache->checkerboardHandle());

    // Exactly one "failed to load" line for this exact asset path, across
    // BOTH calls -- the second call's failure is silent (D11 dedup), not
    // merely coincidentally deduped by some OTHER mechanism.
    CHECK(capture.count("failed to load") == 1);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== [closure-sweep item 4] combined glTF -> TextureCache -> pixel =====
//
// Task 14 minor 4.6: role inference via glTF import (import_gltf_basisu_
// test.cpp) and pixel-correct rendering of a TextureCache-resident texture
// (the quadrant GPU test above) were each proven in ISOLATION -- no single
// test proved the full path glTF-file -> Registry::importGltf(with a real
// TextureCache) -> resolved bindless texture -> actually-sampled pixels.
// This closes that gap by combining, unmodified, both existing pieces of
// machinery already in THIS binary: import_gltf_basisu_test.cpp's own
// Registry::importGltf(pool, scheduler, textures) call shape (duplicated
// here per this codebase's own established per-file fixture convention --
// see e.g. LogCapture above) feeding directly into this file's own
// buildQuadPipeline()/renderAndReadbackQuadrants() rendering path, with
// zero new rendering/readback machinery. cube_basisu.gltf [Task 14
// vendoring, KHR_texture_basisu] references cube_basisu_misleading_normal
// .ktx2 as its material's baseColorTexture -- a real, already-committed
// fixture, deliberately misleadingly named ("_normal") but used as
// baseColor, and (empirically confirmed via a scratch probe run of this
// exact call chain, since no generator script records this fixture's
// content) quadrant-colored with the SAME TL=red/TR=green/BL=blue/
// BR=yellow layout the KTX2-only quadrant fixture above uses.
TEST_CASE("Combined path: cube_basisu.gltf -> Registry::importGltf(TextureCache) -> resolved bindless "
          "texture -> actual rendered pixels match the fixture's real quadrant content [Task 14 minor 4.6]") {
    auto fixture = makeFixture("rx_asset_tc_gltf_pixel");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    // GeometryPool/Scheduler: the two extra dependencies Registry::
    // importGltf() needs beyond what makeFixture()/makeCache() already
    // built -- built AFTER fixture settles into its final address, same
    // discipline as makeCache() itself (this file's own header comment).
    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader);
    REQUIRE(pool != nullptr);
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);

    rx::asset::Registry registry;
    rx::asset::ImportResult result =
        registry.importGltf(gltfFixturePath("cube_basisu.gltf"), *pool, *scheduler, fixture->cache.get());
    REQUIRE(result.ok());
    REQUIRE(result.materials.size() == 1);

    const rx::asset::MaterialAsset& material = registry.material(result.materials[0]);
    REQUIRE(material.baseColorTexture.present);
    const TextureRecord& record = fixture->cache->resolve(material.baseColorTexture.handle);
    // Proves the full path actually resolved to a REAL texture, not a D11
    // fallback silently substituted somewhere along the way -- the same
    // discriminating check import_gltf_basisu_test.cpp's own role-
    // inference test makes, extended here all the way through to pixels.
    REQUIRE_FALSE(record.isFallback);
    CHECK(record.width == 4);
    CHECK(record.height == 4);

    SamplerDesc nearestDesc;
    nearestDesc.magFilter = 9728;  // NEAREST -- exact texel reads, no filtering ambiguity
    nearestDesc.minFilter = 9728;
    VkSampler sampler = fixture->cache->getOrCreateSampler(nearestDesc);
    REQUIRE(sampler != VK_NULL_HANDLE);
    rx::rhi::BindlessHandle samplerHandle = fixture->bindless.registerSampler(sampler);
    REQUIRE(samplerHandle.isValid());

    auto readback = renderAndReadbackQuadrants(*fixture, record.bindlessIndex, samplerHandle.index(), /*lod=*/0.0F);
    REQUIRE(readback.has_value());

    // Same TL=red/TR=green/BL=blue/BR=yellow layout, same zero-tolerance-
    // appropriate primary-color reasoning as the quadrant GPU test above
    // (empirically confirmed for THIS fixture via a scratch probe run of
    // this exact call chain: 253/0/0, 0/253/0, 0/0/253, 255/255/0 --
    // within tolerance 2 of each primary-color corner).
    CHECK(approxEqual(readback->topLeft, {255, 0, 0}, 2));
    CHECK(approxEqual(readback->topRight, {0, 255, 0}, 2));
    CHECK(approxEqual(readback->bottomLeft, {0, 0, 255}, 2));
    CHECK(approxEqual(readback->bottomRight, {255, 255, 0}, 2));

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== [helmet-texture-fix investigation] Slot-swap discrimination =========
//
// The us-vs-us gap this closes: every existing GPU test in this binary
// (the "Combined path" test just above included) proves ONE material
// slot resolves to a REAL, non-fallback, correctly-shaped texture -- none
// of them prove that FIVE slots resolved SIMULTANEOUSLY each carry ONLY
// their OWN slot's content, distinct from every other slot's. A bug that
// swaps which decoded image lands in which MaterialAsset::TextureRef
// field (e.g. textureRefForSlot() misrouting a role, or a fillRef() call
// site in import_gltf.cpp being wired to the wrong TextureRole/slot pair)
// would still pass every existing single-slot test -- each slot would
// still resolve to SOME real, correctly-shaped, non-fallback texture,
// just the WRONG one -- and would still bake identically into a
// self-generated D17 reference PNG (the exact blind spot the helmet
// texture investigation's own report documents). This test's fixture
// (cube_slot_swap_probe.gltf, assets/test/textures/generate_fixtures.sh)
// binds all five StandardPBR texture slots on ONE material to five
// mutually distinct, pure primary/secondary colors -- baseColor=red,
// metallicRoughness=green, normal=blue, occlusion=magenta,
// emissive=cyan -- so a swap between any two slots is not just wrong, it
// reads back as a DIFFERENT, unmistakable, already-known color (the
// sibling slot's own), never a subtle shift a tolerance band could
// absorb either way.
//
// REVERT PROOF [documented here, not committed as scaffolding]: this
// test was confirmed to actually discriminate by temporarily swapping
// the baseColorTexture/metallicRoughnessTexture `index` values in
// cube_slot_swap_probe.gltf's own material block (0<->1, the exact
// baseColor<->metallicRoughness pair the helmet bug report's own
// green-panel symptom points at) in a scratch git worktree, re-running
// this exact TEST_CASE, and observing it FAIL (baseColorTexture read back
// green, not red) before reverting -- see the task's own report for the
// full transcript.
TEST_CASE("Slot-swap discrimination: five distinct StandardPBR texture slots on ONE material import to FIVE "
          "correctly-distinct textures -- baseColor/metallicRoughness/normal/occlusion/emissive never "
          "cross-contaminate, and swapping any two slot indices is provably caught by this same test "
          "[helmet-texture-fix verification gap]") {
    auto fixture = makeFixture("rx_asset_tc_slot_swap");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader);
    REQUIRE(pool != nullptr);
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);

    rx::asset::Registry registry;
    rx::asset::ImportResult result =
        registry.importGltf(gltfFixturePath("cube_slot_swap_probe.gltf"), *pool, *scheduler, fixture->cache.get());
    REQUIRE(result.ok());
    REQUIRE(result.materials.size() == 1);

    const rx::asset::MaterialAsset& material = registry.material(result.materials[0]);
    REQUIRE(material.baseColorTexture.present);
    REQUIRE(material.metallicRoughnessTexture.present);
    REQUIRE(material.normalTexture.present);
    REQUIRE(material.occlusionTexture.present);
    REQUIRE(material.emissiveTexture.present);

    SamplerDesc nearestDesc;
    nearestDesc.magFilter = 9728;  // NEAREST -- exact texel reads, no filtering ambiguity.
    nearestDesc.minFilter = 9728;
    VkSampler sampler = fixture->cache->getOrCreateSampler(nearestDesc);
    REQUIRE(sampler != VK_NULL_HANDLE);
    rx::rhi::BindlessHandle samplerHandle = fixture->bindless.registerSampler(sampler);
    REQUIRE(samplerHandle.isValid());

    // One (name, TextureRef, expected color) tuple per slot -- the
    // per-slot loop below asserts BOTH halves of the discrimination:
    // reads back as its OWN color (the "correct" half) AND does not read
    // back as any sibling's color (the "a swap would be caught" half,
    // made explicit rather than merely implied by the first half
    // passing).
    struct SlotProbe {
        const char* name;
        const rx::asset::TextureRef* ref;
        std::array<uint8_t, 3> expected;
    };
    const std::array<uint8_t, 3> kRed{255, 0, 0};
    const std::array<uint8_t, 3> kGreen{0, 255, 0};
    const std::array<uint8_t, 3> kBlue{0, 0, 255};
    const std::array<uint8_t, 3> kMagenta{255, 0, 255};
    const std::array<uint8_t, 3> kCyan{0, 255, 255};
    const std::array<SlotProbe, 5> probes{{
        {"baseColor", &material.baseColorTexture, kRed},
        {"metallicRoughness", &material.metallicRoughnessTexture, kGreen},
        {"normal", &material.normalTexture, kBlue},
        {"occlusion", &material.occlusionTexture, kMagenta},
        {"emissive", &material.emissiveTexture, kCyan},
    }};
    const std::array<std::array<uint8_t, 3>, 5> kAllColors{kRed, kGreen, kBlue, kMagenta, kCyan};

    for (const SlotProbe& probe : probes) {
        CAPTURE(probe.name);
        const TextureRecord& record = fixture->cache->resolve(probe.ref->handle);
        REQUIRE_FALSE(record.isFallback);  // a real, distinct texture was actually registered for this slot.

        auto readback = renderAndReadbackQuadrants(*fixture, record.bindlessIndex, samplerHandle.index(), /*lod=*/0.0F);
        REQUIRE(readback.has_value());

        // The "correct" half: this slot's own color, everywhere (the
        // fixture is flat -- all four quadrant corners read identically).
        CHECK(approxEqual(readback->topLeft, probe.expected, 2));
        CHECK(approxEqual(readback->topRight, probe.expected, 2));
        CHECK(approxEqual(readback->bottomLeft, probe.expected, 2));
        CHECK(approxEqual(readback->bottomRight, probe.expected, 2));

        // The "a swap would be caught" half: NOT any sibling slot's own
        // color. A material-resolution bug that swapped this slot's
        // handle with a DIFFERENT slot's would make exactly one of these
        // five probes' "correct" half above fail, reading back as the
        // wrong-but-already-known sibling color instead -- these checks
        // make that failure mode explicit rather than merely implied.
        for (const std::array<uint8_t, 3>& other : kAllColors) {
            if (other == probe.expected) {
                continue;
            }
            CHECK_FALSE(approxEqual(readback->topLeft, other, 2));
        }
    }

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Fix round, sampler-wrap P0 -- the missing regression class the helmet
// investigation named] `sampler_wrap_probe.gltf` (assets/test/, new,
// committed) is a single quad whose ONE material's baseColorTexture
// references `textures/quadrant.png` (already-committed, 8x8, TL=red/
// TR=green/BL=blue/BR=yellow -- see this file's own generate_fixtures.sh)
// through an EXPLICIT-BUT-EMPTY glTF sampler object (`"samplers": [{}]`,
// referenced by `"textures":[{"source":0,"sampler":0}]`) -- byte-for-byte
// the SAME shape DamagedHelmet's own real glTF file uses (`samplers:
// [{}]`, every one of its 5 textures referencing that one empty sampler),
// which fastgltf/this importer resolve to glTF's documented "sampler
// unspecified" default: REPEAT wrap, auto (LINEAR) filtering.
//
// This exercises the REAL, full production path the original helmet
// investigation's own probes never touched (its own UV-gradient/color-
// mean checks were structurally blind at the sampler layer -- see this
// task's own report addendum): import_gltf.cpp's fillRef() parses this
// texture's glTF sampler into `TextureRef::sampler`
// (mesh_asset.h's SamplerDesc) -> THIS test resolves it through
// TextureCache::getOrCreateSamplerBindlessIndex() (the fix round's own new
// method -- the exact one samples/08_gltf_viewer/main.cpp's own
// resolveSamplerIndex() calls in production) -> a real bindless
// Texture2D/SamplerState pair is sampled by a real GPU shader.
//
// DISCRIMINATION: the probe quad's own UV is deliberately OUTSIDE [0,1] in
// V (constant per draw, matching DamagedHelmet's own "V wholly outside
// [0,1]" shape exactly, not just "some UVs wrap"), landing EXACTLY on a
// quadrant.png texel CENTER post-wrap so the assertion is filter-mode-
// independent (no bilinear-boundary ambiguity, no reliance on any specific
// interpolated screen fraction): V=1.3125 -- under REPEAT, frac(1.3125) =
// 0.3125, the exact center of texel row 2 (quadrant.png's TOP/red half,
// rows 0-3) -- expects RED. Under CLAMP_TO_EDGE (a re-hardcoded-clamp
// revert), V=1.3125 clamps to exactly 1.0 -- beyond every real texel
// center, so CLAMP_TO_EDGE deterministically reads the LAST row's texel
// (row 7, quadrant.png's BOTTOM/blue half) -- expects BLUE instead. This
// is the discriminating probe: RED-under-fix vs. BLUE-under-bug, the exact
// signature class the real DamagedHelmet defect had (the whole mesh
// reading its texture's bottom edge row). A second probe (V=1.6875, wraps
// to row 5's own texel center, BLUE either way) is a non-discriminating
// sanity check only -- proves the texture/pipeline are not simply
// degenerate, not proof of the fix on its own.
TEST_CASE("Sampler-wrap regression: glTF-default REPEAT (samplers:[{}], DamagedHelmet's own shape) resolved through "
          "TextureCache::getOrCreateSamplerBindlessIndex() and sampled through the FULL import path -- UVs outside "
          "[0,1] read the WRAPPED texel, not the CLAMP_TO_EDGE bottom-edge row [helmet sampler-wrap fix, missing "
          "regression class]") {
    auto fixture = makeFixture("rx_asset_tc_sampler_wrap");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader);
    REQUIRE(pool != nullptr);
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);

    rx::asset::Registry registry;
    rx::asset::ImportResult result =
        registry.importGltf(gltfFixturePath("sampler_wrap_probe.gltf"), *pool, *scheduler, fixture->cache.get());
    REQUIRE(result.ok());
    REQUIRE(result.materials.size() == 1);

    const rx::asset::MaterialAsset& material = registry.material(result.materials[0]);
    REQUIRE(material.baseColorTexture.present);
    const TextureRecord& record = fixture->cache->resolve(material.baseColorTexture.handle);
    REQUIRE_FALSE(record.isFallback);

    // `material.baseColorTexture.sampler` is import_gltf.cpp's own parsed
    // glTF sampler -- for this fixture's empty `samplers:[{}]` object, that
    // is `SamplerDesc`'s own default-constructed values (mesh_asset.h),
    // which the fixture's OWN JSON comment (and this TEST_CASE's own
    // header comment) already establishes ARE glTF's documented "sampler
    // unspecified" default (REPEAT/auto) -- asserted here directly so a
    // FUTURE regression in fillRef()'s own sampler-parsing (import_gltf.cpp
    // :869-874) that started leaving `wrapS`/`wrapT` at some OTHER,
    // non-REPEAT value would be caught structurally, not just visually.
    CHECK(material.baseColorTexture.sampler.wrapS == 10497);  // glTF Wrap::Repeat.
    CHECK(material.baseColorTexture.sampler.wrapT == 10497);

    // THE fix under test: resolve the REAL per-texture bindless sampler
    // index through TextureCache -- exactly samples/08_gltf_viewer/
    // main.cpp's own resolveSamplerIndex() call.
    std::optional<uint32_t> samplerIndex =
        fixture->cache->getOrCreateSamplerBindlessIndex(material.baseColorTexture.sampler);
    REQUIRE(samplerIndex.has_value());

    // Two CONSTANT-UV quads (every vertex shares the identical (u,v) --
    // removes any dependency on interpolation/rasterization precision at a
    // specific screen fraction; the sampled texel is the same at every
    // point on the quad by construction) -- see this TEST_CASE's own
    // header comment for the exact texel-center arithmetic.
    constexpr std::array<QuadVertex, 4> kRepeatRowTop{{
        {{-1.0F, -1.0F, 0.0F}, {0.3125F, 1.3125F}},
        {{1.0F, -1.0F, 0.0F}, {0.3125F, 1.3125F}},
        {{1.0F, 1.0F, 0.0F}, {0.3125F, 1.3125F}},
        {{-1.0F, 1.0F, 0.0F}, {0.3125F, 1.3125F}},
    }};
    constexpr std::array<QuadVertex, 4> kRepeatRowBottom{{
        {{-1.0F, -1.0F, 0.0F}, {0.3125F, 1.6875F}},
        {{1.0F, -1.0F, 0.0F}, {0.3125F, 1.6875F}},
        {{1.0F, 1.0F, 0.0F}, {0.3125F, 1.6875F}},
        {{-1.0F, 1.0F, 0.0F}, {0.3125F, 1.6875F}},
    }};

    auto topReadback = renderAndReadbackQuadrants(*fixture, record.bindlessIndex, *samplerIndex, /*lod=*/0.0F,
                                                    /*extent=*/64, kRepeatRowTop);
    REQUIRE(topReadback.has_value());
    auto bottomReadback = renderAndReadbackQuadrants(*fixture, record.bindlessIndex, *samplerIndex, /*lod=*/0.0F,
                                                       /*extent=*/64, kRepeatRowBottom);
    REQUIRE(bottomReadback.has_value());

    // THE discriminating assertion: V=1.3125 (wraps to row 2, the top/red
    // half) must read RED, not the BLUE a CLAMP_TO_EDGE revert would
    // produce (V clamps to 1.0, reading the bottom/blue edge row instead --
    // this is byte-for-byte the same failure SHAPE the real DamagedHelmet
    // defect had). All four corners assert identically -- the quad's UV is
    // constant, so a correct implementation reads the SAME texel
    // everywhere.
    const std::array<uint8_t, 3> kRed{255, 0, 0};
    const std::array<uint8_t, 3> kBlue{0, 0, 255};
    CHECK(approxEqual(topReadback->topLeft, kRed, 2));
    CHECK(approxEqual(topReadback->topRight, kRed, 2));
    CHECK(approxEqual(topReadback->bottomLeft, kRed, 2));
    CHECK(approxEqual(topReadback->bottomRight, kRed, 2));
    // Explicit negative half, mirroring the slot-swap TEST_CASE's own
    // "make the failure mode explicit" discipline above.
    CHECK_FALSE(approxEqual(topReadback->topLeft, kBlue, 2));

    // Sanity probe (non-discriminating alone -- both fix and bug read BLUE
    // here, since V=1.6875 already wraps to the SAME row a CLAMP_TO_EDGE
    // revert would also land on): proves the texture/pipeline are real and
    // not degenerate, and that BOTH quadrant.png halves are genuinely
    // reachable through this exact code path.
    CHECK(approxEqual(bottomReadback->topLeft, kBlue, 2));
    CHECK(approxEqual(bottomReadback->bottomRight, kBlue, 2));

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Oversized-texture staging [texture-path round, item B] =============
//
// Prior defect (found on the real Workshop asset, real NVIDIA): a texture
// whose mip-0 bytes exceed the Uploader's staging-ring capacity (a
// 4096x4096 RGBA8 mip 0 is 64MB, over the 16MiB default ring) was
// REJECTED by Uploader::uploadImageMips() and silently replaced by the
// D11 checkerboard fallback -- size was, incorrectly, a fallback reason.
// `oversized_quadrant.png` (assets/test/textures/generate_fixtures.sh's
// own comment) is the SAME quadrant4x4.png source nearest-neighbor-
// upscaled to 4096x4096 -- real quadrant content, not a degenerate flat
// fixture, so a chunk landing at the wrong subresource offset would be
// visible as a wrong quadrant color, not just "some pixels came back
// non-black".
TEST_CASE("TextureCache GPU [texture-path round, item B]: a 4096x4096 stb PNG (64MB mip 0, exceeding the "
          "16MiB default staging ring) imports through the FULL Registry::importGltf path to a real, "
          "non-fallback texture with its complete runtime-generated mip chain (item A composed with item B), "
          "and its 4 quadrant colors render correctly -- proving Uploader::uploadImageMips()'s chunked-row "
          "staging path end to end, not just format bookkeeping") {
    auto fixture = makeFixture("rx_asset_tc_oversized");
    if (!fixture.has_value()) {
        return;
    }
    makeCache(*fixture);

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader);
    REQUIRE(pool != nullptr);
    auto scheduler = rx::task::Scheduler::create(2);
    REQUIRE(scheduler != nullptr);

    rx::asset::Registry registry;
    rx::asset::ImportResult result = registry.importGltf(gltfFixturePath("oversized_texture_probe.gltf"), *pool,
                                                            *scheduler, fixture->cache.get());
    REQUIRE(result.ok());
    REQUIRE(result.materials.size() == 1);

    const rx::asset::MaterialAsset& material = registry.material(result.materials[0]);
    REQUIRE(material.baseColorTexture.present);
    const TextureRecord& record = fixture->cache->resolve(material.baseColorTexture.handle);
    // THE headline assertion: NOT the checkerboard/fallback the pre-fix
    // behavior silently substituted for any texture whose mip-0 bytes
    // exceeded the staging cap.
    REQUIRE_FALSE(record.isFallback);
    CHECK(record.width == 4096);
    CHECK(record.height == 4096);
    // floor(log2(4096))+1 == 13 -- the full runtime-generated chain
    // (texture-path round item A), composed with item B's own chunked
    // upload for mip 0 (64MB, chunked) and mip 1 (2048x2048 == exactly
    // 16MiB, still a single-trip level) alike -- "works composed with
    // item A" per this round's own brief.
    CHECK(record.mipLevels == 13);

    std::optional<uint32_t> samplerIndex =
        fixture->cache->getOrCreateSamplerBindlessIndex(material.baseColorTexture.sampler);
    REQUIRE(samplerIndex.has_value());

    auto readback = renderAndReadbackQuadrants(*fixture, record.bindlessIndex, *samplerIndex, /*lod=*/0.0F);
    REQUIRE(readback.has_value());

    // Same TL=red/TR=green/BL=blue/BR=yellow quadrant layout as every
    // other quadrant fixture in this file (oversized_quadrant.png is the
    // SAME quadrant4x4.png source, just nearest-neighbor-upscaled).
    CHECK(approxEqual(readback->topLeft, {255, 0, 0}, 2));
    CHECK(approxEqual(readback->topRight, {0, 255, 0}, 2));
    CHECK(approxEqual(readback->bottomLeft, {0, 0, 255}, 2));
    CHECK(approxEqual(readback->bottomRight, {255, 255, 0}, 2));

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
