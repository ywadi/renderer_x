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

#include <rx_core/debug_checks.h>
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
#include <rx_task/scheduler.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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
//
// Phase 4 Task 7 [spec D4 amendment]: Executor::create() now also requires a
// real rx::task::Scheduler& (parallelism is the engine default -- there is
// no null-scheduler construction path). `scheduler` is declared BEFORE
// `executor` so it is destroyed AFTER it (members destruct in reverse
// declaration order) -- Executor::create()'s own doc comment requires the
// Scheduler to outlive the Executor built against it.
struct GpuFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    std::unique_ptr<rx::task::Scheduler> scheduler;
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

    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);

    auto executor = Executor::create(*device, *scheduler);
    REQUIRE(executor != nullptr);

    return GpuFixture{std::move(*window),    std::move(*context),  std::move(*device),
                       std::move(*allocator), std::move(scheduler), std::move(executor)};
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

// --- Phase 4 Task 1's history GPU test: the "fill" pipeline -- a
// fullscreen triangle (same SV_VertexID trick as kInvertShaderSource
// above) that writes a push-constant solid color, with NO texture/
// descriptor-set dependency at all (unlike the invert pipeline, which
// needs a real BindlessTable to substitute at set 0). Used by the
// history-output-writing pass to paint a known, frame-varying pattern
// into "hist"'s write slot -- PipelineLayoutBuilder::build() is called
// with NO externalSet0 below precisely because this shader declares no
// descriptor sets at all for it to need substituting. -------------------
constexpr const char* kFillShaderSource = R"(
struct PushConstants {
    float4 color;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

struct VSOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vertexID : SV_VertexID)
{
    float2 positions[3] = float2[3](
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0)
    );
    VSOut o;
    o.position = float4(positions[vertexID], 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fsMain() : SV_Target
{
    return gPush.color;
}
)";

const std::vector<std::string> kFillEntryPoints = {"vsMain", "fsMain"};

struct FillPipeline {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t pushConstantOffset = 0;
    uint32_t pushConstantSize = 0;
    VkShaderStageFlags pushConstantStages = 0;
};

void destroyFillPipeline(VkDevice device, FillPipeline& p) {
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

std::optional<FillPipeline> buildFillPipeline(VkDevice device, VkFormat colorFormat) {
    FillPipeline result;

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        return std::nullopt;
    }

    rx::shader::CompileResult compileResult =
        compiler->compileFromSource("RxGraphFillModule", kFillShaderSource, kFillEntryPoints);
    if (!compileResult.ok) {
        MESSAGE("rx_graph fill shader compile failed: ", compileResult.diagnostics);
        return std::nullopt;
    }

    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value() || layoutInfo->pushRanges.size() != 1) {
        return std::nullopt;
    }

    // No externalSet0 -- this shader declares no descriptor sets at all
    // (see this block's own comment above).
    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, *layoutInfo);
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
            destroyFillPipeline(device, result);
            return std::nullopt;
        }
        if (blob.entryPointName == "vsMain") {
            result.vertModule = module;
        } else if (blob.entryPointName == "fsMain") {
            result.fragModule = module;
        } else {
            vkDestroyShaderModule(device, module, nullptr);
            destroyFillPipeline(device, result);
            return std::nullopt;
        }
    }
    if (result.vertModule == VK_NULL_HANDLE || result.fragModule == VK_NULL_HANDLE) {
        destroyFillPipeline(device, result);
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
        destroyFillPipeline(device, result);
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

TEST_CASE(
    "Phase 4 Task 1: a history resource's write slot survives into the NEXT frame's read, and frame 1's read is "
    "reported invalid") {
    // Pass "write_history" (setHistoryOutput("hist") only, no reader of
    // its own) writes a frame-varying solid color into "hist"'s write slot
    // via the "fill" pipeline; pass "read_history" (addHistoryInput("hist")
    // + addColorOutput("bb")) samples "hist" bindlessly through the SAME
    // "invert" pipeline/shader test_execute_gpu.cpp's very first TEST_CASE
    // above already builds and proves end to end -- reused verbatim here
    // rather than writing a third near-duplicate "sample and pass through"
    // shader, and its channel inversion is simply accounted for in this
    // test's own expected-pixel arithmetic below (black -> white,
    // red -> cyan) instead of removed.
    //
    // Ping-pong parity [design contract point 2]: execute() call N writes
    // slot (N % 2), reads slot ((N + 1) % 2) -- call 1 writes slot 1,
    // reads slot 0 (never written by any real pass -- only black-cleared
    // at creation [design contract point 3], so historyValid() must read
    // false and the sampled result must be exactly the black-clear
    // color, inverted = white); call 2 writes slot 0, reads slot 1 (=
    // call 1's write -- historyValid() must now read true, and the
    // sampled result must be exactly call 1's fill color, inverted).
    auto fixture = makeFixture("rx_graph_gpu_history");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    auto bindlessTable =
        rx::rhi::BindlessTable::create(physicalDevice, device, rx::rhi::BindlessTable::Capacities{4, 2, 1});
    REQUIRE(bindlessTable.has_value());

    auto fillPipeline = buildFillPipeline(device, kFormat);
    REQUIRE(fillPipeline.has_value());

    auto samplePipeline = buildInvertPipeline(device, kFormat, bindlessTable->descriptorSetLayout());
    REQUIRE(samplePipeline.has_value());

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

    rx::rhi::DeletionQueue deletionQueue;

    // Mutated by the test between execute() calls; captured by reference in
    // "write_history"'s callback below -- the SAME compiled/realized graph
    // is executed twice, exactly like the resize-rerealize and buffer-reuse
    // cases above already do.
    std::array<float, 4> currentFillColor{1.0F, 0.0F, 0.0F, 1.0F};  // frame 1: red
    bool observedHistoryValid = false;

    RenderGraph graph;
    graph.addPass("write_history").setHistoryOutput("hist", absoluteColorDesc(kExtent, kExtent)).setExecute(
        [&](PassContext& ctx) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fillPipeline->pipeline);
            vkCmdPushConstants(ctx.cmd, fillPipeline->layoutBundle.layout, fillPipeline->pushConstantStages,
                                fillPipeline->pushConstantOffset, fillPipeline->pushConstantSize,
                                currentFillColor.data());

            VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width),
                                 static_cast<float>(ctx.renderArea.height), 0.0F, 1.0F};
            VkRect2D scissor{{0, 0}, ctx.renderArea};
            vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
            vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        });

    rx::rhi::BindlessHandle histHandle;
    graph.addPass("read_history")
        .addHistoryInput("hist")
        .addColorOutput("bb", absoluteColorDesc(kExtent, kExtent))
        .setExecute([&](PassContext& ctx) {
            observedHistoryValid = ctx.historyValid("hist");

            histHandle =
                bindlessTable->registerSampledImage(ctx.imageView("hist"), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            REQUIRE(histHandle.isValid());

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, samplePipeline->pipeline);

            VkDescriptorSet set = bindlessTable->descriptorSet();
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, samplePipeline->layoutBundle.layout, 0,
                                     1, &set, 0, nullptr);

            struct {
                uint32_t textureIndex;
                uint32_t samplerIndex;
            } push{histHandle.index(), samplerHandle.index()};
            vkCmdPushConstants(ctx.cmd, samplePipeline->layoutBundle.layout, samplePipeline->pushConstantStages,
                                samplePipeline->pushConstantOffset, samplePipeline->pushConstantSize, &push);

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
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);

    fixture->executor->realize(graph);

    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    auto readBackCorner = [&]() -> std::array<uint8_t, 4> {
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
        return corner;
    };

    // ---- frame 1 (execute() call #1): writes slot 1, reads slot 0 (never
    // written -- only black-cleared at creation). historyValid() must be
    // false; the sampled result is the black clear color, inverted =
    // white. ---------------------------------------------------------------
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });
    deletionQueue.retire([&bindlessTable, histHandle] { bindlessTable->release(histHandle); }, 0);
    deletionQueue.onFrameFenceSignaled(0);

    CHECK_FALSE(observedHistoryValid);
    const std::array<uint8_t, 4> frame1Corner = readBackCorner();
    CHECK(frame1Corner[0] == 255);
    CHECK(frame1Corner[1] == 255);
    CHECK(frame1Corner[2] == 255);
    CHECK(frame1Corner[3] == 255);

    // ---- frame 2 (execute() call #2): writes slot 0 (a fresh green fill,
    // overwriting the slot frame 1 read from), reads slot 1 -- exactly
    // frame 1's red fill. historyValid() must now be true; the sampled
    // result is red, inverted = cyan -- proving frame 2's readback
    // contains frame 1's pattern, not frame 2's own (still-being-written)
    // one. --------------------------------------------------------------
    currentFillColor = {0.0F, 1.0F, 0.0F, 1.0F};
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });
    deletionQueue.retire([&bindlessTable, histHandle] { bindlessTable->release(histHandle); }, 1);
    deletionQueue.onFrameFenceSignaled(1);

    CHECK(observedHistoryValid);
    const std::array<uint8_t, 4> frame2Corner = readBackCorner();
    CHECK(frame2Corner[0] == 0);
    CHECK(frame2Corner[1] == 255);
    CHECK(frame2Corner[2] == 255);
    CHECK(frame2Corner[3] == 255);

    vkDeviceWaitIdle(device);
    bindlessTable->release(samplerHandle);
    vkDestroySampler(device, sampler, nullptr);
    destroyInvertPipeline(device, *samplePipeline);
    destroyFillPipeline(device, *fillPipeline);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// --- Phase 4 Task 7 [spec D4 amendment]: Pass::setExecuteChunked() TDD
