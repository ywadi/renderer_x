// Sample 02: hot reload.
//
// Fullscreen triangle whose fragment shader is compiled AT RUNTIME by
// rx_shader's Compiler (in-process Slang -> SPIR-V) rather than pre-baked
// offline by slangc like samples/01_triangle's shaders -- and, unlike
// 01_triangle's hand-typed empty VkPipelineLayoutCreateInfo, the pipeline
// layout here always comes from rx::shader::reflect() +
// rx::rhi::PipelineLayoutBuilder::build(), driven by whatever the compiled
// shader actually declares (in this sample: zero descriptor bindings, one
// push-constant range holding a `float time`).
//
// Two modes, one shared pipeline-building path (buildPipelineFromSource /
// buildPipelineFromFile below, both routed through the same
// buildPipelineFromCompileResult core -- mirrors 01_triangle's single
// createTrianglePipeline shared by both its modes):
//
//   (default / no args) Headless correctness gate. Compiles an embedded
//   source ("source A", a solid white fullscreen triangle, no push
//   constants at all) and renders it into a dedicated offscreen image
//   (never an unacquired swapchain image -- same discipline as
//   01_triangle) across 2 frames, then reads the result back. Then
//   compiles a second embedded source ("source B", solid black),
//   swaps the pipeline, renders again, and asserts the readback actually
//   changed. Registered as ctest sample_02_hotreload_headless; exits 0/1.
//   (White/black, not e.g. red/green, is deliberate -- see the comment
//   above kEmbeddedSourceA/B below for why.)
//
//   --present. Opens a real window and runs the canonical frames-in-flight
//   present loop (rx::rhi::FrameSync, exactly like 01_triangle's
//   --present), except every ~250ms (4 Hz, plain std::filesystem::
//   last_write_time polling -- no inotify/watcher dependency [R:D1]) it
//   checks whether hotreload.slang (deployed next to this binary at build
//   time -- see CMakeLists.txt -- and located at runtime via
//   SDL_GetBasePath(), so a redistributed copy of this sample's directory
//   works identically) changed on disk. On a change: recompile via
//   Compiler::compileFromFile; on success, build the new pipeline and swap
//   it in; on failure, log Slang's diagnostics (already logged by
//   rx_shader's Compiler itself) and keep rendering the last-good
//   pipeline -- the running window never goes blank or crashes because of
//   a shader typo.
//
// Pipeline swap strategy (present mode only): Task 4 of this phase
// (DeletionQueue, fence-keyed deferred destruction) had not landed at the
// time this sample was written. Per the coordinator's explicit sign-off
// for this task, the old pipeline is retired with a plain
// vkDeviceWaitIdle() immediately before destroying it, rather than a
// DeletionQueue::retire() call -- acceptable because reloads are a rare,
// human-interactive event (one file save every few seconds at most), not
// a per-frame operation, so the stall this causes is invisible in
// practice. A later pass wiring this sample to DeletionQueue can drop the
// wait entirely; nothing else about the swap needs to change.
//
// Redistribution: this is the sample that proves the Slang runtime-lib
// deployment mechanism (rx_shader_deploy_runtime_libs(), CMakeLists.txt)
// for real -- linking rx_shader pulls in slang::slang, and this binary
// does real in-process compilation on every run (both modes), so if the
// runtime libs weren't copied next to it with a working $ORIGIN RPATH,
// Compiler::create() would fail immediately.
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>

#include <SDL3/SDL.h>
#include <volk.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kHeadlessWidth = 256;
constexpr uint32_t kHeadlessHeight = 256;
constexpr VkDeviceSize kPixelBytes = static_cast<VkDeviceSize>(kHeadlessWidth) * kHeadlessHeight * 4;
constexpr uint32_t kCenterX = kHeadlessWidth / 2;
constexpr uint32_t kCenterY = kHeadlessHeight / 2;

// --present mode's window size -- arbitrary, viewport/scissor are dynamic
// state (same reasoning as 01_triangle).
constexpr uint32_t kPresentWidth = 800;
constexpr uint32_t kPresentHeight = 600;

// ~4 Hz mtime poll, per the brief's "no new deps" instruction (plain
// std::filesystem::last_write_time, not inotify/a file-watcher library).
constexpr auto kPollInterval = std::chrono::milliseconds(250);

constexpr const char* kShaderFilename = "hotreload.slang";

const std::vector<std::string> kEntryPointNames = {"vsMain", "fsMain"};

