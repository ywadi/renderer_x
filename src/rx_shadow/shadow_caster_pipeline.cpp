#include <rx_shadow/shadow_caster_pipeline.h>

#include <rx_core/log.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/device.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>

#include <array>
#include <cstddef>

// The scene-path shadow-caster pipeline [D21, Phase 4 Stage 2 Task 22,
// gate ruling RC3]. See shadow_caster_pipeline.h's own class comment for
// the full standard-Z / dynamic-depth-bias / depth-clamp rationale --
// this file is the mechanism, that header is the contract.
#ifndef RX_SHADOW_SHADER_DIR
#error "RX_SHADOW_SHADER_DIR must be defined by rx_shadow's own CMakeLists.txt"
#endif

namespace rx::shadow {

namespace {

struct MaterialVertexLayoutStride {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv[2];
};
static_assert(sizeof(MaterialVertexLayoutStride) == 48,
              "must match rx::asset::PoolVertex/rx_material's own MaterialVertexLayout stride exactly -- the "
              "shadow-caster pipeline binds the SAME GeometryPool vertex buffers the main forward pass does");

}  // namespace

std::unique_ptr<ShadowCasterPipeline> ShadowCasterPipeline::create(rx::rhi::Device& device,
                                                                     rx::rhi::BindlessTable& bindless,
                                                                     const ShadowCasterPipelineDesc& desc,
                                                                     const std::filesystem::path& shaderDir) {
    const std::filesystem::path effectiveShaderDir = !shaderDir.empty() ? shaderDir : std::filesystem::path(RX_SHADOW_SHADER_DIR);
    const std::filesystem::path shaderPath = effectiveShaderDir / "shadow_caster.vert.slang";

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        RX_LOG_ERROR("rx_shadow: ShadowCasterPipeline::create: rx::shader::Compiler::create failed");
        return nullptr;
    }

