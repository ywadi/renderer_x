// Sample 09: scene fly-through + stress-v2 + release prep [Phase 4 Stage 2
// Task 24, the phase-exit sample -- gate matrix-issue15-sample09.md as
// amended by gate/rulings-2026-08-18.md's #15 section]. Registry ->
// rx::scene::Scene -> rx::scene::DrawListBuilder -> rx::graph -- the FIRST
// real production consumer of Scene/DrawListBuilder/rx_debug_ui::Overlay/
// rx_platform's Task 20 input surface outside their own test suites.
//
// SCENE CONTENT: headless/CI and present-mode default alike = an instanced
// DamagedHelmet grid (4 rows x 4 cols = 16 instances, ONE real glTF import
// shared by every instance's own Scene::createRenderable() call -- see
// buildHelmetGrid() below). `--scene sponza` overrides present mode to the
// fetched Sponza asset (tools/fetch_assets.sh --sponza; CI never fetches
// it, D16).
//
// DETERMINISTIC CULLING VIA LAYER MASK, NOT FRUSTUM GEOMETRY: the headless
// gate's EXACT counter assertions (matrix row 24) need a culled/visible
// split that is exact and platform-independent. Frustum-vs-AABB culling is
// real floating-point geometry (this sample DOES exercise it too, via
// buildShadow()'s own extruded ortho box and DrawListBuilder's own
// cullingFrustumPlanes() test -- never bypassed), but pinning the ONE
// counter this ctest gate hard-asserts to a frustum-boundary computation
// would make the gate's own correctness depend on exact FOV/AABB trig
// reproducing bit-for-bit across every platform this project targets. This
// sample instead assigns one LAYER BIT per grid row (`1u << row`) and sets
// the camera's own default `cullMask` to include only rows [0,
// kVisibleRowCount) -- the SAME mechanism the HUD's own "layer-mask
// instance-group toggles" feature (matrix row 20) exposes interactively,
// not two parallel mechanisms serving one purpose each.
//
// D27 PRE-RESOLUTION + CHUNKED RECORDING: `rx::scene::resolveDrawGroups()`
// is called exactly once per frame, on the main thread, BEFORE
// `executor->execute()` -- never `rx::scene::recordDrawList()`'s own
// internal `scheduler.parallelFor()` fan-out, which would nest a SECOND,
// independent parallel-recording mechanism inside the render graph's own
// executor-driven chunk fan-out (`Pass::setExecuteChunked()`, which already
// supplies the one real per-chunk `VkCommandBuffer` this sample needs via
// `PassContext::chunkCommandBuffer()`). `resolveDrawGroups()`'s own
// materialIndex-adjacency scan has no `blockId` awareness (see
// draw_recording.h's own header comment) -- this sample's own
// `rx::samples9::splitByBlockAndGroup()` closes that gap on the recording
// side, unit-tested directly (tests/test_draw_recording.cpp) with a
// synthetic group-spans-two-blocks fixture.
//
// D26.1 DRAW-DATA ADDRESSING: one `DrawDataGpu` row PER `ViewLists::
// payloads` ENTRY (not per Scene renderable) -- `DrawCommand::firstInstance`
// addresses `payloads[]` directly, and a D26.3-collapsed instanced draw's
// hardware instances (`SV_VulkanInstanceID` in `[firstInstance,
// firstInstance+instanceCount)`) each need their own row, since hardware
// hardware instancing auto-increments `gl_InstanceIndex` per copy and
// forward_entry.slang reads `gDrawData[...][SV_VulkanInstanceID]` verbatim
// -- this is genuine GPU-side instancing (`instanceCount > 1` in a single
// `vkCmdDrawIndexed` call), the first real consumer of that path in this
// codebase (every earlier sample issues `instanceCount=1` unconditionally).
//
// STRESS-V2 (`--stress`): a self-contained, Registry-free instanced field
// (procedural cube geometry via `GeometryPool::upload()` directly, 4
// Unlit material variants via 2 alphaModes x 2 doubleSided states, D28's
// real fixed-function axis) driven through the SAME Scene/DrawListBuilder/
// chunked-executor path -- `asset::Registry::registerMesh()` is private,
// friend-scoped to import_gltf.cpp alone (verified directly against
// registry.h), so a Registry-free workload supplies its OWN
// `MeshSubmeshesFn`/`MaterialResolveFn`/`MeshBoundsFn` closures instead of
// `xFromRegistry()`'s production adapters -- exactly the injected-seam
// escape hatch draw_list.h's own top comment documents ("this library's
// own tests bind a trivial in-test callable instead"), applied here at
// sample scope rather than invented. See buildStressField() below.
#include <rx_asset/geometry_pool.h>
#include <rx_asset/registry.h>
#include <rx_asset/texture_cache.h>
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <rx_debug_ui/overlay.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_material/draw_data.h>
#include <rx_material/material_system.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/descriptor_arena.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_rhi_vk/memory_report.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_rhi_vk/upload.h>
#include <rx_scene/camera.h>
#include <rx_scene/draw_list.h>
#include <rx_scene/scene.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <rx_shadow/shadow_caster_pipeline.h>
#include <rx_shadow/shadow_frustum.h>
#include <rx_task/scheduler.h>

#include "draw_recording.h"
#include "fly_camera.h"
#include <reference_gate.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <volk.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// --- Constants --------------------------------------------------------------
constexpr uint32_t kHeadlessWidth = 256;
constexpr uint32_t kHeadlessHeight = 256;
constexpr VkDeviceSize kHeadlessPixelBytes = static_cast<VkDeviceSize>(kHeadlessWidth) * kHeadlessHeight * 4;

constexpr uint32_t kPresentWidth = 1280;
constexpr uint32_t kPresentHeight = 720;

constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kShadowMapResolution = 1024;

// Helmet-grid composition -- see this file's own header comment ("layer
// mask, not frustum geometry") for the full deterministic-culling
// rationale this feeds.
constexpr uint32_t kGridRows = 4;
constexpr uint32_t kGridCols = 4;
constexpr uint32_t kGridInstanceCount = kGridRows * kGridCols;
// Rows [0, kVisibleRowCount) are in the default camera cullMask; the rest
// are deterministically excluded BY LAYER MASK.
constexpr uint32_t kVisibleRowCount = 2;
constexpr uint32_t kDefaultVisibleInstances = kVisibleRowCount * kGridCols;
constexpr uint32_t kDefaultCulledInstances = kGridInstanceCount - kDefaultVisibleInstances;
// [gate ruling #15] Collapse-ratio formula, BLESSED verbatim:
// `1 - drawsSubmitted/recordsIn`, as a percentage. For the default grid,
// every visible instance shares identical (blockId, mesh range, material,
// priority) draw identity, so drawsSubmitted == 1 always.
constexpr uint32_t kDefaultDrawsSubmitted = 1;

// Row 0 alone carries a distinct light-CHANNEL bit (0x01) -- the HUD's
// light-channel demo toggle masks it in/out of the directional light's own
// `channels`, independent of the layer-mask (visibility) axis above -- see
// this file's own header comment ("TWO visibly distinct mask controls").
constexpr uint8_t kHighlightChannelBit = 0x01;

// --- Stress-v2 field [--stress] -----------------------------------------
constexpr uint32_t kDefaultStressDraws = 30000;
constexpr uint32_t kStressVariantCount = 4;
constexpr float kStressInstanceScale = 0.85F;
constexpr float kStressInstanceSpacing = 2.4F;

// --- CLI arguments ------------------------------------------------------
struct Args {
    std::string scenePath;  // empty == default helmet grid; "sponza" == the fetched Sponza asset.
    rx::rhi::PresentMode vsyncMode = rx::rhi::PresentMode::VsyncOn;
    bool validate = false;
    bool present = false;
    bool fullscreen = false;
    bool stress = false;
    uint32_t stressDraws = kDefaultStressDraws;
    uint32_t threads = 0;  // 0 == Scheduler default (hardware_concurrency() - 1).
    // [D17] Undocumented-to-users escape hatch -- see samples/08_gltf_viewer's
    // own identical field for the full rationale; the ONLY way
    // tools/regen_references.sh produces this sample's new reference PNG.
    std::string writeReferencesDir;
};

std::optional<Args> parseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--present") {
            args.present = true;
        } else if (arg == "--validate") {
            args.validate = true;
        } else if (arg == "--fullscreen") {
            args.fullscreen = true;
        } else if (arg == "--stress") {
            args.stress = true;
        } else if (arg == "--scene" && i + 1 < argc) {
            args.scenePath = argv[++i];
        } else if (arg == "--write-references" && i + 1 < argc) {
            args.writeReferencesDir = argv[++i];
        } else if (arg == "--vsync" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            if (value == "off") {
                args.vsyncMode = rx::rhi::PresentMode::VsyncOff;
            } else if (value == "on") {
                args.vsyncMode = rx::rhi::PresentMode::VsyncOn;
            } else {
                RX_LOG_ERROR("sample_09_scene: --vsync expects 'on' or 'off', got '{}' -- defaulting to on", value);
            }
        } else if (arg == "--stress-draws" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            uint32_t parsed = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), parsed).ec != std::errc{} || parsed == 0) {
                RX_LOG_ERROR("sample_09_scene: --stress-draws expects a positive integer, got '{}'", value);
                return std::nullopt;
            }
            args.stressDraws = parsed;
        } else if (arg == "--threads" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            uint32_t parsed = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), parsed).ec != std::errc{} || parsed == 0) {
                RX_LOG_ERROR("sample_09_scene: --threads expects a positive integer, got '{}'", value);
                return std::nullopt;
            }
            args.threads = parsed;
        } else {
            RX_LOG_ERROR("sample_09_scene: unrecognized argument '{}'", arg);
            return std::nullopt;
        }
    }
    return args;
}

// --- Path resolution [samples/08_gltf_viewer's own basePathDirectory()/
// resolveDefaultScenePath() idiom] --------------------------------------
std::filesystem::path basePathDirectory() {
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        RX_LOG_WARN("sample_09_scene: SDL_GetBasePath failed ({}); using the current directory instead",
                    SDL_GetError());
        return ".";
    }
    return std::filesystem::path(basePath);
}

std::filesystem::path resolveHelmetScenePath() {
    std::filesystem::path packaged = basePathDirectory() / "assets" / "DamagedHelmet" / "glTF" / "DamagedHelmet.gltf";
    if (std::filesystem::exists(packaged)) {
        return packaged;
    }
    return std::filesystem::path(RX_REPO_ROOT_DIR) / "assets" / "fetched" / "DamagedHelmet" / "glTF" /
           "DamagedHelmet.gltf";
}

// [D16] `--scene sponza` resolves here -- CI never passes this flag. A
// caller without a fetched copy gets a clear, named "run
// tools/fetch_assets.sh --sponza first" error (matrix row 2's own
// acceptance criterion), never a silent fallback or a confusing importer
// failure two layers down.
std::filesystem::path resolveSponzaScenePath() {
    std::filesystem::path packaged = basePathDirectory() / "assets" / "Sponza" / "glTF" / "Sponza.gltf";
    if (std::filesystem::exists(packaged)) {
        return packaged;
    }
    return std::filesystem::path(RX_REPO_ROOT_DIR) / "assets" / "fetched" / "Sponza" / "glTF" / "Sponza.gltf";
}

// --- Fly-through camera [Task 20 input surface -- gate ruling #15's own
// "Task 20 input surface" correction of the plan's stray "D16 input"
// citation] --------------------------------------------------------------
// WASD (via `Window::isKeyDown`) + relative-mouse-mode look + gamepad
// (left stick move, right stick look, triggers = speed multiplier) drive a
// plain yaw/pitch `rx::scene::Camera`. Movement/look are both continuous,
// framerate-scaled (`dt`). The struct itself, and the axis->local-move-delta
// mapping updateFlyCamera() below drives it with, now live in
// fly_camera.h -- device-free, unit-tested directly
// (tests/test_fly_camera.cpp), same "pull pure logic out of main.cpp into
// its own header" precedent draw_recording.h already established in this
// sample.
using rx::samples9::FlyCamera;

// --- Shared pass infra [tonemap pass -- shaders/multipass/tonemap.{vert,
// frag}.slang, VERBATIM -- same convention samples/07_stress/08_gltf_viewer's
// own copies establish] ---------------------------------------------------
struct TonemapPushConstants {
    uint32_t hdrTextureIndex;
    uint32_t hdrSamplerIndex;
};

struct CompiledPass {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t pushConstantOffset = 0;
    uint32_t pushConstantSize = 0;
    VkShaderStageFlags pushConstantStages = 0;
};

void destroyCompiledPass(VkDevice device, CompiledPass& pass) {
    if (pass.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pass.pipeline, nullptr);
        pass.pipeline = VK_NULL_HANDLE;
    }
    if (pass.fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, pass.fragModule, nullptr);
        pass.fragModule = VK_NULL_HANDLE;
    }
    if (pass.vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, pass.vertModule, nullptr);
        pass.vertModule = VK_NULL_HANDLE;
    }
    pass.layoutBundle = rx::rhi::PipelineLayoutBundle{};
}

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

std::optional<std::string> readAndConcatenate(const std::vector<std::string>& paths) {
    std::string combined;
    for (const auto& path : paths) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            RX_LOG_ERROR("sample_09_scene: failed to open shader file '{}'", path);
            return std::nullopt;
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        combined += contents.str();
        combined += "\n";
    }
    return combined;
}

struct ReflectedModule {
    rx::shader::CompileResult compileResult;
    rx::shader::ShaderLayoutInfo layoutInfo;
};

std::optional<ReflectedModule> compileAndReflect(rx::shader::Compiler& compiler, const std::string& moduleName,
                                                  const std::vector<std::string>& paths,
                                                  const std::vector<std::string>& entryPoints) {
    auto source = readAndConcatenate(paths);
    if (!source.has_value()) {
        return std::nullopt;
    }
    rx::shader::CompileResult compileResult = compiler.compileFromSource(moduleName, *source, entryPoints);
    if (!compileResult.ok) {
        RX_LOG_ERROR("sample_09_scene: shader compile failed for module '{}':\n{}", moduleName,
                     compileResult.diagnostics);
        return std::nullopt;
    }
    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("sample_09_scene: reflect() failed for module '{}'", moduleName);
        return std::nullopt;
    }
    return ReflectedModule{std::move(compileResult), std::move(*layoutInfo)};
}

bool assignShaderModules(VkDevice device, const rx::shader::CompileResult& compileResult, const char* vertEntryName,
                          const char* fragEntryName, CompiledPass& pass) {
    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModule module = createShaderModuleFromSpirv(device, blob.code);
        if (module == VK_NULL_HANDLE) {
            RX_LOG_ERROR("sample_09_scene: vkCreateShaderModule failed for entry point '{}'", blob.entryPointName);
            return false;
        }
        if (blob.entryPointName == vertEntryName) {
            pass.vertModule = module;
        } else if (fragEntryName != nullptr && blob.entryPointName == fragEntryName) {
            pass.fragModule = module;
        } else {
            RX_LOG_ERROR("sample_09_scene: unexpected entry point '{}'", blob.entryPointName);
            vkDestroyShaderModule(device, module, nullptr);
            return false;
        }
    }
    if (pass.vertModule == VK_NULL_HANDLE || (fragEntryName != nullptr && pass.fragModule == VK_NULL_HANDLE)) {
        RX_LOG_ERROR("sample_09_scene: missing an expected entry point after a successful compile");
        return false;
    }
    return true;
}

bool buildTonemapPipeline(VkDevice device, rx::shader::Compiler& compiler, VkDescriptorSetLayout bindlessSetLayout,
                          VkFormat backbufferFormat, const std::filesystem::path& tonemapVertPath,
                          const std::filesystem::path& tonemapFragPath, CompiledPass& tonemapPass) {
    destroyCompiledPass(device, tonemapPass);
    auto reflected = compileAndReflect(compiler, "SceneTonemapModule", {tonemapVertPath.string(), tonemapFragPath.string()},
                                        {"vsMain", "fsMain"});
    if (!reflected.has_value()) {
        return false;
    }
    if (reflected->layoutInfo.pushRanges.size() != 1 ||
        reflected->layoutInfo.pushRanges[0].size != sizeof(TonemapPushConstants)) {
        RX_LOG_ERROR("sample_09_scene: tonemap shader reflects an unexpected push-constant shape");
        return false;
    }
    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, reflected->layoutInfo, bindlessSetLayout);
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("sample_09_scene: PipelineLayoutBuilder::build failed for the tonemap pass");
        return false;
    }
    tonemapPass.layoutBundle = std::move(*layoutBundle);
    tonemapPass.pushConstantOffset = reflected->layoutInfo.pushRanges[0].offset;
    tonemapPass.pushConstantSize = reflected->layoutInfo.pushRanges[0].size;
    tonemapPass.pushConstantStages = reflected->layoutInfo.pushRanges[0].stages;
    if (!assignShaderModules(device, reflected->compileResult, "vsMain", "fsMain", tonemapPass)) {
        destroyCompiledPass(device, tonemapPass);
        return false;
    }

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
    renderingCreateInfo.pColorAttachmentFormats = &backbufferFormat;
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = tonemapPass.vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = tonemapPass.fragModule;
    stages[1].pName = "main";
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
    pipelineInfo.layout = tonemapPass.layoutBundle.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &tonemapPass.pipeline) !=
        VK_SUCCESS) {
        RX_LOG_ERROR("sample_09_scene: vkCreateGraphicsPipelines failed for the tonemap pass");
        destroyCompiledPass(device, tonemapPass);
        return false;
    }
    return true;
}

// --- Material GPU binding [D26.1/D28 -- same shape as samples/08_gltf_viewer's
// own MaterialGpuBinding] -------------------------------------------------
struct MaterialGpuBinding {
    rx::material::MaterialHandle handle;
    VkDescriptorSet paramSet = VK_NULL_HANDLE;
    std::optional<rx::rhi::Buffer> paramBuffer;
    uint32_t pushOffset = 0;
    uint32_t pushSize = 0;
    VkShaderStageFlags pushStages = 0;
};

// [D7/D27] The scene forward pass's own attachment shape -- one HDR color
// target + one REVERSED-Z depth target [D13/RC3], single-sampled.
rx::material::PassSignature forwardPassSignature() {
    rx::material::PassSignature sig;
    sig.colorFormats[0] = kHdrFormat;
    sig.colorCount = 1;
    sig.depthFormat = kDepthFormat;
    sig.samples = VK_SAMPLE_COUNT_1_BIT;
    return sig;
}

// --- HUD state [matrix rows 18-20; TWO visibly distinct mask controls] --
struct HudState {
    // 60-frame rolling FPS/frame-ms average [gate ruling #15].
    std::deque<double> frameTimesMs;
    static constexpr size_t kRollingWindow = 60;
    double lastFrameMs = 0.0;

    // vsync toggle -- drives the SAME setPresentMode+recreateSwapchain path
    // the --vsync CLI flag uses (present mode only; no live present target
    // headless).
    bool vsyncOn = true;

    // Layer-mask instance-group toggles -- one bool per grid row, ANDed
    // into camera.cullMask (kGridRows bits) -- visibility ONLY, never a
    // lighting effect. Default matches the deterministic headless
    // composition (rows [0,kVisibleRowCount) visible).
    std::array<bool, kGridRows> layerRowVisible{};

    // Light-channel demo toggle -- masks kHighlightChannelBit out of the
    // directional light's own `channels` (lighting/shadow-casting on row 0
    // ONLY; every other row keeps its default 0xFF channels and is
    // unaffected either way) -- a DIFFERENT axis from layerRowVisible
    // above: toggling this never hides geometry, only changes whether row
    // 0 is lit/casts a shadow.
    bool lightChannelHighlightOn = true;

    HudState() {
        // Deterministic default: rows [0, kVisibleRowCount) visible, the
        // rest hidden -- matches the headless gate's own exact culled/
        // visible split (see this file's own header comment).
        for (uint32_t row = 0; row < kGridRows; ++row) {
            layerRowVisible[row] = row < kVisibleRowCount;
        }
    }