// Embedded ctest sources: no push constants at all (deliberately simpler
// than hotreload.slang's live-editable version) -- the headless gate only
// needs two deterministic, different constant colors, and exercising the
// "shader declares zero push ranges" path is worth covering on its own.
// White/black (not, say, red/green) is deliberate and load-bearing, not a
// style choice: this device's swapchainFormat() is BGRA, and every channel
// in each color is exactly 0.0 or 1.0 -- both fixed points of the sRGB
// transfer function AND invariant under an R<->B channel swap -- so the
// readback is byte-exact no matter the component order or UNORM/SRGB
// choice (same reasoning, and the same two colors, 01_triangle's own
// headless gate relies on). Verified directly: an earlier red/green
// version of this test read back (0,0,255,255) for "red" -- correct BGRA
// bytes for red, wrong under this file's naive RGBA-order pixelAt().
constexpr const char* kEmbeddedVertexAndSharedHeader = R"(
struct VSOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vertexID : SV_VertexID)
{
    float2 positions[3] = float2[3](
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    );
    VSOut o;
    o.position = float4(positions[vertexID], 0.0, 1.0);
    return o;
}
)";

const std::string kEmbeddedSourceA = std::string(kEmbeddedVertexAndSharedHeader) + R"(
[shader("fragment")]
float4 fsMain() : SV_Target
{
    return float4(1.0, 1.0, 1.0, 1.0);
}
)";

const std::string kEmbeddedSourceB = std::string(kEmbeddedVertexAndSharedHeader) + R"(
[shader("fragment")]
float4 fsMain() : SV_Target
{
    return float4(0.0, 0.0, 0.0, 1.0);
}
)";

VkShaderModule createShaderModuleFromSpirv(VkDevice device, const std::vector<uint32_t>& code) {
    if (code.empty()) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();

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

// One reloadable pipeline's worth of state: the shader modules and
// VkPipeline built from a single successful compile+reflect+build cycle,
// plus enough of that cycle's reflected push-constant shape (at most one
// range, this sample's only supported shape) to drive vkCmdPushConstants
// generically -- `pushConstantSize == 0` means "this shader declared no
// push constant, don't push anything" (the embedded ctest sources' case).
//
// pushConstantOffset is `pushRanges[0].offset` verbatim, not assumed to be
// 0 -- post-review fix (an earlier version of this file hardcoded offset 0
// into the vkCmdPushConstants call below instead of tracking it here; that
// was only correct by coincidence, because hotreload.slang's single
// `[[vk::push_constant]]` global happens to reflect at offset 0 today, and
// nothing enforced that staying true). This sample rejects >1 push range
// (see buildPipelineFromCompileResult) but never assumes *where* the one
// range it does support starts -- honoring whatever reflect() reports is
// strictly more correct than hand-waving it away, and a live-editable
// shader (the entire point of this sample) deserves the host code
// tracking what the shader actually declares rather than silently
// mis-pushing bytes to the wrong offset the moment an edit changes it.
//
// Move-only (implicitly, via the non-copyable rx::rhi::PipelineLayoutBundle
// member) -- raw VkShaderModule/VkPipeline handles are torn down explicitly
// by destroyReloadablePipeline() below, same discipline as 01_triangle's
// TrianglePipeline; layoutBundle owns its VkDescriptorSetLayouts/
// VkPipelineLayout via RAII and is destroyed by simply being replaced/
// going out of scope.
struct ReloadablePipeline {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t pushConstantOffset = 0;
    uint32_t pushConstantSize = 0;
    VkShaderStageFlags pushConstantStages = 0;
};

void destroyReloadablePipeline(VkDevice device, ReloadablePipeline& p) {
    if (p.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, p.pipeline, nullptr);
    }
    if (p.fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, p.fragModule, nullptr);
    }
    if (p.vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, p.vertModule, nullptr);
    }
    // Move-assigning a fresh, empty bundle over the old one destroys the
    // old one's VkPipelineLayout/VkDescriptorSetLayouts first (see
    // PipelineLayoutBundle::operator=), then takes on the empty state --
    // exactly the teardown this function needs, with no separate API.
    p.layoutBundle = rx::rhi::PipelineLayoutBundle{};
    p.pipeline = VK_NULL_HANDLE;
    p.fragModule = VK_NULL_HANDLE;
    p.vertModule = VK_NULL_HANDLE;
    p.pushConstantOffset = 0;
    p.pushConstantSize = 0;
    p.pushConstantStages = 0;
}

