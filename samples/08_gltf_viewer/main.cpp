// Sample 08: glTF viewer [Phase 4 Stage 1 Task 16, spec D22/D26.1/D28,
// gate ruling #8/RC1] -- the stage's USER-FACING SAMPLE: imports a real
// glTF asset (DamagedHelmet by default, `--scene <path>` override)
// ASYNCHRONOUSLY (Task 15's own pipeline -- this sample is the async-import
// demonstration vehicle, per the plan's own text) with a rendered loading
// state, renders it through StandardPBR/Unlit (D22's shipped material
// library) driven entirely via D26.1's bindless per-draw addressing
// (SV_VulkanInstanceID, never a per-draw push constant -- sample 07's own
// `gPush.instanceIndex` is the named anti-pattern this does not repeat),
// with a mouse-drag orbit camera and a manual `--exposure` control.
//
// D28: each glTF material's alphaMode/doubleSided become MaterialSystem's
// own fixed-function pipeline-state axis (MaterialFixedFunctionState) --
// StandardPBR/Unlit are loaded ONCE PER DISTINCT (module, alphaMode,
// doubleSided) COMBINATION, not once per shader file; two glTF materials
// sharing standard_pbr.slang's bytes but differing only in alphaMode/
// doubleSided are two independent loadMaterial() calls (see
// setupMaterial() below), yielding two independently-cached VkPipelines.
//
// ORBIT CAMERA [Fix round, item 7: corrected to match the actual code --
// see below for what changed]: reads SDL mouse state directly via SDL3's
// own global `SDL_GetMouseState()` (takes no window argument at all in
// SDL3 -- it queries OS-level cursor state directly, not scoped through
// any particular `SDL_Window*`), satisfying gate ruling #8's own intent
// ("sample 08 reads SDL mouse state directly... the exposed escape hatch
// -- no rx_platform scope pulled forward, no reordering") without actually
// needing `Window::sdlWindow()` at all for this specific query -- that
// accessor exists on `rx::platform::Window` for a caller that DOES need a
// window-scoped SDL call (event polling, cursor confinement, ...) and
// remains this sample's own escape hatch for that; this one query simply
// doesn't require it. Still sample-local either way, not a new
// rx_platform input surface (Task 20, Stage 2, is that real surface;
// sample 09 migrates to it then).
//
// REVERSED-Z: NOT yet -- D13's reversed-Z camera convention arrives with
// rx::scene::Camera in Stage 2 (Task 18). This sample uses the SAME
// standard-Z convention (near=0/far=1, depth cleared to 1.0, compare-op
// LESS) every earlier sample already relies on via MaterialSystem's own
// fixed `depthCompareOp` -- a future Stage-2 migration point, not
// reproduced here as a TODO beyond this comment.
//
// TONEMAP: reuses shaders/multipass/tonemap.{vert,frag}.slang VERBATIM
// (this task's own binding constraint: "you may NOT touch
// shaders/multipass/") -- `--exposure`'s pre-tonemap `2^exposure` multiply
// happens INSIDE the material forward pass instead (forward_entry.slang's
// own fragmentMain, material.slang's `RxMaterialGlobals::exposure`), so
// the shared tonemap shaders' own SPIR-V is byte-for-byte unchanged by
// this task -- see the exposure-neutral regression test this sample's own
// headless gate runs.
//
// D17 TOLERANCE GATE: headless mode captures TWO frames (a loading-state
// frame, captured on frame 0 before the async import can possibly have
// completed, and a loaded-scene frame, captured once
// Registry::importProgress() reaches a terminal state) and compares each
// against its own committed 256x256 lavapipe reference PNG via
// samples/common/reference_gate.h's reusable tolerance-gate helper
// (+-4/255 per channel, <0.5% failing-pixel budget) -- local-GPU
// divergence is reported as INFO, never a failure (D17's own text): this
// binary detects "am I lavapipe?" via the reported VkPhysicalDeviceProperties
// device name and only enforces the gate's PASS/FAIL verdict when it is.
//
// QUIT-DURING-LOAD (`--quit-during-load`): starts the async import, lets it
// run for a short bounded window (enough for >=1 real GPU resource --
// texture or geometry -- to plausibly have landed), then calls
// cancelImport() and tears down immediately -- the standing lesson this
// whole stage's review history keeps finding rollback bugs in exactly this
// territory (abandon/teardown paths need REAL GPU-resource tests, never a
// mock). Asserted via zero unfiltered Vulkan validation errors through a
// real cancel-mid-import-then-destroy sequence.
#include <rx_asset/geometry_pool.h>
#include <rx_asset/registry.h>
#include <rx_asset/texture_cache.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>
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
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_rhi_vk/upload.h>
#include <rx_scene/camera.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <rx_task/scheduler.h>

#include <reference_gate.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <SDL3/SDL.h>
#include <volk.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
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

// A distinct, unmistakably-not-black "loading" color -- see recordLoadingPass()
// below. Chosen to be far from both the tonemapped scene's own likely
// palette and pure black/white, so a wrong-frame-shown-instead-of-loading
// bug (or vice versa) is visually obvious, not just barely distinguishable.
constexpr VkClearColorValue kLoadingClearColor{{0.06F, 0.10F, 0.22F, 1.0F}};

// --- CLI arguments ------------------------------------------------------
struct Args {
    std::string scenePath;  // empty == use the default (DamagedHelmet) resolution.
    float exposure = 0.0F;  // pre-tonemap 2^exposure multiplier; 0 == neutral (2^0 == 1).
    rx::rhi::PresentMode vsyncMode = rx::rhi::PresentMode::VsyncOn;
    bool validate = false;
    bool present = false;
    bool quitDuringLoad = false;
    // [Phase 4 Task 17, FG7] Borderless-desktop fullscreen, applied at
    // startup only (no in-sample runtime hotkey) -- same present-mode-flag
    // convention --vsync above already follows.
    bool fullscreen = false;
    // [D17] Undocumented-to-users, documented-to-maintainers escape hatch:
    // when non-empty, runHeadless() WRITES its two captured frames
    // (loading_state.png/loaded_scene.png) into this directory instead of
    // comparing them against the committed references -- the ONLY way
    // tools/regen_references.sh (never auto-run) produces a new pair of D17
    // reference PNGs. Not listed in any user-facing --help text this sample
    // prints (there is none) -- discoverable only by reading this source or
    // that script.
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
        } else if (arg == "--quit-during-load") {
            args.quitDuringLoad = true;
        } else if (arg == "--scene" && i + 1 < argc) {
            args.scenePath = argv[++i];
        } else if (arg == "--write-references" && i + 1 < argc) {
            args.writeReferencesDir = argv[++i];
        } else if (arg == "--exposure" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            float parsed = 0.0F;
            if (std::from_chars(value.data(), value.data() + value.size(), parsed).ec != std::errc{}) {
                RX_LOG_ERROR("sample_08_gltf_viewer: --exposure expects a real number, got '{}'", value);
                return std::nullopt;
            }
            args.exposure = parsed;
        } else if (arg == "--vsync" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            if (value == "off") {
                args.vsyncMode = rx::rhi::PresentMode::VsyncOff;
            } else if (value == "on") {
                args.vsyncMode = rx::rhi::PresentMode::VsyncOn;
            } else {
                RX_LOG_ERROR("sample_08_gltf_viewer: --vsync expects 'on' or 'off', got '{}' -- defaulting to on",
                             value);
            }
        } else if (arg == "--fullscreen") {
            args.fullscreen = true;
        } else {
            RX_LOG_ERROR("sample_08_gltf_viewer: unrecognized argument '{}'", arg);
            return std::nullopt;
        }
    }
    return args;
}

// --- Path resolution [samples/06_materials' own basePathDirectory() idiom,
// applied here for BOTH the shared material shaders AND the default scene
// asset] ---------------------------------------------------------------
std::filesystem::path basePathDirectory() {
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        RX_LOG_WARN("sample_08_gltf_viewer: SDL_GetBasePath failed ({}); using the current directory instead",
                    SDL_GetError());
        return ".";
    }
    return std::filesystem::path(basePath);
}

// Resolution order: (1) a packaged, pre-staged copy next to the binary
// (`assets/DamagedHelmet/glTF/DamagedHelmet.gltf` -- see this sample's own
// CMakeLists.txt / tools/package_samples.sh, which stage exactly this
// layout); (2) this checked-out repository's own fetched copy
// (`assets/fetched/DamagedHelmet/glTF/DamagedHelmet.gltf`,
// tools/fetch_assets.sh's own output) for an ordinary build-tree run. Never
// silently falls through to a THIRD guess -- a caller that gets neither
// gets a clear, named "scene not found" error instead of a confusing
// import failure two layers down.
std::filesystem::path resolveDefaultScenePath() {
    std::filesystem::path packaged = basePathDirectory() / "assets" / "DamagedHelmet" / "glTF" / "DamagedHelmet.gltf";
    if (std::filesystem::exists(packaged)) {
        return packaged;
    }
    std::filesystem::path devTree =
        std::filesystem::path(RX_REPO_ROOT_DIR) / "assets" / "fetched" / "DamagedHelmet" / "glTF" / "DamagedHelmet.gltf";
    return devTree;
}

// --- Orbit camera [gate ruling #8: SDL mouse state read directly via
// SDL3's own global SDL_GetMouseState(), sample-local -- see this file's
// own header comment] -----------------------------------------------------
struct OrbitCamera {
    float azimuthRadians = glm::radians(45.0F);
    float elevationRadians = glm::radians(20.0F);
    float distance = 3.0F;
    glm::vec3 target{0.0F, 0.0F, 0.0F};

    static constexpr float kMinElevation = glm::radians(-85.0F);
    static constexpr float kMaxElevation = glm::radians(85.0F);
    static constexpr float kMinDistanceFraction = 0.15F;  // relative to the distance the scene AABB derived.
    static constexpr float kMaxDistanceFraction = 6.0F;

    [[nodiscard]] glm::vec3 eye() const {
        const float ce = std::cos(elevationRadians);
        return target + distance * glm::vec3(ce * std::sin(azimuthRadians), std::sin(elevationRadians),
                                              ce * std::cos(azimuthRadians));
    }
};

// [Task 22, D13/RC3 migration] The hand-rolled `vulkanPerspective()`
// helper this comment used to document (a plain `glm::perspective()` +
// row-1 Y-flip, standard-Z) is GONE -- superseded by `computeViewProj()`'s
// own `rx::scene::Camera::cullingProj()` call below, which already bakes
// in the identical Vulkan Y-flip internally (camera.cpp's own
// `reversedZPerspective()`: `m[1][1] = -fY`) AND the D13 reversed-Z
// convention `MaterialSystem::getPipeline()` now requires -- see that
// method's own comment for why front-face winding (CCW, glTF's own
// convention) is unaffected by either the Y-flip or the reversed-Z
// migration (neither touches handedness/winding, only depth).

// --- Tonemap pass [shaders/multipass/tonemap.{vert,frag}.slang, VERBATIM --
// see this file's own header comment] -- byte-identical machinery to
// samples/05_multipass's own buildTonemapPipeline()/compileAndReflect()/
// assignShaderModules(), copied (not shared -- matching this project's own
// per-sample-independence convention) rather than re-derived. ------------
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
    pass.pushConstantOffset = 0;
    pass.pushConstantSize = 0;
    pass.pushConstantStages = 0;
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
            RX_LOG_ERROR("sample_08_gltf_viewer: failed to open shader file '{}'", path);
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
        RX_LOG_ERROR("sample_08_gltf_viewer: shader compile failed for module '{}':\n{}", moduleName,
                     compileResult.diagnostics);
        return std::nullopt;
    }
    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: reflect() failed for module '{}'", moduleName);
        return std::nullopt;
    }
    return ReflectedModule{std::move(compileResult), std::move(*layoutInfo)};
}