// gates. Both cases below draw a 2x2 grid of 4 solid-color cells (the
// FillPipeline above, reused verbatim -- no bindless dependency, so it is
// the simplest pipeline this file already has to reuse) via SCISSOR-clipped
// draws of the same full-clip-space triangle, one cell per logical "item",
// distributed across chunks by (chunkIndex, chunkCount) -- the exact
// ceil-division slicing samples/05_multipass and samples/06_materials'
// migrated forward passes use for real objects.
namespace {

constexpr uint32_t kCellCount = 4;
constexpr std::array<std::array<float, 4>, kCellCount> kCellColors{{
    {1.0F, 0.0F, 0.0F, 1.0F},  // cell 0: red
    {0.0F, 1.0F, 0.0F, 1.0F},  // cell 1: green
    {0.0F, 0.0F, 1.0F, 1.0F},  // cell 2: blue
    {1.0F, 1.0F, 0.0F, 1.0F},  // cell 3: yellow
}};

// Records cell [begin, end) of the 2x2 grid onto `cmd` -- shared verbatim by
// both the chunked callback (one slice per chunk) and the whole-pass
// callback (one call covering all 4 cells at once), which is exactly what
// makes the two TEST_CASEs below a genuine apples-to-apples byte-identical
// comparison rather than two independently-written draw loops that merely
// happen to agree.
void recordCells(VkCommandBuffer cmd, const FillPipeline& pipeline, uint32_t extent, uint32_t begin, uint32_t end) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
    VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent), static_cast<float>(extent), 0.0F, 1.0F};
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    const uint32_t half = extent / 2;
    for (uint32_t cell = begin; cell < end; ++cell) {
        const uint32_t cx = cell % 2;
        const uint32_t cy = cell / 2;
        VkRect2D scissor{{static_cast<int32_t>(cx * half), static_cast<int32_t>(cy * half)}, {half, half}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdPushConstants(cmd, pipeline.layoutBundle.layout, pipeline.pushConstantStages,
                            pipeline.pushConstantOffset, pipeline.pushConstantSize, kCellColors[cell].data());
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
}

std::array<uint8_t, 4> readCellCenter(const std::array<uint8_t, 4>* pixels, uint32_t extent, uint32_t cell) {
    const uint32_t half = extent / 2;
    const uint32_t cx = cell % 2;
    const uint32_t cy = cell / 2;
    const uint32_t x = cx * half + half / 2;
    const uint32_t y = cy * half + half / 2;
    return pixels[static_cast<size_t>(y) * extent + x];
}

}  // namespace

