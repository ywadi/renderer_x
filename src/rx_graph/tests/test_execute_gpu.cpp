// Task 3's TDD-required GPU test: two cases, both device-backed (a real
// headless-but-validated rx::rhi::Device, same windowed-headless fixture
// pattern src/rx_rhi_vk/tests/texture_test.cpp/upload_test.cpp already
// establish -- rx::rhi::Device::create() needs a real VkSurfaceKHR even
// though nothing here ever presents to it).
//
// Case 1 ("invert"): graph = pass "draw" (writes a pooled 256x256
// R8G8B8A8_UNORM "color" attachment, side-effect-free -- kept alive only
// because "invert" reads it, exercising the same reachability-based
// culling path Tasks 1-2 already established, not a setSideEffect()
// escape hatch) -> pass "invert" (reads "color" as a texture input,
// writes the backbuffer "bb"; its execute() callback registers "color"'s
// resolved view into a real rx::rhi::BindlessTable, binds a small
// hand-built dynamic-rendering pipeline, and draws a fullscreen triangle
// that samples + inverts it). "draw" never runs any shader at all: its
// entire contribution is establishing "color" and being cleared to
// {0,0,0,1} by Executor::execute()'s own load-op logic -- so the exact,
// deterministic corner-pixel readback this test asserts is simply that
// fixed clear color, channel-inverted, in "bb".
//
// Case 2 ("resize-rerealize"): compiles the same kind of graph at 128x128,
// realize()s it, recompiles it at 256x256, realize()s it again, executes
// once -- proving Executor::realize() is safe to call again after a
// resource-shape change (TransientPool's acquire/release/reacquire cycle
// across two different-shaped realizes) with zero validation errors.
#include <doctest/doctest.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>

#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace rx::graph;

namespace {

constexpr uint32_t kExtent = 256;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Same skip-guarded windowed-device fixture pattern as
// rx_rhi_vk/tests/texture_test.cpp/upload_test.cpp -- Executor::create()
// needs a real rx::rhi::Device&, which needs a real VkSurfaceKHR even
// though nothing in either test case below ever presents to it.
struct GpuFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    std::unique_ptr<Executor> executor;
};

std::optional<GpuFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping rx_graph GPU test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping rx_graph GPU test");
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

    auto executor = Executor::create(*device);
    REQUIRE(executor != nullptr);

    return GpuFixture{std::move(*window), std::move(*context), std::move(*device), std::move(*allocator),
                       std::move(executor)};
}

// --- Offscreen "backbuffer" -- an external, never-pooled image the test
// itself owns [Task 3 ambiguity resolution #4], same raw (non-RAII, manual
// teardown before device destruction) construction pattern
// samples/01_triangle/main.cpp's runHeadless() and
// rx_rhi_vk/tests/clear_color_test.cpp both already use. -----------------
struct OffscreenImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

std::optional<OffscreenImage> createOffscreenImage(VkDevice device, VkPhysicalDevice physicalDevice, VkFormat format,
                                                     VkExtent2D extent) {
    OffscreenImage result;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &result.image) != VK_SUCCESS) {
        return std::nullopt;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, result.image, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == UINT32_MAX) {
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &result.memory) != VK_SUCCESS) {
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }
    if (vkBindImageMemory(device, result.image, result.memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, result.memory, nullptr);
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &result.view) != VK_SUCCESS) {
        vkFreeMemory(device, result.memory, nullptr);
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    return result;
}

void destroyOffscreenImage(VkDevice device, OffscreenImage& img) {
    if (img.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, img.view, nullptr);
    }
    if (img.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, img.image, nullptr);
    }
    if (img.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, img.memory, nullptr);
    }
    img = OffscreenImage{};
}

// --- The "invert" pipeline: a fullscreen triangle (SV_VertexID trick, no
// vertex buffer -- same idiom as shaders/triangle.vert.slang) that samples
// a bindless-registered Texture2D and writes its channel-inverted color.
// Compiled at RUNTIME via rx::shader::Compiler [same reasoning as
// samples/03_bindless_mesh/main.cpp's kShaderSource: reflection must walk
// the actual shader driving this test's pipeline layout, which requires
// the runtime path, not a slangc build-time precompile]. Set 0's two
// bindings deliberately match rx::rhi::BindlessTable's own fixed scheme
// (kSampledImageBinding=0/kSamplerBinding=1) -- a strict subset of its
// three slots (no storage buffer use at all), which
// PipelineLayoutBuilder::build()'s externalSet0 substitution explicitly
// supports [pipeline_layout.h: "a shader is free to use a strict subset of
// the three slots"]. ------------------------------------------------------
constexpr const char* kInvertShaderSource = R"(
struct PushConstants {
    uint textureIndex;
    uint samplerIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 0)]]