    void pushFrameTime(double ms) {
        lastFrameMs = ms;
        frameTimesMs.push_back(ms);
        while (frameTimesMs.size() > kRollingWindow) {
            frameTimesMs.pop_front();
        }
    }
    [[nodiscard]] double rollingAverageMs() const {
        if (frameTimesMs.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (double v : frameTimesMs) {
            sum += v;
        }
        return sum / static_cast<double>(frameTimesMs.size());
    }
    [[nodiscard]] double rollingFps() const {
        const double avg = rollingAverageMs();
        return avg > 0.0 ? 1000.0 / avg : 0.0;
    }
    [[nodiscard]] uint32_t cullMaskFromToggles() const {
        uint32_t mask = 0;
        for (uint32_t row = 0; row < kGridRows; ++row) {
            if (layerRowVisible[row]) {
                mask |= (1u << row);
            }
        }
        return mask;
    }
};

// --- App state [EVERY GPU-object-owning member constructed IN PLACE via a
// heap-allocated std::unique_ptr<App> -- see samples/08_gltf_viewer's own
// identical App struct comment for the full "dangling-reference-to-a-
// moved-from-local" rationale this mirrors verbatim] ----------------------
struct App {
    std::optional<rx::platform::Window> window;
    std::optional<rx::rhi::Context> context;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::optional<rx::rhi::Device> device;
    std::optional<rx::rhi::Allocator> allocator;
    std::optional<rx::rhi::Uploader> uploader;
    std::optional<rx::rhi::BindlessTable> bindless;
    std::optional<rx::rhi::DeletionQueue> deletionQueue;
    std::unique_ptr<rx::material::MaterialSystem> materialSystem;
    std::unique_ptr<rx::asset::GeometryPool> geometryPool;
    std::unique_ptr<rx::asset::TextureCache> textureCache;
    std::unique_ptr<rx::asset::Registry> registry;  // unused (nullptr) in --stress mode.
    std::unique_ptr<rx::task::Scheduler> scheduler;
    std::unique_ptr<rx::graph::Executor> executor;
    std::unique_ptr<rx::shadow::ShadowCasterPipeline> shadowCaster;
    std::optional<rx::debug_ui::Overlay> overlay;

    std::filesystem::path sharedShaderDir;
    std::filesystem::path shadowShaderDir;

    CompiledPass tonemapPass;
    VkSampler defaultSampler = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle defaultSamplerHandle;
    VkImageView lastHdrView = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle hdrHandle;

    // Set-1 (material params) descriptor infrastructure -- identical shape
    // to samples/08_gltf_viewer's own (one VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    // at binding 0, VERTEX|FRAGMENT) -- see that file's own comment for the
    // Vulkan pipeline-layout-compatibility argument this relies on.
    //
    // [P0 crash fix] The SET LAYOUT is mode-independent (created once, in
    // makeApp()) but the POOL is NOT: how many set-1 VkDescriptorSets this
    // run ever needs depends entirely on which mode is active -- 1 for the
    // DamagedHelmet grid, kStressVariantCount for --stress, or the REAL,
    // scene-dependent material count for a `--scene <path>` import (Sponza's
    // own glTF has 25). A single fixed-size pool created before any of that
    // is known (the pre-fix `kMaxMaterialParamSets = 8` constant) is sized
    // for the wrong mode by construction the moment a caller picks a
    // different one -- that undersizing crashed `--present --scene
    // Sponza.gltf` on a real, limit-enforcing NVIDIA driver (25 > 8) while
    // passing clean under lavapipe, which never enforces VkDescriptorPool's
    // own maxSets/pool-size ceilings at all (see descriptor_arena.h's own
    // BUDGETS ARE ARENA-ENFORCED comment). rx::rhi::DescriptorArena is this
    // engine's own existing, already-tested "size it, and refuse
    // over-budget allocations before ever calling the driver" abstraction
    // (src/rx_rhi_vk/tests/descriptor_arena_test.cpp) -- reused here at
    // framesInFlight=1 (this pool is allocated ONCE per run and never reset;
    // set-1 material params are static for a material's lifetime, unlike
    // rx_material's own per-instance, per-frame ParamArena) instead of hand-
    // rolling a second, unaccounted VkDescriptorPool. Created by
    // createMaterialParamArena() below, from the REAL material count for
    // whichever mode this run selected, once that count is known -- never
    // before runHeadless()/runPresent() have picked a mode.
    VkDescriptorSetLayout materialParamSetLayout = VK_NULL_HANDLE;
    std::optional<rx::rhi::DescriptorArena> materialParamArena;

    // --- Scene (grid or stress -- mutually exclusive per run) -----------
    bool stressMode = false;
    std::optional<rx::scene::Scene> scene;
    std::optional<rx::scene::DrawListBuilder> drawListBuilder;
    rx::scene::LightHandle lightHandle;
    std::vector<rx::scene::RenderableHandle> renderableHandles;  // grid mode: index == row*kGridCols+col.

    // Grid-mode material (DamagedHelmet, real Registry-backed).
    MaterialGpuBinding helmetMaterial;
    rx::asset::AABB helmetLocalBounds;

    // [D5 caller-side accommodation] GeometryPool::bind() is main-thread-
    // only (every accessor in that class is, by the library's own uniform
    // D5 posture) -- but the chunked forward pass's own worker chunks
    // (chunkIndex >= 1) each need to bind a block's vertex/index buffers
    // into THEIR OWN secondary command buffer (a secondary buffer
    // inherits no bound state from any other, per Pass::setExecuteChunked()'s
    // own contract). This cache is refreshed on the main thread, once per
    // frame (updateSceneFrame(), cheap -- one or a handful of blocks),
    // from GeometryPool's own guarded vertexBufferHandle()/
    // indexBufferHandle() accessors; recordForwardChunk() reads this
    // PLAIN, already-resolved map from any thread and issues the
    // equivalent raw vkCmdBindVertexBuffers/vkCmdBindIndexBuffer calls
    // itself (byte-identical to what GeometryPool::bind() itself records
    // -- binding 0/offset 0/VK_INDEX_TYPE_UINT32, D9's u32-only policy)
    // rather than calling the guarded method.
    struct BlockBuffers {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
    };
    std::unordered_map<uint32_t, BlockBuffers> blockBufferCache;

    // [Phase 4 exit fix wave, I3] Main-thread-cached, exactly like
    // blockBufferCache above (same rationale: MaterialSystem::
    // pipelineLayout() is now guarded main-thread-only -- docs/threading.md
    // -- so a worker chunk cannot call it directly). Set once, at setup,
    // by warmMaterialPipelines() -- every participating material shares
    // the identical pipeline-layout SHAPE (Vulkan spec 14.2.2
    // set-compatibility), so caching any one binding's layout here is
    // valid for the whole run, not just one frame.
    VkPipelineLayout cachedAnyMaterialLayout = VK_NULL_HANDLE;

    // Mode-agnostic materialIndex -> real MaterialHandle resolver, bound
    // once at setup depending on which scene kind is active (grid/stress/
    // custom-import via --scene) -- lets updateSceneFrame()'s D27
    // resolvePipeline lambda stay identical across all three without a
    // mode-specific branch each.
    std::function<rx::material::MaterialHandle(uint32_t)> resolveMaterialIndexToHandle;
    // [Sponza texture-misassignment fix] Sibling of resolveMaterialIndexToHandle
    // above, returning the material's OWN MaterialGpuBinding (paramSet/
    // push-constant shape) directly, keyed by the SAME materialIndex --
    // NOT by pipelineToken. See draw_recording.h's own materialIndexForSpan()
    // header comment for why keying descriptor-set selection off
    // pipelineToken is wrong in general: DISTINCT materials legitimately
    // share one VkPipeline whenever their fixed-function state matches
    // (Sponza: 22/25 materials), so a pipelineToken -> binding map can only
    // ever remember ONE of them. recordForwardChunk() below resolves each
    // RecordSpan's REAL materialIndex via materialIndexForSpan() and looks
    // it up through this resolver instead.
    std::function<const MaterialGpuBinding*(uint32_t)> resolveMaterialIndexToBinding;
    // --scene <path> (present-mode only, e.g. Sponza) -- a real imported
    // scene's own materials, keyed by asset::MaterialHandle::index() (the
    // SAME index space DrawPayload::materialIndex uses), mirroring
    // samples/08_gltf_viewer's own materialHandleToBinding map for the
    // "many materials" case the deterministic single-material grid never
    // needs.
    std::vector<MaterialGpuBinding> customMaterialBindings;
    std::unordered_map<uint32_t, size_t> customMaterialIndexToBinding;

    // Stress-mode Registry-free fabrication [see this file's own header
    // comment] -- ONE fabricated MeshHandle/Submesh (the procedural cube)
    // + kStressVariantCount fabricated MaterialHandles/bindings, indexed
    // directly by their own `.index()` (0..3).
    rx::asset::MeshHandle stressMeshHandle;
    rx::asset::AABB stressCubeLocalBounds;
    rx::asset::Submesh stressCubeSubmesh;
    std::array<rx::asset::MaterialHandle, kStressVariantCount> stressMaterialHandles;
    std::array<MaterialGpuBinding, kStressVariantCount> stressMaterialBindings;
    std::array<bool, kStressVariantCount> stressVariantDoubleSided{};

    // D26.1 per-pass draw-data buffer -- capacity sized ONCE, generously,
    // from the ACTUAL submesh-instance total each populate*() function
    // computes for its own mode (kGridInstanceCount for the grid;
    // `totalSubmeshCount` across every instance's own submesh count for an
    // imported scene -- see populateImportedInstances()'s own comment for
    // why `scene->renderableCount()` alone undercounts a multi-submesh mesh
    // like Sponza; that undercount was the H1 heap-overflow bug 1ea8a01
    // fixed) so the buffer never needs live Vulkan recreation even though
    // `ViewLists::payloads.size()` legitimately varies frame to frame
    // (culling/collapse).
    //
    // [Phase 4 exit fix wave, I1] TWO physical buffers per logical buffer,
    // one per `rx::rhi::FrameSync::kFramesInFlight` slot -- NOT one shared
    // buffer -- selected each frame by `currentFrameSlot` below. A single
    // shared buffer rewritten every frame races frame N-1's GPU reads of
    // it (frame N's CPU write can land while frame N-1's vertex/fragment
    // shaders are still reading the SAME buffer, with FramesInFlight==2 and
    // no synchronization between the two): the present loop only proves,
    // via its own slot fence wait, that frame N-2 (the last frame that used
    // THIS SAME slot's buffer) has completed -- frame N-1 always used the
    // OTHER slot, so per-slot buffers make that proof sufficient. Mirrors
    // `rx::material::MaterialSystem`'s own `ParamArena` FIF discipline (the
    // named precedent: `material_system.h`'s `beginFrame()` documents the
    // identical "only after the caller's own fence wait for this slot"
    // contract). `currentFrameSlot`-th buffer is memcpy'd + flushed by
    // `uploadSceneFrameGpuBuffers()`, called AFTER the present loop's own
    // `vkWaitForFences(currentFence)` -- `updateSceneFrame()` below only
    // builds/populates the CPU-side row mirrors (list-building can safely
    // stay before the fence wait; only the GPU-visible write cannot).
    std::array<std::optional<rx::rhi::Buffer>, rx::rhi::FrameSync::kFramesInFlight> drawDataBuffers;
    std::array<rx::rhi::BindlessHandle, rx::rhi::FrameSync::kFramesInFlight> drawDataBufferHandles;
    std::vector<rx::material::DrawDataGpu> drawDataRows;  // CPU staging mirror, reused capacity.
    uint32_t drawDataCapacityRows = 0;

    // [Phase 4 exit fix wave, I1] Which of the two FIF buffer slots this
    // frame's GPU-visible draw data lives in -- set exactly once per frame,
    // on the main thread, before `executor->execute()` (present mode: right
    // after the slot fence wait, alongside `uploadSceneFrameGpuBuffers()`;
    // headless mode: always 0, since `captureFrame()`'s `runOnce()` is
    // fully synchronous with zero cross-frame GPU overlap to race). Read
    // (never written) by `recordForwardChunk()`'s worker chunks and
    // `recordShadowPass()` -- safe because the write above always
    // happens-before `executor->execute()`'s chunk fan-out, the same
    // established precedent `App::blockBufferCache`'s own comment documents.
    uint32_t currentFrameSlot = 0;

    // Shadow pass state [D21, RC3] -- built only when NOT --stress (see
    // this file's own header comment: stress-v2's own A/B comparability
    // contract never mentions shadows, so it skips the pass entirely for a
    // leaner, more directly comparable workload).
    //
    // [Phase 4 exit fix wave, C1] NO sample-owned VkImage/VkDeviceMemory/
    // VkImageView here anymore -- "shadowmap" is a genuine rx_graph
    // TRANSIENT resource now (declareGraph()'s "shadow" pass writes it,
    // "forward" reads it via addTextureInput()), so its physical image is
    // owned and lifetime-tracked by the Executor's own TransientPool, not
    // this sample. `lastShadowMapView`/`shadowMapHandle` mirror
    // `lastHdrView`/`hdrHandle`'s own established re-registration pattern
    // (`recordTonemapDraw()` below; also samples/05_multipass's
    // `recordLitDrawsChunked()`) -- (re-)registered lazily, from chunk 0 of
    // the chunked "forward" pass (`recordForwardChunk()`), the one place a
    // chunked pass may safely call the main-thread-only BindlessTable API
    // [D4 chunk-0 affordance].
    bool shadowEnabled = false;
    // [Phase 4 exit fix wave, C1] Debug/test-only toggle -- when true,
    // forces every row's shadowMapTextureIndex to the documented
    // 0xFFFFFFFF fully-lit sentinel for this frame, exactly reproducing the
    // phase-exit review's own runtime discrimination probe (which flipped
    // `app.shadowEnabled` via gdb -- see updateSceneFrame()'s own comment
    // on why THIS flag, not shadowEnabled itself, is the clean equivalent:
    // it forces the shader-side bypass without touching the shadow PASS's
    // own recording/list-building, so the graph structure and the shadow
    // map's own contents stay exactly as they'd otherwise be). Read only by
    // updateSceneFrame()'s row-population loop; never true outside the
    // headless gate's own D17 discrimination re-proof (runHeadlessGrid()).
    bool debugForceShadowSentinel = false;
    VkImageView lastShadowMapView = VK_NULL_HANDLE;
    VkSampler shadowCompareSampler = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle shadowCompareSamplerHandle;
    rx::rhi::BindlessHandle shadowMapHandle;
    std::array<std::optional<rx::rhi::Buffer>, rx::rhi::FrameSync::kFramesInFlight> shadowDrawDataBuffers;
    std::array<rx::rhi::BindlessHandle, rx::rhi::FrameSync::kFramesInFlight> shadowDrawDataBufferHandles;
    std::vector<rx::shadow::ShadowDrawData> shadowDrawDataRows;
    uint32_t shadowDrawDataCapacityRows = 0;
    rx::shadow::ShadowFrustumFit shadowFit;
    glm::vec3 lightDirWorld{0.4F, -1.0F, 0.3F};  // travel direction (matches LightRecord::direction's own convention).

    // Per-frame culling output -- reused, caller-owned storage [D26 amendment].
    rx::scene::ViewLists viewLists;
    rx::scene::ShadowLists shadowLists;
    std::vector<rx::scene::ResolvedDrawGroup> resolvedGroups;  // populated once/frame, chunk 0, before fan-out.

    FlyCamera flyCamera;
    float moveSpeed = FlyCamera::kBaseMoveSpeed;
    HudState hud;

    bool sceneReady = false;  // grid mode: false until the sync import + buildHelmetGrid() complete.
};

// Builds every field up to (but not including) scene content -- device/
// allocator/uploader/bindless/materialSystem/geometryPool/textureCache/
// scheduler/executor, the shadow-caster pipeline, the tonemap pipeline, and
// the shared material-param descriptor infrastructure. Returns nullptr
// (logged) on any failure.
std::unique_ptr<App> makeApp(const std::string& windowTitle, uint32_t width, uint32_t height, bool visible,
                              bool validate, bool stressMode) {
    auto app = std::make_unique<App>();
    app->stressMode = stressMode;

    auto window = rx::platform::Window::create(windowTitle.c_str(), static_cast<int>(width), static_cast<int>(height),
                                                 visible);
    if (!window.has_value()) {
        RX_LOG_ERROR("sample_09_scene: Window::create failed: no display backend available");
        return nullptr;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        RX_LOG_ERROR("sample_09_scene: video driver reports no Vulkan surface extensions");
        return nullptr;
    }
    auto context = rx::rhi::Context::create(extensions, validate);
    if (!context.has_value()) {
        RX_LOG_ERROR("sample_09_scene: Context::create failed");
        return nullptr;
    }
    app->window.emplace(std::move(*window));
    app->context.emplace(std::move(*context));

    app->surface = app->window->createVulkanSurface(app->context->instance());
    if (app->surface == VK_NULL_HANDLE) {
        RX_LOG_ERROR("sample_09_scene: createVulkanSurface failed");
        return nullptr;
    }
    auto device = rx::rhi::Device::create(*app->context, app->surface);
    if (!device.has_value()) {
        RX_LOG_ERROR("sample_09_scene: Device::create failed");
        return nullptr;
    }
    app->device.emplace(std::move(*device));

    auto allocator = rx::rhi::Allocator::create(*app->context, *app->device);
    if (!allocator.has_value()) {
        RX_LOG_ERROR("sample_09_scene: Allocator::create failed");
        return nullptr;
    }
    app->allocator.emplace(std::move(*allocator));

    auto uploader = rx::rhi::Uploader::create(*app->allocator, *app->device);
    if (!uploader.has_value()) {
        RX_LOG_ERROR("sample_09_scene: Uploader::create failed");
        return nullptr;
    }
    app->uploader.emplace(std::move(*uploader));

    // Bindless capacities [fix round, Finding H1(a)]: sized for the LARGEST
    // scene this file actually loads, not just the default DamagedHelmet
    // grid -- `--scene sponza` (present-mode) goes through this SAME
    // BindlessTable, and Sponza's own ~25 materials x up to ~3 textures
    // each exhausted a previous, helmet-only-justified 64 partway through
    // import (an independent review reproduced this directly: real
    // capacity-exhaustion errors starting at material#20, which then
    // exposed a separate TextureCache lifecycle bug on the failure path --
    // see texture_cache.cpp's own registerRealTexture() fix). Mirrors
    // samples/08_gltf_viewer's own identical 256 (that sample's own
    // largest supported scene is likewise arbitrary imported glTF content,
    // not just its own default asset) rather than inventing a smaller,
    // scene-specific number that would need re-justifying every time this
    // sample's own `--scene` surface grows. `samplers` bumped from 16 to
    // 32 for the same reason (more distinct materials plausibly means more
    // distinct wrap/filter combinations, even after TextureCache's own
    // per-descriptor sampler dedup) -- shadow map + comparison sampler,
    // the per-pass DrawDataGpu/ShadowDrawData storage buffers, and ImGui's
    // own font atlas (registered by the vendored backend, NOT through this
    // table -- see rx_debug_ui/overlay.h's own FONT/TEXTURE UPLOAD
    // section) round out the remaining headroom.
    rx::rhi::BindlessTable::Capacities capacities{/*sampledImages=*/256, /*samplers=*/32, /*storageBuffers=*/8};
    capacities.comparisonSamplers = 1;
    auto bindless = rx::rhi::BindlessTable::create(app->device->physicalDevice(), app->device->device(), capacities);
    if (!bindless.has_value()) {
        RX_LOG_ERROR("sample_09_scene: BindlessTable::create failed");
        return nullptr;
    }
    app->bindless.emplace(std::move(*bindless));
    app->deletionQueue.emplace();

    const std::filesystem::path basePath = basePathDirectory();
    app->sharedShaderDir = basePath / "material_shaders";
    app->shadowShaderDir = basePath / "shadow_shaders";

    app->materialSystem = rx::material::MaterialSystem::create(*app->device, *app->bindless,
                                                                 basePath / "scene_pipeline.cache", app->sharedShaderDir);
    if (app->materialSystem == nullptr) {
        RX_LOG_ERROR("sample_09_scene: MaterialSystem::create failed");
        return nullptr;
    }

    app->geometryPool = rx::asset::GeometryPool::create(*app->allocator, *app->device, *app->uploader);
    if (app->geometryPool == nullptr) {
        RX_LOG_ERROR("sample_09_scene: GeometryPool::create failed");
        return nullptr;
    }
    app->textureCache =
        rx::asset::TextureCache::create(*app->allocator, *app->device, *app->uploader, *app->bindless, *app->deletionQueue);
    if (app->textureCache == nullptr) {
        RX_LOG_ERROR("sample_09_scene: TextureCache::create failed");
        return nullptr;
    }
    if (!stressMode) {
        app->registry = std::make_unique<rx::asset::Registry>();
    }

    app->scheduler = nullptr;  // constructed by the caller (thread count is a CLI-configurable, per-run choice).

    // Default LINEAR/REPEAT sampler -- shared by the tonemap pass's own HDR
    // read and rx_sampleTexture's fallback overload, same idiom
    // samples/08_gltf_viewer's own App::defaultSampler establishes.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(app->device->device(), &samplerInfo, nullptr, &app->defaultSampler) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_09_scene: vkCreateSampler (default sampler) failed");
        return nullptr;
    }
    app->defaultSamplerHandle = app->bindless->registerSampler(app->defaultSampler);
    if (!app->defaultSamplerHandle.isValid()) {
        RX_LOG_ERROR("sample_09_scene: BindlessTable::registerSampler (default sampler) failed");
        return nullptr;
    }

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        RX_LOG_ERROR("sample_09_scene: rx::shader::Compiler::create failed");
        return nullptr;
    }
    if (!buildTonemapPipeline(app->device->device(), *compiler, app->bindless->descriptorSetLayout(),
                              app->device->swapchainFormat(), basePath / "tonemap.vert.slang",
                              basePath / "tonemap.frag.slang", app->tonemapPass)) {
        return nullptr;
    }

    // Shadow-caster pipeline [D21, RC3] -- built outside MaterialSystem.
    app->shadowCaster = rx::shadow::ShadowCasterPipeline::create(*app->device, *app->bindless, {}, app->shadowShaderDir);
    if (app->shadowCaster == nullptr) {
        RX_LOG_ERROR("sample_09_scene: ShadowCasterPipeline::create failed");
        return nullptr;
    }

    // Material set-1 param descriptor SET LAYOUT -- see
    // samples/08_gltf_viewer's own identical block for the full rationale.
    // Mode-independent (every mode uses the same one-UBO-binding shape), so
    // this is still built here, unconditionally. The POOL itself is NOT
    // built here anymore -- see App::materialParamArena's own comment for
    // why: it needs the real, mode-dependent material count, which isn't
    // known yet at this point in startup. createMaterialParamArena() below
    // builds it once that count is known.
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &uboBinding;
    if (vkCreateDescriptorSetLayout(app->device->device(), &setLayoutInfo, nullptr, &app->materialParamSetLayout) !=
        VK_SUCCESS) {
        RX_LOG_ERROR("sample_09_scene: vkCreateDescriptorSetLayout (material params) failed");
        return nullptr;
    }

    return app;
}

