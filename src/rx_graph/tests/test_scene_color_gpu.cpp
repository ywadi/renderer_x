// [Task 3 (#39), gate rulings -- rulings-2026-08-20.md "T3 (#39)"; matrix
// rows 1/2/3/6/9/10, matrix-p5t03-hdr-scene-color.md] This ticket's own
// primary gate: prove, by VALUE, that `rx::graph::kHdrFormat`
// (VK_FORMAT_B10G11R11_UFLOAT_PACK32) actually carries a >1.0 radiance
// value through a real draw and readback (not just "it compiles"), that
// its documented blue-channel mantissa precision deficit is REAL (not
// merely asserted in a comment) via an honest CPU-side decode of the raw
// packed bits, that a wrong (UNORM8-family) target genuinely clamps
// (discrimination proof -- the same mechanism against a deliberately
// broken target must fail differently), that the documented escape hatch
// (`kHdrFormatHighPrecision`) is actually exercised (not merely declared),
// and that both formats empirically support the exact format features this
// codebase's own consumers need on THIS driver (never assumed from a
// memorized spec table -- matrix row 3/10's own verification-health note).
//
// Same real-windowed-Device-but-never-presenting fixture pattern as
// test_execute_gpu.cpp/test_compute_gpu.cpp -- an independent, small local
// copy of `makeFixture`/`OffscreenImage`/a push-constant fill pipeline
// rather than a shared header, matching this codebase's own established
// per-file-helper precedent (see e.g. test_compute_gpu.cpp's own comment
// on why it duplicates test_execute_gpu.cpp's near-identical fixture
// rather than sharing it).
#include <doctest/doctest.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_graph/scene_color.h>

#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <rx_task/scheduler.h>

#include <glm/gtc/packing.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace rx::graph;

namespace {

// Small extent -- every case below fills its whole target with one
// constant color via a fullscreen triangle, so a single corner texel
// (like test_execute_gpu.cpp's own `corner` readback) carries the entire
// proof; matches test_compute_gpu.cpp's own `kBbExtent = 4` precedent for
// the same reason (minimal, not 256, since no spatial pattern is tested
// here).
constexpr uint32_t kExtent = 4;

struct SceneColorGpuFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    std::unique_ptr<rx::task::Scheduler> scheduler;
    std::unique_ptr<Executor> executor;
};

std::optional<SceneColorGpuFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping rx_graph scene-color GPU test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping rx_graph "
                 "scene-color GPU test");
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

    return SceneColorGpuFixture{std::move(*window),    std::move(*context),  std::move(*device),
                                 std::move(*allocator), std::move(scheduler), std::move(executor)};
}

// --- Offscreen "backbuffer" -- format-parameterized, same shape/rationale
// as test_execute_gpu.cpp's own OffscreenImage (an external, never-pooled
// image the test itself owns, read back once execute() finishes). --------
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

// --- Fill pipeline: a fullscreen triangle (no vertex buffer, SV_VertexID
// trick) writing a push-constant float4 color -- same shader source and
// generic colorFormat parameterization as test_execute_gpu.cpp's own
// FillPipeline (its "history-output-writing pass" consumer), reused here
// verbatim as a controlled-value draw for a completely different purpose:
// this is exactly the "controlled draw" matrix row 1 asks for -- a real
// fragment shader stage writes a caller-chosen HDR value into a real
// attachment of a real color format, going through the SAME fixed-function
// color-attachment-write hardware path production rendering uses (never a
// CPU-side encode of the expected bits). --------------------------------
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
        compiler->compileFromSource("RxSceneColorFillModule", kFillShaderSource, kFillEntryPoints);
    if (!compileResult.ok) {
        MESSAGE("rx_graph scene-color fill shader compile failed: ", compileResult.diagnostics);
        return std::nullopt;
    }

    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value() || layoutInfo->pushRanges.size() != 1) {
        return std::nullopt;
    }

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

AttachmentDesc absoluteColorDesc(VkFormat format, uint32_t width, uint32_t height) {
    AttachmentDesc desc;
    desc.format = format;
    desc.sizeClass = SizeClass::Absolute;
    desc.width = static_cast<float>(width);
    desc.height = static_cast<float>(height);
    return desc;
}