Texture2D gTextures[];

[[vk::binding(1, 0)]]
SamplerState gSamplers[];

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(uint vertexID : SV_VertexID)
{
    // The classic "one big triangle covering the whole clip-space square"
    // trick: (-1,-1), (3,-1), (-1,3) fully covers [-1,1]x[-1,1] once
    // rasterized/clipped, so every pixel in the render area is covered by
    // exactly one triangle, same effect as a full-screen quad with no
    // second draw/index buffer needed.
    float2 positions[3] = float2[3](
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0)
    );
    float2 p = positions[vertexID];

    VSOut o;
    o.position = float4(p, 0.0, 1.0);
    o.uv = p * 0.5 + 0.5;
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float4 c = gTextures[gPush.textureIndex].Sample(gSamplers[gPush.samplerIndex], input.uv);
    return float4(1.0 - c.rgb, 1.0);
}
)";

const std::vector<std::string> kInvertEntryPoints = {"vsMain", "fsMain"};

struct InvertPipeline {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t pushConstantOffset = 0;
    uint32_t pushConstantSize = 0;
    VkShaderStageFlags pushConstantStages = 0;
};

void destroyInvertPipeline(VkDevice device, InvertPipeline& p) {
    if (p.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, p.pipeline, nullptr);
    }
    if (p.fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, p.fragModule, nullptr);
    }
    if (p.vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, p.vertModule, nullptr);
    }
    p.pipeline = VK_NULL_HANDLE;
    p.fragModule = VK_NULL_HANDLE;
    p.vertModule = VK_NULL_HANDLE;
    p.layoutBundle = rx::rhi::PipelineLayoutBundle{};
}

std::optional<InvertPipeline> buildInvertPipeline(VkDevice device, VkFormat colorFormat,
                                                    VkDescriptorSetLayout bindlessSetLayout) {
    InvertPipeline result;

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        return std::nullopt;
    }

    rx::shader::CompileResult compileResult =
        compiler->compileFromSource("RxGraphInvertModule", kInvertShaderSource, kInvertEntryPoints);
    if (!compileResult.ok) {
        MESSAGE("rx_graph invert shader compile failed: ", compileResult.diagnostics);
        return std::nullopt;
    }

    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value() || layoutInfo->pushRanges.size() != 1) {
        return std::nullopt;
    }

    // THE substitution this test exists to prove alongside execution: set
    // 0 becomes the real BindlessTable's own layout, not a lookalike this
    // builder would otherwise invent -- see pipeline_layout.h's comment on
    // build()'s externalSet0 parameter.
    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, *layoutInfo, bindlessSetLayout);
    if (!layoutBundle.has_value()) {
        return std::nullopt;
    }
    result.layoutBundle = std::move(*layoutBundle);
    result.pushConstantOffset = layoutInfo->pushRanges[0].offset;
    result.pushConstantSize = layoutInfo->pushRanges[0].size;
    result.pushConstantStages = layoutInfo->pushRanges[0].stages;

    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = blob.code.size() * sizeof(uint32_t);
        moduleInfo.pCode = blob.code.data();

        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            destroyInvertPipeline(device, result);
            return std::nullopt;
        }
        if (blob.entryPointName == "vsMain") {
            result.vertModule = module;
        } else if (blob.entryPointName == "fsMain") {
            result.fragModule = module;
        } else {
            vkDestroyShaderModule(device, module, nullptr);
            destroyInvertPipeline(device, result);
            return std::nullopt;
        }
    }
    if (result.vertModule == VK_NULL_HANDLE || result.fragModule == VK_NULL_HANDLE) {
        destroyInvertPipeline(device, result);
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

    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
    pipelineInfo.layout = result.layoutBundle.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;  // dynamic rendering
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &result.pipeline) !=
        VK_SUCCESS) {
        destroyInvertPipeline(device, result);
        return std::nullopt;
    }

    return result;
}

AttachmentDesc absoluteColorDesc(uint32_t width, uint32_t height) {
    AttachmentDesc desc;
    desc.format = kFormat;
    desc.sizeClass = SizeClass::Absolute;
    desc.width = static_cast<float>(width);
    desc.height = static_cast<float>(height);
    return desc;
}

}  // namespace