// Builds App::materialParamArena, sized EXACTLY from `materialCount` -- the
// real number of set-1 VkDescriptorSets this run will ever allocate (1 for
// the DamagedHelmet grid, kStressVariantCount for --stress, or
// result.materials.size() for a `--scene <path>` import), never a fixed
// magic number picked for one mode and silently reused for every other one
// [the exact bug this replaces -- see App::materialParamArena's own
// comment]. Must be called exactly once, after the caller has determined
// which mode is active and (for a custom import) after importGltf() has
// returned its real material count, and before the first
// finalizeMaterialBinding() call for that mode. `materialCount == 0` is
// clamped to 1 -- rx::rhi::DescriptorArena::create() rejects a zero
// capacity outright, and an empty-material scene still needs a valid (if
// never-used) arena for destroyApp()'s own unconditional teardown path.
bool createMaterialParamArena(App& app, uint32_t materialCount) {
    const uint32_t sets = std::max<uint32_t>(materialCount, 1);
    const rx::rhi::DescriptorArena::Capacities capacities{/*maxSets=*/sets, /*uniformBuffers=*/sets};
    auto arena = rx::rhi::DescriptorArena::create(app.device->device(), /*framesInFlight=*/1, capacities);
    if (!arena.has_value()) {
        RX_LOG_ERROR("sample_09_scene: DescriptorArena::create (material params) failed for {} material(s)",
                     materialCount);
        return false;
    }
    app.materialParamArena = std::move(*arena);
    RX_LOG_INFO("sample_09_scene: material-params descriptor arena sized for {} material(s)", sets);
    return true;
}

void destroyShadowResources(App& app) {
    const VkDevice device = app.device->device();
    for (uint32_t slot = 0; slot < rx::rhi::FrameSync::kFramesInFlight; ++slot) {
        if (app.shadowDrawDataBufferHandles[slot].isValid()) {
            app.bindless->release(app.shadowDrawDataBufferHandles[slot]);
        }
        app.shadowDrawDataBuffers[slot].reset();
    }
    // [Phase 4 exit fix wave, C1] No sample-owned shadow VkImage/
    // VkDeviceMemory/VkImageView to destroy anymore -- "shadowmap" is a
    // graph transient now, torn down by the Executor's own TransientPool
    // (see App::lastShadowMapView's own comment). `shadowMapHandle` is a
    // bindless registration ONLY (release the slot; the view it pointed at
    // is someone else's resource to destroy).
    if (app.shadowMapHandle.isValid()) {
        app.bindless->release(app.shadowMapHandle);
    }
    if (app.shadowCompareSamplerHandle.isValid()) {
        app.bindless->release(app.shadowCompareSamplerHandle);
    }
    if (app.shadowCompareSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, app.shadowCompareSampler, nullptr);
        app.shadowCompareSampler = VK_NULL_HANDLE;
    }
}

void destroyApp(App& app) {
    if (app.device.has_value() && app.device->device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(app.device->device());
    }
    app.overlay.reset();  // must precede bindless/device teardown -- owns its own descriptor pool/pipeline/font texture.
    destroyShadowResources(app);
    for (MaterialGpuBinding& binding : app.stressMaterialBindings) {
        binding.paramBuffer.reset();
    }
    // [Fix round] `customMaterialBindings` (--scene <path> present-mode
    // overrides, e.g. Sponza's own 25 materials) was never torn down here
    // -- a real resource leak an independent review's own end-to-end
    // Sponza run surfaced directly: VUID-vkDestroyDevice-device-00378
    // ("child objects... has not been destroyed") fired once per leaked
    // material param VkBuffer at process exit. helmetMaterial/
    // stressMaterialBindings above were both handled; this one was missed.
    for (MaterialGpuBinding& binding : app.customMaterialBindings) {
        binding.paramBuffer.reset();
    }
    app.helmetMaterial.paramBuffer.reset();
    // rx::rhi::DescriptorArena's own destructor (via std::optional::reset())
    // destroys its underlying VkDescriptorPool(s) -- exactly what the
    // explicit vkDestroyDescriptorPool() call this replaces did. A run that
    // never called createMaterialParamArena() (e.g. an early setupApp()
    // failure before any mode was selected) leaves this optional empty --
    // reset() on an empty optional is a documented no-op.
    app.materialParamArena.reset();
    if (app.materialParamSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(app.device->device(), app.materialParamSetLayout, nullptr);
        app.materialParamSetLayout = VK_NULL_HANDLE;
    }
    for (uint32_t slot = 0; slot < rx::rhi::FrameSync::kFramesInFlight; ++slot) {
        if (app.drawDataBufferHandles[slot].isValid() && app.bindless.has_value()) {
            app.bindless->release(app.drawDataBufferHandles[slot]);
        }
        app.drawDataBuffers[slot].reset();
    }
    destroyCompiledPass(app.device->device(), app.tonemapPass);
    if (app.defaultSamplerHandle.isValid() && app.bindless.has_value()) {
        app.bindless->release(app.defaultSamplerHandle);
    }
    if (app.defaultSampler != VK_NULL_HANDLE) {
        vkDestroySampler(app.device->device(), app.defaultSampler, nullptr);
        app.defaultSampler = VK_NULL_HANDLE;
    }
    app.drawListBuilder.reset();
    app.scene.reset();
    app.shadowCaster.reset();
    app.registry.reset();
    app.textureCache.reset();
    app.geometryPool.reset();
    app.materialSystem.reset();
    app.executor.reset();
    app.scheduler.reset();
    app.deletionQueue.reset();
    app.bindless.reset();
    app.uploader.reset();
    app.allocator.reset();
    app.device.reset();
}

// --- Material param helper [samples/08_gltf_viewer's own setMaterialParam()
// verbatim] -----------------------------------------------------------
template <typename T>
bool setMaterialParam(std::vector<uint8_t>& blob, const std::vector<rx::material::MaterialParamInfo>& params,
                      const char* name, const T& value) {
    for (const auto& p : params) {
        if (p.name == name) {
            if (p.size != sizeof(T)) {
                RX_LOG_ERROR("sample_09_scene: material param '{}' reflected size {} != sizeof(T) {}", name, p.size,
                             sizeof(T));
                return false;
            }
            std::memcpy(blob.data() + p.offset, &value, sizeof(T));
            return true;
        }
    }
    RX_LOG_ERROR("sample_09_scene: material param '{}' not found in reflection", name);
    return false;
}

rx::material::AlphaMode toMaterialAlphaMode(rx::asset::AlphaMode mode) {
    switch (mode) {
        case rx::asset::AlphaMode::Opaque:
            return rx::material::AlphaMode::Opaque;
        case rx::asset::AlphaMode::Mask:
            return rx::material::AlphaMode::Mask;
        case rx::asset::AlphaMode::Blend:
            return rx::material::AlphaMode::Blend;
    }
    return rx::material::AlphaMode::Opaque;
}

uint32_t resolveTextureIndex(rx::asset::TextureCache& textures, const rx::asset::TextureRef& ref,
                              rx::asset::TextureRole role) {
    if (!ref.present) {
        return textures.resolve(textures.fallbackHandle(role)).bindlessIndex;
    }
    return textures.resolve(ref.handle).bindlessIndex;
}

uint32_t resolveSamplerIndex(rx::asset::TextureCache& textures, const rx::asset::TextureRef& ref,
                              uint32_t fallbackIndex) {
    std::optional<uint32_t> index = textures.getOrCreateSamplerBindlessIndex(ref.sampler);
    return index.value_or(fallbackIndex);
}

std::array<float, 4> uvTransformParam(const rx::asset::TextureRef& ref) {
    return {ref.uvOffset.x, ref.uvOffset.y, ref.uvScale.x, ref.uvScale.y};
}

// Builds a real UBO + set-1 VkDescriptorSet for `binding` from `blob`
// (already-populated param bytes) -- shared tail both setupHelmetMaterial()
// and setupStressMaterials() call.
bool finalizeMaterialBinding(App& app, MaterialGpuBinding& binding, const std::vector<uint8_t>& blob,
                              uint32_t blockSize) {
    auto paramBuffer =
        app.allocator->createHostVisibleBuffer(blockSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, rx::rhi::MemoryCategory::Internal);
    if (!paramBuffer.has_value()) {
        RX_LOG_ERROR("sample_09_scene: createHostVisibleBuffer (material params) failed");
        return false;
    }
    std::memcpy(paramBuffer->mappedData(), blob.data(), blockSize);
    paramBuffer->flush();

    // [P0 crash fix] Allocated through App::materialParamArena, NOT a raw
    // vkAllocateDescriptorSets against a hand-sized VkDescriptorPool -- the
    // arena tracks this pool's own declared capacity and refuses (clean
    // VK_NULL_HANDLE, logged) BEFORE ever calling the driver if this
    // allocation would exceed it, deterministically on every driver
    // including lavapipe (see descriptor_arena.h's own BUDGETS ARE
    // ARENA-ENFORCED comment). createMaterialParamArena() sizes the arena
    // from the real per-run material count, so a legitimate call here
    // should never be refused -- if it is, either that sizing call was
    // skipped/wrong for the active mode, or materialCount undercounted the
    // scene's real materials; both are caller bugs this refusal surfaces
    // immediately instead of crashing past it.
    VkDescriptorSet paramSet = app.materialParamArena->allocate(app.materialParamSetLayout, /*uniformBufferDescriptorCount=*/1);
    if (paramSet == VK_NULL_HANDLE) {
        RX_LOG_ERROR("sample_09_scene: material-params descriptor arena allocation failed (pool exhausted or driver "
                     "rejection -- see DescriptorArena's own preceding log line)");
        return false;
    }
    VkDescriptorBufferInfo bufferInfo{paramBuffer->handle(), 0, blockSize};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = paramSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(app.device->device(), 1, &write, 0, nullptr);

    binding.paramBuffer = std::move(paramBuffer);
    binding.paramSet = paramSet;

    const rx::shader::ShaderLayoutInfo& layout = app.materialSystem->layoutInfo(binding.handle);
    if (layout.pushRanges.empty()) {
        RX_LOG_ERROR("sample_09_scene: material reflects no RxMaterialGlobals push-constant range");
        return false;
    }
    binding.pushOffset = layout.pushRanges[0].offset;
    binding.pushSize = layout.pushRanges[0].size;
    binding.pushStages = layout.pushRanges[0].stages;

    // [D27] Pre-resolve this material's forward-pass VkPipeline on the
    // main thread, before any draw list is ever built -- discarded (the
    // per-frame resolveDrawGroups() call below re-fetches the SAME cached
    // pipeline, never recompiling it).
    try {
        app.materialSystem->getPipeline({binding.handle, forwardPassSignature(), 0});
    } catch (const std::exception& ex) {
        RX_LOG_ERROR("sample_09_scene: D27 pipeline pre-resolution failed: {}", ex.what());
        return false;
    }
    return true;
}

// Loads DamagedHelmet's own real StandardPBR material (via MaterialSystem,
// D22/D26.1/D28) from `registry`'s already-imported MaterialAsset --
// mirrors samples/08_gltf_viewer's own setupMaterials() for the
// single-material case.
bool setupHelmetMaterial(App& app, const rx::asset::MaterialAsset& asset) {
    rx::material::MaterialFixedFunctionState fixedFunctionState;
    fixedFunctionState.alphaMode = toMaterialAlphaMode(asset.alphaMode);
    fixedFunctionState.doubleSided = asset.doubleSided;

    MaterialGpuBinding binding;
    try {
        binding.handle = app.materialSystem->loadMaterial(app.sharedShaderDir / "standard_pbr.slang", fixedFunctionState);
    } catch (const std::exception& ex) {
        RX_LOG_ERROR("sample_09_scene: loadMaterial(standard_pbr) failed: {}", ex.what());
        return false;
    }

    const uint32_t blockSize = app.materialSystem->paramBlockSize(binding.handle);
    std::vector<uint8_t> blob(blockSize, uint8_t{0});
    const std::vector<rx::material::MaterialParamInfo> params = app.materialSystem->materialParams(binding.handle);

    const float appliedAlphaCutoff = asset.alphaMode == rx::asset::AlphaMode::Mask ? asset.alphaCutoff : 0.0F;
    const std::array<float, 4> baseColorFactor{asset.baseColorFactor.x, asset.baseColorFactor.y, asset.baseColorFactor.z,
                                                asset.baseColorFactor.w};
    const std::array<float, 4> emissiveFactorAndPad{asset.emissiveFactor.x * asset.emissiveStrength,
                                                     asset.emissiveFactor.y * asset.emissiveStrength,
                                                     asset.emissiveFactor.z * asset.emissiveStrength, 0.0F};
    bool ok = true;
    ok = ok && setMaterialParam(blob, params, "baseColorFactor", baseColorFactor);
    ok = ok && setMaterialParam(blob, params, "metallicFactor", asset.metallicFactor);
    ok = ok && setMaterialParam(blob, params, "roughnessFactor", asset.roughnessFactor);
    ok = ok && setMaterialParam(blob, params, "normalScale", asset.normalScale);
    ok = ok && setMaterialParam(blob, params, "occlusionStrength", asset.occlusionStrength);
    ok = ok && setMaterialParam(blob, params, "alphaCutoff", appliedAlphaCutoff);
    ok = ok && setMaterialParam(blob, params, "emissiveFactorAndPad", emissiveFactorAndPad);
    ok = ok && setMaterialParam(blob, params, "baseColorTexture",
                                resolveTextureIndex(*app.textureCache, asset.baseColorTexture, rx::asset::TextureRole::BaseColor));
    ok = ok && setMaterialParam(blob, params, "metallicRoughnessTexture",
                                resolveTextureIndex(*app.textureCache, asset.metallicRoughnessTexture,
                                                      rx::asset::TextureRole::MetallicRoughness));
    ok = ok && setMaterialParam(blob, params, "normalTexture",
                                resolveTextureIndex(*app.textureCache, asset.normalTexture, rx::asset::TextureRole::Normal));
    ok = ok && setMaterialParam(blob, params, "occlusionTexture",
                                resolveTextureIndex(*app.textureCache, asset.occlusionTexture, rx::asset::TextureRole::Occlusion));
    ok = ok && setMaterialParam(blob, params, "emissiveTexture",
                                resolveTextureIndex(*app.textureCache, asset.emissiveTexture, rx::asset::TextureRole::Emissive));
    ok = ok && setMaterialParam(blob, params, "baseColorSampler",
                                resolveSamplerIndex(*app.textureCache, asset.baseColorTexture, app.defaultSamplerHandle.index()));
    ok = ok && setMaterialParam(blob, params, "metallicRoughnessSampler",
                                resolveSamplerIndex(*app.textureCache, asset.metallicRoughnessTexture,
                                                      app.defaultSamplerHandle.index()));
    ok = ok && setMaterialParam(blob, params, "normalSampler",
                                resolveSamplerIndex(*app.textureCache, asset.normalTexture, app.defaultSamplerHandle.index()));
    ok = ok && setMaterialParam(blob, params, "occlusionSampler",
                                resolveSamplerIndex(*app.textureCache, asset.occlusionTexture, app.defaultSamplerHandle.index()));
    ok = ok && setMaterialParam(blob, params, "emissiveSampler",
                                resolveSamplerIndex(*app.textureCache, asset.emissiveTexture, app.defaultSamplerHandle.index()));
    ok = ok && setMaterialParam(blob, params, "baseColorUvOffsetScale", uvTransformParam(asset.baseColorTexture));
    ok = ok && setMaterialParam(blob, params, "metallicRoughnessUvOffsetScale", uvTransformParam(asset.metallicRoughnessTexture));
    ok = ok && setMaterialParam(blob, params, "normalUvOffsetScale", uvTransformParam(asset.normalTexture));
    ok = ok && setMaterialParam(blob, params, "occlusionUvOffsetScale", uvTransformParam(asset.occlusionTexture));
    ok = ok && setMaterialParam(blob, params, "emissiveUvOffsetScale", uvTransformParam(asset.emissiveTexture));
    if (!ok) {
        RX_LOG_ERROR("sample_09_scene: helmet material param setup failed");
        return false;
    }

    if (!finalizeMaterialBinding(app, binding, blob, blockSize)) {
        return false;
    }
    app.helmetMaterial = std::move(binding);
    app.resolveMaterialIndexToHandle = [&app](uint32_t) { return app.helmetMaterial.handle; };  // DamagedHelmet: exactly 1 material.
    app.resolveMaterialIndexToBinding = [&app](uint32_t) -> const MaterialGpuBinding* { return &app.helmetMaterial; };
    return true;
}