bool assignShaderModules(VkDevice device, const rx::shader::CompileResult& compileResult, const char* vertEntryName,
                          const char* fragEntryName, CompiledPass& pass) {
    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModule module = createShaderModuleFromSpirv(device, blob.code);
        if (module == VK_NULL_HANDLE) {
            RX_LOG_ERROR("sample_08_gltf_viewer: vkCreateShaderModule failed for entry point '{}'",
                         blob.entryPointName);
            return false;
        }
        if (blob.entryPointName == vertEntryName) {
            pass.vertModule = module;
        } else if (fragEntryName != nullptr && blob.entryPointName == fragEntryName) {
            pass.fragModule = module;
        } else {
            RX_LOG_ERROR("sample_08_gltf_viewer: unexpected entry point '{}'", blob.entryPointName);
            vkDestroyShaderModule(device, module, nullptr);
            return false;
        }
    }
    if (pass.vertModule == VK_NULL_HANDLE) {
        RX_LOG_ERROR("sample_08_gltf_viewer: missing vertex entry point '{}'", vertEntryName);
        return false;
    }
    if (fragEntryName != nullptr && pass.fragModule == VK_NULL_HANDLE) {
        RX_LOG_ERROR("sample_08_gltf_viewer: missing fragment entry point '{}'", fragEntryName);
        return false;
    }
    return true;
}

bool buildTonemapPipeline(VkDevice device, rx::shader::Compiler& compiler, VkDescriptorSetLayout bindlessSetLayout,
                          VkFormat backbufferFormat, const std::filesystem::path& tonemapVertPath,
                          const std::filesystem::path& tonemapFragPath, CompiledPass& tonemapPass) {
    destroyCompiledPass(device, tonemapPass);

    auto reflected = compileAndReflect(compiler, "GltfViewerTonemapModule",
                                        {tonemapVertPath.string(), tonemapFragPath.string()}, {"vsMain", "fsMain"});
    if (!reflected.has_value()) {
        return false;
    }
    if (reflected->layoutInfo.pushRanges.size() != 1 ||
        reflected->layoutInfo.pushRanges[0].size != sizeof(TonemapPushConstants)) {
        RX_LOG_ERROR(
            "sample_08_gltf_viewer: tonemap shader reflects an unexpected push-constant shape ({} range(s), size={} "
            "vs. sizeof(TonemapPushConstants)={}) -- shaders/multipass/tonemap.*.slang must stay byte-for-byte "
            "unchanged (this task's own binding constraint)",
            reflected->layoutInfo.pushRanges.size(),
            reflected->layoutInfo.pushRanges.empty() ? 0 : reflected->layoutInfo.pushRanges[0].size,
            sizeof(TonemapPushConstants));
        return false;
    }

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, reflected->layoutInfo, bindlessSetLayout);
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: PipelineLayoutBuilder::build failed for the tonemap pass");
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
        RX_LOG_ERROR("sample_08_gltf_viewer: vkCreateGraphicsPipelines failed for the tonemap pass");
        destroyCompiledPass(device, tonemapPass);
        return false;
    }
    return true;
}

// --- Material setup [D22/D26.1/D28] -----------------------------------
// One binding per DISTINCT glTF material: a loaded StandardPBR/Unlit
// MaterialHandle (D28's own fixed-function state baked in at loadMaterial()
// time), its reflected `TParams` blob copied into a real UBO + a set-1
// VkDescriptorSet allocated against THIS sample's own `materialParamSetLayout`
// (App, below) -- written ONCE here, at setup (material parameters never
// change per-frame in this sample, so there is no need for a per-frame
// arena) -- and the material's own push-constant range shape
// (offset/size/stages), cached once from `layoutInfo()` per that method's
// own CAUTION comment (its returned reference is invalidated by a LATER
// loadMaterial() call; setupMaterials() below loads every material before
// reading any of them, so copying these three scalars once, here, is safe).
//
// DELIBERATELY NOT `MaterialSystem::bindInstance()`: that method is the
// documented pre-D26.1 LEGACY path (material_system.cpp's own bindInstance()
// header comment: "a real D26.1 caller (StandardPBR/Unlit, samples/
// 08_gltf_viewer) drives its OWN real per-draw buffer and pushes this same
// push-constant range itself, never through this method") -- bindInstance()
// always pushes MaterialSystem's own DEFAULT (identity) drawDataBufferIndex
// and exposure=0.0, neither of which this sample can use (it has a REAL
// per-submesh transform buatch and a real `--exposure`). recordForwardPass()
// below (this file) is this sample's own equivalent of bindInstance() --
// pipeline bind, push-constant write, descriptor-set-1 bind -- built by hand
// against this one small, always-identical (set 1, binding 0,
// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VERTEX|FRAGMENT) reflected shape every
// material.slang-conforming module produces [D6/D8's own "one
// ParameterBlock<TParams> gParams" convention] -- see App::
// materialParamSetLayout's own comment for why a SEPARATELY-created
// VkDescriptorSetLayout with identical content is valid to bind against a
// pipeline layout MaterialSystem itself built (Vulkan's own "identically
// defined" pipeline-layout-compatibility rule, spec 14.2.2), and D27's own
// pre-resolution (setupMaterials() calls getPipeline() once per binding
// against the forward pass's own PassSignature so the FIRST real draw never
// stalls on a cold Slang-to-VkPipeline compile).
struct MaterialGpuBinding {
    rx::material::MaterialHandle handle;
    std::vector<uint8_t> paramBlob;
    std::optional<rx::rhi::Buffer> paramBuffer;
    VkDescriptorSet paramSet = VK_NULL_HANDLE;
    uint32_t pushOffset = 0;
    uint32_t pushSize = 0;
    VkShaderStageFlags pushStages = 0;
};

// [D7/D27] The forward pass's own attachment shape -- one HDR color target
// + one depth target, both fixed formats (kHdrFormat/kDepthFormat above),
// single-sampled. Built as a plain value (not stored per-instance state)
// since every field is a compile-time constant of this sample -- both
// setupMaterials()'s own D27 pre-resolution call and declareGraph()'s own
// Pass::addColorOutput()/setDepthStencilOutput() calls must derive the
// EXACT SAME PassSignature bit-for-bit (getPipeline()'s own cache key is
// this struct's hash()), so this one function is the sole place that shape
// is spelled out.
rx::material::PassSignature forwardPassSignature() {
    rx::material::PassSignature sig;
    sig.colorFormats[0] = kHdrFormat;
    sig.colorCount = 1;
    sig.depthFormat = kDepthFormat;
    sig.samples = VK_SAMPLE_COUNT_1_BIT;
    return sig;
}

// One forward-pass draw: a submesh's own geometry range (GeometryPool
// addressing) plus which row of the D26.1 bindless per-draw buffer
// (gDrawData[...][SV_VulkanInstanceID]) it reads and which material binds
// its own set-1 params.
struct DrawItem {
    uint32_t blockId = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t vertexOffset = 0;
    uint32_t drawDataRow = 0;
    size_t materialBindingIndex = 0;
};