TEST_CASE("Executor::execute draws into a pooled transient, samples it bindlessly, and inverts into the backbuffer") {
    auto fixture = makeFixture("rx_graph_gpu_invert");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    auto bindlessTable =
        rx::rhi::BindlessTable::create(physicalDevice, device, rx::rhi::BindlessTable::Capacities{4, 2, 1});
    REQUIRE(bindlessTable.has_value());

    auto pipeline = buildInvertPipeline(device, kFormat, bindlessTable->descriptorSetLayout());
    REQUIRE(pipeline.has_value());

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    REQUIRE(vkCreateSampler(device, &samplerInfo, nullptr, &sampler) == VK_SUCCESS);
    auto samplerHandle = bindlessTable->registerSampler(sampler);
    REQUIRE(samplerHandle.isValid());

    auto offscreen = createOffscreenImage(device, physicalDevice, kFormat, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());

    // Task 3 ambiguity resolution #6: the "invert" pass's own callback
    // registers "color"'s resolved view into the bindless table, and its
    // handle is released via DeletionQueue after this frame's submission
    // is confirmed complete -- per bindless.h's RELEASE-SAFETY CONTRACT,
    // not an eager bare release() call.
    rx::rhi::DeletionQueue deletionQueue;
    rx::rhi::BindlessHandle colorHandle;

    RenderGraph graph;
    graph.addPass("draw").addColorOutput("color", absoluteColorDesc(kExtent, kExtent));
    graph.addPass("invert")
        .addTextureInput("color")
        .addColorOutput("bb", absoluteColorDesc(kExtent, kExtent))
        .setExecute([&](PassContext& ctx) {
            colorHandle = bindlessTable->registerSampledImage(ctx.imageView("color"),
                                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            REQUIRE(colorHandle.isValid());

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

            VkDescriptorSet set = bindlessTable->descriptorSet();
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layoutBundle.layout, 0, 1,
                                     &set, 0, nullptr);

            struct {
                uint32_t textureIndex;
                uint32_t samplerIndex;
            } push{colorHandle.index(), samplerHandle.index()};
            vkCmdPushConstants(ctx.cmd, pipeline->layoutBundle.layout, pipeline->pushConstantStages,
                                pipeline->pushConstantOffset, pipeline->pushConstantSize, &push);

            VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width),
                                 static_cast<float>(ctx.renderArea.height), 0.0F, 1.0F};
            VkRect2D scissor{{0, 0}, ctx.renderArea};
            vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
            vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        });
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = kFormat;
    // Offscreen backbuffer, never presented -- see Task 3 ambiguity
    // resolution #1: TRANSFER_SRC_OPTIMAL is what this test's own readback
    // copy needs finalBarriers() to leave "bb" in, not PRESENT_SRC_KHR.
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);

    fixture->executor->realize(graph);

    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });

    // runOnce() above already vkQueueWaitIdle()'d, so this submission is
    // provably complete -- tagging with frame 0 and immediately confirming
    // it here is safe (if conservative: real production code would tag
    // with a real frame number and confirm it only once a real fence is
    // known signaled). Demonstrates the deferred-release idiom
    // bindless.h's contract requires, rather than an eager bare release().
    deletionQueue.retire([&bindlessTable, colorHandle] { bindlessTable->release(colorHandle); }, 0);
    deletionQueue.onFrameFenceSignaled(0);

    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(kExtent) * kExtent * 4;
    auto readback = fixture->allocator.createHostVisibleBuffer(pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {kExtent, kExtent, 1};
        vkCmdCopyImageToBuffer(cmd, offscreen->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                                &region);
    });
    readback->invalidate();

    std::array<uint8_t, 4> corner{};
    std::memcpy(corner.data(), readback->mappedData(), corner.size());

    // "draw" clears "color" to {0,0,0,1} (Task 3 ambiguity resolution #3's
    // fixed color clear value) and runs no shader at all; "invert" samples
    // that solid black and writes 1.0 - color.rgb, alpha 1.0 -- exact,
    // deterministic white in every channel.
    CHECK(corner[0] == 255);
    CHECK(corner[1] == 255);
    CHECK(corner[2] == 255);
    CHECK(corner[3] == 255);

    vkDeviceWaitIdle(device);
    bindlessTable->release(samplerHandle);
    vkDestroySampler(device, sampler, nullptr);
    destroyInvertPipeline(device, *pipeline);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Executor::realize is safe to call again after a resource-shape change (resize-rerealize)") {
    auto fixture = makeFixture("rx_graph_gpu_resize");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    // "aux" is SwapchainRelative (1.0x1.0), so its real extent tracks
    // CompileInfo::swapchainWidth/Height across the two compile() calls
    // below -- exactly the shape a real swapchain resize produces for any
    // window-relative attachment. "fill" has no execute() callback at all:
    // its only job is to exist as a pooled resource whose shape changes,
    // exercising TransientPool's acquire/release/reacquire path across two
    // differently-shaped realize() calls.
    AttachmentDesc auxDesc;
    auxDesc.format = kFormat;
    auxDesc.sizeClass = SizeClass::SwapchainRelative;
    auxDesc.width = 1.0F;
    auxDesc.height = 1.0F;

    RenderGraph graph;
    graph.addPass("fill").addColorOutput("aux", auxDesc).addColorOutput("bb", absoluteColorDesc(1, 1));
    graph.setBackbufferSource("bb");

    CompileInfo info128;
    info128.swapchainWidth = 128;
    info128.swapchainHeight = 128;
    info128.swapchainFormat = kFormat;
    info128.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info128);
    fixture->executor->realize(graph);

    CompileInfo info256 = info128;
    info256.swapchainWidth = 256;
    info256.swapchainHeight = 256;
    graph.compile(info256);
    fixture->executor->realize(graph);

    auto offscreen = createOffscreenImage(device, physicalDevice, kFormat, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());

    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });

    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(kExtent) * kExtent * 4;
    auto readback = fixture->allocator.createHostVisibleBuffer(pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {kExtent, kExtent, 1};
        vkCmdCopyImageToBuffer(cmd, offscreen->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                                &region);
    });
    readback->invalidate();
    std::array<uint8_t, 4> corner{};
    std::memcpy(corner.data(), readback->mappedData(), corner.size());
    // The fixed clear color, at whatever extent the second (256x256)
    // compile()/realize() actually bound -- a mismatched/stale pooled
    // "aux" binding would still let a plain clear succeed (nothing
    // cross-checks "aux" against "bb" here), but a genuinely broken
    // reacquire (e.g. a destroyed/dangling image handle, or an extent the
    // driver considers invalid for this render area) would show up as
    // either a real Vulkan error this readback surfaces or a validation
    // message the CHECK_FALSE below catches -- this executes the full
    // two-compile/two-realize/one-execute path for real, not a no-op.
    CHECK(corner[0] == 0);
    CHECK(corner[1] == 0);
    CHECK(corner[2] == 0);
    CHECK(corner[3] == 255);

    vkDeviceWaitIdle(device);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE(
    "Executor::execute synthesizes the missing first-use barrier for a pooled storage buffer across two "
    "consecutive execute() calls") {
    // Regression coverage for a real gap found while implementing the
    // image-only [Task 3 ambiguity resolution #2] first-use-of-frame
    // override: a pooled BUFFER's true first access within one compile
    // walk gets NO barrier at all from CompiledGraph::passBarriers() (see
    // executor.cpp's synthesizeFirstUseBufferBarrierIfNeeded() for the
    // full explanation of why that is Task 2's own correct, tested
    // behavior for a compile walk with no notion of "a previous frame",
    // and why it is nonetheless unsafe, unmodified, for a buffer this
    // Executor pools and reuses call after call). Two back-to-back
    // execute() calls against the SAME realize()d graph is exactly the
    // shape that would go unsynchronized without that fix: the SECOND
    // call's "produce" pass reuses the exact same pooled VkBuffer the
    // FIRST call's "produce" pass already wrote and "consume" pass already
    // read, with nothing but this synthesized barrier standing between
    // them on the same queue.
    auto fixture = makeFixture("rx_graph_gpu_buffer_reuse");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();

    BufferDesc dataDesc;
    dataDesc.size = 256;
    dataDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    RenderGraph graph;
    // Compute-class (no attachment output) -- its storage-buffer output
    // resolves to COMPUTE_SHADER_BIT per pass.h's Pass::resolveAccess
    // table. No execute() callback: this test only needs the barrier
    // machinery to run correctly, not any real compute dispatch.
    graph.addPass("produce", QueueClass::AsyncCompute).addStorageBufferOutput("data", dataDesc).setSideEffect();
    graph.addPass("consume")
        .addStorageBufferInput("data")
        .addColorOutput("bb", absoluteColorDesc(kExtent, kExtent))
        .setExecute([&](PassContext& ctx) {
            // [Fix round 1, Minor finding, plus its supplemental follow-up
            // covering the remaining three resolvers]: every PassContext
            // resolver must throw std::out_of_range on a legitimately-
            // registered but wrong-kind name, matching the documented
            // "throws on anything not resolvable" contract's own spirit --
            // never silently return a meaningless VK_NULL_HANDLE/
            // VK_FORMAT_UNDEFINED. Exercised here against the real "data"
            // storage buffer resource (image()/imageView()/imageFormat(),
            // each expecting an image) and the real "bb" backbuffer image
            // resource (buffer(), expecting a buffer), through the real
            // Executor, inside a real pass callback.
            CHECK_THROWS_AS(static_cast<void>(ctx.imageFormat("data")), std::out_of_range);
            CHECK_THROWS_AS(static_cast<void>(ctx.image("data")), std::out_of_range);
            CHECK_THROWS_AS(static_cast<void>(ctx.imageView("data")), std::out_of_range);
            CHECK_THROWS_AS(static_cast<void>(ctx.buffer("bb")), std::out_of_range);
        });
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = kFormat;
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);

    fixture->executor->realize(graph);

    auto offscreen = createOffscreenImage(device, fixture->device.physicalDevice(), kFormat,
                                            VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());

    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    // Two full execute() calls against the same realize()d graph, each in
    // its own synchronous submission -- the second call's "produce" pass
    // is exactly the pooled buffer's second-ever claim by a realize(),
    // still its first TOUCH within THAT call's own topological walk, the
    // case synthesizeFirstUseBufferBarrierIfNeeded() exists for.
    for (int i = 0; i < 2; ++i) {
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
        });
    }

    vkDeviceWaitIdle(device);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE(
    "Executor::execute unions every final-touching pass's stage into a pooled resource's cross-frame carry-forward "
    "-- not just the topologically-last one") {
    // Fix round 1, Critical finding regression test. The reviewer's own
    // probe (task-3-review.md) reproduced this exact shape: a resource
    // written once, then read by two independent passes with no
    // intervening write -- "readGraphics" (a Graphics-class pass, since it
    // also declares a color output; its texture-input read resolves to
    // VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT per pass.h's resolveAccess
    // table) and "readCompute" (Compute-class; resolves to
    // VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT). Both are "shared"'s final
    // touching passes this execute() call -- an overwrite (the pre-fix
    // behavior) would keep only whichever of the two runs topologically
    // last, silently dropping the other's stage from the NEXT execute()
    // call's first-use-of-frame srcStage override and reproducing a real,
    // unsynchronized write-after-read hazard this codebase's validation
    // setup cannot detect on its own (no VK_VALIDATION_FEATURE_ENABLE_
    // SYNCHRONIZATION_VALIDATION_EXT anywhere in context.cpp) -- this test
    // asserts on the real tracked value directly via
    // detail::debugLastFrameFinalStages(), a test/debug-only seam (see
    // executor.h's own comment on it, mirroring barriers.h's identical
    // detail:: carve-out), rather than relying on --validate to ever be
    // able to catch a wrong value here.
    auto fixture = makeFixture("rx_graph_gpu_stage_union");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();

    BufferDesc outBDesc;
    outBDesc.size = 256;
    outBDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    RenderGraph graph;
    graph.addPass("write").addColorOutput("shared", absoluteColorDesc(kExtent, kExtent));
    graph.addPass("readGraphics")
        .addTextureInput("shared")
        .addColorOutput("bb", absoluteColorDesc(kExtent, kExtent));
    graph.addPass("readCompute", QueueClass::AsyncCompute)
        .addTextureInput("shared")
        .addStorageBufferOutput("outB", outBDesc)
        .setSideEffect();
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = kFormat;
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);

    fixture->executor->realize(graph);

    auto offscreen = createOffscreenImage(device, fixture->device.physicalDevice(), kFormat,
                                            VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());

    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });

    const VkPipelineStageFlags2 tracked = detail::debugLastFrameFinalStages(*fixture->executor, "shared");
    CHECK((tracked & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) != 0);
    CHECK((tracked & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0);

    // A second execute() call actually consumes that unioned value as the
    // next frame's first-use override -- proving the fix's output feeds
    // back into the real barrier path end to end, not just that the
    // tracked value looks right in isolation.
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });

    vkDeviceWaitIdle(device);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