// Loads the 4 --stress Unlit material variants [D28's real fixed-function
// axis: 2 alphaModes (Opaque/Mask, cutoff always 0 -- never trips the
// discard, so Mask behaves visually identically to Opaque here) x 2
// doubleSided states -- 4 genuinely independently-cached VkPipeline
// objects, matching sample 07_stress's own "4 mesh/pipeline-state
// variations" in spirit through this sample's own real D28 axis, not a
// re-derivation of sample 07's cull-mode-only scheme]. Distinct
// `baseColorFactor` per variant (red/green/blue/magenta) for visual/
// dominance continuity with sample 07's own 4 colors.
bool setupStressMaterials(App& app) {
    static constexpr std::array<rx::asset::AlphaMode, kStressVariantCount> kAlphaModes{
        rx::asset::AlphaMode::Opaque, rx::asset::AlphaMode::Opaque, rx::asset::AlphaMode::Mask, rx::asset::AlphaMode::Mask};
    static constexpr std::array<bool, kStressVariantCount> kDoubleSided{false, true, false, true};
    static constexpr std::array<std::array<float, 4>, kStressVariantCount> kColors{{
        {1.0F, 0.05F, 0.05F, 1.0F},
        {0.05F, 1.0F, 0.05F, 1.0F},
        {0.05F, 0.05F, 1.0F, 1.0F},
        {0.9F, 0.05F, 0.9F, 1.0F},
    }};

    for (uint32_t i = 0; i < kStressVariantCount; ++i) {
        rx::material::MaterialFixedFunctionState fixedFunctionState;
        fixedFunctionState.alphaMode = toMaterialAlphaMode(kAlphaModes[i]);
        fixedFunctionState.doubleSided = kDoubleSided[i];
        app.stressVariantDoubleSided[i] = kDoubleSided[i];

        MaterialGpuBinding binding;
        try {
            binding.handle = app.materialSystem->loadMaterial(app.sharedShaderDir / "unlit.slang", fixedFunctionState);
        } catch (const std::exception& ex) {
            RX_LOG_ERROR("sample_09_scene: loadMaterial(unlit, variant {}) failed: {}", i, ex.what());
            return false;
        }
        // Fabricated materialIndex: this variant's OWN slot index (0..3),
        // used verbatim as ResolvedMaterial::materialIndex/DrawPayload::
        // materialIndex by this sample's own Registry-free MaterialResolveFn
        // (see buildStressField() below) -- never derived from
        // binding.handle's own (MaterialSystem-internal) index/generation.
        app.stressMaterialHandles[i] = rx::asset::MaterialHandle(i, 1);

        const uint32_t blockSize = app.materialSystem->paramBlockSize(binding.handle);
        std::vector<uint8_t> blob(blockSize, uint8_t{0});
        const std::vector<rx::material::MaterialParamInfo> params = app.materialSystem->materialParams(binding.handle);
        bool ok = true;
        const uint32_t fallbackTextureIndex =
            app.textureCache->resolve(app.textureCache->fallbackHandle(rx::asset::TextureRole::BaseColor)).bindlessIndex;
        ok = ok && setMaterialParam(blob, params, "baseColorFactor", kColors[i]);
        ok = ok && setMaterialParam(blob, params, "baseColorTexture", fallbackTextureIndex);
        ok = ok && setMaterialParam(blob, params, "baseColorSampler", app.defaultSamplerHandle.index());
        ok = ok && setMaterialParam(blob, params, "alphaCutoff", 0.0F);
        ok = ok && setMaterialParam(blob, params, "baseColorUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});
        if (!ok) {
            RX_LOG_ERROR("sample_09_scene: stress material param setup failed for variant {}", i);
            return false;
        }
        if (!finalizeMaterialBinding(app, binding, blob, blockSize)) {
            return false;
        }
        app.stressMaterialBindings[i] = std::move(binding);
    }
    app.resolveMaterialIndexToHandle = [&app](uint32_t idx) { return app.stressMaterialBindings[idx % kStressVariantCount].handle; };
    app.resolveMaterialIndexToBinding = [&app](uint32_t idx) -> const MaterialGpuBinding* {
        return &app.stressMaterialBindings[idx % kStressVariantCount];
    };
    return true;
}

// Loads every material `registry` reports for a `--scene <path>` import
// (Sponza or any other custom asset, present-mode only -- see this file's
// own header comment on why the deterministic grid never needs this: it
// has exactly 1 material, handled directly by setupHelmetMaterial()
// above). Mirrors samples/08_gltf_viewer's own setupMaterials() loop,
// generalized to StandardPBR OR Unlit per material's own disposition [D22,
// gate ruling C5].
bool setupImportedMaterials(App& app, const rx::asset::Registry& registry, const std::vector<rx::asset::MaterialHandle>& materials) {
    const rx::material::PassSignature passSig = forwardPassSignature();
    for (rx::asset::MaterialHandle assetHandle : materials) {
        const rx::asset::MaterialAsset& asset = registry.material(assetHandle);
        const bool isUnlit = asset.disposition == rx::asset::MaterialDisposition::Unlit;
        const std::filesystem::path modulePath = app.sharedShaderDir / (isUnlit ? "unlit.slang" : "standard_pbr.slang");

        rx::material::MaterialFixedFunctionState fixedFunctionState;
        fixedFunctionState.alphaMode = toMaterialAlphaMode(asset.alphaMode);
        fixedFunctionState.doubleSided = asset.doubleSided;

        MaterialGpuBinding binding;
        try {
            binding.handle = app.materialSystem->loadMaterial(modulePath, fixedFunctionState);
        } catch (const std::exception& ex) {
            RX_LOG_ERROR("sample_09_scene: loadMaterial('{}') failed for material '{}': {}", modulePath.string(), asset.name,
                         ex.what());
            return false;
        }
        const uint32_t blockSize = app.materialSystem->paramBlockSize(binding.handle);
        std::vector<uint8_t> blob(blockSize, uint8_t{0});
        const std::vector<rx::material::MaterialParamInfo> params = app.materialSystem->materialParams(binding.handle);
        const float appliedAlphaCutoff = asset.alphaMode == rx::asset::AlphaMode::Mask ? asset.alphaCutoff : 0.0F;
        const std::array<float, 4> baseColorFactor{asset.baseColorFactor.x, asset.baseColorFactor.y, asset.baseColorFactor.z,
                                                    asset.baseColorFactor.w};
        bool ok = true;
        if (isUnlit) {
            ok = ok && setMaterialParam(blob, params, "baseColorFactor", baseColorFactor);
            ok = ok && setMaterialParam(blob, params, "baseColorTexture",
                                        resolveTextureIndex(*app.textureCache, asset.baseColorTexture, rx::asset::TextureRole::BaseColor));
            ok = ok && setMaterialParam(blob, params, "baseColorSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.baseColorTexture, app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(blob, params, "alphaCutoff", appliedAlphaCutoff);
            ok = ok && setMaterialParam(blob, params, "baseColorUvOffsetScale", uvTransformParam(asset.baseColorTexture));
        } else {
            const std::array<float, 4> emissiveFactorAndPad{asset.emissiveFactor.x * asset.emissiveStrength,
                                                             asset.emissiveFactor.y * asset.emissiveStrength,
                                                             asset.emissiveFactor.z * asset.emissiveStrength, 0.0F};
            ok = ok && setMaterialParam(blob, params, "baseColorFactor", baseColorFactor);
            ok = ok && setMaterialParam(blob, params, "metallicFactor", asset.metallicFactor);
            ok = ok && setMaterialParam(blob, params, "roughnessFactor", asset.roughnessFactor);
            ok = ok && setMaterialParam(blob, params, "normalScale", asset.normalScale);
            ok = ok && setMaterialParam(blob, params, "occlusionStrength", asset.occlusionStrength);
            ok = ok && setMaterialParam(blob, params, "alphaCutoff", appliedAlphaCutoff);
            ok = ok && setMaterialParam(blob, params, "emissiveFactorAndPad", emissiveFactorAndPad);
            ok = ok && setMaterialParam(blob, params, "baseColorTexture",
                                        resolveTextureIndex(*app.textureCache, asset.baseColorTexture, rx::asset::TextureRole::BaseColor));
            ok = ok && setMaterialParam(blob, params, "metallicRoughnessTexture",
                                        resolveTextureIndex(*app.textureCache, asset.metallicRoughnessTexture,
                                                              rx::asset::TextureRole::MetallicRoughness));
            ok = ok && setMaterialParam(blob, params, "normalTexture",
                                        resolveTextureIndex(*app.textureCache, asset.normalTexture, rx::asset::TextureRole::Normal));
            ok = ok && setMaterialParam(blob, params, "occlusionTexture",
                                        resolveTextureIndex(*app.textureCache, asset.occlusionTexture, rx::asset::TextureRole::Occlusion));
            ok = ok && setMaterialParam(blob, params, "emissiveTexture",
                                        resolveTextureIndex(*app.textureCache, asset.emissiveTexture, rx::asset::TextureRole::Emissive));
            ok = ok && setMaterialParam(blob, params, "baseColorSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.baseColorTexture, app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(blob, params, "metallicRoughnessSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.metallicRoughnessTexture,
                                                              app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(blob, params, "normalSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.normalTexture, app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(blob, params, "occlusionSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.occlusionTexture, app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(blob, params, "emissiveSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.emissiveTexture, app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(blob, params, "baseColorUvOffsetScale", uvTransformParam(asset.baseColorTexture));
            ok = ok && setMaterialParam(blob, params, "metallicRoughnessUvOffsetScale", uvTransformParam(asset.metallicRoughnessTexture));
            ok = ok && setMaterialParam(blob, params, "normalUvOffsetScale", uvTransformParam(asset.normalTexture));
            ok = ok && setMaterialParam(blob, params, "occlusionUvOffsetScale", uvTransformParam(asset.emissiveTexture));
        }
        if (!ok) {
            RX_LOG_ERROR("sample_09_scene: imported material param setup failed for '{}'", asset.name);
            return false;
        }
        if (!finalizeMaterialBinding(app, binding, blob, blockSize)) {
            return false;
        }
        app.customMaterialIndexToBinding[assetHandle.index()] = app.customMaterialBindings.size();
        app.customMaterialBindings.push_back(std::move(binding));
    }
    app.resolveMaterialIndexToHandle = [&app](uint32_t idx) -> rx::material::MaterialHandle {
        auto it = app.customMaterialIndexToBinding.find(idx);
        return it != app.customMaterialIndexToBinding.end() ? app.customMaterialBindings[it->second].handle
                                                              : app.customMaterialBindings.front().handle;
    };
    app.resolveMaterialIndexToBinding = [&app](uint32_t idx) -> const MaterialGpuBinding* {
        auto it = app.customMaterialIndexToBinding.find(idx);
        return it != app.customMaterialIndexToBinding.end() ? &app.customMaterialBindings[it->second]
                                                              : &app.customMaterialBindings.front();
    };
    return true;
}

// [Phase 4 exit fix wave, I1] Shared by all three populate*()/build*()
// call sites below -- allocates `rx::rhi::FrameSync::kFramesInFlight`
// SEPARATE physical draw-data buffers (not one shared buffer -- see
// App::drawDataBuffers' own comment for the frames-in-flight race this
// closes) and registers each into bindless, sized for `rows` rows.
// `app.drawDataCapacityRows`/`app.drawDataRows` (the CPU staging mirror)
// are set here too, once, for every mode.
bool createDrawDataBuffers(App& app, uint32_t rows) {
    app.drawDataCapacityRows = rows;
    app.drawDataRows.assign(rows, rx::material::DrawDataGpu{});
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(rows) * sizeof(rx::material::DrawDataGpu);
    for (uint32_t slot = 0; slot < rx::rhi::FrameSync::kFramesInFlight; ++slot) {
        app.drawDataBuffers[slot] =
            app.allocator->createHostVisibleBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rx::rhi::MemoryCategory::Internal);
        if (!app.drawDataBuffers[slot].has_value()) {
            RX_LOG_ERROR("sample_09_scene: createHostVisibleBuffer (draw data, slot {}) failed", slot);
            return false;
        }
        app.drawDataBufferHandles[slot] = app.bindless->registerStorageBuffer(app.drawDataBuffers[slot]->handle(), bytes);
        if (!app.drawDataBufferHandles[slot].isValid()) {
            RX_LOG_ERROR("sample_09_scene: registerStorageBuffer (draw data, slot {}) failed", slot);
            return false;
        }
    }
    return true;
}

// Populates `scene`/`drawListBuilder` from a real ImportResult's own
// `scene.instances` (--scene <path> present-mode override, e.g. Sponza) --
// every instance keeps its DEFAULT layers(~0u)/channels(0xFF) (no
// grid-row-style demo grouping; that is the deterministic default
// composition's own feature, not a property of arbitrary imported
// content). `registry` is needed to size the D26.1 draw-data buffer
// correctly [fix round: a real bug an independent review's own end-to-end
// Sponza run surfaced -- see this function's own drawDataCapacityRows
// comment below].
bool populateImportedInstances(App& app, const rx::asset::Registry& registry, const rx::asset::ImportedScene& scene) {
    app.renderableHandles.clear();
    app.renderableHandles.reserve(scene.instances.size());
    // [Fix round] Total SUBMESH count across every instance, NOT the
    // renderable (instance) count -- `ViewLists::payloads` gets one row
    // PER SUBMESH-INSTANCE PAIRING (D26.1), and a single renderable's own
    // mesh can carry many submeshes (Sponza: 1 renderable, 25 submeshes,
    // one per material -- confirmed directly against a real fetched
    // Sponza import). Sizing `drawDataCapacityRows` off
    // `renderableHandles.size()` alone (1, for Sponza) let
    // updateSceneFrame() write up to 25 DrawDataGpu rows into a
    // 1-row-capacity buffer every frame -- a real heap buffer overflow,
    // reproduced directly as a validation-layer-internal segfault inside
    // vkCmdDrawIndexed (the corrupted heap state manifesting several
    // allocations later, not at the write itself) the first time this
    // task's own review ran `--scene sponza --present --validate` against
    // real content. The default DamagedHelmet grid and the procedural
    // --stress field both happen to have exactly 1 submesh per instance,
    // which is why this was never hit before a real multi-submesh mesh
    // (Sponza) was actually imported through this code path.
    uint32_t totalSubmeshCount = 0;
    for (const rx::asset::InstanceRecord& instance : scene.instances) {
        totalSubmeshCount += static_cast<uint32_t>(registry.mesh(instance.mesh).submeshes.size());
        rx::scene::RenderableDesc desc;
        desc.mesh = instance.mesh;
        desc.transform = instance.worldTransform;
        app.renderableHandles.push_back(app.scene->createRenderable(desc));
    }
    if (app.renderableHandles.empty()) {
        RX_LOG_ERROR("sample_09_scene: imported scene produced zero renderables");
        return false;
    }
    app.lightHandle = app.scene->createDirectionalLight(
        rx::scene::DirectionalLightDesc{app.lightDirWorld, glm::vec3(5.0F, 5.0F, 5.0F), true, 0xFFu});

    rx::asset::AABB totalBounds{};
    for (const rx::asset::AABB& b : app.scene->worldBoundsSpan()) {
        totalBounds = rx::asset::AABB::unionOf(totalBounds, b);
    }
    if (!totalBounds.isValid()) {
        totalBounds = rx::asset::AABB{glm::vec3(-1.0F), glm::vec3(1.0F)};
    }
    const glm::vec3 center = 0.5F * (totalBounds.min + totalBounds.max);
    const float radius = std::max(0.5F * glm::length(totalBounds.max - totalBounds.min), 0.1F);
    app.flyCamera.camera.position = center + glm::vec3(0.0F, radius * 0.4F, radius * 1.6F);
    const glm::vec3 lookDir = glm::normalize(center - app.flyCamera.camera.position);
    app.flyCamera.pitchRadians = std::asin(std::clamp(lookDir.y, -1.0F, 1.0F));
    app.flyCamera.yawRadians = std::atan2(-lookDir.x, -lookDir.z);
    app.flyCamera.syncOrientation();
    app.flyCamera.camera.nearPlane = std::max(0.01F, radius * 0.01F);
    app.flyCamera.camera.cullingFarPlane = radius * 8.0F;
    app.flyCamera.camera.cullMask = ~0u;
    app.moveSpeed = std::max(radius * 0.5F, 1.0F);

    if (!createDrawDataBuffers(app, std::max<uint32_t>(totalSubmeshCount, 1))) {
        return false;
    }
    return true;
}

// --- Grid layout [deterministic layer-mask culling -- see this file's own
// header comment] ---------------------------------------------------------
glm::mat4 gridTransform(uint32_t row, uint32_t col, float spacing) {
    const float x = (static_cast<float>(col) - 0.5F * static_cast<float>(kGridCols - 1)) * spacing;
    const float z = -(static_cast<float>(row) * spacing);
    return glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.0F, z));
}

// Populates the helmet grid's Scene/DrawListBuilder/renderables given the
// real imported MeshHandle (Registry already holds the MeshAsset by the
// time this runs -- see runHeadless()/runPresent()'s own import call).
bool populateHelmetGrid(App& app, rx::asset::MeshHandle helmetMesh) {
    const glm::vec3 size = app.helmetLocalBounds.isValid() ? (app.helmetLocalBounds.max - app.helmetLocalBounds.min)
                                                             : glm::vec3(1.0F);
    const float radius = std::max(0.5F * glm::length(size), 0.01F);
    const float spacing = radius * 3.0F;  // generous non-overlap margin (radius-scale gap either side).

    app.scene.emplace(rx::scene::meshBoundsFromRegistry(*app.registry));
    app.drawListBuilder.emplace(rx::scene::meshSubmeshesFromRegistry(*app.registry),
                                 rx::scene::materialResolveFromRegistry(*app.registry));

    app.renderableHandles.clear();
    app.renderableHandles.reserve(kGridInstanceCount);
    for (uint32_t row = 0; row < kGridRows; ++row) {
        for (uint32_t col = 0; col < kGridCols; ++col) {
            rx::scene::RenderableDesc desc;
            desc.mesh = helmetMesh;
            desc.transform = gridTransform(row, col, spacing);
            desc.layers = (1u << row);
            // Row 0 alone carries the highlight channel EXCLUSIVELY (never
            // 0xFF) -- see HudState's own comment for why this isolates
            // row 0 as the light-channel demo's subject.
            desc.channels = (row == 0) ? kHighlightChannelBit : 0xFFu;
            const rx::scene::RenderableHandle handle = app.scene->createRenderable(desc);
            app.renderableHandles.push_back(handle);
        }
    }

    app.lightHandle = app.scene->createDirectionalLight(rx::scene::DirectionalLightDesc{
        app.lightDirWorld, glm::vec3(5.0F, 5.0F, 5.0F), /*castsShadows=*/true, /*channels=*/0xFFu});

    // Fixed startup camera [gate ruling #15's own "fixed startup camera
    // pose" requirement] -- elevated, angled down, framing the whole grid
    // generously (deterministic culling is via layer mask, not frustum
    // extent -- see this file's own header comment -- so this framing only
    // needs to be reasonable, not exactly boundary-tuned).
    const float halfWidth = 0.5F * static_cast<float>(kGridCols - 1) * spacing;
    const float halfDepth = static_cast<float>(kGridRows - 1) * spacing;
    const glm::vec3 gridCenter(0.0F, 0.0F, -0.5F * halfDepth);
    const float camDistance = std::max(halfWidth, halfDepth) * 0.9F + radius * 4.0F;
    app.flyCamera.camera.position = gridCenter + glm::vec3(0.0F, camDistance * 0.45F, camDistance);
    const glm::vec3 lookDir = glm::normalize(gridCenter - app.flyCamera.camera.position);
    app.flyCamera.pitchRadians = std::asin(std::clamp(lookDir.y, -1.0F, 1.0F));
    app.flyCamera.yawRadians = std::atan2(-lookDir.x, -lookDir.z);
    app.flyCamera.syncOrientation();
    app.flyCamera.camera.nearPlane = std::max(0.01F, radius * 0.05F);
    app.flyCamera.camera.cullingFarPlane = camDistance * 4.0F + halfDepth * 2.0F;
    app.flyCamera.camera.cullMask = app.hud.cullMaskFromToggles();  // == (1<<kVisibleRowCount)-1 by HudState's own default.
    app.moveSpeed = std::max(radius * 2.0F, 0.5F);

    if (!createDrawDataBuffers(app, kGridInstanceCount)) {
        return false;
    }
    return true;
}

// --- Stress-v2 field [Registry-free -- see this file's own header comment]
// -----------------------------------------------------------------------
struct HostGeometry {
    std::vector<rx::asset::PoolVertex> vertices;
    std::vector<uint32_t> indices;
};

void addStressQuad(HostGeometry& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 normal) {
    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    const std::array<glm::vec3, 4> pts{a, b, c, d};
    const std::array<glm::vec2, 4> uvs{glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1)};
    for (size_t i = 0; i < 4; ++i) {
        rx::asset::PoolVertex v{};
        v.px = pts[i].x;
        v.py = pts[i].y;
        v.pz = pts[i].z;
        v.nx = normal.x;
        v.ny = normal.y;
        v.nz = normal.z;
        // Arbitrary, non-degenerate tangent -- this workload's Unlit
        // materials never read it, but forward_entry.slang's vertexMain
        // unconditionally transforms it (safeNormalize() guards the
        // exact-zero case regardless -- see that shader's own comment).
        v.tx = 1.0F;
        v.ty = 0.0F;
        v.tz = 0.0F;
        v.tw = 1.0F;
        v.u = uvs[i].x;
        v.v = uvs[i].y;
        mesh.vertices.push_back(v);
    }
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 3);
}