// The shared core: given an already-attempted CompileResult (from either
// compileFromSource or compileFromFile -- see the two thin wrappers below),
// walks its reflection, builds a matching VkPipelineLayout (never a
// hand-typed one), creates the two shader modules, and builds the dynamic-
// rendering VkPipeline targeting `colorFormat`. Returns nullopt on any
// failure, having cleaned up whatever was partially created -- callers
// never need to call destroyReloadablePipeline() themselves on a nullopt
// return, same contract as 01_triangle's createTrianglePipeline.
//
// Diagnostics for an outright compile failure are already logged by
// rx_shader's Compiler itself (RX_LOG_WARN on success-with-warnings,
// RX_LOG_ERROR on failure -- see compiler.cpp); everything logged directly
// in this function covers failures *past* that point (reflection, layout
// building, this sample's own "at most one push range" contract, shader
// module / pipeline creation).
std::optional<ReloadablePipeline> buildPipelineFromCompileResult(VkDevice device,
                                                                  const rx::shader::CompileResult& compileResult,
                                                                  VkFormat colorFormat) {
    if (!compileResult.ok) {
        return std::nullopt;
    }
    if (compileResult.entryPointCode.size() != kEntryPointNames.size()) {
        RX_LOG_ERROR("sample_02_hotreload: expected {} entry points (vsMain, fsMain), got {}",
                     kEntryPointNames.size(), compileResult.entryPointCode.size());
        return std::nullopt;
    }

    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("sample_02_hotreload: reflect() failed on an otherwise-successful compile");
        return std::nullopt;
    }
    if (!layoutInfo->bindings.empty()) {
        RX_LOG_ERROR(
            "sample_02_hotreload: shader declares {} descriptor binding(s); this sample supports none "
            "(fullscreen color pattern only -- see samples/03_bindless_mesh for textured/bindless draws)",
            layoutInfo->bindings.size());
        return std::nullopt;
    }
    if (layoutInfo->pushRanges.size() > 1) {
        RX_LOG_ERROR("sample_02_hotreload: shader declares {} push-constant ranges; this sample supports at most 1",
                     layoutInfo->pushRanges.size());
        return std::nullopt;
    }

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, *layoutInfo);
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("sample_02_hotreload: PipelineLayoutBuilder::build failed");
        return std::nullopt;
    }

    ReloadablePipeline result;
    result.layoutBundle = std::move(*layoutBundle);
    if (!layoutInfo->pushRanges.empty()) {
        result.pushConstantOffset = layoutInfo->pushRanges[0].offset;
        result.pushConstantSize = layoutInfo->pushRanges[0].size;
        result.pushConstantStages = layoutInfo->pushRanges[0].stages;
    }

    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModule module = createShaderModuleFromSpirv(device, blob.code);
        if (module == VK_NULL_HANDLE) {
            RX_LOG_ERROR("sample_02_hotreload: vkCreateShaderModule failed for entry point '{}'",
                         blob.entryPointName);
            destroyReloadablePipeline(device, result);
            return std::nullopt;
        }
        if (blob.entryPointName == "vsMain") {
            result.vertModule = module;
        } else if (blob.entryPointName == "fsMain") {
            result.fragModule = module;
        } else {
            RX_LOG_ERROR("sample_02_hotreload: unexpected entry point '{}'", blob.entryPointName);
            vkDestroyShaderModule(device, module, nullptr);
            destroyReloadablePipeline(device, result);
            return std::nullopt;
        }
    }
    if (result.vertModule == VK_NULL_HANDLE || result.fragModule == VK_NULL_HANDLE) {
        RX_LOG_ERROR("sample_02_hotreload: missing vsMain/fsMain shader module after a successful compile");
        destroyReloadablePipeline(device, result);
        return std::nullopt;
    }

    // Slang's SPIR-V backend emits each entry point as its own standalone
    // module (one getEntryPointCode() call per entry point, exactly what
    // rx_shader's Compiler does -- see compiler.cpp) with its
    // OpEntryPoint's name always "main", regardless of the source-level
    // function name ("vsMain"/"fsMain" here) -- verified directly: naming
    // these "vsMain"/"fsMain" here instead produces
    // VUID-VkPipelineShaderStageCreateInfo-pName-00707 ("No entrypoint
    // found named `vsMain`") at pipeline creation. This matches
    // shaders/triangle.vert.slang / .frag.slang's own entry functions,
    // which are literally named "main" in source for the same reason.
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
        RX_LOG_ERROR("sample_02_hotreload: vkCreateGraphicsPipelines failed");
        destroyReloadablePipeline(device, result);
        return std::nullopt;
    }

    return result;
}