// Renders `fillColor` into a single kExtent x kExtent attachment of
// `format` (the graph's ONLY pass, named kSceneColorResourceName and set
// directly as the backbuffer -- exactly matching how a real forward pass
// establishes "hdr" today, samples/09_scene/main.cpp's own
// `.addColorOutput("hdr", swapchainRelativeDesc(kHdrFormat))` site), then
// copies the raw, still-packed-per-`format` corner texel back to a
// host-visible buffer. Returns the raw bytes (`sizeof` = format's own
// per-texel byte count -- 4 for kHdrFormat/UNORM8, 8 for
// kHdrFormatHighPrecision) or std::nullopt on any setup failure (already
// REQUIRE()'d against inside, so a std::nullopt return only happens after
// this test has already been marked failed).
std::optional<std::array<uint8_t, 8>> renderAndReadbackTexel(SceneColorGpuFixture& fixture, VkFormat format,
                                                               glm::vec4 fillColor, VkDeviceSize texelBytes) {
    const VkDevice device = fixture.device.device();
    const VkPhysicalDevice physicalDevice = fixture.device.physicalDevice();

    auto pipeline = buildFillPipeline(device, format);
    REQUIRE(pipeline.has_value());

    auto offscreen = createOffscreenImage(device, physicalDevice, format, VkExtent2D{kExtent, kExtent});
    REQUIRE(offscreen.has_value());

    RenderGraph graph;
    graph.addPass("fill")
        .addColorOutput(kSceneColorResourceName, absoluteColorDesc(format, kExtent, kExtent))
        .setExecute([&](PassContext& ctx) {
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

            struct {
                glm::vec4 color;
            } push{fillColor};
            vkCmdPushConstants(ctx.cmd, pipeline->layoutBundle.layout, pipeline->pushConstantStages,
                                pipeline->pushConstantOffset, pipeline->pushConstantSize, &push);

            VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width),
                                 static_cast<float>(ctx.renderArea.height), 0.0F, 1.0F};
            VkRect2D scissor{{0, 0}, ctx.renderArea};
            vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
            vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
        });
    graph.setBackbufferSource(kSceneColorResourceName);

    CompileInfo info;
    info.swapchainWidth = kExtent;
    info.swapchainHeight = kExtent;
    info.swapchainFormat = format;
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);

    fixture.executor->realize(graph);

    auto cmdCtx = rx::rhi::CommandContext::create(device, fixture.device.graphicsQueue(),
                                                   fixture.device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        fixture.executor->execute(graph, cmd, offscreen->image, offscreen->view, VkExtent2D{kExtent, kExtent});
    });

    auto readback = fixture.allocator.createHostVisibleBuffer(texelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        // A single texel (the corner) is all any case below needs -- the
        // fill shader writes the identical value to every pixel.
        region.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(cmd, offscreen->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                                &region);
    });
    readback->invalidate();

    std::array<uint8_t, 8> raw{};
    std::memcpy(raw.data(), readback->mappedData(), static_cast<size_t>(texelBytes));

    vkDeviceWaitIdle(device);
    destroyFillPipeline(device, *pipeline);
    destroyOffscreenImage(device, *offscreen);

    CHECK_FALSE(fixture.context.hasValidationErrors());
    return raw;
}

}  // namespace