HostGeometry generateStressCube(float h) {
    HostGeometry mesh;
    addStressQuad(mesh, {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h}, {1, 0, 0});
    addStressQuad(mesh, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h}, {-1, 0, 0});
    addStressQuad(mesh, {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}, {0, 1, 0});
    addStressQuad(mesh, {-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {0, -1, 0});
    addStressQuad(mesh, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});
    addStressQuad(mesh, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});
    return mesh;
}

uint32_t stressGridSize(uint32_t drawCount) { return static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(drawCount)))); }

glm::vec3 stressGridPosition(uint32_t i, uint32_t drawCount) {
    const uint32_t gridSize = stressGridSize(drawCount);
    const uint32_t gx = i % gridSize;
    const uint32_t gz = i / gridSize;
    const float half = static_cast<float>(gridSize - 1) * 0.5F;
    return glm::vec3((static_cast<float>(gx) - half) * kStressInstanceSpacing, 0.0F,
                      (static_cast<float>(gz) - half) * kStressInstanceSpacing);
}

// Builds the Registry-free `drawCount`-instance field [see this file's own
// header comment for the full "why fabricated handles, not Registry"
// rationale]. Camera frames the WHOLE field (top-down-ish, generous) so the
// default `--stress` run's own `culled == 0` -- the A/B comparability
// contract's own "held identical: 30k instances... per-instance data"
// clause is best served by a run where the ONLY difference from sample 07's
// own unconditional-30000-draws number is genuine D26.3 collapse, not an
// additional culling effect stacked on top (a caller free to move the fly-
// through camera afterward still exercises culling live, in --present mode
// -- this default framing is this run's own PUBLISHED-number posture, not
// a claim that culling is untested elsewhere).
bool buildStressField(App& app, uint32_t drawCount) {
    HostGeometry cube = generateStressCube(kStressInstanceScale);
    const rx::asset::MeshRange range = app.geometryPool->upload(cube.vertices, cube.indices);
    if (range.indexCount == 0) {
        RX_LOG_ERROR("sample_09_scene: GeometryPool::upload failed for the stress-v2 cube");
        return false;
    }

    app.stressMeshHandle = rx::asset::MeshHandle(0, 1);
    app.stressCubeLocalBounds = rx::asset::AABB{glm::vec3(-kStressInstanceScale), glm::vec3(kStressInstanceScale)};
    app.stressCubeSubmesh = rx::asset::Submesh{};
    app.stressCubeSubmesh.range = range;
    app.stressCubeSubmesh.bounds = app.stressCubeLocalBounds;
    app.stressCubeSubmesh.material = app.stressMaterialHandles[0];  // never read verbatim -- every instance overrides it.

    app.scene.emplace([&app](rx::asset::MeshHandle) { return app.stressCubeLocalBounds; });
    app.drawListBuilder.emplace(
        [&app](rx::asset::MeshHandle) -> std::span<const rx::asset::Submesh> {
            return std::span<const rx::asset::Submesh>(&app.stressCubeSubmesh, 1);
        },
        [&app](rx::asset::MaterialHandle handle) -> rx::scene::ResolvedMaterial {
            const uint32_t idx = handle.index() % kStressVariantCount;
            const rx::asset::AlphaMode alphaMode = (idx < 2) ? rx::asset::AlphaMode::Opaque : rx::asset::AlphaMode::Mask;
            return rx::scene::ResolvedMaterial{idx, alphaMode, app.stressVariantDoubleSided[idx],
                                                rx::asset::MaterialDisposition::Unlit};
        });

    app.renderableHandles.clear();
    app.renderableHandles.reserve(drawCount);
    std::array<std::optional<rx::asset::MaterialHandle>, 1> overrideStorage{};
    for (uint32_t i = 0; i < drawCount; ++i) {
        const glm::vec3 pos = stressGridPosition(i, drawCount);
        rx::scene::RenderableDesc desc;
        desc.mesh = app.stressMeshHandle;
        desc.transform = glm::translate(glm::mat4(1.0F), pos);
        overrideStorage[0] = app.stressMaterialHandles[i % kStressVariantCount];
        desc.submeshMaterialOverrides = overrideStorage;
        app.renderableHandles.push_back(app.scene->createRenderable(desc));
    }

    // Camera: elevated, looking straight down, framing the WHOLE field.
    const uint32_t gridSize = stressGridSize(drawCount);
    const float halfExtent = static_cast<float>(gridSize - 1) * 0.5F * kStressInstanceSpacing + kStressInstanceScale + 1.0F;
    app.flyCamera.camera.position = glm::vec3(0.0F, halfExtent * 2.5F + 10.0F, 0.01F);
    app.flyCamera.pitchRadians = glm::radians(-89.0F);
    app.flyCamera.yawRadians = 0.0F;
    app.flyCamera.syncOrientation();
    app.flyCamera.camera.nearPlane = 0.1F;
    app.flyCamera.camera.cullingFarPlane = halfExtent * 6.0F + 20.0F;
    app.flyCamera.camera.cullMask = ~0u;
    app.moveSpeed = 10.0F;

    if (!createDrawDataBuffers(app, drawCount)) {
        return false;
    }
    return true;
}

// Pre-warms MaterialSystem's own pipeline cache for every participating
// material (a D27 cache HIT at first real use in updateSceneFrame()'s own
// resolvePipeline callback, never a mid-frame recompile) and caches the
// shared pipeline-LAYOUT every one of them uses -- see
// App::cachedAnyMaterialLayout's own comment. `bindings` is whichever set
// is active this run (helmet: a 1-element span; stress: 4; custom-import:
// N). [Sponza texture-misassignment fix] Deliberately does NOT build a
// pipelineToken -> binding map anymore: distinct materials legitimately
// SHARE one VkPipeline whenever their fixed-function state matches (D28;
// Sponza: 22/25 materials), so such a map can only ever remember one
// binding per shared token -- see draw_recording.h's own
// materialIndexForSpan() header comment. Descriptor-set selection at
// record time now goes through App::resolveMaterialIndexToBinding instead,
// keyed by the REAL per-span materialIndex.
void warmMaterialPipelines(App& app, std::span<const MaterialGpuBinding> bindings) {
    const rx::material::PassSignature sig = forwardPassSignature();
    for (const MaterialGpuBinding& binding : bindings) {
        app.materialSystem->getPipeline({binding.handle, sig, 0});
    }
    // [Phase 4 exit fix wave, I3] Hoisted here, main-thread-only, setup-time
    // (this function is never called mid-frame) -- NOT recomputed inside
    // recordForwardChunk() anymore, which ran on worker chunks >= 1 and
    // called the now-guarded MaterialSystem::pipelineLayout() from there
    // (a sibling of the Task-24 GeometryPool::bind() violation). Every
    // participating material shares the IDENTICAL pipeline-layout SHAPE
    // (external bindless set-0 substitution + material set-1 -- Vulkan spec
    // 14.2.2 set-compatibility), so any one binding's layout is legal to
    // cache and reuse for every chunk regardless of which pipeline a given
    // draw ends up binding.
    if (!bindings.empty()) {
        app.cachedAnyMaterialLayout = app.materialSystem->pipelineLayout(bindings.front().handle);
    }
}

// --- Shadow setup [D21, RC3] ----------------------------------------------
// [Phase 4 exit fix wave, C1] No raw vkCreateImage/vkAllocateMemory/
// vkCreateImageView here anymore: "shadowmap" is declared as a genuine
// rx_graph transient (declareGraph()'s "shadow" pass writes it,
// shadowAttachmentDesc()), so the Executor's own TransientPool owns and
// lifetime-tracks its physical VkImage -- registering it into bindless
// happens lazily, from recordForwardChunk()'s chunk 0, exactly like
// "hdr"'s own lastHdrView/hdrHandle re-registration pattern
// (recordTonemapDraw() below). This function now only builds the
// comparison sampler (a real, independent Vulkan object -- not
// graph-owned) and the light-frustum fit + per-FIF shadow draw-data
// buffers.
bool setupShadow(App& app) {
    const VkDevice device = app.device->device();

    // Comparison sampler [D21, matches rx_material's own
    // test_standard_pbr_shadow_gpu.cpp rig verbatim].
    VkSamplerCreateInfo compareSamplerInfo{};
    compareSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    compareSamplerInfo.magFilter = VK_FILTER_LINEAR;
    compareSamplerInfo.minFilter = VK_FILTER_LINEAR;
    compareSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    compareSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    compareSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    compareSamplerInfo.compareEnable = VK_TRUE;
    compareSamplerInfo.compareOp = VK_COMPARE_OP_LESS;
    compareSamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &compareSamplerInfo, nullptr, &app.shadowCompareSampler) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_09_scene: vkCreateSampler(shadow compare) failed");
        return false;
    }
    app.shadowCompareSamplerHandle = app.bindless->registerComparisonSampler(app.shadowCompareSampler);
    if (!app.shadowCompareSamplerHandle.isValid()) {
        RX_LOG_ERROR("sample_09_scene: registerComparisonSampler failed");
        return false;
    }

    // Fitted, texel-snapped light frustum -- computed ONCE from the
    // scene's own total world bounds (a bounded, static-content scene;
    // see this sample's own header comment for why a per-frame refit is
    // not needed at this scope) with a generous depth-padding margin so
    // every caster this scene can ever contain lands within [0,1].
    rx::asset::AABB totalBounds{};
    for (const rx::asset::AABB& b : app.scene->worldBoundsSpan()) {
        totalBounds = rx::asset::AABB::unionOf(totalBounds, b);
    }
    if (!totalBounds.isValid()) {
        totalBounds = rx::asset::AABB{glm::vec3(-1.0F), glm::vec3(1.0F)};
    }
    const glm::mat4 lightView = rx::shadow::lightSpaceView(glm::normalize(app.lightDirWorld));
    const float depthPadding = 0.5F * glm::length(totalBounds.max - totalBounds.min) + 1.0F;
    app.shadowFit = rx::shadow::fitShadowFrustum(totalBounds.min, totalBounds.max, lightView, kShadowMapResolution, depthPadding);

    // [Fix round] Reuse the SAME (already-computed, submesh-count-correct
    // -- see populateImportedInstances()'s own comment) capacity the
    // forward pass's own drawDataCapacityRows uses, NOT
    // scene->renderableCount() -- ShadowLists::payloads is bounded by the
    // identical "one row per submesh-instance pairing" shape ViewLists::
    // payloads is (shadow casters are a SUBSET of all renderables), so the
    // same undercount bug (renderable count != submesh count for a
    // multi-submesh mesh like Sponza) applies here too. setupShadow() is
    // always called AFTER populateHelmetGrid()/populateImportedInstances()
    // in every call site, so `app.drawDataCapacityRows` is already correct
    // by this point.
    app.shadowDrawDataCapacityRows = app.drawDataCapacityRows;
    app.shadowDrawDataRows.assign(app.shadowDrawDataCapacityRows, rx::shadow::ShadowDrawData{});
    const VkDeviceSize shadowBytes =
        std::max<VkDeviceSize>(static_cast<VkDeviceSize>(app.shadowDrawDataCapacityRows) * sizeof(rx::shadow::ShadowDrawData),
                                sizeof(rx::shadow::ShadowDrawData));
    // [Phase 4 exit fix wave, I1] Per-FIF-slot, same rationale as
    // createDrawDataBuffers() above -- the shadow pass's own draw-data SSBO
    // is exactly as exposed to the frames-in-flight host-write race as the
    // forward pass's.
    for (uint32_t slot = 0; slot < rx::rhi::FrameSync::kFramesInFlight; ++slot) {
        app.shadowDrawDataBuffers[slot] =
            app.allocator->createHostVisibleBuffer(shadowBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rx::rhi::MemoryCategory::Internal);
        if (!app.shadowDrawDataBuffers[slot].has_value()) {
            RX_LOG_ERROR("sample_09_scene: createHostVisibleBuffer (shadow draw data, slot {}) failed", slot);
            return false;
        }
        app.shadowDrawDataBufferHandles[slot] = app.bindless->registerStorageBuffer(app.shadowDrawDataBuffers[slot]->handle(), shadowBytes);
        if (!app.shadowDrawDataBufferHandles[slot].isValid()) {
            RX_LOG_ERROR("sample_09_scene: registerStorageBuffer (shadow draw data, slot {}) failed", slot);
            return false;
        }
    }
    app.shadowEnabled = true;
    return true;
}

// --- Per-frame update -----------------------------------------------------
// Cull+collapse (DrawListBuilder::build()/buildShadow()), populate the CPU
// staging mirror of both D26.1 draw-data buffers from the resulting
// `payloads[]` (NOT from a per-renderable loop -- see this file's own
// header comment), and D27-pre-resolve every distinct materialIndex
// boundary -- all on the main thread, BEFORE `executor->execute()` (chunk
// fan-out never touches any of this again).
//
// [Phase 4 exit fix wave, I1] Deliberately does NOT write to the GPU-visible
// draw-data buffers anymore -- only `app.drawDataRows`/`app.shadowDrawDataRows`
// (plain CPU `std::vector`s, list-building's own scratch, safe to touch at
// any point before the frame's GPU work is submitted). The actual
// memcpy+flush into `app.drawDataBuffers[slot]`/`app.shadowDrawDataBuffers[slot]`
// is `uploadSceneFrameGpuBuffers()` below, called separately, AFTER the
// caller's own frame-slot fence wait -- see that function's own comment for
// why this split is load-bearing, not cosmetic (a single host-visible
// buffer written here, before the fence wait, is exactly the frames-in-flight
// host-write race this fix wave's I1 finding closed).
void updateSceneFrame(App& app, rx::task::Scheduler& scheduler) {
    // Refresh the block-buffer cache [see App::blockBufferCache's own
    // comment] -- main-thread-only GeometryPool accessors, called here so
    // the chunked forward pass's own worker chunks never touch them.
    for (uint32_t blockId = 0; blockId < app.geometryPool->blockCount(); ++blockId) {
        app.blockBufferCache[blockId] =
            App::BlockBuffers{app.geometryPool->vertexBufferHandle(blockId), app.geometryPool->indexBufferHandle(blockId)};
    }

    app.drawListBuilder->build(*app.scene, app.flyCamera.camera, scheduler, app.viewLists);

    const glm::mat4 viewProjTransposed = glm::transpose(app.flyCamera.camera.viewProj());
    const glm::vec3 cameraPos = app.flyCamera.camera.position;
    const uint8_t lightChannels = app.hud.lightChannelHighlightOn ? 0xFFu : static_cast<uint8_t>(0xFFu & ~kHighlightChannelBit);
    if (app.scene->isLightAlive(app.lightHandle)) {
        app.scene->setLightChannels(app.lightHandle, lightChannels);
    }

    const auto transforms = app.scene->transformsSpan();
    for (size_t i = 0; i < app.viewLists.payloads.size(); ++i) {
        const rx::scene::DrawPayload& payload = app.viewLists.payloads[i];
        const glm::mat4& model = transforms[payload.instanceDataIndex];
        const glm::mat3 normalMat3 = glm::transpose(glm::inverse(glm::mat3(model)));

        rx::material::DrawDataGpu& row = app.drawDataRows[i];
        row.model = glm::transpose(model);
        row.normalMatrix = glm::transpose(glm::mat4(normalMat3));
        row.viewProj = viewProjTransposed;
        row.lightDirWorld = glm::vec4(-glm::normalize(app.lightDirWorld), 0.0F);
        row.lightColor = glm::vec4(5.0F, 5.0F, 5.0F, 0.0F);
        row.ambientColor = glm::vec4(0.12F, 0.12F, 0.12F, 0.0F);
        row.cameraPosWorld = glm::vec4(cameraPos, 0.0F);
        row.materialIndex = payload.materialIndex;
        // [Phase 4 exit fix wave, C1] Also requires `app.shadowMapHandle.
        // isValid()` -- NOT just `app.shadowEnabled` -- since the handle is
        // now (re-)registered lazily, from recordForwardChunk()'s chunk 0,
        // the first time the graph-owned "shadowmap" transient is realized
        // (see App::lastShadowMapView's own comment). Before that first
        // registration (only ever true on the very first frame of a run:
        // the physical resource is never resized/evicted afterward, so the
        // handle stays valid and correct for every later frame), rows fall
        // back to the SAME documented 0xFFFFFFFF fully-lit sentinel the
        // shadowEnabled==false path already uses -- a well-defined,
        // one-frame-only degrade, never a read of an unregistered/stale
        // bindless slot. `!app.debugForceShadowSentinel`: see that field's
        // own comment -- the D17 discrimination re-proof's clean equivalent
        // of the review's own gdb-flip probe.
        if (app.shadowEnabled && app.shadowMapHandle.isValid() && !app.debugForceShadowSentinel) {
            row.lightViewProj = glm::transpose(app.shadowFit.lightViewProj);
            row.shadowMapTextureIndex = app.shadowMapHandle.index();
            row.shadowCompareSamplerIndex = app.shadowCompareSamplerHandle.index();
            row.shadowTexelSize = 1.0F / static_cast<float>(kShadowMapResolution);
        } else {
            row.shadowMapTextureIndex = 0xFFFFFFFFu;
        }
    }

    if (app.shadowEnabled) {
        app.drawListBuilder->buildShadow(*app.scene, app.lightHandle, app.flyCamera.camera, scheduler, app.shadowLists);
        for (size_t i = 0; i < app.shadowLists.payloads.size(); ++i) {
            const rx::scene::DrawPayload& payload = app.shadowLists.payloads[i];
            rx::shadow::ShadowDrawData& row = app.shadowDrawDataRows[i];
            row.model = glm::transpose(transforms[payload.instanceDataIndex]);
            row.lightViewProj = glm::transpose(app.shadowFit.lightViewProj);
        }
    }

    // [D27] Main-thread pre-resolution -- once per frame, before any chunk
    // fan-out. `resolvePipeline` returns a `getPipeline()` cache HIT every
    // time, by construction (warmMaterialPipelines() pre-warmed every
    // participating material's pipeline at setup).
    const rx::material::PassSignature passSig = forwardPassSignature();
    const uint64_t passSigHash = passSig.hash();
    app.resolvedGroups = rx::scene::resolveDrawGroups(
        app.viewLists, passSigHash, 0, [&app, &passSig](const rx::scene::PipelineRequestKey& key) -> uint64_t {
            const rx::material::MaterialHandle handle = app.resolveMaterialIndexToHandle(key.materialIndex);
            VkPipeline pipeline = app.materialSystem->getPipeline({handle, passSig, 0});
            return std::bit_cast<uint64_t>(pipeline);
        });
}