template <typename T>
bool setMaterialParam(std::vector<uint8_t>& blob, const std::vector<rx::material::MaterialParamInfo>& params,
                      const char* name, const T& value) {
    for (const auto& p : params) {
        if (p.name == name) {
            if (p.size != sizeof(T)) {
                RX_LOG_ERROR("sample_08_gltf_viewer: material param '{}' reflected size {} != sizeof(T) {}", name,
                             p.size, sizeof(T));
                return false;
            }
            std::memcpy(blob.data() + p.offset, &value, sizeof(T));
            return true;
        }
    }
    RX_LOG_ERROR("sample_08_gltf_viewer: material param '{}' not found in reflection", name);
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

// Resolves `ref`'s bindless index through `textures`, defensively falling
// back to the role-appropriate D11 utility texture itself (rather than
// trusting `ref.handle` unconditionally) for an unbound slot -- see
// mesh_asset.h's own TextureRef comment ("handle is always the registry's
// D11 fallback ... Task 14 resolve[s] the real texture") for why this is
// belt-and-suspenders, not a workaround for a known gap.
uint32_t resolveTextureIndex(rx::asset::TextureCache& textures, const rx::asset::TextureRef& ref,
                              rx::asset::TextureRole role) {
    if (!ref.present) {
        return textures.resolve(textures.fallbackHandle(role)).bindlessIndex;
    }
    return textures.resolve(ref.handle).bindlessIndex;
}

// [Fix round, sampler-wrap P0] Resolves `ref`'s bindless SAMPLER through
// `textures`'s own per-texture sampler cache (TextureCache::
// getOrCreateSamplerBindlessIndex()) -- THE actual fix for the shipped
// sampler-wrap defect (helmet-sampler-fix-brief.md): every texture slot
// used to sample through ONE hardcoded process-wide CLAMP_TO_EDGE sampler
// (`app.defaultSampler`, above) regardless of that texture's own glTF wrap
// mode. `ref.sampler` is passed straight through, present or not: for a
// PRESENT slot it is either the real glTF sampler object import_gltf.cpp's
// own fillRef() parsed (honoring that texture's real wrap/filter state) or,
// if the glTF texture referenced no sampler object at all, `SamplerDesc`'s
// own default-constructed values -- which ARE glTF's documented "sampler
// unspecified" default (REPEAT wrap, auto filtering), NOT an engine-chosen
// fallback. For an ABSENT slot (`!ref.present`) `ref.sampler` is that SAME
// default-constructed value: the slot samples a role-neutral D11 fallback
// texture (resolveTextureIndex() above), whose wrap mode is irrelevant, so
// the spec-correct REPEAT default is exactly as harmless there as any
// other choice would be, with no special-casing needed. `fallbackIndex`
// (the caller's own already-resolved bindless index for `app.
// defaultSampler`) is used ONLY on a genuine
// getOrCreateSamplerBindlessIndex() failure (vkCreateSampler failure /
// bindless sampler-capacity exhaustion) -- belt-and-suspenders, mirroring
// resolveTextureIndex()'s own fallback philosophy above.
uint32_t resolveSamplerIndex(rx::asset::TextureCache& textures, const rx::asset::TextureRef& ref,
                              uint32_t fallbackIndex) {
    std::optional<uint32_t> index = textures.getOrCreateSamplerBindlessIndex(ref.sampler);
    return index.value_or(fallbackIndex);
}

std::array<float, 4> uvTransformParam(const rx::asset::TextureRef& ref) {
    return {ref.uvOffset.x, ref.uvOffset.y, ref.uvScale.x, ref.uvScale.y};
}

// --- Application state -----------------------------------------------------
// EVERY GPU-object-owning member below is constructed IN PLACE, directly
// into this struct's own fields, via a heap-allocated `std::unique_ptr<App>`
// (see makeApp() below) -- NEVER built as a local variable first and then
// moved/aggregate-constructed into a returned struct. This is load-bearing,
// not stylistic: MaterialSystem::create()/GeometryPool::create()/
// TextureCache::create() each take `rx::rhi::Device&`/`rx::rhi::BindlessTable&`
// and STORE A RAW POINTER internally -- constructing any of these against a
// temporary that later gets moved again invalidates that pointer (a
// dangling-reference-to-a-moved-from-local bug this task's own development
// hit AND fixed, the hard way, in src/rx_material/tests/
// test_standard_pbr_unlit.cpp's own StandardPbrRig -- see that file's own
// header comment on the fix). `rx_asset/tests/async_import_test.cpp`'s own
// AsyncTestFixture/attachPool()/attachTextureCache() two-step pattern is the
// SAME discipline, independently established; this struct follows it too.
// std::optional on window/context/device/allocator/uploader below, not
// plain values: NONE of rx::platform::Window / rx::rhi::Context / Device /
// Allocator / Uploader has an accessible default constructor (each is
// buildable only through its own static create(), verified directly
// against every one of those five headers before writing this struct) --
// a plain (non-optional) member here would fail to compile the moment
// `std::make_unique<App>()` tried to default-construct it. Exactly the
// same reason samples/06_materials' own Scene wraps its
// rx::rhi::BindlessTable in std::optional (that struct's own comment).
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
    std::unique_ptr<rx::asset::Registry> registry;
    std::unique_ptr<rx::task::Scheduler> scheduler;
    std::unique_ptr<rx::graph::Executor> executor;

    std::filesystem::path sharedShaderDir;
    std::filesystem::path standardPbrPath;
    std::filesystem::path unlitPath;

    // Tonemap pass (shared shaders, verbatim -- see this file's own header
    // comment) + the ONE default LINEAR sampler both the tonemap pass's own
    // HDR read AND MaterialSystem's own rx_sampleTexture() share (same
    // "one process-wide default sampler, real bindless index carried via a
    // push constant" idiom every earlier sample already establishes).
    CompiledPass tonemapPass;
    VkSampler defaultSampler = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle defaultSamplerHandle;
    VkImageView lastHdrView = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle hdrHandle;

    // Set-1 (material params) descriptor infrastructure -- ONE
    // VkDescriptorSetLayout this sample builds BY HAND, structurally
    // IDENTICAL (one VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER at binding 0,
    // VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT) to what
    // MaterialSystem::reflectMaterialLayout() (material_system.cpp) builds
    // internally for EVERY material's own set 1 -- verified directly against
    // that function's own source (kMaterialParamBlockSet=1,
    // kMaterialParamBlockBinding=0, kMaterialStageFlags=VERTEX|FRAGMENT).
    // Two SEPARATELY created VkDescriptorSetLayout objects with identical
    // content are "identically defined" per the Vulkan spec's own pipeline-
    // layout-COMPATIBILITY rule (14.2.2) -- a VkDescriptorSet allocated from
    // THIS layout is valid to bind at set 1 against a VkPipelineLayout
    // MaterialSystem itself built, with no cross-library accessor needed for
    // MaterialSystem's own (private) layout object. ONE shared pool sized
    // for this scene's material count (kMaxMaterialParamSets below).
    VkDescriptorSetLayout materialParamSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool materialParamPool = VK_NULL_HANDLE;

    std::vector<MaterialGpuBinding> materialBindings;
    std::unordered_map<uint32_t, size_t> materialHandleToBinding;  // rx::asset::MaterialHandle::index() -> materialBindings[].
    std::vector<DrawItem> draws;
    std::optional<rx::rhi::Buffer> drawDataBuffer;
    rx::rhi::BindlessHandle drawDataBufferHandle;
    // CPU-side mirror of every row currently uploaded to `drawDataBuffer` --
    // `model`/`normalMatrix`/`materialIndex` are written once (buildDrawList())
    // and never change again (a static scene); `viewProj`/`lightDirWorld`/
    // `lightColor`/`ambientColor`/`cameraPosWorld` are per-PASS (D26.1's own
    // "repeated per row" design, draw_data.h) and are overwritten here, every
    // frame, by updateDrawDataPerPassFields() before the whole vector is
    // re-uploaded -- simpler than tracking a separate "did the camera move"
    // dirty flag, and cheap at this sample's draw-call scale.
    std::vector<rx::material::DrawDataGpu> drawDataRows;

    rx::asset::AABB sceneBoundsWorld;
    OrbitCamera camera;
    float exposure = 0.0F;

    rx::asset::AsyncImportHandle importHandle;
    bool importStarted = false;
    bool sceneReady = false;
};

// Builds every field up to (but not including) the imported-scene GPU
// state -- device/allocator/uploader/bindless/materialSystem/geometryPool/
// textureCache/registry/scheduler/executor, the tonemap pipeline, and the
// shared material-param descriptor infrastructure. Returns nullptr (logged)
// on any failure.
std::unique_ptr<App> makeApp(const std::string& windowTitle, uint32_t width, uint32_t height, bool visible,
                              bool validate) {
    auto app = std::make_unique<App>();

    auto window = rx::platform::Window::create(windowTitle.c_str(), static_cast<int>(width),
                                                 static_cast<int>(height), visible);
    if (!window.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: Window::create failed: no display backend available");
        return nullptr;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: video driver reports no Vulkan surface extensions");
        return nullptr;
    }
    auto context = rx::rhi::Context::create(extensions, validate);
    if (!context.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: Context::create failed");
        return nullptr;
    }
    app->window.emplace(std::move(*window));
    app->context.emplace(std::move(*context));

    app->surface = app->window->createVulkanSurface(app->context->instance());
    if (app->surface == VK_NULL_HANDLE) {
        RX_LOG_ERROR("sample_08_gltf_viewer: createVulkanSurface failed");
        return nullptr;
    }

    auto device = rx::rhi::Device::create(*app->context, app->surface);
    if (!device.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: Device::create failed");
        return nullptr;
    }
    app->device.emplace(std::move(*device));

    auto allocator = rx::rhi::Allocator::create(*app->context, *app->device);
    if (!allocator.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: Allocator::create failed");
        return nullptr;
    }
    app->allocator.emplace(std::move(*allocator));

    auto uploader = rx::rhi::Uploader::create(*app->allocator, *app->device);
    if (!uploader.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: Uploader::create failed");
        return nullptr;
    }
    app->uploader.emplace(std::move(*uploader));

    // Everything from here on takes a REFERENCE into `app`'s own now-stable
    // fields -- see this struct's own header comment.
    // [Phase 4 Stage 2 Task 22 fix round, F1] Same requirement as sample
    // 06's own identical comment: material.slang unconditionally declares
    // `gShadowCompareSamplers` at binding 3 now, so every BindlessTable
    // feeding MaterialSystem needs it. This sample's own DrawDataGpu rows
    // (buildDrawList()) leave shadowMapTextureIndex at its "no shadow"
    // sentinel default -- this viewer does not build a real shadow map
    // (that is Task 24/sample 09's own scene-path job) -- so 1 slot is
    // enough (never more than one comparison sampler live at once).
    rx::rhi::BindlessTable::Capacities capacities{/*sampledImages=*/256, /*samplers=*/16, /*storageBuffers=*/4};
    capacities.comparisonSamplers = 1;
    auto bindless =
        rx::rhi::BindlessTable::create(app->device->physicalDevice(), app->device->device(), capacities);
    if (!bindless.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: BindlessTable::create failed");
        return nullptr;
    }
    app->bindless.emplace(std::move(*bindless));
    app->deletionQueue.emplace();

    const std::filesystem::path basePath = basePathDirectory();
    app->sharedShaderDir = basePath / "material_shaders";
    app->standardPbrPath = app->sharedShaderDir / "standard_pbr.slang";
    app->unlitPath = app->sharedShaderDir / "unlit.slang";

    app->materialSystem = rx::material::MaterialSystem::create(
        *app->device, *app->bindless, basePath / "gltf_viewer_pipeline.cache", app->sharedShaderDir);
    if (app->materialSystem == nullptr) {
        RX_LOG_ERROR("sample_08_gltf_viewer: MaterialSystem::create failed");
        return nullptr;
    }

    app->geometryPool = rx::asset::GeometryPool::create(*app->allocator, *app->device, *app->uploader);
    if (app->geometryPool == nullptr) {
        RX_LOG_ERROR("sample_08_gltf_viewer: GeometryPool::create failed");
        return nullptr;
    }

    app->textureCache = rx::asset::TextureCache::create(*app->allocator, *app->device, *app->uploader,
                                                          *app->bindless, *app->deletionQueue);
    if (app->textureCache == nullptr) {
        RX_LOG_ERROR("sample_08_gltf_viewer: TextureCache::create failed");
        return nullptr;
    }

    app->registry = std::make_unique<rx::asset::Registry>();

    app->scheduler = rx::task::Scheduler::create();
    if (app->scheduler == nullptr) {
        RX_LOG_ERROR("sample_08_gltf_viewer: Scheduler::create failed");
        return nullptr;
    }
    app->executor = rx::graph::Executor::create(*app->device, *app->scheduler);
    if (app->executor == nullptr) {
        RX_LOG_ERROR("sample_08_gltf_viewer: Executor::create failed");
        return nullptr;
    }

    // Default LINEAR/REPEAT sampler -- shared by the tonemap pass's own HDR
    // read (wrap mode irrelevant there: the fullscreen tonemap quad's own
    // UVs never leave [0,1]) and, via `push.defaultSamplerIndex`
    // (recordSceneDraws() below), `rx_sampleTexture`'s two-argument
    // fallback overload and resolveSamplerIndex()'s own belt-and-
    // suspenders fallback. Registered directly here (not through
    // MaterialSystem, which creates its OWN internal default sampler,
    // unused by this sample's D26.1 draw path -- see recordSceneDraws()'s
    // own header comment) since the TONEMAP pass is entirely outside
    // MaterialSystem and needs its own registered index.
    //
    // [Fix round, sampler-wrap P0] REPEAT, NOT the CLAMP_TO_EDGE this used
    // to be hardcoded to: this WAS the actual sampler standard_pbr.slang/
    // unlit.slang sampled EVERY texture slot through, unconditionally
    // (via `rx_sampleTexture`'s old two-argument-only signature) --
    // DamagedHelmet's own TEXCOORD_0 V lies wholly in [1.0005, 1.9987]
    // (glTF-spec-legal, relying on glTF-default REPEAT), so every fragment
    // clamped to V=1.0 and the whole mesh sampled each texture's bottom
    // edge row. Real per-slot sampler wiring (setupMaterials()'s own
    // resolveSamplerIndex(), StandardPbrParams/UnlitParams' new `*Sampler`
    // fields) is the actual fix; this default's own wrap mode only matters
    // as a fallback now, but REPEAT (glTF's OWN "sampler unspecified"
    // default) is still the honest, spec-correct choice for it, matching
    // MaterialSystem::create()'s own new default (material_system.cpp's
    // `defaultSamplerInfo` comment has the full account).
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(app->device->device(), &samplerInfo, nullptr, &app->defaultSampler) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_08_gltf_viewer: vkCreateSampler (default sampler) failed");
        return nullptr;
    }
    app->defaultSamplerHandle = app->bindless->registerSampler(app->defaultSampler);
    if (!app->defaultSamplerHandle.isValid()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: BindlessTable::registerSampler (default sampler) failed");
        return nullptr;
    }

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: rx::shader::Compiler::create failed");
        return nullptr;
    }
    if (!buildTonemapPipeline(app->device->device(), *compiler, app->bindless->descriptorSetLayout(),
                              app->device->swapchainFormat(), basePath / "tonemap.vert.slang",
                              basePath / "tonemap.frag.slang", app->tonemapPass)) {
        return nullptr;
    }

    // Material set-1 param descriptor infrastructure -- see App::
    // materialParamSetLayout's own comment. Sized generously (kMaxMaterialParamSets
    // sets -- every glTF asset this sample ships/documents has far fewer
    // materials than that; a larger asset simply needs this bumped, logged
    // loudly if ever exceeded via vkAllocateDescriptorSets' own
    // VK_ERROR_OUT_OF_POOL_MEMORY).
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
        RX_LOG_ERROR("sample_08_gltf_viewer: vkCreateDescriptorSetLayout (material params) failed");
        return nullptr;
    }
    constexpr uint32_t kMaxMaterialParamSets = 64;
    std::array<VkDescriptorPoolSize, 1> poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxMaterialParamSets}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxMaterialParamSets;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(app->device->device(), &poolInfo, nullptr, &app->materialParamPool) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_08_gltf_viewer: vkCreateDescriptorPool (material params) failed");
        return nullptr;
    }

    return app;
}