TEST_CASE(
    "SceneColorGpu: a >1.0 radiance value survives a real draw into kHdrFormat, with the documented blue-mantissa "
    "precision deficit proven, not just asserted") {
    auto fixture = makeFixture("rx_graph_scene_color_survival");
    if (!fixture.has_value()) {
        return;
    }

    // [matrix row 1] "render a synthetic radiance value >1.0... via a
    // controlled draw". 4.31875 is deliberately NOT a value kHdrFormat's
    // 11-bit (R/G) grid can represent any more precisely than it already
    // can -- chosen so its true mantissa fraction, expressed in R/G's
    // 6-bit grid units, is 5.1 (not an integer -- both channels must
    // round) while the SAME true value expressed in B's 5-bit grid units
    // is 2.55 (also not an integer, but on the far side of ITS OWN
    // nearest grid line from where 5.1 sits relative to R/G's). Because
    // B's representable grid (multiples of 2^-5) is a literal subset of
    // R/G's representable grid (multiples of 2^-6) at the same exponent,
    // the nearest-B-grid-point distance is PROVABLY >= the
    // nearest-R/G-grid-point distance for ANY true value and ANY
    // (round-to-nearest or truncating) hardware rounding convention -- and
    // this specific value was picked so that inequality is comfortably
    // STRICT under both conventions (not a coincidental tie), making the
    // R-vs-B error comparison below a robust discrimination proof rather
    // than a lucky one-off.
    constexpr float kValue = 4.31875F;
    auto raw = renderAndReadbackTexel(*fixture, kHdrFormat, glm::vec4(kValue, kValue, kValue, 1.0F), 4);
    REQUIRE(raw.has_value());

    uint32_t packed = 0;
    std::memcpy(&packed, raw->data(), sizeof(packed));
    // glm::unpackF2x11_1x10 decodes the EXACT VK_FORMAT_B10G11R11_UFLOAT_PACK32
    // bit layout (R in bits [0,11), G in bits [11,22), B in bits [22,32) --
    // verified directly against GLM 1.0.3's own packF2x11_1x10 encoder,
    // which this format's Vulkan/GL/D3D-shared packed-float layout mirrors
    // bit-for-bit) -- reused rather than a hand-rolled decoder, per this
    // repo's "don't reinvent the wheel" rule.
    glm::vec3 decoded = glm::unpackF2x11_1x10(packed);

    // The core claim: the value is NOT clamped to [0,1) the way a UNORM
    // target would (row 1's own value-survival criterion) -- every channel
    // reads back well above 1.0, close to the 4.31875 that was written.
    CHECK(decoded.r > 4.0F);
    CHECK(decoded.g > 4.0F);
    CHECK(decoded.b > 4.0F);
    CHECK(decoded.r == doctest::Approx(kValue).epsilon(0.01));
    CHECK(decoded.g == doctest::Approx(kValue).epsilon(0.01));
    // B's own precision is documented as coarser -- give it a looser
    // (but still tight, HDR-value-preserving) tolerance rather than
    // asserting it matches R/G's.
    CHECK(decoded.b == doctest::Approx(kValue).epsilon(0.02));

    // R and G share an identical 11-bit encoding, applied to an identical
    // input value -- they must decode to the exact same bits.
    CHECK(decoded.r == decoded.g);

    // THE HONEST PRECISION PROOF [task brief: "B10G11R11 has no sign bit
    // and reduced blue mantissa -- the tests must prove the chosen format
    // against the criteria honestly"]: blue's coarser 5-bit mantissa must
    // produce a STRICTLY larger rounding error against the true written
    // value than red/green's 6-bit mantissa does, for this specific input
    // (see the value-choice rationale above -- this is not a >= that could
    // pass by coincidence, it is a > that was engineered to hold under
    // either plausible hardware rounding convention).
    const float errorR = std::abs(decoded.r - kValue);
    const float errorB = std::abs(decoded.b - kValue);
    CHECK(errorB > errorR);
    MESSAGE("kHdrFormat precision proof: input=", kValue, " decodedR=", decoded.r, " decodedB=", decoded.b,
            " errorR=", errorR, " errorB=", errorB);
}

TEST_CASE(
    "SceneColorGpu: the same >1.0 value clamps to ~1.0 against a deliberately wrong UNORM8-family target "
    "(discrimination proof)") {
    auto fixture = makeFixture("rx_graph_scene_color_mutant");
    if (!fixture.has_value()) {
        return;
    }

    // [matrix row 1] "the same test run against a deliberately wrong
    // UNORM8-family target must show the value clamped to ~1.0, proving
    // the test actually discriminates rather than passing by
    // construction." Same mechanism (renderAndReadbackTexel), same input
    // magnitude, ONLY the target format differs.
    constexpr float kValue = 4.31875F;
    auto raw = renderAndReadbackTexel(*fixture, VK_FORMAT_R8G8B8A8_UNORM, glm::vec4(kValue, kValue, kValue, 1.0F), 4);
    REQUIRE(raw.has_value());

    // UNORM8 clamps to [0,1] before quantizing to a byte -- any value
    // >= 1.0 becomes exactly byte 255.
    CHECK((*raw)[0] == 255);
    CHECK((*raw)[1] == 255);
    CHECK((*raw)[2] == 255);
}