// [Phase 4 exit fix wave, I1] The GPU-write half `updateSceneFrame()` above
// deliberately no longer performs -- memcpy's `app.drawDataRows`/
// `app.shadowDrawDataRows` (already populated by `updateSceneFrame()`) into
// THIS frame's `frameSlot`-th physical buffer and flushes it. Sets
// `app.currentFrameSlot = frameSlot` first (read by `recordForwardChunk()`'s
// worker chunks and `recordShadowPass()` to select the matching bindless
// handle -- see App::currentFrameSlot's own comment).
//
// CALLER CONTRACT: must be called AFTER the caller has confirmed, via its
// own fence wait, that frame `frameSlot`'s prior GPU work (the last frame
// that wrote/read this SAME physical slot, `rx::rhi::FrameSync::
// kFramesInFlight` frames ago) has completed -- mirrors
// `rx::material::MaterialSystem::beginFrame()`'s own documented "only after
// the caller's own fence wait for this slot" contract (material_system.h),
// the named FIF-discipline precedent this fix wave's I1 finding cites.
// samples/09_scene's present loop calls this right after
// `vkWaitForFences(frameSync->currentFence())`; headless mode (no real
// frames-in-flight overlap -- `captureFrame()`'s `runOnce()` submits and
// waits synchronously every call) always passes slot 0.
void uploadSceneFrameGpuBuffers(App& app, uint32_t frameSlot) {
    app.currentFrameSlot = frameSlot;

    if (!app.viewLists.payloads.empty()) {
        const VkDeviceSize bytes = app.viewLists.payloads.size() * sizeof(rx::material::DrawDataGpu);
        std::memcpy(app.drawDataBuffers[frameSlot]->mappedData(), app.drawDataRows.data(), static_cast<size_t>(bytes));
        app.drawDataBuffers[frameSlot]->flush();
    }

    if (app.shadowEnabled && !app.shadowLists.payloads.empty()) {
        const VkDeviceSize bytes = app.shadowLists.payloads.size() * sizeof(rx::shadow::ShadowDrawData);
        std::memcpy(app.shadowDrawDataBuffers[frameSlot]->mappedData(), app.shadowDrawDataRows.data(), static_cast<size_t>(bytes));
        app.shadowDrawDataBuffers[frameSlot]->flush();
    }
}

// --- Recording -------------------------------------------------------------
void recordShadowPass(rx::graph::PassContext& ctx, App& app) {
    if (!app.shadowEnabled || app.shadowLists.commands.empty()) {
        return;
    }
    VkCommandBuffer cmd = ctx.cmd;
    VkViewport viewport{0.0F, 0.0F, static_cast<float>(kShadowMapResolution), static_cast<float>(kShadowMapResolution), 0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, {kShadowMapResolution, kShadowMapResolution}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Slope-scaled bias -- fixed, reasonable defaults for this scene's own
    // scale (D21's own mechanism; this sample does not expose bias
    // tuning as a CLI knob).
    app.shadowCaster->bindAndSetDepthBias(cmd, /*constantFactor=*/2.0F, /*slopeFactor=*/2.5F);
    VkDescriptorSet set = app.bindless->descriptorSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.shadowCaster->pipelineLayout(), 0, 1, &set, 0, nullptr);
    // [Phase 4 exit fix wave, I1] `app.currentFrameSlot`-th buffer -- see
    // that field's own comment; set by uploadSceneFrameGpuBuffers() before
    // executor->execute() ever reaches this callback.
    rx::shadow::ShadowCasterPushConstants push{app.shadowDrawDataBufferHandles[app.currentFrameSlot].index()};
    vkCmdPushConstants(cmd, app.shadowCaster->pipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    // ShadowLists is a single-pipeline pass (RC3) -- `blocks` alone (no
    // pipeline-group resolution needed) gives contiguous per-block runs.
    for (const rx::scene::BlockRange& blockRange : app.shadowLists.blocks) {
        app.geometryPool->bind(cmd, blockRange.blockId);
        for (uint32_t i = blockRange.firstCommand; i < blockRange.firstCommand + blockRange.commandCount; ++i) {
            const rx::scene::DrawCommand& c = app.shadowLists.commands[i];
            vkCmdDrawIndexed(cmd, c.indexCount, c.instanceCount, c.firstIndex, c.vertexOffset, c.firstInstance);
        }
    }
}

void recordForwardChunk(rx::graph::PassContext& ctx, App& app, uint32_t chunkIndex, uint32_t chunkCount) {
    if (chunkIndex == 0 && app.shadowEnabled) {
        // [Phase 4 exit fix wave, C1] (Re-)register "shadowmap"'s CURRENT
        // resolved view into bindless, once per execute() call -- NOT once
        // per draw -- main-thread-only (BindlessTable registration,
        // docs/threading.md), safe here ONLY because chunk 0 of a
        // setExecuteChunked() callback is guaranteed to run synchronously,
        // on the calling (main) thread, before any other chunk begins
        // [pass.h: Pass::setExecuteChunked()'s own doc comment]. Placed
        // BEFORE every early return below (unlike the rest of this
        // function's body) so it always runs on chunk 0 regardless of
        // whether this particular frame has any forward draws at all --
        // exactly samples/05_multipass's own recordLitDrawsChunked()
        // pattern, applied to "shadowmap" instead of "hdr". `ctx.imageView`
        // resolves against whatever Executor::realize() most recently
        // bound "shadowmap" to (declareGraph()'s "shadow" pass output) --
        // legal to call here specifically because "forward" now declares
        // addTextureInput("shadowmap"), making the name part of this
        // compiled graph's own resolvable resource set.
        VkImageView shadowView = ctx.imageView("shadowmap");
        if (shadowView != app.lastShadowMapView) {
            if (app.shadowMapHandle.isValid()) {
                app.bindless->release(app.shadowMapHandle);
            }
            app.shadowMapHandle = app.bindless->registerSampledImage(shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            app.lastShadowMapView = shadowView;
        }
    }

    VkCommandBuffer cmd = ctx.chunkCommandBuffer();
    VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width), static_cast<float>(ctx.renderArea.height),
                        0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, ctx.renderArea};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const uint32_t totalCommands = static_cast<uint32_t>(app.viewLists.commands.size());
    if (totalCommands == 0) {
        return;
    }
    const uint32_t perChunk = (totalCommands + chunkCount - 1) / chunkCount;
    const uint32_t begin = std::min(chunkIndex * perChunk, totalCommands);
    const uint32_t end = std::min(begin + perChunk, totalCommands);
    if (begin >= end) {
        return;
    }

    if (!app.resolveMaterialIndexToBinding) {
        return;
    }
    VkDescriptorSet set = app.bindless->descriptorSet();
    // [Phase 4 exit fix wave, I3] Main-thread-cached at setup
    // (warmMaterialPipelines()) -- NOT resolved here anymore.
    // MaterialSystem::pipelineLayout() is guarded main-thread-only
    // (docs/threading.md); calling it from this worker chunk (chunkIndex
    // can be >= 1) was the exact violation class Task 24 already fixed for
    // GeometryPool::bind() (see App::blockBufferCache's own comment) but
    // left unfixed one call site later, here. Every participating material
    // shares the IDENTICAL pipeline-layout SHAPE (external bindless set-0
    // substitution + material set-1) -- any one binding's layout is legal
    // to bind set 0 against regardless of which pipeline ends up bound
    // later (Vulkan spec 14.2.2 set-compatibility, same precedent
    // samples/08_gltf_viewer's own recordSceneDraws() already relies on).
    VkPipelineLayout anyLayout = app.cachedAnyMaterialLayout;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, anyLayout, /*firstSet=*/0, 1, &set, 0, nullptr);

    // [D27 caller-side fix -- see draw_recording.h's own header comment]
    // resolveDrawGroups()'s own materialIndex-adjacency scan has no
    // blockId awareness; splitByBlockAndGroup() further subdivides this
    // chunk's own command range at every block boundary too, so
    // GeometryPool::bind()'s "one block per bind-to-next-bind span"
    // contract always holds.
    const auto spans = rx::samples9::splitByBlockAndGroup(app.resolvedGroups, app.viewLists.blocks, begin, end - begin);

    // [Sponza texture-misassignment fix] `span.pipelineToken` and the span's
    // REAL materialIndex are tracked and re-bound INDEPENDENTLY on purpose:
    // rebinding the VkPipeline only when the token actually changes is
    // still a valid, cheap state-sort optimization (many spans genuinely do
    // share one pipeline back-to-back), but the material-params descriptor
    // set (textures) MUST be re-selected every time the real materialIndex
    // changes, even across two spans that happen to share one pipelineToken
    // -- see draw_recording.h's own materialIndexForSpan() header comment
    // for why those two are NOT the same condition in general (D28
    // fixed-function-state collapse: Sponza's own glTF has 22/25 materials
    // sharing one VkPipeline). Binding set 1 off `pipelineTokenToBinding`
    // (the historical bug) silently rendered every one of those 22
    // materials' geometry with whichever ONE of them happened to be
    // registered last -- e.g. a roof/parapet material's textures appearing
    // on column shafts and arches, reproducible on any driver.
    uint32_t lastBlock = UINT32_MAX;
    uint64_t lastToken = 0;
    bool havePipeline = false;
    uint32_t lastMaterialIndex = UINT32_MAX;
    bool haveMaterial = false;
    for (const rx::samples9::RecordSpan& span : spans) {
        if (span.blockId != lastBlock) {
            // NOT GeometryPool::bind() -- main-thread-only, unsafe from a
            // worker chunk. Byte-identical raw calls against the
            // main-thread-cached handles instead (see App::blockBufferCache's
            // own comment): binding 0/offset 0/VK_INDEX_TYPE_UINT32
            // matches bind()'s own documented contract exactly.
            auto it = app.blockBufferCache.find(span.blockId);
            if (it != app.blockBufferCache.end()) {
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &it->second.vertexBuffer, &offset);
                vkCmdBindIndexBuffer(cmd, it->second.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            }
            lastBlock = span.blockId;
        }
        if (!havePipeline || span.pipelineToken != lastToken) {
            VkPipeline pipeline = std::bit_cast<VkPipeline>(span.pipelineToken);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            lastToken = span.pipelineToken;
            havePipeline = true;
        }
        const uint32_t materialIndex =
            rx::samples9::materialIndexForSpan(app.viewLists.commands, app.viewLists.payloads, span);
        if (!haveMaterial || materialIndex != lastMaterialIndex) {
            const MaterialGpuBinding* mb = app.resolveMaterialIndexToBinding(materialIndex);
            if (mb != nullptr) {
                // [Phase 4 exit fix wave, I1] `app.currentFrameSlot`-th
                // buffer -- see that field's own comment.
                rx::material::MaterialGlobalsPush push{app.defaultSamplerHandle.index(),
                                                         app.drawDataBufferHandles[app.currentFrameSlot].index(), 0.0F};
                vkCmdPushConstants(cmd, anyLayout, mb->pushStages, mb->pushOffset, mb->pushSize, &push);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, anyLayout, /*firstSet=*/1, 1, &mb->paramSet, 0,
                                         nullptr);
            }
            lastMaterialIndex = materialIndex;
            haveMaterial = true;
        }
        for (uint32_t i = span.commandOffset; i < span.commandOffset + span.commandCount; ++i) {
            const rx::scene::DrawCommand& c = app.viewLists.commands[i];
            vkCmdDrawIndexed(cmd, c.indexCount, c.instanceCount, c.firstIndex, c.vertexOffset, c.firstInstance);
        }
    }
}

void recordTonemapDraw(rx::graph::PassContext& ctx, App& app) {
    VkImageView hdrView = ctx.imageView("hdr");
    if (hdrView != app.lastHdrView) {
        if (app.hdrHandle.isValid()) {
            app.bindless->release(app.hdrHandle);
        }
        app.hdrHandle = app.bindless->registerSampledImage(hdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        app.lastHdrView = hdrView;
    }
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.tonemapPass.pipeline);
    VkDescriptorSet set = app.bindless->descriptorSet();
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.tonemapPass.layoutBundle.layout, 0, 1, &set, 0,
                             nullptr);
    VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width), static_cast<float>(ctx.renderArea.height),
                        0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, ctx.renderArea};
    vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);
    TonemapPushConstants push{};
    push.hdrTextureIndex = app.hdrHandle.index();
    push.hdrSamplerIndex = app.defaultSamplerHandle.index();
    vkCmdPushConstants(ctx.cmd, app.tonemapPass.layoutBundle.layout, app.tonemapPass.pushConstantStages,
                       app.tonemapPass.pushConstantOffset, app.tonemapPass.pushConstantSize, &push);
    vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
}

rx::graph::AttachmentDesc swapchainRelativeDesc(VkFormat format) {
    rx::graph::AttachmentDesc desc;
    desc.format = format;
    desc.sizeClass = rx::graph::SizeClass::SwapchainRelative;
    desc.width = 1.0F;
    desc.height = 1.0F;
    return desc;
}

rx::graph::AttachmentDesc swapchainRelativeReversedDepthDesc(VkFormat format) {
    rx::graph::AttachmentDesc desc = swapchainRelativeDesc(format);
    desc.depthConvention = rx::graph::DepthConvention::Reversed;
    return desc;
}

// [D29] Shadow map: STANDARD-Z, ABSOLUTE size (never resized with the
// window) -- the two shadow-pass-specific deviations from the main pass's
// own swapchainRelativeReversedDepthDesc() above.
rx::graph::AttachmentDesc shadowAttachmentDesc() {
    rx::graph::AttachmentDesc desc;
    desc.format = kShadowFormat;
    desc.sizeClass = rx::graph::SizeClass::Absolute;
    desc.width = static_cast<float>(kShadowMapResolution);
    desc.height = static_cast<float>(kShadowMapResolution);
    desc.depthConvention = rx::graph::DepthConvention::Standard;
    return desc;
}

void declareGraph(rx::graph::RenderGraph& graph, App& app, VkFormat backbufferFormat) {
    if (app.shadowEnabled) {
        graph.addPass("shadow")
            .setDepthStencilOutput("shadowmap", shadowAttachmentDesc())
            .setExecute([&app](rx::graph::PassContext& ctx) { recordShadowPass(ctx, app); });
    }

    rx::graph::Pass& forward = graph.addPass("forward");
    if (app.shadowEnabled) {
        // [Phase 4 exit fix wave, C1] THE fix: "forward" genuinely CONSUMES
        // "shadowmap" through the graph -- this both (a) un-culls the
        // "shadow" pass (RenderGraph::compile()'s reachability walk now
        // reaches it transitively: "forward" is reachable from the
        // backbuffer writer, and "forward" depends on "shadowmap"'s
        // writer), and (b) buys the depth-write -> sampled-read barrier
        // (layout transition + availability/visibility) the graph derives
        // for every addTextureInput() declaration -- exactly D29's
        // documented mixed-convention composition (reversed-Z "forward" +
        // standard-Z "shadow" in one graph) working as designed, NOT a
        // setSideEffect() band-aid on the "shadow" pass (which would keep
        // it alive without ever proving anything downstream actually reads
        // its output, or deriving the layout transition the forward
        // shader's SampleCmp taps require).
        forward.addTextureInput("shadowmap");
    }
    forward.addColorOutput("hdr", swapchainRelativeDesc(kHdrFormat))
        .setDepthStencilOutput("depth", swapchainRelativeReversedDepthDesc(kDepthFormat))
        .setExecuteChunked(
            [&app](rx::graph::PassContext& ctx, uint32_t chunkIndex, uint32_t chunkCount) {
                recordForwardChunk(ctx, app, chunkIndex, chunkCount);
            });

    graph.addPass("tonemap")
        .addTextureInput("hdr")
        .addColorOutput("backbuffer", swapchainRelativeDesc(backbufferFormat))
        .setExecute([&app](rx::graph::PassContext& ctx) { recordTonemapDraw(ctx, app); });

    if (app.overlay.has_value()) {
        // [gate ruling #16] Declared AFTER the scene passes above -- LOAD,
        // not CLEAR, so the HUD composites over the already-rendered scene
        // (Overlay::addPass()'s own comment: rx_graph::Executor derives
        // this automatically from write order, never a caller-set flag).
        app.overlay->addPass(graph, "backbuffer");
    }

    graph.setBackbufferSource("backbuffer");
}