// Tears down EVERYTHING except `app.context`/`app.window`/`app.surface` --
// deliberately stops one step short of Context so both callers (runHeadless()/
// runPresent()) can still call `app.context->hasValidationErrors()`
// immediately afterward and have it mean something: the debug-utils
// messenger callback that populates that flag is owned by Context and stays
// live throughout every vkDestroy* call this function itself makes (device,
// allocator, uploader, bindless pool, tonemap pipeline, material descriptor
// pool, ...) -- so checking it right after this function returns, before
// `app` itself is destroyed, is what actually satisfies this task's own
// binding constraint ("zero unfiltered validation errors INCLUDING
// teardown-time"), matching sample07/06's own identical
// `destroyScene(); if (validate && context->hasValidationErrors())` ordering.
void destroyApp(App& app) {
    if (app.device.has_value() && app.device->device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(app.device->device());
    }
    for (MaterialGpuBinding& binding : app.materialBindings) {
        binding.paramBuffer.reset();  // paramSet itself is freed implicitly by vkDestroyDescriptorPool below.
    }
    if (app.materialParamPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(app.device->device(), app.materialParamPool, nullptr);
        app.materialParamPool = VK_NULL_HANDLE;
    }
    if (app.materialParamSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(app.device->device(), app.materialParamSetLayout, nullptr);
        app.materialParamSetLayout = VK_NULL_HANDLE;
    }
    if (app.drawDataBufferHandle.isValid() && app.bindless.has_value()) {
        app.bindless->release(app.drawDataBufferHandle);
    }
    app.drawDataBuffer.reset();
    destroyCompiledPass(app.device->device(), app.tonemapPass);
    if (app.defaultSamplerHandle.isValid() && app.bindless.has_value()) {
        app.bindless->release(app.defaultSamplerHandle);
    }
    if (app.defaultSampler != VK_NULL_HANDLE) {
        vkDestroySampler(app.device->device(), app.defaultSampler, nullptr);
        app.defaultSampler = VK_NULL_HANDLE;
    }
    // Registry/TextureCache/GeometryPool/MaterialSystem/Executor/Scheduler
    // are all torn down (via their own unique_ptr resets) BEFORE Allocator/
    // Uploader/BindlessTable/Device, matching every other sample's own
    // "high-level owners of GPU objects go first" discipline -- reset()
    // explicitly here rather than relying on member-declaration order, since
    // App's own field order is grouped for readability, not teardown order.
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

// One MaterialGpuBinding per entry in `materialHandles` (ImportResult::
// materials, source Material-array order) -- StandardPBR/Unlit selected by
// MaterialAsset::disposition [D22, gate ruling C5: "KHR_materials_unlit maps
// here at import"], D28's fixed-function state (alphaMode/doubleSided) baked
// in via the loadMaterial() overload, every reflected TParams field written
// once via setMaterialParam() (texture slots resolved through `textures`,
// D11 fallback-safe; UV transforms carried straight from TextureRef::
// uvOffset/uvScale [consume-now, gate ruling C4]), and D27's own
// pre-resolution (getPipeline() called once per binding, result discarded).
// Returns false (logged) on any failure -- this sample has no partial-scene
// fallback for a material it cannot set up.
bool setupMaterials(App& app, const rx::asset::Registry& registry,
                     const std::vector<rx::asset::MaterialHandle>& materialHandles) {
    const rx::material::PassSignature passSig = forwardPassSignature();

    for (rx::asset::MaterialHandle assetHandle : materialHandles) {
        const rx::asset::MaterialAsset& asset = registry.material(assetHandle);
        const bool isUnlit = asset.disposition == rx::asset::MaterialDisposition::Unlit;

        rx::material::MaterialFixedFunctionState fixedFunctionState;
        fixedFunctionState.alphaMode = toMaterialAlphaMode(asset.alphaMode);
        fixedFunctionState.doubleSided = asset.doubleSided;

        const std::filesystem::path& modulePath = isUnlit ? app.unlitPath : app.standardPbrPath;

        MaterialGpuBinding binding;
        try {
            binding.handle = app.materialSystem->loadMaterial(modulePath, fixedFunctionState);
        } catch (const std::exception& ex) {
            RX_LOG_ERROR("sample_08_gltf_viewer: loadMaterial('{}') failed for material '{}': {}",
                         modulePath.string(), asset.name, ex.what());
            return false;
        }

        const uint32_t blockSize = app.materialSystem->paramBlockSize(binding.handle);
        binding.paramBlob.assign(blockSize, uint8_t{0});
        // Copied by value -- materialParams()'s returned reference is
        // invalidated by a LATER loadMaterial() call on this same
        // MaterialSystem instance (material_system.h's own CAUTION comment);
        // this loop calls loadMaterial() again on its next iteration.
        const std::vector<rx::material::MaterialParamInfo> params = app.materialSystem->materialParams(binding.handle);

        const float appliedAlphaCutoff = asset.alphaMode == rx::asset::AlphaMode::Mask ? asset.alphaCutoff : 0.0F;
        const std::array<float, 4> baseColorFactor{asset.baseColorFactor.x, asset.baseColorFactor.y,
                                                     asset.baseColorFactor.z, asset.baseColorFactor.w};

        bool ok = true;
        if (isUnlit) {
            ok = ok && setMaterialParam(binding.paramBlob, params, "baseColorFactor", baseColorFactor);
            ok = ok && setMaterialParam(binding.paramBlob, params, "baseColorTexture",
                                        resolveTextureIndex(*app.textureCache, asset.baseColorTexture,
                                                              rx::asset::TextureRole::BaseColor));
            // [Fix round, sampler-wrap P0] real per-texture sampler --
            // resolveSamplerIndex()'s own header comment has the full
            // account; app.defaultSamplerHandle.index() is only reached on
            // a genuine TextureCache failure (belt-and-suspenders).
            ok = ok && setMaterialParam(binding.paramBlob, params, "baseColorSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.baseColorTexture,
                                                              app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(binding.paramBlob, params, "alphaCutoff", appliedAlphaCutoff);
            ok = ok && setMaterialParam(binding.paramBlob, params, "baseColorUvOffsetScale",
                                        uvTransformParam(asset.baseColorTexture));
        } else {
            const std::array<float, 4> emissiveFactorAndPad{asset.emissiveFactor.x * asset.emissiveStrength,
                                                               asset.emissiveFactor.y * asset.emissiveStrength,
                                                               asset.emissiveFactor.z * asset.emissiveStrength, 0.0F};
            ok = ok && setMaterialParam(binding.paramBlob, params, "baseColorFactor", baseColorFactor);
            ok = ok && setMaterialParam(binding.paramBlob, params, "metallicFactor", asset.metallicFactor);
            ok = ok && setMaterialParam(binding.paramBlob, params, "roughnessFactor", asset.roughnessFactor);
            ok = ok && setMaterialParam(binding.paramBlob, params, "normalScale", asset.normalScale);
            ok = ok && setMaterialParam(binding.paramBlob, params, "occlusionStrength", asset.occlusionStrength);
            ok = ok && setMaterialParam(binding.paramBlob, params, "alphaCutoff", appliedAlphaCutoff);
            ok = ok && setMaterialParam(binding.paramBlob, params, "emissiveFactorAndPad", emissiveFactorAndPad);
            ok = ok && setMaterialParam(binding.paramBlob, params, "baseColorTexture",
                                        resolveTextureIndex(*app.textureCache, asset.baseColorTexture,
                                                              rx::asset::TextureRole::BaseColor));
            ok = ok && setMaterialParam(binding.paramBlob, params, "metallicRoughnessTexture",
                                        resolveTextureIndex(*app.textureCache, asset.metallicRoughnessTexture,
                                                              rx::asset::TextureRole::MetallicRoughness));
            ok = ok && setMaterialParam(binding.paramBlob, params, "normalTexture",
                                        resolveTextureIndex(*app.textureCache, asset.normalTexture,
                                                              rx::asset::TextureRole::Normal));
            ok = ok && setMaterialParam(binding.paramBlob, params, "occlusionTexture",
                                        resolveTextureIndex(*app.textureCache, asset.occlusionTexture,
                                                              rx::asset::TextureRole::Occlusion));
            ok = ok && setMaterialParam(binding.paramBlob, params, "emissiveTexture",
                                        resolveTextureIndex(*app.textureCache, asset.emissiveTexture,
                                                              rx::asset::TextureRole::Emissive));
            // [Fix round, sampler-wrap P0] one real per-texture sampler per
            // slot -- see resolveSamplerIndex()'s own header comment (this
            // file, above) for the full account. This is THE fix for
            // DamagedHelmet's own broken render: its 5 texture slots all
            // share the SAME glTF sampler object (an empty `{}`, i.e. glTF-
            // default REPEAT/auto), so all 5 calls below dedupe to one real
            // REPEAT-wrapping VkSampler/bindless index -- exactly what its
            // TEXCOORD_0 V range ([1.0005,1.9987]) requires.
            ok = ok && setMaterialParam(binding.paramBlob, params, "baseColorSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.baseColorTexture,
                                                              app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(binding.paramBlob, params, "metallicRoughnessSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.metallicRoughnessTexture,
                                                              app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(binding.paramBlob, params, "normalSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.normalTexture,
                                                              app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(binding.paramBlob, params, "occlusionSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.occlusionTexture,
                                                              app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(binding.paramBlob, params, "emissiveSampler",
                                        resolveSamplerIndex(*app.textureCache, asset.emissiveTexture,
                                                              app.defaultSamplerHandle.index()));
            ok = ok && setMaterialParam(binding.paramBlob, params, "baseColorUvOffsetScale",
                                        uvTransformParam(asset.baseColorTexture));
            ok = ok && setMaterialParam(binding.paramBlob, params, "metallicRoughnessUvOffsetScale",
                                        uvTransformParam(asset.metallicRoughnessTexture));
            ok = ok && setMaterialParam(binding.paramBlob, params, "normalUvOffsetScale",
                                        uvTransformParam(asset.normalTexture));
            ok = ok && setMaterialParam(binding.paramBlob, params, "occlusionUvOffsetScale",
                                        uvTransformParam(asset.occlusionTexture));
            ok = ok && setMaterialParam(binding.paramBlob, params, "emissiveUvOffsetScale",
                                        uvTransformParam(asset.emissiveTexture));
        }
        if (!ok) {
            RX_LOG_ERROR("sample_08_gltf_viewer: material param setup failed for material '{}'", asset.name);
            return false;
        }

        // Cache this material's push-constant range shape -- see
        // MaterialGpuBinding's own header comment for why this is read
        // exactly once, here, rather than via a live layoutInfo() call at
        // record time.
        const rx::shader::ShaderLayoutInfo& layout = app.materialSystem->layoutInfo(binding.handle);
        if (layout.pushRanges.empty()) {
            RX_LOG_ERROR("sample_08_gltf_viewer: material '{}' reflects no RxMaterialGlobals push-constant range",
                         asset.name);
            return false;
        }
        binding.pushOffset = layout.pushRanges[0].offset;
        binding.pushSize = layout.pushRanges[0].size;
        binding.pushStages = layout.pushRanges[0].stages;

        // Real UBO + set-1 VkDescriptorSet, written ONCE -- see
        // MaterialGpuBinding's own header comment.
        auto paramBuffer = app.allocator->createHostVisibleBuffer(blockSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                                     rx::rhi::MemoryCategory::Internal);
        if (!paramBuffer.has_value()) {
            RX_LOG_ERROR("sample_08_gltf_viewer: createHostVisibleBuffer (material params) failed for material '{}'",
                         asset.name);
            return false;
        }
        std::memcpy(paramBuffer->mappedData(), binding.paramBlob.data(), blockSize);
        paramBuffer->flush();

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = app.materialParamPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &app.materialParamSetLayout;
        VkDescriptorSet paramSet = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(app.device->device(), &allocInfo, &paramSet) != VK_SUCCESS) {
            RX_LOG_ERROR("sample_08_gltf_viewer: vkAllocateDescriptorSets (material params) failed for material '{}'",
                         asset.name);
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

        // [D27] Pre-resolve this material's forward-pass VkPipeline on the
        // main thread, before any draw is recorded -- discarded, see
        // MaterialGpuBinding's own header comment.
        try {
            app.materialSystem->getPipeline({binding.handle, passSig, 0});
        } catch (const std::exception& ex) {
            RX_LOG_ERROR("sample_08_gltf_viewer: D27 pipeline pre-resolution failed for material '{}': {}",
                         asset.name, ex.what());
            return false;
        }

        app.materialHandleToBinding[assetHandle.index()] = app.materialBindings.size();
        app.materialBindings.push_back(std::move(binding));
    }
    return true;
}

// --- Draw list + D26.1 draw-data buffer -------------------------------
// Flattens `scene.instances` x each instance's own MeshAsset::submeshes
// into `app.draws` (one DrawItem per submesh-instance, GeometryPool
// addressing) and `app.drawDataRows` (one DrawDataGpu row per entry, in the
// SAME order -- DrawItem::drawDataRow is simply that entry's own index).
// Also accumulates `app.sceneBoundsWorld` (union of every instance's own
// world-space AABB, InstanceRecord::worldBounds -- already correctly
// transformed under rotation/negative scale, G11) so frameCameraToScene()
// below can frame the orbit camera to whatever asset was actually loaded,
// not just DamagedHelmet. Returns false (logged) if the scene produces zero
// draws or a submesh references a material with no MaterialGpuBinding (a
// setupMaterials()/import bug, never a legitimate content case), or on any
// GPU allocation failure for the real bindless buffer.
bool buildDrawList(App& app, const rx::asset::Registry& registry, const rx::asset::ImportedScene& scene) {
    app.draws.clear();
    app.drawDataRows.clear();
    app.sceneBoundsWorld = rx::asset::AABB{};

    for (const rx::asset::InstanceRecord& instance : scene.instances) {
        app.sceneBoundsWorld = rx::asset::AABB::unionOf(app.sceneBoundsWorld, instance.worldBounds);

        const rx::asset::MeshAsset& mesh = registry.mesh(instance.mesh);

        // [draw_data.h MATRIX LAYOUT] -- the empirically-verified,
        // GPU-tested pattern src/rx_material/tests/test_standard_pbr_unlit.cpp's
        // own makeHeadOnRow() already established for this exact field
        // (copied verbatim, not re-derived): BOTH the standard normal-matrix
        // formula's own transpose(inverse(...)) AND the row-major-storage
        // layout fix's transpose are applied -- they do NOT cancel out.
        const glm::mat3 normalMat3 = glm::transpose(glm::inverse(glm::mat3(instance.worldTransform)));
        const glm::mat4 modelTransposed = glm::transpose(instance.worldTransform);
        const glm::mat4 normalMatrixTransposed = glm::transpose(glm::mat4(normalMat3));

        for (const rx::asset::Submesh& submesh : mesh.submeshes) {
            auto it = app.materialHandleToBinding.find(submesh.material.index());
            if (it == app.materialHandleToBinding.end()) {
                RX_LOG_ERROR(
                    "sample_08_gltf_viewer: submesh references a material with no MaterialGpuBinding (index {})",
                    submesh.material.index());
                return false;
            }

            DrawItem draw;
            draw.blockId = submesh.range.blockId;
            draw.firstIndex = submesh.range.firstIndex;
            draw.indexCount = submesh.range.indexCount;
            draw.vertexOffset = submesh.range.vertexOffset;
            draw.drawDataRow = static_cast<uint32_t>(app.drawDataRows.size());
            draw.materialBindingIndex = it->second;

            rx::material::DrawDataGpu row;
            row.model = modelTransposed;
            row.normalMatrix = normalMatrixTransposed;
            row.materialIndex = static_cast<uint32_t>(it->second);
            // viewProj/light*/cameraPosWorld are per-PASS, not per-draw --
            // left at DrawDataGpu's own documented defaults here;
            // updateDrawDataPerPassFields() (below) overwrites every row's
            // copy of them, every frame, before the buffer is ever read by
            // a real draw.
            app.drawDataRows.push_back(row);

            app.draws.push_back(draw);
        }
    }

    if (app.draws.empty()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: imported scene produced zero draws");
        return false;
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(app.drawDataRows.size()) * sizeof(rx::material::DrawDataGpu);
    auto buffer = app.allocator->createHostVisibleBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                            rx::rhi::MemoryCategory::Internal);
    if (!buffer.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: createHostVisibleBuffer (draw data, {} rows) failed",
                     app.drawDataRows.size());
        return false;
    }
    std::memcpy(buffer->mappedData(), app.drawDataRows.data(), static_cast<size_t>(bytes));
    buffer->flush();

    rx::rhi::BindlessHandle handle = app.bindless->registerStorageBuffer(buffer->handle(), bytes);
    if (!handle.isValid()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: registerStorageBuffer (draw data) failed");
        return false;
    }

    app.drawDataBuffer = std::move(buffer);
    app.drawDataBufferHandle = handle;
    return true;
}

// Frames `app.camera` (target + distance) to `app.sceneBoundsWorld` -- a
// fixed, deterministic derivation (bounding-sphere radius / sin(halfFov),
// with a constant 1.6x margin) so the same committed glTF asset always
// produces the same default framing, which is what makes the D17 reference
// PNG this default framing renders reproducible from the asset alone.
// A no-op (keeps OrbitCamera's own struct defaults) if the scene produced
// no valid bounds at all -- never divides by a zero/degenerate radius.
void frameCameraToScene(App& app) {
    if (!app.sceneBoundsWorld.isValid()) {
        return;
    }
    const glm::vec3 center = 0.5F * (app.sceneBoundsWorld.min + app.sceneBoundsWorld.max);
    const float radius = 0.5F * glm::length(app.sceneBoundsWorld.max - app.sceneBoundsWorld.min);
    app.camera.target = center;
    if (radius <= 1e-5F) {
        return;  // a degenerate (point-like) scene -- keep the default distance.
    }
    const float halfFovRadians = 0.5F * glm::radians(45.0F);
    app.camera.distance = std::max(radius / std::sin(halfFovRadians) * 1.6F, 0.01F);
}

// [D13, gate ruling RC3 -- "reversed-Z main-camera migration lands here
// for the scene path", Task 22] `rx::scene::Camera` is D13's own
// projection-helpers source of truth ("rx::scene::Camera owns the
// projection helpers so samples cannot get it inconsistently wrong") --
// this is the Stage-2 migration this sample's own header comment (Task
// 16's "reversed-Z NOT yet -- camera helpers arrive Stage 2") promised:
// sample 08 draws exclusively through
// MaterialSystem, whose getPipeline() now hardcodes GREATER_OR_EQUAL
// (material_system.cpp's own D13/RC3 comment) for every pipeline it
// builds -- so this sample's projection must be reversed-Z too, or its
// depth test rejects every fragment. Reuses `Camera::cullingProj()`
// specifically (FINITE far, reversed-Z -- see camera.h's own "TWO
// PROJECTIONS, ON PURPOSE" comment), not `Camera::proj()` (INFINITE far):
// this sample's own near/far are content-derived per-scene finite values
// (immediately below), which is exactly cullingProj()'s own shape, not
// proj()'s. Only the PROJECTION half is reused -- `view` stays this
// sample's own `glm::lookAt()`-based orbit-camera construction verbatim
// (Camera's `position`/`orientation` fields are left at their unused
// defaults; nothing here ever calls `Camera::view()`).
glm::mat4 computeViewProj(const OrbitCamera& camera, float aspect) {
    const glm::mat4 view = glm::lookAt(camera.eye(), camera.target, glm::vec3(0.0F, 1.0F, 0.0F));
    // Near/far derived from the camera's own distance (not a fixed pair of
    // literals) so this stays reasonable regardless of which real-world
    // glTF asset's own scale --scene loads -- a fixed 0.05/1000 pair would
    // either clip a tiny asset's own near geometry or waste the whole
    // depth-buffer's precision on an asset 1000x smaller than DamagedHelmet.
    const float nearZ = std::max(0.01F, camera.distance * 0.01F);
    const float farZ = camera.distance * 50.0F + 10.0F;

    rx::scene::Camera reversedZCamera;
    reversedZCamera.verticalFovRadians = glm::radians(45.0F);
    reversedZCamera.aspectRatio = aspect;
    reversedZCamera.nearPlane = nearZ;
    reversedZCamera.cullingFarPlane = farZ;
    const glm::mat4 proj = reversedZCamera.cullingProj();
    return proj * view;
}

// Overwrites every row's own per-PASS fields (viewProj/light*/cameraPosWorld
// -- D26.1's own "repeated per row" design, draw_data.h) in `app.drawDataRows`
// and re-uploads the whole buffer -- called once per frame (the camera may
// have moved; the draw-data buffer is the only thing that needs to know).
// [FG2 deferral] glTF punctual lights are PRESERVE-LATER, unconsumed in
// Phase 4 (mesh_asset.h's own LightData comment) -- this sample uses a
// fixed key light direction/color and a small flat FG1 ambient term
// (deterministic, asset-independent -- required for the D17 reference PNG
// to be reproducible from the committed asset alone, independent of
// whatever lights that glTF file happens to declare). VALUES: brighter than
// DrawDataGpu's own bare struct-field defaults (which exist purely to keep
// an unconfigured row non-degenerate, not as a rendering recommendation) --
// tuned empirically against DamagedHelmet's own dark base-color texture
// (this sample's default asset) so its shape/materials read clearly with
// only Lambertian + FG1 flat ambient (no IBL until the techniques phase);
// `kAmbient` mirrors shaders/multipass/lit.frag.slang's own established
// "small constant ambient term keeps a fully-shadowed surface visibly
// non-black" precedent (that file's own `kAmbient = 0.08`), nudged slightly
// brighter here since THIS sample has no second light/shadow term to lean
// on at all.
void updateDrawDataPerPassFields(App& app, const glm::mat4& viewProj, const glm::vec3& cameraPosWorld) {
    const glm::vec3 lightDirWorld = glm::normalize(glm::vec3(0.4F, 1.0F, 0.6F));
    const glm::vec3 lightColor(5.0F, 5.0F, 5.0F);
    const glm::vec3 ambientColor(0.18F, 0.18F, 0.18F);

    const glm::mat4 viewProjTransposed = glm::transpose(viewProj);  // [MATRIX LAYOUT] see draw_data.h.
    for (rx::material::DrawDataGpu& row : app.drawDataRows) {
        row.viewProj = viewProjTransposed;
        row.lightDirWorld = glm::vec4(lightDirWorld, 0.0F);
        row.lightColor = glm::vec4(lightColor, 0.0F);
        row.ambientColor = glm::vec4(ambientColor, 0.0F);
        row.cameraPosWorld = glm::vec4(cameraPosWorld, 0.0F);
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(app.drawDataRows.size()) * sizeof(rx::material::DrawDataGpu);
    std::memcpy(app.drawDataBuffer->mappedData(), app.drawDataRows.data(), static_cast<size_t>(bytes));
    app.drawDataBuffer->flush();
}

// --- Forward pass recording [D26.1] -------------------------------------
// Set 0 (bindless) is content-IDENTICAL across every material's own
// pipeline layout (PipelineLayoutBuilder's external-set-0 substitution --
// material_system.cpp -- always the SAME `app.bindless->descriptorSetLayout()`
// object, not merely an equivalent one) -- bound ONCE, before the draw loop,
// against ANY one material's own pipelineLayout(), and never rebound again
// even as different materials' own VkPipelines get bound later (Vulkan spec
// 14.2.2 set-compatibility; matches samples/07_stress's own identical
// "legal before any vkCmdBindPipeline" precedent, that file's own
// recordForwardChunked()).
void recordSceneDraws(VkCommandBuffer cmd, App& app) {
    VkDescriptorSet bindlessSet = app.bindless->descriptorSet();
    VkPipelineLayout anyLayout = app.materialSystem->pipelineLayout(app.materialBindings.front().handle);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, anyLayout, /*firstSet=*/0, 1, &bindlessSet, 0,
                             nullptr);

    const rx::material::PassSignature passSig = forwardPassSignature();
    uint32_t lastBoundBlock = UINT32_MAX;
    // Default-constructed (invalid, generation()==0) -- never equal to any
    // REAL material handle (loadMaterial() always returns generation()>=1),
    // so the very first draw always takes the "material changed" branch.
    rx::material::MaterialHandle lastPipelineMaterial;

    for (const DrawItem& draw : app.draws) {
        MaterialGpuBinding& material = app.materialBindings[draw.materialBindingIndex];

        if (!(material.handle == lastPipelineMaterial)) {
            // [D27] Cache HIT -- setupMaterials() already pre-resolved this
            // exact (material, forward pass signature, 0) key; this call
            // never compiles anything, it only looks the cached VkPipeline
            // up (MaterialSystem::getPipeline()'s own doc comment).
            VkPipeline pipeline = app.materialSystem->getPipeline({material.handle, passSig, 0});
            VkPipelineLayout layout = app.materialSystem->pipelineLayout(material.handle);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            // [D26.1] The real per-scene draw-data buffer + this frame's
            // real --exposure -- NEVER MaterialSystem::bindInstance()'s own
            // default identity row (that method is the documented pre-
            // D26.1 legacy path; material_system.cpp's own bindInstance()
            // header comment names this sample as the real caller that
            // drives its own buffer here instead -- see MaterialGpuBinding's
            // own header comment).
            rx::material::MaterialGlobalsPush push;
            push.defaultSamplerIndex = app.defaultSamplerHandle.index();
            push.drawDataBufferIndex = app.drawDataBufferHandle.index();
            push.exposure = app.exposure;
            vkCmdPushConstants(cmd, layout, material.pushStages, material.pushOffset, material.pushSize, &push);

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, /*firstSet=*/1, 1,
                                     &material.paramSet, 0, nullptr);

            lastPipelineMaterial = material.handle;
        }

        if (draw.blockId != lastBoundBlock) {
            app.geometryPool->bind(cmd, draw.blockId);
            lastBoundBlock = draw.blockId;
        }

        // [D26.1] `firstInstance` IS the D26.1 row address --
        // forward_entry.slang's own vertexMain reads it back via
        // SV_VulkanInstanceID (NEVER SV_InstanceID -- draw_data.h's own
        // comment on why), never a per-draw push constant (sample07's own
        // named anti-pattern, `gPush.instanceIndex`).
        vkCmdDrawIndexed(cmd, draw.indexCount, /*instanceCount=*/1, draw.firstIndex, draw.vertexOffset,
                         /*firstInstance=*/draw.drawDataRow);
    }
}

// The forward pass's own whole-pass callback -- either the D17 loading-state
// clear (scene not yet ready) or the real scene draws.
void recordForward(rx::graph::PassContext& ctx, App& app) {
    if (!app.sceneReady) {
        // [D17] The render graph's own automatic clear (Executor::execute(),
        // fixed {0,0,0,1} -- see that method's own header comment: per-pass
        // configurable clear values are not yet a render-graph feature) has
        // already run before this callback was invoked, inside the SAME
        // active dynamic-rendering scope this callback is now recording
        // into -- vkCmdClearAttachments overwrites it with this sample's
        // own distinct, unmistakably-not-black `kLoadingClearColor` right
        // here, with no separate pass/attachment declaration needed.
        VkClearAttachment clearAttachment{};
        clearAttachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearAttachment.colorAttachment = 0;
        clearAttachment.clearValue.color = kLoadingClearColor;
        VkClearRect clearRect{};
        clearRect.rect = VkRect2D{{0, 0}, ctx.renderArea};
        clearRect.layerCount = 1;
        vkCmdClearAttachments(ctx.cmd, 1, &clearAttachment, 1, &clearRect);
        return;
    }

    VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width),
                        static_cast<float>(ctx.renderArea.height), 0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, ctx.renderArea};
    vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

    recordSceneDraws(ctx.cmd, app);
}

// --- Tonemap pass recording [shaders/multipass/tonemap.{vert,frag}.slang,
// VERBATIM -- see this file's own header comment] -- byte-identical to
// samples/07_stress's own recordTonemapDraw(), copied (not shared -- this
// project's own per-sample-independence convention) rather than re-derived.
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
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.tonemapPass.layoutBundle.layout, 0, 1,
                             &set, 0, nullptr);

    VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width),
                        static_cast<float>(ctx.renderArea.height), 0.0F, 1.0F};
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