std::optional<ReloadablePipeline> buildPipelineFromSource(VkDevice device, rx::shader::Compiler& compiler,
                                                           const std::string& moduleName, const std::string& source,
                                                           VkFormat colorFormat) {
    return buildPipelineFromCompileResult(device, compiler.compileFromSource(moduleName, source, kEntryPointNames),
                                           colorFormat);
}

// Deliberately creates its OWN fresh Compiler (and, underneath it, a fresh
// slang::ISession) on every call rather than accepting one from the
// caller -- verified directly to be load-bearing, not defensive
// over-engineering: reusing a single Compiler/session across repeated
// compileFromFile calls on the SAME path (which always derives the same
// module name from the file's stem -- see compiler.cpp) makes the SECOND
// reload of an edited file fail with Slang's own
// "module already loaded with different source" diagnostic, since a
// session apparently caches loaded modules by name and refuses to accept
// new source under a name it already has different source cached for. A
// brand-new ISession has no such cache, so this sidesteps the issue
// entirely. The process-wide IGlobalSession (the genuinely expensive part
// -- stdlib load) is unaffected: Compiler::create() only pays that cost
// once per process, reusing it here exactly as documented. Creating a new
// ISession per reload is the "cheap...session per target config" cost
// compiler.h's own comment describes -- acceptable given reloads are a
// rare, human-interactive event (one file save at most every few
// seconds), never a per-frame operation.
std::optional<ReloadablePipeline> buildPipelineFromFile(VkDevice device, const std::string& path,
                                                          VkFormat colorFormat) {
    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        RX_LOG_ERROR("sample_02_hotreload: rx::shader::Compiler::create failed");
        return std::nullopt;
    }
    return buildPipelineFromCompileResult(device, compiler->compileFromFile(path, kEntryPointNames), colorFormat);
}

// Pushes `timeSeconds` into the pipeline's reflected push-constant range
// (if it declared one -- see ReloadablePipeline's comment) before a draw
// call. A no-op for a shader with zero push ranges (the embedded ctest
// sources), so this same helper is safe to call unconditionally from both
// modes' render code.
//
// `data` is sized to exactly the reflected range's byte length and pushed
// at exactly its reflected offset (p.pushConstantOffset) -- not
// hardcoded to offset 0 -- so this stays correct even if a future shader
// edit reflects a nonzero offset (e.g. more than one global ends up
// sharing the push-constant block in some future edit); `time` still
// lands at the start of `data`, which is the struct-relative offset 0
// PushConstants::time actually has, regardless of where that struct's
// bytes start within the overall push-constant block.
void pushTimeIfDeclared(VkCommandBuffer cmd, const ReloadablePipeline& p, float timeSeconds) {
    if (p.pushConstantSize == 0) {
        return;
    }
    std::vector<uint8_t> data(p.pushConstantSize, 0);
    if (data.size() >= sizeof(float)) {
        std::memcpy(data.data(), &timeSeconds, sizeof(float));
    }
    vkCmdPushConstants(cmd, p.layoutBundle.layout, p.pushConstantStages, p.pushConstantOffset, p.pushConstantSize,
                       data.data());
}

// Per-swapchain-image VkImageViews for --present mode -- identical
// mechanism to 01_triangle's createSwapchainViews/destroySwapchainViews.
bool createSwapchainViews(VkDevice device, const rx::rhi::Device& rhiDevice, std::vector<VkImageView>& views) {
    views.assign(rhiDevice.swapchainImages().size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < views.size(); ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = rhiDevice.swapchainImages()[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = rhiDevice.swapchainFormat();
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &viewInfo, nullptr, &views[i]) != VK_SUCCESS) {
            RX_LOG_ERROR("sample_02_hotreload: vkCreateImageView(swapchain image {}) failed", i);
            return false;
        }
    }
    return true;
}

void destroySwapchainViews(VkDevice device, std::vector<VkImageView>& views) {
    for (VkImageView view : views) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
        }
    }
    views.clear();
}