// --- HUD widgets [matrix rows 18-20; gate ruling #15] ---------------------
// Draws the real ImGui widget content this task's own HUD content list
// requires. Called between `overlay->beginFrame()` and `executor->execute()`
// -- see runHeadless()'s own two-frame split (D17-gated capture draws NO
// widgets at all; the separate HUD-smoke-test frame calls this).
void drawHud(App& app, rx::rhi::Device& device, VkSurfaceKHR surface, bool presentMode) {
    // Explicit size on first appearance -- avoids Dear ImGui's own "brand
    // new auto-fit window's first frame only measures content, doesn't
    // draw it" behavior (src/rx_debug_ui/tests/test_overlay_gpu.cpp's own
    // GPU test sets both position AND size for the identical reason).
    ImGui::SetNextWindowSize(ImVec2(280.0F, 420.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("sample_09_scene");

    ImGui::Text("Frame: %.3f ms  |  FPS (60-frame avg): %.1f", app.hud.lastFrameMs, app.hud.rollingFps());
    ImGui::Separator();

    ImGui::Text("Cull counters");
    ImGui::Text("  imported/candidates: %u", app.viewLists.counters.totalCandidates);
    ImGui::Text("  culled (layer+frustum): %u (layer=%u frustum=%u)",
                app.viewLists.counters.culledByLayerMask + app.viewLists.counters.culledByFrustum,
                app.viewLists.counters.culledByLayerMask, app.viewLists.counters.culledByFrustum);
    ImGui::Text("  visible: %u", app.viewLists.counters.visible);
    ImGui::Text("  recordsIn: %u  drawsSubmitted: %u", app.viewLists.counters.recordsIn, app.viewLists.counters.drawsSubmitted);
    const double collapseRatio = app.viewLists.counters.recordsIn > 0
                                      ? 100.0 * (1.0 - static_cast<double>(app.viewLists.counters.drawsSubmitted) /
                                                            static_cast<double>(app.viewLists.counters.recordsIn))
                                      : 0.0;
    ImGui::Text("  instancing-collapse ratio: %.2f%%", collapseRatio);
    ImGui::Separator();

    // vsync toggle -- the SAME setPresentMode+recreateSwapchain path the
    // --vsync CLI flag drives (present mode only; no live swapchain to
    // toggle headless).
    if (presentMode) {
        if (ImGui::Checkbox("vsync", &app.hud.vsyncOn)) {
            const rx::rhi::PresentMode mode = app.hud.vsyncOn ? rx::rhi::PresentMode::VsyncOn : rx::rhi::PresentMode::VsyncOff;
            device.setPresentMode(mode);
            if (!device.recreateSwapchain(surface)) {
                RX_LOG_ERROR("sample_09_scene: recreateSwapchain failed applying the HUD vsync toggle");
            } else {
                RX_LOG_INFO("sample_09_scene: --present: present mode in use: {}", rx::rhi::presentModeName(device.presentMode()));
            }
        }
    } else {
        ImGui::Text("vsync: %s (present-mode only)", app.hud.vsyncOn ? "on" : "off");
    }
    ImGui::Separator();

    // TWO VISIBLY DISTINCT MASK CONTROLS [gate ruling #15]: layer-mask
    // (camera cullMask u32, visibility ONLY) vs light-channel (u8,
    // lighting/shadow-casting ONLY) -- never one shared control.
    ImGui::Text("Layer-mask instance-group toggles (visibility ONLY -- camera.cullMask, u32)");
    bool cullMaskChanged = false;
    for (uint32_t row = 0; row < kGridRows; ++row) {
        char label[32];
        std::snprintf(label, sizeof(label), "row %u", row);
        cullMaskChanged |= ImGui::Checkbox(label, &app.hud.layerRowVisible[row]);
        if (row + 1 < kGridRows) {
            ImGui::SameLine();
        }
    }
    if (cullMaskChanged) {
        app.flyCamera.camera.cullMask = app.hud.cullMaskFromToggles();
    }
    ImGui::Checkbox("light-channel demo: highlight row 0 lit", &app.hud.lightChannelHighlightOn);
    ImGui::TextWrapped(
        "Toggling the layer checkboxes above hides/shows whole rows outright. Toggling the light-channel box "
        "below only changes whether row 0 is LIT/casts a shadow -- it never hides geometry.");
    ImGui::Separator();

    // Pool stats [D8/D9, gate ruling #15].
    const rx::asset::PoolStats poolStats = app.geometryPool->stats();
    ImGui::Text("GeometryPool: %u block(s), vtx %.1f/%.1f KiB, idx %.1f/%.1f KiB", poolStats.blockCount,
                static_cast<double>(poolStats.vertexBytesUsed) / 1024.0, static_cast<double>(poolStats.vertexBytesCapacity) / 1024.0,
                static_cast<double>(poolStats.indexBytesUsed) / 1024.0, static_cast<double>(poolStats.indexBytesCapacity) / 1024.0);

    // #27 memory report [D24(c)].
    const rx::rhi::RxMemoryReport memReport = app.allocator->report();
    ImGui::Text("Memory report (source: %s)", rx::rhi::memoryBudgetSourceName(memReport.budgetSource));
    for (size_t i = 0; i < rx::rhi::kMemoryCategoryCount; ++i) {
        const auto category = static_cast<rx::rhi::MemoryCategory>(i);
        const rx::rhi::MemoryCategoryStats& stats = memReport.category(category);
        ImGui::Text("  %s: %.1f KiB (%u alloc(s))", rx::rhi::memoryCategoryName(category),
                    static_cast<double>(stats.bytes) / 1024.0, stats.count);
    }
    for (const rx::rhi::MemoryHeapReport& heap : memReport.heaps) {
        ImGui::Text("  heap %u: %.1f/%.1f MiB", heap.heapIndex, static_cast<double>(heap.usageBytes) / (1024.0 * 1024.0),
                    static_cast<double>(heap.budgetBytes) / (1024.0 * 1024.0));
    }

    ImGui::End();
}

// --- Fly-through camera input update [Task 20 input surface] -------------
void updateFlyCamera(App& app, rx::platform::Window& window, float dt) {
    if (ImGui::GetIO().WantCaptureKeyboard) {
        return;  // [gate ruling #16] HUD focus gates platform-input consumption.
    }
    float forward = 0.0F;
    float strafe = 0.0F;
    float vertical = 0.0F;
    if (window.isKeyDown(SDL_SCANCODE_W)) forward += 1.0F;
    if (window.isKeyDown(SDL_SCANCODE_S)) forward -= 1.0F;
    if (window.isKeyDown(SDL_SCANCODE_D)) strafe += 1.0F;
    if (window.isKeyDown(SDL_SCANCODE_A)) strafe -= 1.0F;
    if (window.isKeyDown(SDL_SCANCODE_SPACE)) vertical += 1.0F;
    if (window.isKeyDown(SDL_SCANCODE_LCTRL)) vertical -= 1.0F;
    const bool fast = window.isKeyDown(SDL_SCANCODE_LSHIFT);

    const rx::platform::GamepadState pad = window.poll();
    if (pad.connected) {
        strafe += pad.leftStick.x;
        forward += -pad.leftStick.y;  // SDL's own Y-down stick convention -- forward is -Y.
    }

    if (!ImGui::GetIO().WantCaptureMouse) {
        const rx::platform::MouseDelta delta = window.consumeMouseDelta();
        app.flyCamera.applyLookDelta(delta.x * FlyCamera::kMouseSensitivity, delta.y * FlyCamera::kMouseSensitivity);
    } else {
        window.consumeMouseDelta();  // still drain the accumulator so a later frame doesn't see a stale jump.
    }
    if (pad.connected) {
        app.flyCamera.applyLookDelta(pad.rightStick.x * FlyCamera::kGamepadLookSensitivity * dt,
                                      -pad.rightStick.y * FlyCamera::kGamepadLookSensitivity * dt);
    }

    const float speed = app.moveSpeed * (fast || (pad.connected && pad.rightTrigger > 0.5F) ? FlyCamera::kFastMoveMultiplier : 1.0F);
    const glm::vec3 localDelta = rx::samples9::flyCameraLocalMoveDelta(forward, strafe, vertical);
    if (glm::length(localDelta) > 0.0001F) {
        app.flyCamera.moveLocal(glm::normalize(localDelta) * speed * dt);
    }
}

// --- D17 reference-gate support [samples/08_gltf_viewer's own identical
// helpers] -----------------------------------------------------------------
struct ChannelIndices {
    int r;
    int g;
    int b;
};

std::optional<ChannelIndices> channelIndicesForFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return ChannelIndices{0, 1, 2};
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return ChannelIndices{2, 1, 0};
        default:
            return std::nullopt;
    }
}

bool canonicalizeToRgba8(std::vector<uint8_t>& pixels, VkFormat format) {
    const auto channels = channelIndicesForFormat(format);
    if (!channels.has_value()) {
        RX_LOG_ERROR("sample_09_scene: backbuffer format {} is not one of the R8G8B8A8/B8G8R8A8 families the D17 gate knows",
                     static_cast<int>(format));
        return false;
    }
    if (channels->r == 0 && channels->b == 2) {
        return true;
    }
    for (size_t offset = 0; offset + 4 <= pixels.size(); offset += 4) {
        std::swap(pixels[offset + 0], pixels[offset + 2]);
    }
    return true;
}

bool isLavapipeDevice(VkPhysicalDevice physicalDevice) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    return std::string_view(props.deviceName).find("llvmpipe") != std::string_view::npos;
}

constexpr uint32_t kHeadlessFrameCount = 3;

// --- Headless mode ----------------------------------------------------
// Default composition (--stress absent): a SYNC DamagedHelmet import into
// the deterministic 4x4 grid, EXACT counter assertions across
// `kHeadlessFrameCount` static-camera frames, a D17 tolerance-pixel gate
// against the committed (no-HUD) reference PNG, then one SEPARATE,
// non-pixel-gated frame proving the HUD overlay pass actually renders
// (non-empty ImGui draw data) -- see this file's own header comment for
// why these are deliberately two different frames (HUD content is
// non-deterministic wall-clock text; the D17 gate cannot compare against
// it).
//
// --stress: the same self-contained field, a SMALL default draw count
// suitable for a ctest gate (see this sample's own CMakeLists.txt --
// `--stress-draws 64` there), asserting exact counters + (debug builds,
// --threads>1) the D27 worker-guard.
int runHeadless(const Args& args) {
    auto app = makeApp("RendererX -- 09_scene (headless)", kHeadlessWidth, kHeadlessHeight, /*visible=*/false, args.validate,
                        args.stress);
    if (app == nullptr) {
        return 1;
    }

    app->scheduler = rx::task::Scheduler::create(args.threads);
    if (app->scheduler == nullptr) {
        RX_LOG_ERROR("sample_09_scene: Scheduler::create failed");
        destroyApp(*app);
        return 1;
    }
    app->executor = rx::graph::Executor::create(*app->device, *app->scheduler);
    if (app->executor == nullptr) {
        RX_LOG_ERROR("sample_09_scene: Executor::create failed");
        destroyApp(*app);
        return 1;
    }

    bool gateOk = true;
    uint32_t expectedTotalCandidates = 0;
    uint32_t expectedCulled = 0;
    uint32_t expectedVisible = 0;
    uint32_t expectedRecordsIn = 0;
    uint32_t expectedDrawsSubmitted = 0;

    if (args.stress) {
        if (!createMaterialParamArena(*app, kStressVariantCount)) {
            destroyApp(*app);
            return 1;
        }
        if (!setupStressMaterials(*app)) {
            destroyApp(*app);
            return 1;
        }
        if (!buildStressField(*app, args.stressDraws)) {
            destroyApp(*app);
            return 1;
        }
        warmMaterialPipelines(*app, app->stressMaterialBindings);
        app->shadowEnabled = false;
        expectedTotalCandidates = args.stressDraws;
        expectedCulled = 0;
        expectedVisible = args.stressDraws;
        expectedRecordsIn = args.stressDraws;
        expectedDrawsSubmitted = std::min<uint32_t>(kStressVariantCount, args.stressDraws);
        RX_LOG_INFO("sample_09_scene: --stress: {} instances, {} variants, Registry-free", args.stressDraws, kStressVariantCount);
    } else {
        const std::filesystem::path scenePath = resolveHelmetScenePath();
        if (!std::filesystem::exists(scenePath)) {
            RX_LOG_ERROR("sample_09_scene: scene file not found: '{}' -- run tools/fetch_assets.sh first", scenePath.string());
            destroyApp(*app);
            return 1;
        }
        rx::asset::ImportResult result = app->registry->importGltf(scenePath, *app->geometryPool, *app->scheduler, app->textureCache.get());
        if (!result.ok() || result.meshes.empty() || result.materials.empty()) {
            RX_LOG_ERROR("sample_09_scene: import of '{}' failed: {}", scenePath.string(), rx::asset::importErrorName(result.error));
            destroyApp(*app);
            return 1;
        }
        app->helmetLocalBounds = app->registry->mesh(result.meshes[0]).bounds;
        const size_t submeshCount = app->registry->mesh(result.meshes[0]).submeshes.size();
        RX_LOG_INFO("sample_09_scene: import counts meshes={} submeshes={} materials={} grid_instances={}", result.meshes.size(),
                    submeshCount, result.materials.size(), kGridInstanceCount);
        // Exactly 1 material used (result.materials[0]) regardless of how
        // many the DamagedHelmet asset itself reports -- matches
        // setupHelmetMaterial()'s own single-material call below.
        if (!createMaterialParamArena(*app, 1)) {
            destroyApp(*app);
            return 1;
        }
        if (!setupHelmetMaterial(*app, app->registry->material(result.materials[0]))) {
            destroyApp(*app);
            return 1;
        }
        if (!populateHelmetGrid(*app, result.meshes[0])) {
            destroyApp(*app);
            return 1;
        }
        warmMaterialPipelines(*app, std::span<const MaterialGpuBinding>(&app->helmetMaterial, 1));
        if (!setupShadow(*app)) {
            destroyApp(*app);
            return 1;
        }
        expectedTotalCandidates = kGridInstanceCount;
        expectedCulled = kDefaultCulledInstances;
        expectedVisible = kDefaultVisibleInstances;
        expectedRecordsIn = kDefaultVisibleInstances;  // 1 submesh per instance.
        expectedDrawsSubmitted = kDefaultDrawsSubmitted;
    }
    app->sceneReady = true;

    app->overlay = rx::debug_ui::Overlay::create(*app->device, *app->window, app->device->swapchainFormat());
    if (!app->overlay.has_value()) {
        RX_LOG_ERROR("sample_09_scene: Overlay::create failed");
        destroyApp(*app);
        return 1;
    }

    const VkDevice vkDevice = app->device->device();
    const VkFormat targetFormat = app->device->swapchainFormat();

    rx::graph::RenderGraph graph;
    declareGraph(graph, *app, targetFormat);
    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = kHeadlessWidth;
    compileInfo.swapchainHeight = kHeadlessHeight;
    compileInfo.swapchainFormat = targetFormat;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);
    app->executor->realize(graph);

    VkImage offscreenImage = VK_NULL_HANDLE;
    VkDeviceMemory offscreenMemory = VK_NULL_HANDLE;
    VkImageView offscreenView = VK_NULL_HANDLE;
    auto destroyRawResources = [&]() {
        if (offscreenView != VK_NULL_HANDLE) vkDestroyImageView(vkDevice, offscreenView, nullptr);
        if (offscreenImage != VK_NULL_HANDLE) vkDestroyImage(vkDevice, offscreenImage, nullptr);
        if (offscreenMemory != VK_NULL_HANDLE) vkFreeMemory(vkDevice, offscreenMemory, nullptr);
    };

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
        RX_LOG_ERROR("sample_09_scene: vkCreateImage(offscreen target) failed");
        destroyRawResources();
        destroyApp(*app);
        return 1;
    }
    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(vkDevice, offscreenImage, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(app->device->physicalDevice(), &memProps);
    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1U << i)) != 0U && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == UINT32_MAX) {
        RX_LOG_ERROR("sample_09_scene: no device-local memory type found for the offscreen target");
        destroyRawResources();
        destroyApp(*app);
        return 1;
    }
    VkMemoryAllocateInfo memAllocInfo{};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReq.size;
    memAllocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(vkDevice, &memAllocInfo, nullptr, &offscreenMemory) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_09_scene: vkAllocateMemory(offscreen target) failed");
        destroyRawResources();
        destroyApp(*app);
        return 1;
    }
    if (vkBindImageMemory(vkDevice, offscreenImage, offscreenMemory, 0) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_09_scene: vkBindImageMemory(offscreen target) failed");
        destroyRawResources();
        destroyApp(*app);
        return 1;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = offscreenImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = targetFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &offscreenView) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_09_scene: vkCreateImageView(offscreen target) failed");
        destroyRawResources();
        destroyApp(*app);
        return 1;
    }

    auto cmdCtx = rx::rhi::CommandContext::create(vkDevice, app->device->graphicsQueue(), app->device->graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        RX_LOG_ERROR("sample_09_scene: CommandContext::create failed");
        destroyRawResources();
        destroyApp(*app);
        return 1;
    }
    const VkExtent2D extent{kHeadlessWidth, kHeadlessHeight};

    // `recordMsOut`, when non-null, is set to the wall-clock time of ONLY
    // the `executor->execute()` call -- the SAME "cpu_record_ms" metric
    // samples/07_stress's own runHeadless() publishes (timed around ONLY
    // that call, not the readback that follows), so the two samples'
    // published numbers are directly comparable [gate ruling #15's own
    // A/B comparability contract].
    auto captureFrame = [&](double* recordMsOut = nullptr) -> std::vector<uint8_t> {
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            const auto recordStart = std::chrono::steady_clock::now();
            app->executor->execute(graph, cmd, offscreenImage, offscreenView, extent);
            if (recordMsOut != nullptr) {
                *recordMsOut = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - recordStart).count();
            }
        });
        auto readback = app->allocator->createHostVisibleBuffer(kHeadlessPixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (!readback.has_value()) {
            RX_LOG_ERROR("sample_09_scene: createHostVisibleBuffer(readback) failed");
            return {};
        }
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {kHeadlessWidth, kHeadlessHeight, 1};
            vkCmdCopyImageToBuffer(cmd, offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
        });
        readback->invalidate();
        std::vector<uint8_t> pixels(static_cast<size_t>(kHeadlessPixelBytes));
        std::memcpy(pixels.data(), readback->mappedData(), pixels.size());
        if (!canonicalizeToRgba8(pixels, targetFormat)) {
            return {};
        }
        return pixels;
    };

    // --- Main loop: EXACT counter assertions [matrix row 24], NO widgets
    // drawn (D17-gated capture) -----------------------------------------
    std::vector<uint32_t> drawsSubmittedPerFrame;
    std::vector<uint8_t> gatedPixels;
    for (uint32_t frame = 0; frame < kHeadlessFrameCount; ++frame) {
        const auto frameStart = std::chrono::steady_clock::now();
        updateSceneFrame(*app, *app->scheduler);
        // [Phase 4 exit fix wave, I1] Headless capture is fully synchronous
        // (captureFrame()'s cmdCtx->runOnce() submits and waits every
        // call, below) -- no frames-in-flight GPU overlap to race, so slot
        // 0 always is correct and sufficient here.
        uploadSceneFrameGpuBuffers(*app, /*frameSlot=*/0);
        app->overlay->beginFrame();  // no widgets this frame -- HUD pass renders empty draw data, scene pixels untouched.

        const rx::scene::CullCounters& c = app->viewLists.counters;
        if (c.totalCandidates != expectedTotalCandidates) {
            RX_LOG_ERROR("sample_09_scene: frame {}: totalCandidates={} != expected {}", frame, c.totalCandidates, expectedTotalCandidates);
            gateOk = false;
        }
        if (c.culledByLayerMask + c.culledByFrustum != expectedCulled) {
            RX_LOG_ERROR("sample_09_scene: frame {}: culled={} != expected {}", frame, c.culledByLayerMask + c.culledByFrustum,
                         expectedCulled);
            gateOk = false;
        }
        if (c.visible != expectedVisible) {
            RX_LOG_ERROR("sample_09_scene: frame {}: visible={} != expected {}", frame, c.visible, expectedVisible);
            gateOk = false;
        }
        if (c.recordsIn != expectedRecordsIn) {
            RX_LOG_ERROR("sample_09_scene: frame {}: recordsIn={} != expected {}", frame, c.recordsIn, expectedRecordsIn);
            gateOk = false;
        }
        if (c.drawsSubmitted != expectedDrawsSubmitted) {
            RX_LOG_ERROR("sample_09_scene: frame {}: drawsSubmitted={} != expected {}", frame, c.drawsSubmitted, expectedDrawsSubmitted);
            gateOk = false;
        }
        // [gate ruling #15] Blessed collapse-ratio formula: 1 - drawsSubmitted/recordsIn, CI-asserted against
        // the hand-computed grid expectation.
        if (c.recordsIn > 0) {
            const double expectedRatio = 1.0 - static_cast<double>(expectedDrawsSubmitted) / static_cast<double>(expectedRecordsIn);
            const double actualRatio = 1.0 - static_cast<double>(c.drawsSubmitted) / static_cast<double>(c.recordsIn);
            if (std::abs(actualRatio - expectedRatio) > 1e-9) {
                RX_LOG_ERROR("sample_09_scene: frame {}: collapse ratio {:.6f} != expected {:.6f}", frame, actualRatio, expectedRatio);
                gateOk = false;
            }
        }

        double recordMs = 0.0;
        std::vector<uint8_t> pixels = captureFrame(&recordMs);
        if (pixels.empty()) {
            gateOk = false;
        } else if (frame == kHeadlessFrameCount - 1) {
            gatedPixels = pixels;
        }
        if (args.stress) {
            // [gate ruling #15's A/B contract] Same "stress: ..." shape
            // sample_07_stress's own runHeadless() publishes -- see this
            // sample's own report for the joint wall-clock/draws-submitted
            // A/B comparison this line feeds.
            RX_LOG_INFO("stress9: frame={} threads={} cpu_record_ms={:.3f} instances={} recordsIn={} drawsSubmitted={} "
                       "chunkCount={}",
                       frame, app->scheduler->workerCount(), recordMs, expectedTotalCandidates, c.recordsIn, c.drawsSubmitted,
                       rx::graph::detail::chunkCountForWorkerCount(app->scheduler->workerCount()));
        }
        drawsSubmittedPerFrame.push_back(c.drawsSubmitted);

        const double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
        app->hud.pushFrameTime(frameMs);
        RX_FRAME_MARK;
    }
    for (uint32_t v : drawsSubmittedPerFrame) {
        if (v != drawsSubmittedPerFrame.front()) {
            RX_LOG_ERROR("sample_09_scene: drawsSubmitted is NOT deterministic frame-to-frame for the static startup camera");
            gateOk = false;
        }
    }
    if (!(expectedRecordsIn > expectedDrawsSubmitted)) {
        RX_LOG_ERROR("sample_09_scene: expected recordsIn ({}) > drawsSubmitted ({}) -- collapse did not actually happen",
                     expectedRecordsIn, expectedDrawsSubmitted);
        gateOk = false;
    }

    // [gate ruling #6/#15, row 12] D27 worker-guard: under --threads>1,
    // the chunked forward pass's own worker chunks must never trip the
    // main-thread-only resolvePipeline() guard -- reuses the SAME
    // rx::core::debug::detail::setViolationHookForTests() seam
    // draw_list_test.cpp's own D27 test installs.