    rx::shader::CompileResult compileResult = compiler->compileFromFile(shaderPath.string(), {"vsMain"});
    if (!compileResult.ok) {
        RX_LOG_ERROR("rx_shadow: ShadowCasterPipeline::create: shader compile failed for '{}': {}",
                     shaderPath.string(), compileResult.diagnostics);
        return nullptr;
    }

    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("rx_shadow: ShadowCasterPipeline::create: reflect() failed for '{}'", shaderPath.string());
        return nullptr;
    }

    auto result = std::unique_ptr<ShadowCasterPipeline>(new ShadowCasterPipeline());
    result->device_ = device.device();

    // --- Vertex shader module -------------------------------------------
    for (const auto& blob : compileResult.entryPointCode) {
        if (blob.entryPointName != "vsMain") {
            continue;
        }
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = blob.code.size() * sizeof(uint32_t);
        moduleInfo.pCode = blob.code.data();
        if (vkCreateShaderModule(result->device_, &moduleInfo, nullptr, &result->vertModule_) != VK_SUCCESS) {
            RX_LOG_ERROR("rx_shadow: ShadowCasterPipeline::create: vkCreateShaderModule failed");
            return nullptr;
        }
    }
    if (result->vertModule_ == VK_NULL_HANDLE) {
        RX_LOG_ERROR("rx_shadow: ShadowCasterPipeline::create: no 'vsMain' entry point in compiled SPIR-V");
        return nullptr;
    }

    // --- Pipeline layout -- external set-0 substitution, subset shape --
    // (this shader's own reflected set 0 has exactly ONE binding, the
    // storage-buffer slot -- a strict subset of BindlessTable's three
    // fixed slots, which PipelineLayoutBuilder::build() already supports
    // without any change; see this class's own header comment).
    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(result->device_, *layoutInfo, bindless.descriptorSetLayout());
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("rx_shadow: ShadowCasterPipeline::create: PipelineLayoutBuilder::build failed");
        return nullptr;
    }
    result->layoutBundle_ = std::move(*layoutBundle);

    // --- Fixed pipeline state --------------------------------------------
    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = result->vertModule_;
    vertStage.pName = "main";  // Slang's SPIR-V backend always names the entry point "main" regardless of source name.

    // Vertex input: the SAME 48-byte D8 pooled-vertex stride
    // (rx::asset::PoolVertex) the main forward pass's MaterialSystem
    // pipelines bind, so this pipeline can bind the SAME GeometryPool
    // buffers with no separate position-only copy -- only location 0
    // (position) is declared as an ATTRIBUTE, since the shader reads
    // nothing else; the binding's own STRIDE still must match the real
    // buffer layout for the other attributes' bytes to be skipped
    // correctly.
    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding.binding = 0;
    vertexBinding.stride = sizeof(MaterialVertexLayoutStride);
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription positionAttribute{};
    positionAttribute.location = 0;
    positionAttribute.binding = 0;
    positionAttribute.format = VK_FORMAT_R32G32B32_SFLOAT;
    positionAttribute.offset = offsetof(MaterialVertexLayoutStride, position);

    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount = 1;
    vertexInputState.pVertexBindingDescriptions = &vertexBinding;
    vertexInputState.vertexAttributeDescriptionCount = 1;
    vertexInputState.pVertexAttributeDescriptions = &positionAttribute;

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
    inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // DEPTH CLAMP [D21, gate ruling #23: "depthClampEnable=VK_TRUE on
    // casters + device-feature check"] -- opportunistic: only set when
    // the device actually has the feature ENABLED (device.
    // supportsDepthClamp(), not merely advertised -- see that accessor's
    // own header comment for the VUID this guards). No cull -- casters
    // are rendered from BOTH faces (front-face culling as a bias
    // alternative is an explicitly out-of-scope, named escape hatch per
    // the gate's own matrix, not built here).
    if (desc.depthClampEnableOverrideForTesting.has_value() && *desc.depthClampEnableOverrideForTesting &&
        !device.supportsDepthClamp()) {
        RX_LOG_ERROR(
            "rx_shadow: ShadowCasterPipeline::create: depthClampEnableOverrideForTesting=true requested but "
            "device.supportsDepthClamp() is false -- rejecting (would violate "
            "VUID-VkPipelineRasterizationStateCreateInfo-depthClampEnable-00782)");
        return nullptr;
    }
    const bool depthClampEnabled =
        desc.depthClampEnableOverrideForTesting.value_or(device.supportsDepthClamp());
    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.depthClampEnable = depthClampEnabled ? VK_TRUE : VK_FALSE;
    rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = VK_CULL_MODE_NONE;
    rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationState.depthBiasEnable = VK_TRUE;  // constant/slope/clamp factors are DYNAMIC (see dynamicStates below) -- this only turns the mechanism on.
    rasterizationState.lineWidth = 1.0F;
    if (!depthClampEnabled) {
        RX_LOG_WARN(
            "rx_shadow: ShadowCasterPipeline::create: depthClamp feature not enabled on this device -- a caster "
            "crossing the light's near plane may vanish/truncate instead of casting its full silhouette "
            "[Phase 4 limitation, spec D21/gate ruling #23]");
    }

    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // STANDARD-Z, NOT REVERSED [D13 -- required code comment, gate
    // ruling #23's own "wrong-fix-prevention" row]: shadow maps stay
    // standard-Z in Phase 4 -- LESS, matching a clear value of 1.0 (see
    // src/rx_graph's own D29 DepthConvention::Standard, the default every
    // caller of this pipeline's own depth attachment must use). D13's
    // reversed-Z migration is MAIN-CAMERA-ONLY (rx::material::
    // MaterialSystem::getPipeline() carries the matching GREATER_OR_EQUAL
    // flip + its own mirror of this same comment) -- do NOT "fix" this to
    // GREATER_OR_EQUAL to match that one; they are deliberately different
    // conventions. The slope-scaled depth-bias sign this pipeline's own
    // `bindAndSetDepthBias()` applies below is therefore the ORDINARY
    // (non-inverted) convention throughout Phase 4: a positive
    // `depthBiasConstantFactor`/`depthBiasSlopeFactor` pushes a caster's
    // stored depth FARTHER from the light (the intended "push the
    // occluder back" direction) under this LESS/clear-1.0 convention,
    // exactly as the D3D/Vulkan bias formula's ordinary sign already
    // implies -- inverting it here would be the exact plausible-but-wrong
    // "fix" this comment exists to prevent.
    VkPipelineDepthStencilStateCreateInfo depthStencilState{};
    depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState.depthTestEnable = VK_TRUE;
    depthStencilState.depthWriteEnable = VK_TRUE;
    depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS;

    // DYNAMIC DEPTH BIAS [D21, gate ruling #23's own "creation-time
    // detail the ticket omitted"] -- VK_DYNAMIC_STATE_DEPTH_BIAS added
    // alongside the ordinary VIEWPORT/SCISSOR pair every pipeline in this
    // codebase already carries.
    std::array<VkDynamicState, 3> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                                 VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineRenderingCreateInfo renderingCreateInfo{};
    renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCreateInfo.depthAttachmentFormat = desc.depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingCreateInfo;
    pipelineInfo.stageCount = 1;
    pipelineInfo.pStages = &vertStage;
    pipelineInfo.pVertexInputState = &vertexInputState;
    pipelineInfo.pInputAssemblyState = &inputAssemblyState;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizationState;
    pipelineInfo.pMultisampleState = &multisampleState;
    pipelineInfo.pDepthStencilState = &depthStencilState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = result->layoutBundle_.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;  // dynamic rendering
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(result->device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &result->pipeline_) !=
        VK_SUCCESS) {
        RX_LOG_ERROR("rx_shadow: ShadowCasterPipeline::create: vkCreateGraphicsPipelines failed");
        return nullptr;
    }

    result->depthClampEnabled_ = depthClampEnabled;
    return result;
}

ShadowCasterPipeline::~ShadowCasterPipeline() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
    }
    if (vertModule_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, vertModule_, nullptr);
    }
    // layoutBundle_'s own destructor handles the pipeline layout (and any
    // owned, non-external set layouts -- none here, see this class's own
    // header comment) automatically.
}

void ShadowCasterPipeline::bindAndSetDepthBias(VkCommandBuffer cmd, float depthBiasConstantFactor,
                                                float depthBiasSlopeFactor, float depthBiasClamp) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    // Ordinary (non-inverted) sign -- see create()'s own D13 comment for
    // why this pipeline's standard-Z convention never flips it.
    vkCmdSetDepthBias(cmd, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor);
}

}  // namespace rx::shadow