// Copied from samples/07_stress's own identically-named helper (not
// re-derived) -- see that file's own comment for the Vulkan-format-naming
// rationale (SwapchainRelative sizeClass, scale 1.0 -- HDR/depth always
// match whatever extent Executor::execute() is called with this frame).
rx::graph::AttachmentDesc swapchainRelativeDesc(VkFormat format) {
    rx::graph::AttachmentDesc desc;
    desc.format = format;
    desc.sizeClass = rx::graph::SizeClass::SwapchainRelative;
    desc.width = 1.0F;
    desc.height = 1.0F;
    return desc;
}

// [D29/RC3] Must agree with MaterialSystem::getPipeline()'s own
// GREATER_OR_EQUAL flip and computeViewProj()'s own reversed-Z
// `Camera::cullingProj()` -- three independent call sites, one
// convention, per D29's own text.
rx::graph::AttachmentDesc swapchainRelativeReversedDepthDesc(VkFormat format) {
    rx::graph::AttachmentDesc desc = swapchainRelativeDesc(format);
    desc.depthConvention = rx::graph::DepthConvention::Reversed;
    return desc;
}

void declareGraph(rx::graph::RenderGraph& graph, App& app, VkFormat backbufferFormat) {
    graph.addPass("forward")
        .addColorOutput("hdr", swapchainRelativeDesc(kHdrFormat))
        .setDepthStencilOutput("depth", swapchainRelativeReversedDepthDesc(kDepthFormat))
        .setExecute([&app](rx::graph::PassContext& ctx) { recordForward(ctx, app); });

    graph.addPass("tonemap")
        .addTextureInput("hdr")
        .addColorOutput("backbuffer", swapchainRelativeDesc(backbufferFormat))
        .setExecute([&app](rx::graph::PassContext& ctx) { recordTonemapDraw(ctx, app); });

    graph.setBackbufferSource("backbuffer");
}