TEST_CASE(
    "SceneColorGpu: the escape hatch (kHdrFormatHighPrecision) preserves a negative value and decodes with "
    "symmetric channel precision, unlike kHdrFormat") {
    auto fixture = makeFixture("rx_graph_scene_color_escape_hatch");
    if (!fixture.has_value()) {
        return;
    }

    // [Task 3 (#39), rulings.md: "...with a documented A16B16G16R16F escape
    // hatch where precision demands" -- task brief: "the escape hatch must
    // be exercised by at least one test"] Push a NEGATIVE value -- the one
    // thing kHdrFormat (unsigned-only) cannot represent AT ALL, at any
    // precision, demonstrating structurally (not just "it's higher
    // precision") why the escape hatch exists. Also reuses R/G's own
    // kValue from the precision-asymmetry test above for B, to directly
    // contrast: kHdrFormatHighPrecision's B channel gets the SAME
    // treatment as its R/G channels (symmetric 10-bit mantissa across all
    // three), unlike kHdrFormat's proven blue deficit.
    constexpr float kNegative = -2.5F;
    constexpr float kValue = 4.31875F;
    constexpr float kAlpha = 0.75F;  // kHdrFormat's alpha is definitionally
                                      // always 1.0 -- this format's alpha
                                      // channel actually carries this value.
    auto raw = renderAndReadbackTexel(*fixture, kHdrFormatHighPrecision,
                                       glm::vec4(kNegative, kValue, kValue, kAlpha), 8);
    REQUIRE(raw.has_value());

    uint64_t packed = 0;
    std::memcpy(&packed, raw->data(), sizeof(packed));
    // glm::unpackHalf4x16 decodes VK_FORMAT_R16G16B16A16_SFLOAT's own
    // memory layout directly (R in the lowest 16 bits through A in the
    // highest, matching Vulkan's R,G,B,A component-order-is-memory-order
    // convention for this non-packed format) -- again GLM's own reference
    // implementation, not a hand-rolled half-float decoder.
    glm::vec4 decoded = glm::unpackHalf4x16(packed);

    // The negative value survives exactly (a Vulkan mandatory feature, and
    // half-precision floats represent -2.5 with zero rounding error --
    // -2.5 = -1.25 * 2^1, mantissa fraction 0.25 exactly representable in
    // 10 bits).
    CHECK(decoded.r == doctest::Approx(kNegative).epsilon(0.001));
    CHECK(decoded.a == doctest::Approx(kAlpha).epsilon(0.001));

    // Symmetric precision: G and B (both driven from the identical kValue
    // input, both this format's own 10-bit mantissa) decode to the exact
    // same bits -- no blue-specific deficit the way kHdrFormat has.
    CHECK(decoded.g == decoded.b);
    CHECK(decoded.g == doctest::Approx(kValue).epsilon(0.001));
}

TEST_CASE(
    "SceneColorGpu: kHdrFormat and kHdrFormatHighPrecision empirically support COLOR_ATTACHMENT + "
    "linear-filtered sampling on this driver") {
    auto fixture = makeFixture("rx_graph_scene_color_format_support");
    if (!fixture.has_value()) {
        return;
    }

    // [matrix row 3's own verification-health note: the exact Vulkan
    // mandatory-format-support table row could not be independently
    // re-fetched during research -- "routes the actual acceptance
    // criterion through an in-task vkGetPhysicalDeviceFormatProperties
    // empirical query instead of a memorized spec citation."] Queried on
    // whichever real ICD this binary runs against (lavapipe in CI; the
    // real NVIDIA driver when run with --validate against the default
    // ICD per this phase's real-GPU-verification constraint) -- never
    // assumed.
    const VkPhysicalDevice physicalDevice = fixture->device.physicalDevice();

    auto checkFormat = [&](VkFormat format, const char* label) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

        constexpr VkFormatFeatureFlags kRequired =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        // std::string(label), not the bare `const char*` -- doctest 2.5.3's
        // variadic INFO/MESSAGE stringifies a NAMED `const char*` LVALUE
        // incorrectly (empirically confirmed: prints "1" instead of the
        // string content; a string LITERAL argument is unaffected, so this
        // was not caught until a variable was actually used here). Wrapping
        // in std::string routes through doctest's normal std::string
        // stringification instead.
        INFO("format = ", std::string(label));
        CHECK((props.optimalTilingFeatures & kRequired) == kRequired);

        // [matrix row 10] Informational only, never asserted-required here
        // -- kHdrFormat's storage-image support is a real, documented risk
        // for a LATER compute consumer (SSR/volumetrics), not something
        // this task's own opaque-attachment usage needs. Logged so the
        // empirical answer is on record for whichever task queries it
        // next, per scene_color.h's own storage-image caveat comment.
        const bool hasColorBlend = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
        const bool hasStorageImage = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        MESSAGE("format ", std::string(label), ": COLOR_ATTACHMENT_BLEND=", hasColorBlend,
                " STORAGE_IMAGE=", hasStorageImage);
    };

    checkFormat(kHdrFormat, "kHdrFormat (B10G11R11_UFLOAT_PACK32)");
    checkFormat(kHdrFormatHighPrecision, "kHdrFormatHighPrecision (R16G16B16A16_SFLOAT)");
}