TEST_CASE("Pass::setExecuteChunked records every chunk in parallel, drawing each cell exactly once, with zero "
          "validation errors, and its composited output is pixel-identical to the equivalent whole-pass recording") {
    auto fixture = makeFixture("rx_graph_gpu_chunked");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    auto fillPipeline = buildFillPipeline(device, kFormat);
    REQUIRE(fillPipeline.has_value());

    auto offscreenChunked = createOffscreenImage(device, physicalDevice, kFormat, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreenChunked.has_value());
    auto offscreenWhole = createOffscreenImage(device, physicalDevice, kFormat, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreenWhole.has_value());

    // How many times each cell index was actually drawn, summed across every
    // chunk (however many the executor ends up fanning out to) -- must land
    // on exactly 1 per cell, proving the ceil-division slicing below covers
    // every cell exactly once with no gaps or double-draws, regardless of
    // chunkCount. std::atomic: multiple chunks' own callbacks can run this
    // concurrently, on different threads.
    std::array<std::atomic<uint32_t>, kCellCount> drawCounts{};

    RenderGraph chunkedGraph;
    chunkedGraph.addPass("quad_fill")
        .addColorOutput("bb", absoluteColorDesc(kExtent, kExtent))
        .setExecuteChunked([&](PassContext& ctx, uint32_t chunkIndex, uint32_t chunkCount) {
            REQUIRE(chunkCount >= 1);
            const uint32_t cellsPerChunk = (kCellCount + chunkCount - 1) / chunkCount;
            const uint32_t begin = std::min(chunkIndex * cellsPerChunk, kCellCount);
            const uint32_t end = std::min(begin + cellsPerChunk, kCellCount);
            for (uint32_t cell = begin; cell < end; ++cell) {
                drawCounts[cell].fetch_add(1, std::memory_order_relaxed);
            }
            recordCells(ctx.chunkCommandBuffer(), *fillPipeline, kExtent, begin, end);
        });
    chunkedGraph.setBackbufferSource("bb");

    CompileInfo chunkedInfo;
    chunkedInfo.swapchainWidth = kExtent;
    chunkedInfo.swapchainHeight = kExtent;
    chunkedInfo.swapchainFormat = kFormat;
    chunkedInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    chunkedGraph.compile(chunkedInfo);
    fixture->executor->realize(chunkedGraph);

    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(chunkedGraph, cmd, offscreenChunked->image, offscreenChunked->view,
                                    VkExtent2D{kExtent, kExtent});
    });

    for (uint32_t cell = 0; cell < kCellCount; ++cell) {
        CAPTURE(cell);
        CHECK(drawCounts[cell].load() == 1);
    }

    // Chunk-count derivation is the one source of truth samples/07_stress's
    // own headless counter gate will assert against too -- see executor.h's
    // detail::chunkCountForWorkerCount() doc comment.
    const rx::graph::detail::ExecutorChunkDebugStats stats = rx::graph::detail::debugChunkStats(*fixture->executor);
    const uint32_t expectedChunkCount = rx::graph::detail::chunkCountForWorkerCount(fixture->scheduler->workerCount());
    CHECK(stats.lastChunkCount == expectedChunkCount);
    // Pool-allocation budget -- CORRECTED [fix round; see
    // samples/07_stress/main.cpp's own runHeadless() for the full account
    // of the bug this replaces]: "at most one secondary per (frameSlot,
    // threadIndex) pair" is WRONG -- enkiTS's work-stealing gives no
    // guarantee that `expectedChunkCount` chunks land on that many
    // DISTINCT threads; one thread can legitimately grab more than one of
    // this pass's chunks in a single execute() call. The bound that IS
    // provably correct: exactly ONE execute() call happens in this test,
    // and a single execute() call's chunked pass has exactly
    // `expectedChunkCount` chunks total, each consuming exactly one
    // buffer (freshly allocated or reused) -- so total allocations this
    // whole test can ever cause is bounded by `expectedChunkCount` itself.
    CHECK(stats.totalPoolAllocations <= expectedChunkCount);

    // --- Whole-pass twin: SAME recordCells() call, all 4 cells in one shot,
    // through setExecute() (ctx.cmd, not ctx.chunkCommandBuffer()) --------
    RenderGraph wholeGraph;
    wholeGraph.addPass("quad_fill")
        .addColorOutput("bb", absoluteColorDesc(kExtent, kExtent))
        .setExecute([&](PassContext& ctx) { recordCells(ctx.cmd, *fillPipeline, kExtent, 0, kCellCount); });
    wholeGraph.setBackbufferSource("bb");
    wholeGraph.compile(chunkedInfo);
    fixture->executor->realize(wholeGraph);

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(wholeGraph, cmd, offscreenWhole->image, offscreenWhole->view,
                                    VkExtent2D{kExtent, kExtent});
    });

    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(kExtent) * kExtent * 4;
    auto readbackChunked = fixture->allocator.createHostVisibleBuffer(pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readbackChunked.has_value());
    auto readbackWhole = fixture->allocator.createHostVisibleBuffer(pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readbackWhole.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {kExtent, kExtent, 1};
        vkCmdCopyImageToBuffer(cmd, offscreenChunked->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                readbackChunked->handle(), 1, &region);
        vkCmdCopyImageToBuffer(cmd, offscreenWhole->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                readbackWhole->handle(), 1, &region);
    });
    readbackChunked->invalidate();
    readbackWhole->invalidate();

    // BYTE-IDENTICAL, whole-image -- the strongest form of the "chunked
    // recording must not change rendered output" claim this task's brief
    // requires of samples/05_multipass and samples/06_materials' own
    // migrations, proven once here at the executor level with a full
    // memcmp rather than a handful of probe pixels.
    CHECK(std::memcmp(readbackChunked->mappedData(), readbackWhole->mappedData(), static_cast<size_t>(pixelBytes)) ==
          0);

    const auto* chunkedPixels = static_cast<const std::array<uint8_t, 4>*>(readbackChunked->mappedData());
    for (uint32_t cell = 0; cell < kCellCount; ++cell) {
        CAPTURE(cell);
        const std::array<uint8_t, 4> px = readCellCenter(chunkedPixels, kExtent, cell);
        CHECK(px[0] == static_cast<uint8_t>(kCellColors[cell][0] * 255.0F));
        CHECK(px[1] == static_cast<uint8_t>(kCellColors[cell][1] * 255.0F));
        CHECK(px[2] == static_cast<uint8_t>(kCellColors[cell][2] * 255.0F));
        CHECK(px[3] == 255);
    }

    vkDeviceWaitIdle(device);
    destroyFillPipeline(device, *fillPipeline);
    destroyOffscreenImage(device, *offscreenChunked);
    destroyOffscreenImage(device, *offscreenWhole);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("PassContext::chunkCommandBuffer throws from a whole-pass setExecute callback; ctx.cmd stays "
          "VK_NULL_HANDLE inside a chunked one") {
    auto fixture = makeFixture("rx_graph_gpu_chunked_context_misuse");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    // Neither pass callback below issues a single draw call (both only
    // probe PassContext's own accessors) -- no pipeline is needed at all
    // for this test, unlike the previous one.
    auto offscreen = createOffscreenImage(device, physicalDevice, kFormat, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = kFormat;
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    // --- Whole-pass: chunkCommandBuffer() must throw std::out_of_range,
    // and never reach the draw calls below it in the same callback --------
    bool threw = false;
    bool drewAnyway = false;
    RenderGraph wholeGraph;
    wholeGraph.addPass("whole")
        .addColorOutput("bb", absoluteColorDesc(kExtent, kExtent))
        .setExecute([&](PassContext& ctx) {
            CHECK(ctx.cmd != VK_NULL_HANDLE);
            try {
                (void)ctx.chunkCommandBuffer();
                drewAnyway = true;
            } catch (const std::out_of_range&) {
                threw = true;
            }
        });
    wholeGraph.setBackbufferSource("bb");
    wholeGraph.compile(info);
    fixture->executor->realize(wholeGraph);
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(wholeGraph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });
    CHECK(threw);
    CHECK_FALSE(drewAnyway);

    // --- Chunked: ctx.cmd (the field every whole-pass callback reads) must
    // stay at its default VK_NULL_HANDLE -- chunkCommandBuffer() is the only
    // valid way to get a real command buffer out of a chunked callback ----
    bool sawNullCtxCmd = false;
    bool sawRealChunkCmd = false;
    RenderGraph chunkedGraph;
    chunkedGraph.addPass("chunked")
        .addColorOutput("bb", absoluteColorDesc(kExtent, kExtent))
        .setExecuteChunked([&](PassContext& ctx, uint32_t /*chunkIndex*/, uint32_t /*chunkCount*/) {
            if (ctx.cmd == VK_NULL_HANDLE) {
                sawNullCtxCmd = true;
            }
            if (ctx.chunkCommandBuffer() != VK_NULL_HANDLE) {
                sawRealChunkCmd = true;
            }
        });
    chunkedGraph.setBackbufferSource("bb");
    chunkedGraph.compile(info);
    fixture->executor->realize(chunkedGraph);
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(chunkedGraph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });
    CHECK(sawNullCtxCmd);
    CHECK(sawRealChunkCmd);

    vkDeviceWaitIdle(device);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// Phase 4 Task 7 [spec D4 amendment]: "Chunked COMPUTE passes: no rendering