// Resolves hotreload.slang's path next to this executable at runtime, so a
// redistributed copy of this sample's build-tree directory (binary +
// hotreload.slang + the deployed Slang runtime libs) works identically to
// running it in the build tree -- SDL_GetBasePath() (already a dependency
// via rx_platform's SDL3 link) returns the executable's own directory,
// cross-platform, with no argv[0]-parsing fragility. Falls back to the
// current working directory (logged) if SDL can't determine it.
std::string resolveShaderPath() {
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        RX_LOG_WARN("sample_02_hotreload: SDL_GetBasePath failed ({}); looking for {} in the current directory "
                    "instead",
                    SDL_GetError(), kShaderFilename);
        return kShaderFilename;
    }
    return std::string(basePath) + kShaderFilename;
}

// --- Headless mode: offscreen render + pixel readback ----------------------
int runHeadless(bool enableValidation) {
    auto window = rx::platform::Window::create("rx_hotreload_sample", static_cast<int>(kHeadlessWidth),
                                                 static_cast<int>(kHeadlessHeight), /*visible=*/false);
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
    // Context::create() in context.h; a single windowed, validated Context
    // (same extension set --present mode would use) sidesteps the
    // vk-bootstrap process-wide function-pointer caching hazard entirely.
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

    // Device::create takes ownership of `surface` unconditionally. Builds
    // and queries a real swapchain against the window's surface; this
    // sample never writes to any of swapchainImages(), only reads
    // swapchainFormat() below (same discipline as 01_triangle).
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

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        RX_LOG_ERROR("rx::shader::Compiler::create failed");
        return 1;
    }

    const VkDevice vkDevice = device->device();
    const VkFormat targetFormat = device->swapchainFormat();

    VkImage offscreenImage = VK_NULL_HANDLE;
    VkDeviceMemory offscreenMemory = VK_NULL_HANDLE;
    VkImageView offscreenView = VK_NULL_HANDLE;
    ReloadablePipeline pipeline;

    auto destroyRawResources = [&]() {
        destroyReloadablePipeline(vkDevice, pipeline);
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

    // --- Offscreen render target: 256x256, device-local (identical setup
    // to 01_triangle's runHeadless) ---------------------------------------
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = targetFormat;
    imageInfo.extent = {kHeadlessWidth, kHeadlessHeight, 1};
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

    auto readback = allocator->createHostVisibleBuffer(kPixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!readback.has_value()) {
        RX_LOG_ERROR("createHostVisibleBuffer(readback) failed");
        destroyRawResources();
        return 1;
    }

    // Same host-coherence precondition check as 01_triangle: Buffer/
    // Allocator expose no vmaFlush/InvalidateAllocation surface yet (a
    // tracked Phase 2 finding), so this readback relies on every
    // HOST_VISIBLE memory type also being HOST_COHERENT.
    VkPhysicalDeviceMemoryProperties hostMemProps{};
    vkGetPhysicalDeviceMemoryProperties(device->physicalDevice(), &hostMemProps);
    for (uint32_t i = 0; i < hostMemProps.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = hostMemProps.memoryTypes[i].propertyFlags;
        const bool hostVisible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U;
        const bool hostCoherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U;
        if (hostVisible && !hostCoherent) {
            RX_LOG_ERROR("host-visible memory type {} is not host-coherent; readback would need an explicit "
                         "invalidate",
                         i);
            destroyRawResources();
            return 1;
        }
    }

    // Renders `frameCount` frames of `pipeline` into offscreenImage
    // (transitioning from whatever layout it is currently in, tracked by
    // the caller) and reads the result back to the host. Runs entirely in
    // one CommandContext::runOnce() submission, which already
    // vkQueueWaitIdle()s before returning.
    VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auto renderAndReadback = [&](uint32_t frameCount) -> std::vector<uint8_t> {
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            rx::rhi::transitionImage(cmd, offscreenImage, currentLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            for (uint32_t frame = 0; frame < frameCount; ++frame) {
                VkRenderingAttachmentInfo colorAttachment{};
                colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                colorAttachment.imageView = offscreenView;
                colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colorAttachment.clearValue.color = VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}};

                VkRenderingInfo renderingInfo{};
                renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                renderingInfo.renderArea = VkRect2D{{0, 0}, {kHeadlessWidth, kHeadlessHeight}};
                renderingInfo.layerCount = 1;
                renderingInfo.colorAttachmentCount = 1;
                renderingInfo.pColorAttachments = &colorAttachment;

                vkCmdBeginRendering(cmd, &renderingInfo);

                VkViewport viewport{0.0F, 0.0F, static_cast<float>(kHeadlessWidth), static_cast<float>(kHeadlessHeight),
                                     0.0F, 1.0F};
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                VkRect2D scissor{{0, 0}, {kHeadlessWidth, kHeadlessHeight}};
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
                pushTimeIfDeclared(cmd, pipeline, 0.0F);
                vkCmdDraw(cmd, 3, 1, 0, 0);

                vkCmdEndRendering(cmd);

                // RX_FRAME_MARK once per rendered frame [Phase 4 Stage 0
                // Task 3, spec D3] -- headless-mode path: each iteration of
                // this loop re-clears and redraws the same offscreen
                // attachment, i.e. renders one frame's worth of content,
                // even though all `frameCount` iterations share a single
                // runOnce() submission.
                RX_FRAME_MARK;
            }

            rx::rhi::transitionImage(cmd, offscreenImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {kHeadlessWidth, kHeadlessHeight, 1};
            vkCmdCopyImageToBuffer(cmd, offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                                    &region);
        });
        currentLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        std::vector<uint8_t> pixels(static_cast<size_t>(kPixelBytes));
        std::memcpy(pixels.data(), readback->mappedData(), pixels.size());
        return pixels;
    };

    // --- Compile source A, render, read back ------------------------------
    auto pipelineA = buildPipelineFromSource(vkDevice, *compiler, "HotreloadSourceA", kEmbeddedSourceA, targetFormat);
    if (!pipelineA.has_value()) {
        RX_LOG_ERROR("failed to build the pipeline for embedded source A");
        destroyRawResources();
        return 1;
    }
    pipeline = std::move(*pipelineA);

    std::vector<uint8_t> colorA = renderAndReadback(/*frameCount=*/2);

    // --- Compile source B (different constant color), swap, render, read
    // back ------------------------------------------------------------------
    auto pipelineB = buildPipelineFromSource(vkDevice, *compiler, "HotreloadSourceB", kEmbeddedSourceB, targetFormat);
    if (!pipelineB.has_value()) {
        RX_LOG_ERROR("failed to build the pipeline for embedded source B");
        destroyRawResources();
        return 1;
    }
    // See this file's header comment: Task 4's DeletionQueue hasn't landed,
    // so a plain vkDeviceWaitIdle before destroying the old pipeline is the
    // coordinator-approved interim swap strategy. Nothing is actually in
    // flight here (runOnce() above already waited), but this exercises the
    // exact same swap code path --present mode uses.
    if (vkDeviceWaitIdle(vkDevice) != VK_SUCCESS) {
        RX_LOG_ERROR("vkDeviceWaitIdle failed before pipeline swap");
        destroyRawResources();
        return 1;
    }
    destroyReloadablePipeline(vkDevice, pipeline);
    pipeline = std::move(*pipelineB);

    std::vector<uint8_t> colorB = renderAndReadback(/*frameCount=*/2);

    // Raw Vulkan objects are done being used now -- tear them down while
    // `device` is still alive, before any RAII objects unwind (same
    // discipline as 01_triangle).
    destroyRawResources();

    auto pixelAt = [](const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y) -> const uint8_t* {
        return pixels.data() + (static_cast<size_t>(y) * kHeadlessWidth + x) * 4;
    };

    const uint8_t* a = pixelAt(colorA, kCenterX, kCenterY);
    const uint8_t* b = pixelAt(colorB, kCenterX, kCenterY);

    bool pass = true;
    // Source A: solid white.
    for (int c = 0; c < 3; ++c) {
        if (a[c] <= 200) {
            RX_LOG_ERROR("source A center pixel channel {} = {} (expected > 200)", c, a[c]);
            pass = false;
        }
    }
    // Source B: solid black.
    for (int c = 0; c < 3; ++c) {
        if (b[c] >= 20) {
            RX_LOG_ERROR("source B center pixel channel {} = {} (expected < 20)", c, b[c]);
            pass = false;
        }
    }
    // The whole point of this gate: reloading the shader actually changed
    // what gets rendered.
    if (std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0])) < 100) {
        RX_LOG_ERROR("readback did not change between source A and source B -- hot reload had no effect");
        pass = false;
    }
    if (enableValidation && context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during this run");
        pass = false;
    }

    if (pass) {
        RX_LOG_INFO("hotreload headless gate PASSED");
        return 0;
    }
    RX_LOG_ERROR("hotreload headless gate FAILED");
    return 1;
}