// --- D17 reference-gate support ------------------------------------------
struct ChannelIndices {
    int r;
    int g;
    int b;
};

// Copied from samples/05_multipass/06_materials/07_stress's own identically-
// named helper (not re-derived) -- see any of those files' own comment for
// the Vulkan-format-naming-convention rationale.
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

// Rewrites `pixels` (tightly packed RGBA8-shaped bytes, 4 bytes/texel) IN
// PLACE from `format`'s own real channel order into CANONICAL R,G,B,A byte
// order -- both this run's own live capture AND every committed D17
// reference PNG (always produced via THIS SAME function, through
// --write-references) go through this exact conversion first, so comparing
// a live capture against a committed reference is always apples-to-apples
// regardless of which of the two common swapchain format families the
// device driving either run actually reported. Returns false (logged) for
// any format outside those two families.
bool canonicalizeToRgba8(std::vector<uint8_t>& pixels, VkFormat format) {
    const auto channels = channelIndicesForFormat(format);
    if (!channels.has_value()) {
        RX_LOG_ERROR(
            "sample_08_gltf_viewer: backbuffer format {} is not one of the R8G8B8A8/B8G8R8A8 families the D17 gate "
            "knows how to canonicalize",
            static_cast<int>(format));
        return false;
    }
    if (channels->r == 0 && channels->b == 2) {
        return true;  // already canonical -- nothing to swap.
    }
    for (size_t offset = 0; offset + 4 <= pixels.size(); offset += 4) {
        std::swap(pixels[offset + 0], pixels[offset + 2]);  // R <-> B; G/A already in the right place either way.
    }
    return true;
}

// [D17] "am I lavapipe?" -- the ONE case this gate enforces a hard PASS/FAIL
// verdict for (the committed references were themselves rendered on
// lavapipe, tools/regen_references.sh's own documented convention); any
// other driver's own divergence is reported as INFO, never a failure.
bool isLavapipeDevice(VkPhysicalDevice physicalDevice) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    return std::string_view(props.deviceName).find("llvmpipe") != std::string_view::npos;
}