// scope -- secondaries with plain inheritance, same fan-out" -- every other
// chunked-pass test above uses a GRAPHICS pass (an attachment output); this
// one is BARE (no addColorOutput()/setDepthStencilOutput() at all, exactly
// samples/05_multipass's/06_materials's own compute-class shape via
// QueueClass::AsyncCompute + addStorageBufferOutput() + setSideEffect() --
// this file's own "buffer reuse" test above already establishes that
// pattern for a WHOLE-pass compute pass), proving recordChunkedPass()'s
// `!isGraphicsPass` branch (no vkCmdBeginRendering/vkCmdEndRendering at
// all, plain VkCommandBufferInheritanceInfo with no
// VkCommandBufferInheritanceRenderingInfo chained in) is real, exercised
// code, not merely a code path that happens to compile. Each chunk
// vkCmdFillBuffer()s its own, disjoint slice of a storage buffer with a
// chunk-index-derived pattern (real GPU writes, not just draw-free
// bookkeeping) -- read back afterward and checked byte-exact per chunk.
TEST_CASE("Executor::execute chunks a BARE (compute-class, no attachment output) pass with no rendering scope and "
          "plain secondary inheritance") {
    auto fixture = makeFixture("rx_graph_gpu_chunked_compute");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();

    constexpr uint32_t kSlotCount = 8;  // comfortably >= any real chunk count this fixture's Scheduler will derive.
    constexpr uint32_t kSlotBytes = 256;  // vkCmdFillBuffer requires a multiple of 4; well over that.
    constexpr VkDeviceSize kBufferBytes = static_cast<VkDeviceSize>(kSlotCount) * kSlotBytes;

    BufferDesc dataDesc;
    dataDesc.size = kBufferBytes;
    dataDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    std::atomic<uint32_t> chunksSeen{0};
    // Captured from inside the chunked callback (the only place a real
    // PassContext can resolve "data" by name) so this test can read it back
    // afterward -- every chunk resolves the SAME underlying VkBuffer (one
    // resource, resolved once for the whole execute() call), so it is safe
    // for any chunk to be the one that happens to win this race (the value
    // written is identical regardless of which chunk writes it).
    std::atomic<VkBuffer> capturedBuffer{VK_NULL_HANDLE};

    RenderGraph graph;
    graph.addPass("compute_chunked", QueueClass::AsyncCompute)
        .addStorageBufferOutput("data", dataDesc)
        .setSideEffect()
        .setExecuteChunked([&](PassContext& ctx, uint32_t chunkIndex, uint32_t chunkCount) {
            REQUIRE(chunkCount <= kSlotCount);  // fits this test's own fixed slot table.
            chunksSeen.fetch_add(1, std::memory_order_relaxed);
            VkBuffer buf = ctx.buffer("data");
            capturedBuffer.store(buf, std::memory_order_relaxed);
            // Pattern: (chunkIndex + 1) repeated as a uint32 -- never 0, so
            // an accidentally-unfilled slot (chunkCount < kSlotCount, the
            // common case) is trivially distinguishable from a real chunk's
            // own fill.
            const uint32_t pattern = chunkIndex + 1;
            vkCmdFillBuffer(ctx.chunkCommandBuffer(), buf, static_cast<VkDeviceSize>(chunkIndex) * kSlotBytes,
                             kSlotBytes, pattern);
        });

    // RenderGraph::compile() requires a backbuffer source regardless of
    // what else the graph does -- "present_stub" exists only to establish
    // one, exactly like this file's very first ("invert") case's own
    // "draw" pass establishes "color" with no shader at all: its entire
    // contribution is being cleared by Executor::execute()'s own load-op
    // logic. Otherwise fully independent of "compute_chunked" (no shared
    // resource, no declared edge) -- side-effect-free is fine since
    // setBackbufferSource() itself is what keeps a graph's backbuffer
    // writer from being culled (render_graph.cpp's own cull step).
    graph.addPass("present_stub").addColorOutput("bb", absoluteColorDesc(kExtent, kExtent));
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = kFormat;
    graph.compile(info);
    fixture->executor->realize(graph);

    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    // Executor::execute() needs A backbuffer target even though this graph
    // never writes one through the graph itself -- reuse the same tiny
    // offscreen image every other case in this file uses; `graph.
    // setBackbufferSource()` was never called, so compile()'s own
    // no-side-effect-pass-reads-it path is irrelevant here and this image
    // is simply unused by the graph (Executor::execute()'s own signature
    // always takes one, backbuffer-less graphs included).
    auto offscreen = createOffscreenImage(device, fixture->device.physicalDevice(), kFormat,
                                            VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });

    const rx::graph::detail::ExecutorChunkDebugStats stats = rx::graph::detail::debugChunkStats(*fixture->executor);
    const uint32_t expectedChunkCount =
        rx::graph::detail::chunkCountForWorkerCount(fixture->scheduler->workerCount());
    CHECK(stats.lastChunkCount == expectedChunkCount);
    CHECK(chunksSeen.load() == expectedChunkCount);

    const VkBuffer dataBuffer = capturedBuffer.load();
    REQUIRE(dataBuffer != VK_NULL_HANDLE);

    auto readback = fixture->allocator.createHostVisibleBuffer(kBufferBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferCopy region{};
        region.size = kBufferBytes;
        vkCmdCopyBuffer(cmd, dataBuffer, readback->handle(), 1, &region);
    });
    readback->invalidate();

    std::vector<uint32_t> words(static_cast<size_t>(kBufferBytes / sizeof(uint32_t)));
    std::memcpy(words.data(), readback->mappedData(), words.size() * sizeof(uint32_t));
    const uint32_t wordsPerSlot = kSlotBytes / sizeof(uint32_t);
    for (uint32_t chunkIndex = 0; chunkIndex < expectedChunkCount; ++chunkIndex) {
        CAPTURE(chunkIndex);
        const uint32_t expected = chunkIndex + 1;
        for (uint32_t w = 0; w < wordsPerSlot; ++w) {
            CHECK(words[chunkIndex * wordsPerSlot + w] == expected);
        }
    }

    vkDeviceWaitIdle(device);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ---------------------------------------------------------------------