#ifdef RX_DEBUG_CHECKS
    if (app->scheduler->workerCount() > 1) {
        static std::vector<std::string> violations;
        violations.clear();
        rx::core::debug::detail::setViolationHookForTests(
            [](const char* context) { violations.emplace_back(context != nullptr ? context : ""); });
        updateSceneFrame(*app, *app->scheduler);
        uploadSceneFrameGpuBuffers(*app, /*frameSlot=*/0);  // [I1] headless: always slot 0 -- see the main gate loop's own comment.
        app->overlay->beginFrame();  // every captureFrame() call executes the graph, which always runs the HUD pass's
                                      // own unconditional ImGui::Render() -- must pair with a fresh NewFrame() each time.
        std::vector<uint8_t> guardPixels = captureFrame();
        (void)guardPixels;
        rx::core::debug::detail::setViolationHookForTests(nullptr);
        if (!violations.empty()) {
            RX_LOG_ERROR("sample_09_scene: D27 main-thread guard tripped {} time(s) under --threads>1 (worker chunk called a "
                         "main-thread-only resolver)",
                         violations.size());
            gateOk = false;
        }
    }
#endif

    // --- HUD smoke-test frame [matrix row 16] -- real widgets, NOT
    // pixel-gated. ---------------------------------------------------------
    updateSceneFrame(*app, *app->scheduler);
    uploadSceneFrameGpuBuffers(*app, /*frameSlot=*/0);  // [I1] headless: always slot 0.
    app->overlay->beginFrame();
    drawHud(*app, *app->device, app->surface, /*presentMode=*/false);
    std::vector<uint8_t> hudFramePixels = captureFrame();
    (void)hudFramePixels;
    const ImDrawData* hudDrawData = ImGui::GetDrawData();
    if (hudDrawData == nullptr || hudDrawData->CmdListsCount == 0 || hudDrawData->TotalVtxCount == 0) {
        RX_LOG_ERROR("sample_09_scene: HUD overlay pass produced no draw data (CmdListsCount={}, TotalVtxCount={})",
                     hudDrawData != nullptr ? hudDrawData->CmdListsCount : -1, hudDrawData != nullptr ? hudDrawData->TotalVtxCount : -1);
        gateOk = false;
    }

    // --- --write-references escape hatch [D17].
    if (!args.writeReferencesDir.empty()) {
        const std::filesystem::path outDir(args.writeReferencesDir);
        const bool wroteOk = !gatedPixels.empty() &&
                              rx::samples::writeRgba8Png(outDir / "grid_scene.png", kHeadlessWidth, kHeadlessHeight, gatedPixels.data());
        vkDeviceWaitIdle(vkDevice);
        cmdCtx.reset();
        destroyRawResources();
        destroyApp(*app);
        if (!wroteOk) {
            RX_LOG_ERROR("sample_09_scene: --write-references failed to write grid_scene.png");
            return 1;
        }
        RX_LOG_INFO("sample_09_scene: wrote D17 reference PNG to '{}'", outDir.string());
        return 0;
    }

    if (!args.stress) {
        const bool lavapipe = isLavapipeDevice(app->device->physicalDevice());
        const std::filesystem::path referencePath = basePathDirectory() / "references" / "grid_scene.png";
        auto reference = rx::samples::loadRgba8Png(referencePath);
        if (!reference.has_value()) {
            RX_LOG_ERROR("sample_09_scene: D17: failed to load committed reference '{}'", referencePath.string());
            gateOk = false;
        } else if (!gatedPixels.empty()) {
            rx::samples::GateResult result = rx::samples::compareToReference(gatedPixels.data(), kHeadlessWidth, kHeadlessHeight,
                                                                              reference->rgba8.data(), reference->width, reference->height);
            RX_LOG_INFO("sample_09_scene: D17 grid_scene gate: failingPixels={}/{} ({:.4f}%) pass={}{}", result.failingPixelCount,
                        result.totalPixelCount, result.failingPixelFraction * 100.0, result.passed,
                        lavapipe ? "" : " [non-lavapipe driver -- informational only, not enforced]");
            if (lavapipe && !result.passed) {
                RX_LOG_ERROR("sample_09_scene: D17 grid_scene gate FAILED on lavapipe (first mismatch at ({},{}))",
                             result.firstMismatchX, result.firstMismatchY);
                gateOk = false;
            }
        }

        // [Phase 4 exit fix wave, C1] Discrimination re-proof: re-renders
        // the EXACT same static-camera frame with app.debugForceShadowSentinel
        // forced true (every row's shadow term bypassed -- the clean,
        // reproducible equivalent of the phase-exit review's own gdb-flip
        // probe) and asserts the result DIFFERS from `gatedPixels` by more
        // than a noise floor. Before C1's fix this would have found
        // failingPixels == 0 (the committed reference baked the
        // ambient/emissive-only, always-shadowed output, so real shadows
        // and "no shadows at all" rendered identically); the review's own
        // reproduction measured ~726/65536 (1.11%) on lavapipe once a real
        // shadow term existed to turn off.
        if (!gatedPixels.empty()) {
            app->debugForceShadowSentinel = true;
            updateSceneFrame(*app, *app->scheduler);
            uploadSceneFrameGpuBuffers(*app, /*frameSlot=*/0);
            app->overlay->beginFrame();  // pair with the fresh NewFrame() every captureFrame() call needs.
            std::vector<uint8_t> shadowOffPixels = captureFrame();
            app->debugForceShadowSentinel = false;

            if (shadowOffPixels.empty()) {
                RX_LOG_ERROR("sample_09_scene: C1 discrimination re-proof: shadow-disabled capture failed");
                gateOk = false;
            } else {
                rx::samples::GateResult discrimination = rx::samples::compareToReference(
                    gatedPixels.data(), kHeadlessWidth, kHeadlessHeight, shadowOffPixels.data(), kHeadlessWidth, kHeadlessHeight);
                RX_LOG_INFO("sample_09_scene: C1 discrimination re-proof (shadows-on vs. forced-off): "
                            "differingPixels={}/{} ({:.4f}%){}",
                            discrimination.failingPixelCount, discrimination.totalPixelCount,
                            discrimination.failingPixelFraction * 100.0,
                            lavapipe ? "" : " [non-lavapipe driver -- informational only, not enforced]");
                // Floor, not the review's exact 726 -- driver/rasterization
                // noise legitimately shifts the precise count; what must
                // never recur is the C1 bug's own signature (a shadow map
                // that is never sampled meaningfully at all, so toggling it
                // off changes NOTHING -- differingPixels == 0).
                constexpr uint32_t kMinDiscriminatingPixels = 100;
                if (lavapipe && discrimination.failingPixelCount < kMinDiscriminatingPixels) {
                    RX_LOG_ERROR("sample_09_scene: C1 discrimination re-proof FAILED on lavapipe: only {} pixels "
                                 "differ with shadows forced off (< {}) -- the shadow term is not visibly affecting "
                                 "the rendered image, the exact C1 regression class",
                                 discrimination.failingPixelCount, kMinDiscriminatingPixels);
                    gateOk = false;
                }
            }
        }
    }

    RX_LOG_INFO("sample09: perf mode={} frame_ms_avg={:.3f}", args.stress ? "stress" : "grid", app->hud.rollingAverageMs());

    vkDeviceWaitIdle(vkDevice);
    cmdCtx.reset();
    destroyRawResources();
    destroyApp(*app);

    if (args.validate && app->context->hasValidationErrors()) {
        RX_LOG_ERROR("sample_09_scene: Vulkan validation layer reported errors during this run");
        gateOk = false;
    }
    if (gateOk) {
        RX_LOG_INFO("sample_09_scene: headless gate PASSED");
        return 0;
    }
    RX_LOG_ERROR("sample_09_scene: headless gate FAILED");
    return 1;
}

// --- Present mode: interactive window, fly-through camera, real HUD ------
int runPresent(const Args& args) {
    auto app = makeApp("RendererX -- 09_scene", kPresentWidth, kPresentHeight, /*visible=*/true, args.validate, args.stress);
    if (app == nullptr) {
        return 1;
    }
    app->hud.vsyncOn = (args.vsyncMode == rx::rhi::PresentMode::VsyncOn);

    if (args.vsyncMode == rx::rhi::PresentMode::VsyncOff) {
        app->device->setPresentMode(args.vsyncMode);
        if (!app->device->recreateSwapchain(app->surface)) {
            RX_LOG_ERROR("sample_09_scene: Device::recreateSwapchain failed while applying --vsync off");
            destroyApp(*app);
            return 1;
        }
    }
    if (args.fullscreen) {
        if (!app->window->setFullscreen(true)) {
            RX_LOG_ERROR("sample_09_scene: Window::setFullscreen(true) failed while applying --fullscreen");
            destroyApp(*app);
            return 1;
        }
        if (!app->device->recreateSwapchain(app->surface)) {
            RX_LOG_ERROR("sample_09_scene: Device::recreateSwapchain failed while applying --fullscreen");
            destroyApp(*app);
            return 1;
        }
    }
    RX_LOG_INFO("sample_09_scene: --present: present mode in use: {}", rx::rhi::presentModeName(app->device->presentMode()));

    app->scheduler = rx::task::Scheduler::create(args.threads);
    if (app->scheduler == nullptr) {
        RX_LOG_ERROR("sample_09_scene: Scheduler::create failed");
        destroyApp(*app);
        return 1;
    }
    app->executor = rx::graph::Executor::create(*app->device, *app->scheduler);
    if (app->executor == nullptr) {
        RX_LOG_ERROR("sample_09_scene: Executor::create failed");
        destroyApp(*app);
        return 1;
    }

    const VkDevice vkDevice = app->device->device();
    auto frameSync = rx::rhi::FrameSync::create(vkDevice, app->device->graphicsQueueFamily(),
                                                 static_cast<uint32_t>(app->device->swapchainImages().size()));
    if (!frameSync.has_value()) {
        RX_LOG_ERROR("sample_09_scene: FrameSync::create failed");
        destroyApp(*app);
        return 1;
    }

    if (args.stress) {
        if (!createMaterialParamArena(*app, kStressVariantCount)) {
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        if (!setupStressMaterials(*app) || !buildStressField(*app, args.stressDraws)) {
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        warmMaterialPipelines(*app, app->stressMaterialBindings);
        app->shadowEnabled = false;
    } else if (!args.scenePath.empty()) {
        const std::filesystem::path scenePath =
            args.scenePath == "sponza" ? resolveSponzaScenePath() : std::filesystem::path(args.scenePath);
        if (!std::filesystem::exists(scenePath)) {
            RX_LOG_ERROR("sample_09_scene: scene file not found: '{}' -- run `tools/fetch_assets.sh --sponza` first if this is "
                         "the Sponza override",
                         scenePath.string());
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        rx::asset::ImportResult result =
            app->registry->importGltf(scenePath, *app->geometryPool, *app->scheduler, app->textureCache.get());
        if (!result.ok()) {
            RX_LOG_ERROR("sample_09_scene: import of '{}' failed: {}", scenePath.string(), rx::asset::importErrorName(result.error));
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        app->scene.emplace(rx::scene::meshBoundsFromRegistry(*app->registry));
        app->drawListBuilder.emplace(rx::scene::meshSubmeshesFromRegistry(*app->registry),
                                      rx::scene::materialResolveFromRegistry(*app->registry));
        // [P0 crash fix] Sized from THIS scene's real material count
        // (Sponza's own glTF: 25) -- not the fixed grid/stress-only
        // constant that crashed `--present --scene Sponza.gltf` on a real
        // NVIDIA driver. See App::materialParamArena's own comment.
        if (!createMaterialParamArena(*app, static_cast<uint32_t>(result.materials.size()))) {
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        if (!setupImportedMaterials(*app, *app->registry, result.materials) ||
            !populateImportedInstances(*app, *app->registry, result.scene)) {
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        warmMaterialPipelines(*app, app->customMaterialBindings);
        if (!setupShadow(*app)) {
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        RX_LOG_INFO("sample_09_scene: '{}' loaded -- {} renderable(s), {} material(s)", scenePath.string(),
                    app->renderableHandles.size(), result.materials.size());
    } else {
        const std::filesystem::path scenePath = resolveHelmetScenePath();
        if (!std::filesystem::exists(scenePath)) {
            RX_LOG_ERROR("sample_09_scene: scene file not found: '{}' -- run tools/fetch_assets.sh first", scenePath.string());
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        rx::asset::ImportResult result =
            app->registry->importGltf(scenePath, *app->geometryPool, *app->scheduler, app->textureCache.get());
        if (!result.ok() || result.meshes.empty() || result.materials.empty()) {
            RX_LOG_ERROR("sample_09_scene: import of '{}' failed: {}", scenePath.string(), rx::asset::importErrorName(result.error));
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        app->helmetLocalBounds = app->registry->mesh(result.meshes[0]).bounds;
        // Exactly 1 material used (result.materials[0]) -- matches
        // setupHelmetMaterial()'s own single-material call below.
        if (!createMaterialParamArena(*app, 1)) {
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        if (!setupHelmetMaterial(*app, app->registry->material(result.materials[0])) || !populateHelmetGrid(*app, result.meshes[0])) {
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        warmMaterialPipelines(*app, std::span<const MaterialGpuBinding>(&app->helmetMaterial, 1));
        if (!setupShadow(*app)) {
            frameSync.reset();
            destroyApp(*app);
            return 1;
        }
        RX_LOG_INFO("sample_09_scene: default helmet grid loaded -- {} instance(s)", app->renderableHandles.size());
    }
    app->sceneReady = true;
    app->flyCamera.camera.aspectRatio =
        static_cast<float>(app->device->swapchainExtent().width) / static_cast<float>(std::max<uint32_t>(1U, app->device->swapchainExtent().height));

    app->overlay = rx::debug_ui::Overlay::create(*app->device, *app->window, app->device->swapchainFormat());
    if (!app->overlay.has_value()) {
        RX_LOG_ERROR("sample_09_scene: Overlay::create failed");
        frameSync.reset();
        destroyApp(*app);
        return 1;
    }

    rx::graph::RenderGraph graph;
    declareGraph(graph, *app, app->device->swapchainFormat());
    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = app->device->swapchainExtent().width;
    compileInfo.swapchainHeight = app->device->swapchainExtent().height;
    compileInfo.swapchainFormat = app->device->swapchainFormat();
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    graph.compile(compileInfo);
    app->executor->realize(graph);

    std::vector<VkImageView> swapchainViews;
    auto rebuildSwapchainViews = [&]() -> bool {
        for (VkImageView view : swapchainViews) vkDestroyImageView(vkDevice, view, nullptr);
        swapchainViews.clear();
        for (VkImage image : app->device->swapchainImages()) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = app->device->swapchainFormat();
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &view) != VK_SUCCESS) {
                RX_LOG_ERROR("sample_09_scene: vkCreateImageView(swapchain) failed");
                return false;
            }
            swapchainViews.push_back(view);
        }
        return true;
    };
    if (!rebuildSwapchainViews()) {
        frameSync.reset();
        destroyApp(*app);
        return 1;
    }

    bool running = true;
    bool ok = true;
    auto lastFrameTime = std::chrono::steady_clock::now();

    // [gate ruling #16] ImGui feeds every SDL event FIRST, then platform
    // input accumulators (Window::pumpEvents()'s own `preDispatch` seam).
    while (running) {
        app->window->pumpEvents([&](const SDL_Event& event) {
            app->overlay->processEvent(event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
        });
        if (!running) break;

        app->scheduler->pumpMain();

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - lastFrameTime).count(), 0.1F);
        lastFrameTime = now;
        app->hud.pushFrameTime(static_cast<double>(dt) * 1000.0);

        updateFlyCamera(*app, *app->window, dt);
        updateSceneFrame(*app, *app->scheduler);

        VkFence fence = frameSync->currentFence();
        vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);
        auto acquire = app->device->acquireNextImage(frameSync->currentImageAvailableSemaphore());
        if (acquire.status == rx::rhi::SwapchainStatus::Suspended) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            if (!app->device->recreateSwapchain(app->surface)) {
                RX_LOG_ERROR("sample_09_scene: Device::recreateSwapchain failed while suspended (zero-extent retry)");
                ok = false;
                break;
            }
            if (!app->device->isSuspended()) {
                if (!rebuildSwapchainViews() || !frameSync->onSwapchainRecreated(static_cast<uint32_t>(app->device->swapchainImages().size()))) {
                    ok = false;
                    break;
                }
                app->executor->realize(graph);
            }
            continue;
        }
        if (acquire.status == rx::rhi::SwapchainStatus::NeedsRecreate) {
            vkDeviceWaitIdle(vkDevice);
            if (!app->device->recreateSwapchain(app->surface) || !rebuildSwapchainViews() ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(app->device->swapchainImages().size()))) {
                ok = false;
                break;
            }
            app->executor->realize(graph);
            continue;
        }
        if (acquire.status == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("sample_09_scene: device lost during acquireNextImage; exiting present loop");
            ok = false;
            break;
        }

        app->flyCamera.camera.aspectRatio = static_cast<float>(app->device->swapchainExtent().width) /
                                             static_cast<float>(std::max<uint32_t>(1U, app->device->swapchainExtent().height));

        // [Phase 4 exit fix wave, I1] The GPU-write half of the frame
        // update -- AFTER `vkWaitForFences(fence)` above (which just
        // confirmed frame N-2's GPU work, the last frame that used THIS
        // SAME frame-in-flight slot's buffers, is complete) and selected
        // by that same slot index, so this write can never race frame
        // N-1's still-possibly-in-flight GPU reads of a DIFFERENT slot's
        // buffers. `updateSceneFrame()` above only built the CPU-side row
        // mirrors -- see that function's own comment for why this split
        // closes the I1 finding.
        uploadSceneFrameGpuBuffers(*app, frameSync->currentFrameIndex());

        vkResetFences(vkDevice, 1, &fence);
        VkCommandBuffer cmd = frameSync->currentCommandBuffer();
        vkResetCommandPool(vkDevice, frameSync->currentCommandPool(), 0);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        app->overlay->beginFrame();
        drawHud(*app, *app->device, app->surface, /*presentMode=*/true);

        app->executor->execute(graph, cmd, app->device->swapchainImages()[acquire.imageIndex], swapchainViews[acquire.imageIndex],
                               app->device->swapchainExtent());
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        VkSemaphore waitSem = frameSync->currentImageAvailableSemaphore();
        submitInfo.pWaitSemaphores = &waitSem;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        VkSemaphore signalSem = frameSync->renderFinishedSemaphore(acquire.imageIndex);
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSem;
        if (vkQueueSubmit(app->device->graphicsQueue(), 1, &submitInfo, fence) != VK_SUCCESS) {
            ok = false;
            break;
        }
        auto presentStatus = app->device->present(acquire.imageIndex, signalSem);
        if (presentStatus == rx::rhi::SwapchainStatus::NeedsRecreate) {
            vkDeviceWaitIdle(vkDevice);
            if (!app->device->recreateSwapchain(app->surface) || !rebuildSwapchainViews() ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(app->device->swapchainImages().size()))) {
                ok = false;
                break;
            }
            app->executor->realize(graph);
        } else if (presentStatus == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("sample_09_scene: device lost during present; exiting present loop");
            ok = false;
            break;
        }
        frameSync->advanceFrame();
        RX_FRAME_MARK;
    }

    vkDeviceWaitIdle(vkDevice);
    for (VkImageView view : swapchainViews) vkDestroyImageView(vkDevice, view, nullptr);
    frameSync.reset();
    destroyApp(*app);

    if (args.validate && app->context->hasValidationErrors()) {
        RX_LOG_ERROR("sample_09_scene: Vulkan validation layer reported errors during the present loop");
        return 1;
    }
    if (!ok) {
        return 1;
    }
    RX_LOG_INFO("sample_09_scene: window closed cleanly");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    rx::core::log::init();
    auto args = parseArgs(argc, argv);
    if (!args.has_value()) {
        return 1;
    }
    if (args->present) {
        return runPresent(*args);
    }
    return runHeadless(*args);
}