// --- Headless mode --------------------------------------------------------
// Imports the default (or --scene) glTF asset ASYNCHRONOUSLY, captures a
// loading-state frame (before the import can possibly have completed) and a
// loaded-scene frame (once it reaches a terminal state), and gates each
// against its own committed D17 reference PNG. `--write-references <dir>`
// (undocumented escape hatch, Args' own comment) writes the two captures
// instead of comparing them -- the only way tools/regen_references.sh
// (never auto-run) produces a new reference pair. `--quit-during-load`
// starts the import, lets it run for a small BOUNDED number of pumped steps
// (enough for >=1 real GPU resource to plausibly have landed), cancels it,
// and tears down immediately -- this whole stage's own standing lesson:
// abandon/teardown paths get real-GPU-resource tests, never a mock.
int runHeadless(const Args& args) {
    // [Fix round, item 6] Direct std::chrono wall-clock instrumentation --
    // see the "sample08: perf" log line below (this function's own tail)
    // for the CAPTURE METHOD this task report's own performance numbers
    // actually came from: real, timed wall-clock durations from a live
    // headless run, printed here, NOT a live Tracy GUI/network capture
    // (this dev environment has no Tracy GUI and this project does not
    // build Tracy's own separate `tracy-capture` CLI tool as part of its
    // normal build graph -- a real, but materially larger, undertaking
    // this fix round's own scope did not call for). Task 15's own
    // RX_ZONE/Tracy zones around the async-import pipeline remain fully
    // present and unaffected -- they are what a real interactive Tracy
    // GUI session profiles when one IS connected (RX_TRACY=ON in this
    // preset, per CMakePresets.json) -- this is a second, independent,
    // always-on measurement, not a replacement for them.
    const auto processStart = std::chrono::steady_clock::now();

    auto app = makeApp("RendererX -- 08_gltf_viewer (headless)", kHeadlessWidth, kHeadlessHeight, /*visible=*/false,
                        args.validate);
    if (app == nullptr) {
        return 1;
    }
    app->exposure = args.exposure;

    const std::filesystem::path scenePath =
        args.scenePath.empty() ? resolveDefaultScenePath() : std::filesystem::path(args.scenePath);
    if (!std::filesystem::exists(scenePath)) {
        RX_LOG_ERROR("sample_08_gltf_viewer: scene file not found: '{}'", scenePath.string());
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

    // Offscreen "backbuffer" -- raw (non-RAII) construction, matching every
    // other sample's own headless mode.
    VkImage offscreenImage = VK_NULL_HANDLE;
    VkDeviceMemory offscreenMemory = VK_NULL_HANDLE;
    VkImageView offscreenView = VK_NULL_HANDLE;
    auto destroyRawResources = [&]() {
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
        RX_LOG_ERROR("sample_08_gltf_viewer: vkCreateImage(offscreen target) failed");
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
        if ((memReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == UINT32_MAX) {
        RX_LOG_ERROR("sample_08_gltf_viewer: no device-local memory type found for the offscreen target");
        destroyRawResources();
        destroyApp(*app);
        return 1;
    }
    VkMemoryAllocateInfo memAllocInfo{};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReq.size;
    memAllocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(vkDevice, &memAllocInfo, nullptr, &offscreenMemory) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_08_gltf_viewer: vkAllocateMemory(offscreen target) failed");
        destroyRawResources();
        destroyApp(*app);
        return 1;
    }
    if (vkBindImageMemory(vkDevice, offscreenImage, offscreenMemory, 0) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_08_gltf_viewer: vkBindImageMemory(offscreen target) failed");
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
        RX_LOG_ERROR("sample_08_gltf_viewer: vkCreateImageView(offscreen target) failed");
        destroyRawResources();
        destroyApp(*app);
        return 1;
    }

    auto cmdCtx =
        rx::rhi::CommandContext::create(vkDevice, app->device->graphicsQueue(), app->device->graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: CommandContext::create failed");
        destroyRawResources();
        destroyApp(*app);
        return 1;
    }

    const VkExtent2D extent{kHeadlessWidth, kHeadlessHeight};

    // Captures one frame's worth of `graph`'s own execution into a
    // canonical-RGBA8 pixel buffer -- empty (logged) on any failure.
    auto captureFrame = [&]() -> std::vector<uint8_t> {
        cmdCtx->runOnce(
            [&](VkCommandBuffer cmd) { app->executor->execute(graph, cmd, offscreenImage, offscreenView, extent); });

        auto readback =
            app->allocator->createHostVisibleBuffer(kHeadlessPixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (!readback.has_value()) {
            RX_LOG_ERROR("sample_08_gltf_viewer: createHostVisibleBuffer(readback) failed");
            return {};
        }
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {kHeadlessWidth, kHeadlessHeight, 1};
            vkCmdCopyImageToBuffer(cmd, offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                                    &region);
        });
        readback->invalidate();
        std::vector<uint8_t> pixels(static_cast<size_t>(kHeadlessPixelBytes));
        std::memcpy(pixels.data(), readback->mappedData(), pixels.size());
        if (!canonicalizeToRgba8(pixels, targetFormat)) {
            return {};
        }
        return pixels;
    };

    // --- Loading-state capture: BEFORE starting the async import (frame 0
    // -- the scene is guaranteed not-yet-ready) -- exercises the graph's own
    // automatic clear + this sample's own D17 loading-color override.
    app->sceneReady = false;
    std::vector<uint8_t> loadingPixels = captureFrame();
    bool gateOk = !loadingPixels.empty();

    // --- Start the async import [Task 15's own pipeline -- this sample IS
    // the async-import demonstration vehicle, per the plan's own text].
    bool importFinished = false;
    bool importOk = false;
    const auto importStart = std::chrono::steady_clock::now();
    double importMs = 0.0;
    rx::asset::AsyncImportHandle importHandle = app->registry->importGltfAsync(
        scenePath, *app->geometryPool, *app->scheduler, app->textureCache.get(),
        [&](rx::asset::ImportResult result) {
            importFinished = true;
            importOk = result.ok();
            if (!importOk) {
                RX_LOG_ERROR("sample_08_gltf_viewer: import of '{}' failed: {}", scenePath.string(),
                             rx::asset::importErrorName(result.error));
                return;
            }
            if (!setupMaterials(*app, *app->registry, result.materials) ||
                !buildDrawList(*app, *app->registry, result.scene)) {
                importOk = false;
                return;
            }
            frameCameraToScene(*app);
            app->sceneReady = true;
            importMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - importStart)
                           .count();
        });

    if (args.quitDuringLoad) {
        // Pump a small BOUNDED number of steps (never until terminal --
        // that would defeat the point of a "quit DURING load" test), with a
        // short sleep between each so background workers get real wall-
        // clock time to land >=1 real GPU resource (a texture or geometry
        // upload) before cancellation -- matches async_import_test.cpp's
        // own pump-one-bounded-step-at-a-time idiom.
        for (int i = 0; i < 30 && !importFinished; ++i) {
            app->scheduler->pumpMain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        app->registry->cancelImport(importHandle);
        // Drains whatever the cancellation itself still needs pumped
        // (rollback/release work the cancel path posts back to main) --
        // bounded, never until terminal (a cancelled job's completion
        // callback never fires at all, by design).
        for (int i = 0; i < 30; ++i) {
            app->scheduler->pumpMain();
        }
        vkDeviceWaitIdle(vkDevice);
        // `cmdCtx` owns a real VkCommandPool -- MUST be torn down before
        // destroyApp() destroys the VkDevice it was built against (its own
        // destructor calls vkDestroyCommandPool, which is undefined
        // behavior against an already-destroyed device -- reproduced
        // directly during this task's own development as a real
        // VUID-vkDestroyDevice-device-00378 validation error immediately
        // followed by a segfault at process exit, exactly the "abandon/
        // teardown paths need real-GPU-resource tests" lesson this whole
        // stage's own review history keeps finding).
        cmdCtx.reset();
        destroyRawResources();
        destroyApp(*app);
        if (args.validate && app->context->hasValidationErrors()) {
            RX_LOG_ERROR("sample_08_gltf_viewer: Vulkan validation layer reported errors during --quit-during-load");
            return 1;
        }
        RX_LOG_INFO("sample_08_gltf_viewer: --quit-during-load completed cleanly");
        return 0;
    }

    // --- Pump to a terminal state (Done or Failed), bounded by a wall-clock
    // deadline -- never a busy-spin (a short sleep between polls), matching
    // async_import_test.cpp's own pumpUntilTerminal() idiom.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!importFinished) {
        app->scheduler->pumpMain();
        if (importFinished) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            RX_LOG_ERROR(
                "sample_08_gltf_viewer: import of '{}' did not reach a terminal state within the headless deadline",
                scenePath.string());
            gateOk = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!importOk) {
        gateOk = false;
    }

    // --- Loaded-scene capture.
    std::vector<uint8_t> loadedPixels;
    if (app->sceneReady) {
        const float aspect = static_cast<float>(kHeadlessWidth) / static_cast<float>(kHeadlessHeight);
        const glm::mat4 viewProj = computeViewProj(app->camera, aspect);
        updateDrawDataPerPassFields(*app, viewProj, app->camera.eye());
        loadedPixels = captureFrame();
        if (loadedPixels.empty()) {
            gateOk = false;
        } else {
            // [Fix round, item 6] "sample08: perf" -- greppable, matching
            // this project's own established per-sample stats-line
            // convention (sample07's own "stress:" prefix). `import_ms`:
            // wall-clock from the importGltfAsync() kickoff to the
            // completion callback confirming setupMaterials()/
            // buildDrawList() both succeeded (real, measured -- see this
            // function's own header comment on method). `first_frame_ms`:
            // wall-clock from THIS FUNCTION's own first instruction
            // (before Window/Context/Device/Allocator/MaterialSystem/
            // GeometryPool/TextureCache/Executor construction, the tonemap
            // pipeline build, and the loading-state frame) through this
            // exact point -- the first fully rendered, GPU-readback-
            // confirmed post-import frame -- i.e. genuine cold-start time
            // to a real, correct pixel on screen, not just import alone.
            const double firstFrameMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - processStart).count();
            RX_LOG_INFO("sample08: perf scene='{}' import_ms={:.3f} first_frame_ms={:.3f}", scenePath.string(),
                        importMs, firstFrameMs);
        }
    } else {
        gateOk = false;
    }

    // --- --write-references escape hatch [D17] -- see Args' own comment.
    if (!args.writeReferencesDir.empty()) {
        const std::filesystem::path outDir(args.writeReferencesDir);
        bool wroteOk = true;
        if (!loadingPixels.empty()) {
            wroteOk = rx::samples::writeRgba8Png(outDir / "loading_state.png", kHeadlessWidth, kHeadlessHeight,
                                                  loadingPixels.data()) &&
                      wroteOk;
        }
        if (!loadedPixels.empty()) {
            wroteOk = rx::samples::writeRgba8Png(outDir / "loaded_scene.png", kHeadlessWidth, kHeadlessHeight,
                                                  loadedPixels.data()) &&
                      wroteOk;
        }
        vkDeviceWaitIdle(vkDevice);
        cmdCtx.reset();  // see the --quit-during-load branch's own comment on why this must precede destroyApp().
        destroyRawResources();
        destroyApp(*app);
        if (!wroteOk) {
            RX_LOG_ERROR("sample_08_gltf_viewer: --write-references failed to write one or both PNGs");
            return 1;
        }
        RX_LOG_INFO("sample_08_gltf_viewer: wrote D17 reference PNGs to '{}'", outDir.string());
        return 0;
    }

    // --- D17 tolerance gate -- lavapipe-only hard PASS/FAIL, informational
    // otherwise (the committed references were themselves rendered on
    // lavapipe -- tools/regen_references.sh's own documented convention).
    const bool lavapipe = isLavapipeDevice(app->device->physicalDevice());
    const std::filesystem::path referencesDir = basePathDirectory() / "references";
    auto gateOneFrame = [&](const std::vector<uint8_t>& pixels, const char* label,
                            const std::filesystem::path& referencePath) {
        if (pixels.empty()) {
            return;
        }
        auto reference = rx::samples::loadRgba8Png(referencePath);
        if (!reference.has_value()) {
            RX_LOG_ERROR("sample_08_gltf_viewer: D17: failed to load committed reference '{}'",
                         referencePath.string());
            gateOk = false;
            return;
        }
        rx::samples::GateResult result =
            rx::samples::compareToReference(pixels.data(), kHeadlessWidth, kHeadlessHeight, reference->rgba8.data(),
                                            reference->width, reference->height);
        RX_LOG_INFO("sample_08_gltf_viewer: D17 {} gate: failingPixels={}/{} ({:.4f}%) pass={}{}", label,
                    result.failingPixelCount, result.totalPixelCount, result.failingPixelFraction * 100.0,
                    result.passed, lavapipe ? "" : " [non-lavapipe driver -- informational only, not enforced]");
        if (lavapipe && !result.passed) {
            RX_LOG_ERROR(
                "sample_08_gltf_viewer: D17 {} gate FAILED on lavapipe (first mismatch at ({},{}), delta "
                "rgba=({},{},{},{}))",
                label, result.firstMismatchX, result.firstMismatchY, result.firstMismatchDelta[0],
                result.firstMismatchDelta[1], result.firstMismatchDelta[2], result.firstMismatchDelta[3]);
            gateOk = false;
        }
    };
    gateOneFrame(loadingPixels, "loading_state", referencesDir / "loading_state.png");
    gateOneFrame(loadedPixels, "loaded_scene", referencesDir / "loaded_scene.png");

    vkDeviceWaitIdle(vkDevice);
    cmdCtx.reset();  // see the --quit-during-load branch's own comment on why this must precede destroyApp().
    destroyRawResources();
    destroyApp(*app);

    if (args.validate && app->context->hasValidationErrors()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: Vulkan validation layer reported errors during this run");
        gateOk = false;
    }

    if (gateOk) {
        RX_LOG_INFO("sample_08_gltf_viewer: headless gate PASSED");
        return 0;
    }
    RX_LOG_ERROR("sample_08_gltf_viewer: headless gate FAILED");
    return 1;
}