// D29 [gate ruling RC2, Task 22's real rx_graph blocker]: AttachmentDesc::
// depthConvention drives Executor's own clear-value selection at the
// ordinary per-pass site (executor.cpp's dynamic-rendering attachment
// loop) -- the pinned-history init-clear site is a second, independent
// call site (Task 1's own history GPU test already covers that clear
// path structurally); this ticket's own acceptance criterion is "a
// two-pass frame mixing both conventions", which the case below
// satisfies directly with real GPU readback, not an assumption.
// ---------------------------------------------------------------------
namespace {

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

AttachmentDesc absoluteDepthDesc(uint32_t width, uint32_t height, DepthConvention convention) {
    AttachmentDesc desc;
    desc.format = kDepthFormat;
    desc.sizeClass = SizeClass::Absolute;
    desc.width = static_cast<float>(width);
    desc.height = static_cast<float>(height);
    desc.depthConvention = convention;
    return desc;
}

// A pooled depth attachment never gets VK_IMAGE_USAGE_TRANSFER_SRC_BIT
// (PhysicalResource::imageUsage is a union of DECLARED access kinds only
// -- resources.h's own comment: "Task 1's Pass API has no readback/
// screenshot declaration... there is nothing to union it from"), so this
// test cannot vkCmdCopyImageToBuffer a pooled depth image directly --
// exactly the same reason the "invert" case above never copies its own
// pooled "color" resource straight out, instead SAMPLING it bindlessly
// from a later pass and writing the result into the externally-owned,
// copy-friendly backbuffer. This shader is that same idiom applied to a
// depth attachment: reuses kInvertShaderSource's own fullscreen-triangle
// vsMain verbatim (SV_VertexID trick, no vertex buffer) and an ordinary
// (non-comparison) ".Sample()" fragment body -- proven legal for a
// depth-only VK_FORMAT_D32_SFLOAT image view in THIS codebase already
// (shaders/multipass/lit.frag.slang samples sample 05's own shadow map
// the identical way, manual-compare included) -- outputting the raw
// stored depth as a greyscale color so the EXISTING kFormat (R8G8B8A8_
// UNORM) offscreen readback helpers above can read it back unchanged.
// UNORM8 round-trips 0.0/1.0 exactly (no rounding), which is all this
// test's own two clear values ever need.
constexpr const char* kDepthProbeShaderSource = R"(
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
    float depthValue = gTextures[gPush.textureIndex].Sample(gSamplers[gPush.samplerIndex], input.uv).r;
    return float4(depthValue, depthValue, depthValue, 1.0);
}
)";

const std::vector<std::string> kDepthProbeEntryPoints = {"vsMain", "fsMain"};

struct DepthProbePipeline {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t pushConstantOffset = 0;
    uint32_t pushConstantSize = 0;
    VkShaderStageFlags pushConstantStages = 0;
};

void destroyDepthProbePipeline(VkDevice device, DepthProbePipeline& p) {
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

// Byte-for-byte the same shape as buildInvertPipeline() above (compile via
// rx::shader::Compiler, reflect, PipelineLayoutBuilder's externalSet0
// substitution, one color-only dynamic-rendering pipeline) -- not shared
// because this file's own convention is per-case-local pipeline builders
// (buildInvertPipeline/buildFillPipeline above are equally not shared with
// each other).
std::optional<DepthProbePipeline> buildDepthProbePipeline(VkDevice device, VkFormat colorFormat,
                                                            VkDescriptorSetLayout bindlessSetLayout) {
    DepthProbePipeline result;

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        return std::nullopt;
    }

    rx::shader::CompileResult compileResult =
        compiler->compileFromSource("RxGraphDepthProbeModule", kDepthProbeShaderSource, kDepthProbeEntryPoints);
    if (!compileResult.ok) {
        MESSAGE("rx_graph depth-probe shader compile failed: ", compileResult.diagnostics);
        return std::nullopt;
    }

    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value() || layoutInfo->pushRanges.size() != 1) {
        return std::nullopt;
    }
    result.pushConstantOffset = layoutInfo->pushRanges[0].offset;
    result.pushConstantSize = layoutInfo->pushRanges[0].size;
    result.pushConstantStages = layoutInfo->pushRanges[0].stages;

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, *layoutInfo, bindlessSetLayout);
    if (!layoutBundle.has_value()) {
        return std::nullopt;
    }
    result.layoutBundle = std::move(*layoutBundle);

    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = blob.code.size() * sizeof(uint32_t);
        moduleInfo.pCode = blob.code.data();

        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            destroyDepthProbePipeline(device, result);
            return std::nullopt;
        }
        if (blob.entryPointName == "vsMain") {
            result.vertModule = module;
        } else if (blob.entryPointName == "fsMain") {
            result.fragModule = module;
        } else {
            vkDestroyShaderModule(device, module, nullptr);
            destroyDepthProbePipeline(device, result);
            return std::nullopt;
        }
    }
    if (result.vertModule == VK_NULL_HANDLE || result.fragModule == VK_NULL_HANDLE) {
        destroyDepthProbePipeline(device, result);
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
    rasterizationState.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

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
        destroyDepthProbePipeline(device, result);
        return std::nullopt;
    }

    return result;
}

}  // namespace