// --- --present mode: real window, live shader editing -----------------------
int runPresent(bool enableValidation) {
    auto window = rx::platform::Window::create("rx_hotreload_sample (--present)", static_cast<int>(kPresentWidth),
                                                 static_cast<int>(kPresentHeight), /*visible=*/true);
    if (!window.has_value()) {
        RX_LOG_ERROR("Window::create failed: no display backend available");
        return 1;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        RX_LOG_ERROR("video driver reports no Vulkan surface extensions (e.g. dummy driver)");
        return 1;
    }

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

    auto device = rx::rhi::Device::create(*context, surface);
    if (!device.has_value()) {
        RX_LOG_ERROR("Device::create failed");
        return 1;
    }
    const VkDevice vkDevice = device->device();

    auto frameSync = rx::rhi::FrameSync::create(vkDevice, device->graphicsQueueFamily(),
                                                 static_cast<uint32_t>(device->swapchainImages().size()));
    if (!frameSync.has_value()) {
        RX_LOG_ERROR("FrameSync::create failed");
        return 1;
    }

    const std::string shaderPath = resolveShaderPath();
    RX_LOG_INFO("sample_02_hotreload: watching {} for changes (~4Hz poll); edit it and save while this window is "
                "open",
                shaderPath);

    auto pipelineResult = buildPipelineFromFile(vkDevice, shaderPath, device->swapchainFormat());
    if (!pipelineResult.has_value()) {
        RX_LOG_ERROR("failed to compile the initial shader at {} -- see diagnostics above; this file should ship "
                     "next to the binary and compile cleanly out of the box",
                     shaderPath);
        return 1;
    }
    ReloadablePipeline pipeline = std::move(*pipelineResult);

    std::error_code mtimeError;
    std::optional<std::filesystem::file_time_type> lastKnownMtime;
    auto initialMtime = std::filesystem::last_write_time(shaderPath, mtimeError);
    if (!mtimeError) {
        lastKnownMtime = initialMtime;
    }

    std::vector<VkImageView> swapchainViews;
    if (!createSwapchainViews(vkDevice, *device, swapchainViews)) {
        destroySwapchainViews(vkDevice, swapchainViews);
        destroyReloadablePipeline(vkDevice, pipeline);
        return 1;
    }

    RX_LOG_INFO("--present: window open ({}x{}); close the window to exit", kPresentWidth, kPresentHeight);

    // Checked once per loop iteration but internally rate-limited to
    // ~kPollInterval -- see this file's header comment for why a plain
    // vkDeviceWaitIdle swap is acceptable here.
    auto pollAndReloadIfChanged = [&]() {
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(shaderPath, ec);
        if (ec) {
            // File momentarily missing/unreadable (e.g. an editor mid-save)
            // -- try again next poll tick, not fatal.
            return;
        }
        if (lastKnownMtime.has_value() && mtime == *lastKnownMtime) {
            return;
        }
        lastKnownMtime = mtime;

        RX_LOG_INFO("sample_02_hotreload: {} changed on disk, recompiling...", shaderPath);
        auto newPipeline = buildPipelineFromFile(vkDevice, shaderPath, device->swapchainFormat());
        if (!newPipeline.has_value()) {
            RX_LOG_WARN("sample_02_hotreload: reload failed (see diagnostics above); keeping the last-good "
                        "pipeline");
            return;
        }

        if (vkDeviceWaitIdle(vkDevice) != VK_SUCCESS) {
            RX_LOG_ERROR("sample_02_hotreload: vkDeviceWaitIdle failed before pipeline swap; keeping the old "
                         "pipeline");
            return;
        }
        destroyReloadablePipeline(vkDevice, pipeline);
        pipeline = std::move(*newPipeline);
        RX_LOG_INFO("sample_02_hotreload: reload succeeded, pipeline swapped");
    };

    const auto startTime = std::chrono::steady_clock::now();
    auto lastPollTime = startTime;

    bool ok = true;
    bool quit = false;
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                quit = true;
            }
        }
        if (quit) {
            break;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastPollTime >= kPollInterval) {
            lastPollTime = now;
            pollAndReloadIfChanged();
        }

        VkFence fence = frameSync->currentFence();
        if (vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            RX_LOG_ERROR("vkWaitForFences failed in present loop");
            ok = false;
            break;
        }

        auto acquire = device->acquireNextImage(frameSync->currentImageAvailableSemaphore());
        if (acquire.status == rx::rhi::SwapchainStatus::NeedsRecreate) {
            if (vkDeviceWaitIdle(vkDevice) != VK_SUCCESS) {
                RX_LOG_ERROR("vkDeviceWaitIdle failed before swapchain recreation");
                ok = false;
                break;
            }
            destroySwapchainViews(vkDevice, swapchainViews);
            if (!device->recreateSwapchain(surface) ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())) ||
                !createSwapchainViews(vkDevice, *device, swapchainViews)) {
                RX_LOG_ERROR("swapchain recreation failed after acquireNextImage NeedsRecreate");
                ok = false;
                break;
            }
            continue;
        }
        if (acquire.status == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("device lost during acquireNextImage; exiting present loop");
            ok = false;
            break;
        }

        if (vkResetFences(vkDevice, 1, &fence) != VK_SUCCESS) {
            RX_LOG_ERROR("vkResetFences failed in present loop");
            ok = false;
            break;
        }

        VkCommandPool pool = frameSync->currentCommandPool();
        if (vkResetCommandPool(vkDevice, pool, 0) != VK_SUCCESS) {
            RX_LOG_ERROR("vkResetCommandPool failed in present loop");
            ok = false;
            break;
        }
        VkCommandBuffer cmd = frameSync->currentCommandBuffer();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
            RX_LOG_ERROR("vkBeginCommandBuffer failed in present loop");
            ok = false;
            break;
        }

        VkImage swapchainImage = device->swapchainImages()[acquire.imageIndex];
        rx::rhi::transitionImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        const VkExtent2D extent = device->swapchainExtent();

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = swapchainViews[acquire.imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = VkRect2D{{0, 0}, extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F,
                             1.0F};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
        const float timeSeconds = std::chrono::duration<float>(now - startTime).count();
        pushTimeIfDeclared(cmd, pipeline, timeSeconds);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRendering(cmd);

        rx::rhi::transitionImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            RX_LOG_ERROR("vkEndCommandBuffer failed in present loop");
            ok = false;
            break;
        }

        VkSemaphore waitSem = frameSync->currentImageAvailableSemaphore();
        VkSemaphore signalSem = frameSync->renderFinishedSemaphore(acquire.imageIndex);
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSem;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSem;

        if (vkQueueSubmit(device->graphicsQueue(), 1, &submitInfo, fence) != VK_SUCCESS) {
            RX_LOG_ERROR("vkQueueSubmit failed in present loop");
            ok = false;
            break;
        }

        auto presentStatus = device->present(acquire.imageIndex, signalSem);
        if (presentStatus == rx::rhi::SwapchainStatus::NeedsRecreate) {
            if (vkDeviceWaitIdle(vkDevice) != VK_SUCCESS) {
                RX_LOG_ERROR("vkDeviceWaitIdle failed before swapchain recreation");
                ok = false;
                break;
            }
            destroySwapchainViews(vkDevice, swapchainViews);
            if (!device->recreateSwapchain(surface) ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())) ||
                !createSwapchainViews(vkDevice, *device, swapchainViews)) {
                RX_LOG_ERROR("swapchain recreation failed after present NeedsRecreate");
                ok = false;
                break;
            }
        } else if (presentStatus == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("device lost during present; exiting present loop");
            ok = false;
            break;
        }

        // RX_FRAME_MARK once per rendered frame [Phase 4 Stage 0 Task 3,
        // spec D3] -- present-mode path, right after this frame's present/
        // submit has completed.
        RX_FRAME_MARK;
        frameSync->advanceFrame();
    }

    // Shutdown: vkDeviceWaitIdle BEFORE destroying FrameSync (its
    // destructor runs when this function returns), the per-image views,
    // and the pipeline -- the only point any of these may legally die (see
    // frame_sync.h's destructor contract).
    vkDeviceWaitIdle(vkDevice);
    destroySwapchainViews(vkDevice, swapchainViews);
    destroyReloadablePipeline(vkDevice, pipeline);

    if (enableValidation && context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during the present loop");
        return 1;
    }
    if (!ok) {
        return 1;
    }
    RX_LOG_INFO("--present: window closed cleanly");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    rx::core::log::init();

    bool presentMode = false;
    bool enableValidation = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--present") {
            presentMode = true;
        } else if (std::string_view(argv[i]) == "--validate") {
            enableValidation = true;
        }
    }

    if (presentMode) {
        return runPresent(enableValidation);
    }
    return runHeadless(enableValidation);
}