// --- Present mode: interactive window, real FrameSync loop, mouse-drag
// orbit camera [gate ruling #8: SDL mouse state read directly via SDL3's
// own global SDL_GetMouseState() -- see this file's own header comment]
// --------------------------------------------------------------------
int runPresent(const Args& args) {
    auto app = makeApp("RendererX -- 08_gltf_viewer", kPresentWidth, kPresentHeight, /*visible=*/true, args.validate);
    if (app == nullptr) {
        return 1;
    }
    app->exposure = args.exposure;

    // --vsync off, applied before any per-swapchain-image resource is built
    // -- same ordering samples/05_multipass/06_materials/07_stress already
    // establish.
    if (args.vsyncMode == rx::rhi::PresentMode::VsyncOff) {
        app->device->setPresentMode(args.vsyncMode);
        if (!app->device->recreateSwapchain(app->surface)) {
            RX_LOG_ERROR("sample_08_gltf_viewer: Device::recreateSwapchain failed while applying --vsync off");
            destroyApp(*app);
            return 1;
        }
    }

    // --fullscreen [Phase 4 Task 17, FG7]: makeApp() above already created
    // the window (and Device::create() already built the first swapchain
    // against its pre-fullscreen size), so this reuses the SAME
    // recreateSwapchain(surface) call the --vsync block above just used --
    // never a second/bespoke recreation path [gate ruling #25 row 5].
    if (args.fullscreen) {
        if (!app->window->setFullscreen(true)) {
            RX_LOG_ERROR("sample_08_gltf_viewer: Window::setFullscreen(true) failed while applying --fullscreen");
            destroyApp(*app);
            return 1;
        }
        if (!app->device->recreateSwapchain(app->surface)) {
            RX_LOG_ERROR("sample_08_gltf_viewer: Device::recreateSwapchain failed while applying --fullscreen");
            destroyApp(*app);
            return 1;
        }
    }
    RX_LOG_INFO("sample_08_gltf_viewer: --present: present mode in use: {}",
                rx::rhi::presentModeName(app->device->presentMode()));

    const VkDevice vkDevice = app->device->device();

    auto frameSync = rx::rhi::FrameSync::create(vkDevice, app->device->graphicsQueueFamily(),
                                                 static_cast<uint32_t>(app->device->swapchainImages().size()));
    if (!frameSync.has_value()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: FrameSync::create failed");
        destroyApp(*app);
        return 1;
    }

    const std::filesystem::path scenePath =
        args.scenePath.empty() ? resolveDefaultScenePath() : std::filesystem::path(args.scenePath);
    if (!std::filesystem::exists(scenePath)) {
        RX_LOG_ERROR("sample_08_gltf_viewer: scene file not found: '{}'", scenePath.string());
        frameSync.reset();  // see the final teardown's own comment on why this must precede destroyApp().
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
        for (VkImageView view : swapchainViews) {
            vkDestroyImageView(vkDevice, view, nullptr);
        }
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
                RX_LOG_ERROR("sample_08_gltf_viewer: vkCreateImageView(swapchain) failed");
                return false;
            }
            swapchainViews.push_back(view);
        }
        return true;
    };
    if (!rebuildSwapchainViews()) {
        frameSync.reset();  // see the final teardown's own comment on why this must precede destroyApp().
        destroyApp(*app);
        return 1;
    }

    // Kick off the async import immediately -- the loading-state forward
    // pass (recordForward()'s own !app.sceneReady branch) renders every
    // frame until this completes.
    rx::asset::AsyncImportHandle importHandle = app->registry->importGltfAsync(
        scenePath, *app->geometryPool, *app->scheduler, app->textureCache.get(),
        [&](rx::asset::ImportResult result) {
            if (!result.ok()) {
                RX_LOG_ERROR("sample_08_gltf_viewer: import of '{}' failed: {}", scenePath.string(),
                             rx::asset::importErrorName(result.error));
                return;
            }
            if (!setupMaterials(*app, *app->registry, result.materials) ||
                !buildDrawList(*app, *app->registry, result.scene)) {
                RX_LOG_ERROR("sample_08_gltf_viewer: post-import scene setup failed for '{}'", scenePath.string());
                return;
            }
            frameCameraToScene(*app);
            app->sceneReady = true;
            RX_LOG_INFO("sample_08_gltf_viewer: scene '{}' loaded -- {} draw(s)", scenePath.string(),
                        app->draws.size());
        });
    (void)importHandle;  // --quit-during-load is a headless-only test path -- see this sample's own CMakeLists.txt.

    bool running = true;
    bool ok = true;
    bool dragging = false;
    float lastMouseX = 0.0F;
    float lastMouseY = 0.0F;
    SDL_GetMouseState(&lastMouseX, &lastMouseY);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        app->scheduler->pumpMain();

        // Mouse-drag orbit.
        float mouseX = 0.0F;
        float mouseY = 0.0F;
        const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mouseX, &mouseY);
        const bool leftDown = (buttons & SDL_BUTTON_LMASK) != 0U;
        if (leftDown && dragging) {
            constexpr float kSensitivity = 0.006F;
            const float dx = mouseX - lastMouseX;
            const float dy = mouseY - lastMouseY;
            app->camera.azimuthRadians -= dx * kSensitivity;
            app->camera.elevationRadians = std::clamp(app->camera.elevationRadians - dy * kSensitivity,
                                                        OrbitCamera::kMinElevation, OrbitCamera::kMaxElevation);
        }
        dragging = leftDown;
        lastMouseX = mouseX;
        lastMouseY = mouseY;

        VkFence fence = frameSync->currentFence();
        vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);

        auto acquire = app->device->acquireNextImage(frameSync->currentImageAvailableSemaphore());
        if (acquire.status == rx::rhi::SwapchainStatus::Suspended) {
            // Zero-extent/minimize guard [Phase 4 Task 17, FG7]: see
            // samples/01_triangle/main.cpp's runPresent() for the full
            // rationale (same pattern, every --present sample).
            // app->executor->realize(graph) is safe to call unconditionally
            // here (this sample never re-runs graph.compile() on resize --
            // realize() only rebuilds resources sized off the ORIGINAL,
            // startup-time compile, never the live swapchain extent), so no
            // extra isSuspended() guard is needed around it beyond the
            // rebuild-only-once-resumed shape used below.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            if (!app->device->recreateSwapchain(app->surface)) {
                RX_LOG_ERROR("sample_08_gltf_viewer: Device::recreateSwapchain failed while suspended (zero-extent "
                             "retry)");
                ok = false;
                break;
            }
            if (!app->device->isSuspended()) {
                if (!rebuildSwapchainViews() ||
                    !frameSync->onSwapchainRecreated(static_cast<uint32_t>(app->device->swapchainImages().size()))) {
                    RX_LOG_ERROR("sample_08_gltf_viewer: swapchain view rebuild failed after resuming from "
                                 "suspended state");
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
            RX_LOG_ERROR("sample_08_gltf_viewer: device lost during acquireNextImage; exiting present loop");
            ok = false;
            break;
        }

        vkResetFences(vkDevice, 1, &fence);
        VkCommandBuffer cmd = frameSync->currentCommandBuffer();
        vkResetCommandPool(vkDevice, frameSync->currentCommandPool(), 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        if (app->sceneReady) {
            const VkExtent2D swapExtent = app->device->swapchainExtent();
            const float aspect =
                static_cast<float>(swapExtent.width) / static_cast<float>(std::max<uint32_t>(1U, swapExtent.height));
            const glm::mat4 viewProj = computeViewProj(app->camera, aspect);
            updateDrawDataPerPassFields(*app, viewProj, app->camera.eye());
        }

        app->executor->execute(graph, cmd, app->device->swapchainImages()[acquire.imageIndex],
                               swapchainViews[acquire.imageIndex], app->device->swapchainExtent());

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
            RX_LOG_ERROR("sample_08_gltf_viewer: device lost during present; exiting present loop");
            ok = false;
            break;
        }

        frameSync->advanceFrame();
        RX_FRAME_MARK;
    }

    vkDeviceWaitIdle(vkDevice);
    for (VkImageView view : swapchainViews) {
        vkDestroyImageView(vkDevice, view, nullptr);
    }
    // `frameSync` owns real VkCommandPool/VkFence/VkSemaphore objects --
    // MUST be torn down before destroyApp() destroys the VkDevice it was
    // built against (its own destructor calls vkDestroy* against them,
    // undefined behavior against an already-destroyed device -- the exact
    // same class of bug this file's own runHeadless()/cmdCtx already hit
    // and fixed; see that variable's own comment). Reproduced directly
    // during this task's own development: a real SDL-delivered quit (SIGINT
    // under Xvfb, which SDL3 translates into a normal SDL_EVENT_QUIT) left
    // this present loop's own FrameSync objects live across destroyApp()'s
    // vkDestroyDevice call, producing a full page of VUID-vkDestroyDevice-
    // device-00378 "child object not destroyed" validation errors
    // immediately followed by a segfault (core dump) once `frameSync`
    // itself finally went out of scope and tried to destroy its own
    // now-dangling handles.
    frameSync.reset();
    destroyApp(*app);

    if (args.validate && app->context->hasValidationErrors()) {
        RX_LOG_ERROR("sample_08_gltf_viewer: Vulkan validation layer reported errors during the present loop");
        return 1;
    }
    if (!ok) {
        return 1;
    }
    RX_LOG_INFO("sample_08_gltf_viewer: window closed cleanly");
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