TEST_CASE("D29: ONE frame mixes Standard and Reversed depth conventions across two simultaneous depth-bearing "
          "passes, and the pinned-history init-clear site independently honors its own AttachmentDesc's "
          "convention too") {
    // [Task 22 fix round, F4] The earlier version of this test ran TWO
    // SEPARATE single-convention graphs, one after another -- exactly
    // what the review flagged as insufficient: that does not prove
    // depthClearValueFor() is derived PER-PASS/PER-ATTACHMENT within a
    // single frame (a process-wide "current convention" variable
    // toggled between two whole-graph runs could have passed the same
    // way). This version builds ONE graph with FOUR passes and drives it
    // through exactly ONE Executor::execute() call:
    //   "depthStandard" / "depthReversed" -- two ordinary depth outputs,
    //     Standard and Reversed, both cleared within the SAME frame (the
    //     gate's own "ordinary per-pass site", executor.cpp's dynamic-
    //     rendering attachment loop).
    //   "historyWrite" -- setHistoryOutput() of a THIRD, Reversed-
    //     convention depth resource, draw-less (setSideEffect() only) --
    //     establishes the history resource's TWO ping-ponged physical
    //     slots. Per Pass::setHistoryOutput()'s own documented contract,
    //     this pass's write touches only SLOT 1 this (the first) frame;
    //     SLOT 0 is "never written by any real pass" and its contents
    //     are whatever Executor::realize()'s ONE-TIME
    //     initializePinnedHistoryEntry() init-clear left there -- the
    //     PINNED-HISTORY site the review flagged as empirically unproven
    //     (only reachable via a depth-FORMAT history resource; the
    //     existing Task 1 history GPU test above only ever uses a COLOR
    //     one, which exercises depthClearValueFor()'s sibling
    //     VkClearColorValue branch instead).
    //   "probe" -- samples all three (addTextureInput() x2 +
    //     addHistoryInput()) with three scissored draws of the SAME
    //     depth-probe pipeline into three thirds of the one backbuffer,
    //     writing each raw stored depth as greyscale.
    // A single readback of the one backbuffer then proves all three
    // clear values in one shot, from one frame.
    auto fixture = makeFixture("rx_graph_gpu_d29_depth_convention");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    auto bindlessTable =
        rx::rhi::BindlessTable::create(physicalDevice, device, rx::rhi::BindlessTable::Capacities{4, 2, 1});
    REQUIRE(bindlessTable.has_value());

    auto pipeline = buildDepthProbePipeline(device, kFormat, bindlessTable->descriptorSetLayout());
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

    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    auto offscreen = createOffscreenImage(device, physicalDevice, kFormat, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());

    bool observedHistoryValid = true;  // must flip to false inside the probe callback.

    // Three vertical thirds of the one kExtent x kExtent backbuffer.
    const uint32_t thirdWidth = kExtent / 3;
    auto scissorFor = [&](uint32_t slot) {
        return VkRect2D{{static_cast<int32_t>(slot * thirdWidth), 0}, {thirdWidth, kExtent}};
    };
    std::vector<rx::rhi::BindlessHandle> probeHandles;
    auto sampleThird = [&](VkCommandBuffer cmd, VkImageView view, uint32_t slot) {
        rx::rhi::BindlessHandle handle =
            bindlessTable->registerSampledImage(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        REQUIRE(handle.isValid());
        probeHandles.push_back(handle);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
        VkDescriptorSet set = bindlessTable->descriptorSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layoutBundle.layout, 0, 1, &set, 0,
                                 nullptr);
        struct {
            uint32_t textureIndex;
            uint32_t samplerIndex;
        } push{handle.index(), samplerHandle.index()};
        vkCmdPushConstants(cmd, pipeline->layoutBundle.layout, pipeline->pushConstantStages,
                            pipeline->pushConstantOffset, pipeline->pushConstantSize, &push);

        VkViewport viewport{0.0F, 0.0F, static_cast<float>(kExtent), static_cast<float>(kExtent), 0.0F, 1.0F};
        VkRect2D scissor = scissorFor(slot);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        // Deferred release (bindless.h's own RELEASE-SAFETY CONTRACT) --
        // this test has only one in-flight submission at a time
        // (runOnce() waits idle), so releasing all three collected
        // handles after the readback below (once nothing can still be
        // sampling them) is safe -- see `probeHandles` below.
    };

    RenderGraph graph;
    graph.addPass("depthStandard")
        .setDepthStencilOutput("depthA", absoluteDepthDesc(kExtent, kExtent, DepthConvention::Standard))
        .setSideEffect();
    graph.addPass("depthReversed")
        .setDepthStencilOutput("depthB", absoluteDepthDesc(kExtent, kExtent, DepthConvention::Reversed))
        .setSideEffect();
    // Draw-less: this pass exists purely to establish "depthHist"'s two
    // pinned physical slots. It writes slot 1 this (first) frame --
    // slot 0 stays whatever initializePinnedHistoryEntry() init-cleared
    // it to, which "probe" below reads via addHistoryInput().
    graph.addPass("historyWrite")
        .setHistoryOutput("depthHist", absoluteDepthDesc(kExtent, kExtent, DepthConvention::Reversed))
        .setSideEffect();
    graph.addPass("probe")
        .addTextureInput("depthA")
        .addTextureInput("depthB")
        .addHistoryInput("depthHist")
        .addColorOutput("bb", absoluteColorDesc(kExtent, kExtent))
        .setExecute([&](PassContext& ctx) {
            observedHistoryValid = ctx.historyValid("depthHist");
            sampleThird(ctx.cmd, ctx.imageView("depthA"), 0);
            sampleThird(ctx.cmd, ctx.imageView("depthB"), 1);
            sampleThird(ctx.cmd, ctx.imageView("depthHist"), 2);
        });
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = kFormat;
    // Task 3 ambiguity resolution #1: this offscreen "backbuffer" is
    // never presented -- readback needs it left in TRANSFER_SRC_OPTIMAL,
    // not the default PRESENT_SRC_KHR (same override the "invert" case
    // above makes for the identical reason).
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);
    fixture->executor->realize(graph);

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

    auto redAtThirdCenter = [&](uint32_t slot) -> uint8_t {
        const uint32_t x = slot * thirdWidth + thirdWidth / 2;
        const uint32_t y = kExtent / 2;
        const size_t idx = (static_cast<size_t>(y) * kExtent + x) * 4;
        uint8_t value = 0;
        std::memcpy(&value, static_cast<const uint8_t*>(readback->mappedData()) + idx, sizeof(uint8_t));
        return value;
    };

    const uint8_t standardRed = redAtThirdCenter(0);
    const uint8_t reversedRed = redAtThirdCenter(1);
    const uint8_t historyRed = redAtThirdCenter(2);

    // The ordinary per-pass site [depthA/depthB, cleared within the SAME
    // frame as each other]: UNORM8 round-trips 0.0/1.0 exactly --
    // Standard's clear (1.0) reads back as 255; Reversed's (0.0) as 0.
    CAPTURE(static_cast<int>(standardRed));
    CAPTURE(static_cast<int>(reversedRed));
    CHECK(static_cast<int>(standardRed) == 255);
    CHECK(static_cast<int>(reversedRed) == 0);

    // The pinned-history init-clear site [depthHist's untouched slot 0]:
    // this is a REVERSED-convention history resource, so its own
    // initializePinnedHistoryEntry() clear (executor.cpp's depth branch)
    // must ALSO read back 0, independently proving that site derives
    // from AttachmentDesc::depthConvention too, not only the ordinary
    // site above.
    CAPTURE(static_cast<int>(historyRed));
    CHECK(static_cast<int>(historyRed) == 0);
    // Slot 0 was genuinely never written by any real pass this frame --
    // the same "never written" contract the existing color history test
    // above already proves, now also exercised for a depth resource.
    CHECK_FALSE(observedHistoryValid);

    CHECK_FALSE(fixture->context.hasValidationErrors());
    vkDeviceWaitIdle(device);
    for (rx::rhi::BindlessHandle handle : probeHandles) {
        bindlessTable->release(handle);
    }
    bindlessTable->release(samplerHandle);
    vkDestroySampler(device, sampler, nullptr);
    destroyDepthProbePipeline(device, *pipeline);
    destroyOffscreenImage(device, *offscreen);
}

// [Task 23, gate ticket/matrix/ruling #29] lookupResolvedIndex()'s
// heterogeneous `nameToIndex.find(name)` lookup (executor.cpp) must resolve
// a REAL name exactly like the old `find(std::string(name))` did (a HIT),
// and still throw std::out_of_range, naming the resource, for a name that
// was never one of CompiledGraph::resources()' own entries at all (a MISS)
// -- matrix's own proposed test shape for the transparent-hash fix.
TEST_CASE("PassContext resolvers: nameToIndex's transparent string_view lookup resolves a real name (hit) and "
          "throws std::out_of_range naming a nonexistent one (miss), same as the old std::string-keyed lookup did "
          "[Task 23]") {
    auto fixture = makeFixture("rx_graph_gpu_name_lookup");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();

    bool ran = false;
    VkFormat resolvedFormat = VK_FORMAT_UNDEFINED;

    RenderGraph graph;
    graph.addPass("fill").addColorOutput("bb", absoluteColorDesc(kExtent, kExtent)).setExecute([&](PassContext& ctx) {
        ran = true;

        // HIT: "bb" is a real, realized resource name.
        resolvedFormat = ctx.imageFormat("bb");
        CHECK(ctx.imageView("bb") != VK_NULL_HANDLE);

        // MISS: never a CompiledGraph::resources() entry at all -- must
        // throw std::out_of_range, naming the resource, not silently
        // resolve to a meaningless handle or crash.
        try {
            static_cast<void>(ctx.imageView("does_not_exist"));
            FAIL("expected ctx.imageView(\"does_not_exist\") to throw std::out_of_range");
        } catch (const std::out_of_range& e) {
            const std::string what = e.what();
            CHECK(what.find("no realized resource named") != std::string::npos);
            CHECK(what.find("does_not_exist") != std::string::npos);
        }
    });
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = kFormat;
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);
    fixture->executor->realize(graph);

    auto offscreen = createOffscreenImage(device, fixture->device.physicalDevice(), kFormat, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());
    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });

    CHECK(ran);
    CHECK(resolvedFormat == kFormat);

    vkDeviceWaitIdle(device);
    destroyOffscreenImage(device, *offscreen);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Task 23, gate ticket #29 / gate matrix issue29-executor-allocations.md /
// rulings-2026-08-18.md §#29] THE zero-alloc acceptance-criterion test:
// Executor::execute() performs zero heap allocations in steady state
// (unchanged graph, unchanged resource count). Methodology is BOUND by the
// gate ruling: capacity-snapshot via the test-only
// detail::allocationCapacitiesForTesting() accessor, NOT global
// operator-new interposition (this binary links volk, the Vulkan
// validation layers, and -- via rx_shader_deploy_runtime_libs() -- Slang's
// own runtime; a process-wide operator-new/delete override risks
// interacting with all three in ways this task cannot verify are safe
// without a dedicated spike, per the ruling's own reasoning). A capacity
// staying bit-for-bit identical across N further steady-state frames after
// a warm-up is, per that same ruling, a deliberately accepted limitation
// (blind to a same-sized reallocation, astronomically unlikely for
// std::vector's own deterministic doubling growth) rather than a true
// allocation COUNT -- see this task's own report for the revert-probe that
// establishes this methodology's actual discriminating power.
//
// The graph below is deliberately representative of every site this task
// touched, in ONE frame: "produce" (a bare AsyncCompute storage-buffer
// producer -- exercises the pooled-buffer first-use synthesis path,
// combinedAccessScratch), "fillWhole" (an ordinary whole-pass color
// output -- colorPhysIdx/colorAttachments/barrier scratch), "probe" (a
// bare pass resolving TWO real names via PassContext, exercising
// lookupResolvedIndex()'s transparent lookup on the hot path -- see the
// dedicated hit/miss test above for that site's own correctness proof),
// and "fillChunked" (a CHUNKED pass into the backbuffer -- debug-label
// scratch, both chunk-command-buffer scratch arrays). Tracy's own
// dynamic-zone-name allocations (RX_ZONE_DYNAMIC_NAME/RX_GPU_ZONE_DYNAMIC,
// both hit every pass this graph runs) are a documented third-party
// exception this methodology is naturally blind to either way -- see
// combineAccessesByResource()/profile.h/tracy_gpu.h's own comments; this
// project's `-DTRACY_ON_DEMAND=ON` build additionally means neither macro
// reaches its own allocating path at all while no profiler is connected
// (verified this task by tracing TracyVulkan.hpp's transient VkCtxScope
// constructor to Profiler::AllocSourceLocation(), gated by
// `is_active && GetProfiler().IsConnected()` -- the same gate
// profile.h's own RX_ZONE_DYNAMIC_NAME comment already documents for the
// CPU-side macro), which is the representative configuration this test
// (and every ctest run in this repo) actually exercises.
TEST_CASE("Executor::execute performs zero heap allocations in steady state -- every Impl-persistent scratch "
          "buffer's capacity stays bit-for-bit unchanged across many further execute() calls after a warm-up "
          "[Task 23, gate ticket/matrix/ruling #29]") {
    auto fixture = makeFixture("rx_graph_gpu_zero_alloc");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    auto fillPipeline = buildFillPipeline(device, kFormat);
    REQUIRE(fillPipeline.has_value());

    auto offscreen = createOffscreenImage(device, physicalDevice, kFormat, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());

    BufferDesc dataDesc;
    dataDesc.size = 256;
    dataDesc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    RenderGraph graph;
    graph.addPass("produce", QueueClass::AsyncCompute).addStorageBufferOutput("data", dataDesc).setSideEffect();
    graph.addPass("fillWhole")
        .addColorOutput("colorA", absoluteColorDesc(kExtent, kExtent))
        .setExecute([&](PassContext& ctx) { recordCells(ctx.cmd, *fillPipeline, kExtent, 0, kCellCount); });
    graph.addPass("probe")
        .addTextureInput("colorA")
        .addStorageBufferInput("data")
        .setSideEffect()
        .setExecute([&](PassContext& ctx) {
            CHECK(ctx.imageView("colorA") != VK_NULL_HANDLE);
            CHECK(ctx.buffer("data") != VK_NULL_HANDLE);
        });
    graph.addPass("fillChunked")
        .addColorOutput("bb", absoluteColorDesc(kExtent, kExtent))
        .setExecuteChunked([&](PassContext& ctx, uint32_t chunkIndex, uint32_t chunkCount) {
            const uint32_t cellsPerChunk = (kCellCount + chunkCount - 1) / chunkCount;
            const uint32_t begin = std::min(chunkIndex * cellsPerChunk, kCellCount);
            const uint32_t end = std::min(begin + cellsPerChunk, kCellCount);
            recordCells(ctx.chunkCommandBuffer(), *fillPipeline, kExtent, begin, end);
        });
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = kFormat;
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);
    fixture->executor->realize(graph);

    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    auto runOneFrame = [&] {
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
        });
    };

    // Warm-up: enough execute() calls for every Impl-persistent buffer to
    // reach its steady-state peak capacity, including BOTH chunk
    // command-buffer frame slots (chunkFrameSlot alternates 0/1 every
    // call -- see Executor::execute()'s own comment) -- matches
    // acquireChunkCommandBuffer()'s own documented "stabilizes after the
    // first kFramesInFlight execute() calls" budget, doubled here for
    // margin.
    constexpr int kWarmupFrames = 4;
    for (int i = 0; i < kWarmupFrames; ++i) {
        runOneFrame();
    }

    const rx::graph::detail::ExecutorAllocationCapacitiesForTesting before =
        rx::graph::detail::allocationCapacitiesForTesting(*fixture->executor);
    // A completely untouched capacity struct (every field still 0) would
    // make every CHECK below trivially, uselessly pass -- assert the graph
    // genuinely exercised EVERY ONE of the 13 tracked fields (11 scalars +
    // 2 frame-slot arrays, both slots each) before trusting the
    // steady-state comparison that follows [Task 23 review fix-round:
    // the original 2-field guard left 11 fields' own vacuousness
    // unpinned -- durably closed here, not just spot-checked].
    REQUIRE(before.firstBarrierSeen > 0);
    REQUIRE(before.attachmentEverWritten > 0);
    REQUIRE(before.touchedThisExecute > 0);
    REQUIRE(before.finalStageThisExecute > 0);
    REQUIRE(before.finalAccessThisExecute > 0);
    REQUIRE(before.colorPhysIdxScratch > 0);
    REQUIRE(before.colorAttachmentsScratch > 0);
    REQUIRE(before.vkImageBarriersScratch > 0);
    REQUIRE(before.vkBufferBarriersScratch > 0);
    REQUIRE(before.combinedAccessScratch > 0);
    REQUIRE(before.debugLabelScratch > 0);
    for (uint32_t slot = 0; slot < 2; ++slot) {
        CAPTURE(slot);
        REQUIRE(before.chunkBuffersScratch[slot] > 0);
        REQUIRE(before.validChunkBuffersScratch[slot] > 0);
    }

    constexpr int kSteadyFrames = 10;
    for (int frame = 0; frame < kSteadyFrames; ++frame) {
        runOneFrame();
        const rx::graph::detail::ExecutorAllocationCapacitiesForTesting after =
            rx::graph::detail::allocationCapacitiesForTesting(*fixture->executor);

        INFO("frame=", frame);
        CHECK(after.firstBarrierSeen == before.firstBarrierSeen);
        CHECK(after.attachmentEverWritten == before.attachmentEverWritten);
        CHECK(after.touchedThisExecute == before.touchedThisExecute);
        CHECK(after.finalStageThisExecute == before.finalStageThisExecute);
        CHECK(after.finalAccessThisExecute == before.finalAccessThisExecute);
        CHECK(after.colorPhysIdxScratch == before.colorPhysIdxScratch);
        CHECK(after.colorAttachmentsScratch == before.colorAttachmentsScratch);
        CHECK(after.vkImageBarriersScratch == before.vkImageBarriersScratch);
        CHECK(after.vkBufferBarriersScratch == before.vkBufferBarriersScratch);
        CHECK(after.combinedAccessScratch == before.combinedAccessScratch);
        CHECK(after.debugLabelScratch == before.debugLabelScratch);
        for (uint32_t slot = 0; slot < 2; ++slot) {
            CAPTURE(slot);
            CHECK(after.chunkBuffersScratch[slot] == before.chunkBuffersScratch[slot]);
            CHECK(after.validChunkBuffersScratch[slot] == before.validChunkBuffersScratch[slot]);
        }

        // Second, independent signal alongside capacity -- see
        // ExecutorAllocationCapacitiesForTesting's own doc comment
        // (executor.h) for why capacity ALONE is empirically insufficient
        // for a fixed-size-every-call buffer (this task's own revert-probe
        // against the sibling DeletionQueue fix found exactly this gap).
        // std::vector<bool> fields (firstBarrierSeen/attachmentEverWritten/
        // touchedThisExecute) have no `.data()` companion -- see that same
        // comment.
        CHECK(after.finalStageThisExecuteData == before.finalStageThisExecuteData);
        CHECK(after.finalAccessThisExecuteData == before.finalAccessThisExecuteData);
        CHECK(after.colorPhysIdxScratchData == before.colorPhysIdxScratchData);
        CHECK(after.colorAttachmentsScratchData == before.colorAttachmentsScratchData);
        CHECK(after.vkImageBarriersScratchData == before.vkImageBarriersScratchData);
        CHECK(after.vkBufferBarriersScratchData == before.vkBufferBarriersScratchData);
        CHECK(after.combinedAccessScratchData == before.combinedAccessScratchData);
        CHECK(after.debugLabelScratchData == before.debugLabelScratchData);
        for (uint32_t slot = 0; slot < 2; ++slot) {
            CAPTURE(slot);
            CHECK(after.chunkBuffersScratchData[slot] == before.chunkBuffersScratchData[slot]);
            CHECK(after.validChunkBuffersScratchData[slot] == before.validChunkBuffersScratchData[slot]);
        }
    }

    vkDeviceWaitIdle(device);
    destroyFillPipeline(device, *fillPipeline);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Phase 4 exit fix wave, I2; Stage-0 audit F5-remainder, stage0-audit.md:
// 136/390] Proves Executor::realize()/execute()'s new RX_ASSERT_MAIN_THREAD
// guards are genuinely wired in, not merely documented -- mirrors
// rx_asset/tests/thread_guard_test.cpp's own "a plain std::thread stands in
// for a chunk >= 1 worker" pattern (legitimate here for the identical
// reason that file documents: this tests the thread-identity comparison
// the guard performs, not any real rx::task::Scheduler integration -- the
// violating calls below are made directly, never through a chunked pass'
// own worker fan-out). Each worker thread is joined before the next
// guarded call starts, so there is never more than one thread touching
// this Executor's state at a time -- the precondition
// rx::core::debug::detail::ViolationHook's own contract comment requires
// of a test-installed hook that records and returns rather than aborting.
// Graph: a single pass with NO execute() callback at all -- Executor::
// execute()'s own automatic backbuffer clear is enough to prove both
// entry points ran to completion past the (non-aborting, test-hooked)
// guard, with no shader/pipeline machinery needed.
#ifdef RX_DEBUG_CHECKS

namespace {

struct ExecutorViolationCapture {
    std::mutex mutex;
    int callCount = 0;
    std::string lastContext;
};

std::atomic<ExecutorViolationCapture*> g_executorCapture{nullptr};

void captureExecutorViolation(const char* context) {
    ExecutorViolationCapture* capture = g_executorCapture.load(std::memory_order_relaxed);
    if (capture == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->callCount++;
    capture->lastContext = context != nullptr ? context : "";
}

struct ExecutorViolationHookGuard {
    ~ExecutorViolationHookGuard() {
        g_executorCapture.store(nullptr, std::memory_order_relaxed);
        rx::core::debug::detail::setViolationHookForTests(nullptr);
    }
};

}  // namespace

TEST_CASE("Executor::realize/execute trip RX_ASSERT_MAIN_THREAD when called from a worker thread") {
    auto fixture = makeFixture("rx_graph_gpu_executor_guard");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    RenderGraph graph;
    graph.addPass("clear").addColorOutput("bb", absoluteColorDesc(kExtent, kExtent));
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = kFormat;
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);

    auto offscreen = createOffscreenImage(device, physicalDevice, kFormat, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());
    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    ExecutorViolationCapture capture;
    g_executorCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureExecutorViolation);
    ExecutorViolationHookGuard guard;

    std::thread realizeThread([&] { fixture->executor->realize(graph); });
    realizeThread.join();

    std::thread executeThread([&] {
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
        });
    });
    executeThread.join();

    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        CHECK(capture.callCount == 2);
        CHECK(capture.lastContext == "Executor::execute");
    }

    vkDeviceWaitIdle(device);
    destroyOffscreenImage(device, *offscreen);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("Executor::realize/execute do NOT trip the guard for calls genuinely on the main thread") {
    auto fixture = makeFixture("rx_graph_gpu_executor_guard_legal");
    if (!fixture.has_value()) {
        return;
    }

    const VkDevice device = fixture->device.device();
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    RenderGraph graph;
    graph.addPass("clear").addColorOutput("bb", absoluteColorDesc(kExtent, kExtent));
    graph.setBackbufferSource("bb");

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = kFormat;
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);

    auto offscreen = createOffscreenImage(device, physicalDevice, kFormat, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());
    auto cmdCtx =
        rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    ExecutorViolationCapture capture;
    g_executorCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureExecutorViolation);
    ExecutorViolationHookGuard guard;

    fixture->executor->realize(graph);
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture->executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });

    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        CHECK(capture.callCount == 0);
    }

    vkDeviceWaitIdle(device);
    destroyOffscreenImage(device, *offscreen);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

#endif  // RX_DEBUG_CHECKS
