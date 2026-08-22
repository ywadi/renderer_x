// shaders/depth/depth_prepass.vert.slang -- Phase 5 Stage 2 Task 15 [#51,
// gate ruling RC5]: the scene-path MAIN-CAMERA depth prepass's vertex
// shader -- depth-only, no fragment stage at all, mirroring
// shaders/shadow/shadow_caster.vert.slang's own "no fragment stage needed"
// reasoning (RC3 precedent) verbatim: with no color attachments and
// rasterizerDiscardEnable=VK_FALSE, Vulkan writes depth from SV_Position.z
// alone.
//
// REUSES THE SHADOW-CASTER'S OWN ARCHITECTURE, NOT MATERIAL.SLANG [mirrors
// RC3's "depth-only pipeline built OUTSIDE MaterialSystem" ruling, applied
// here to the MAIN camera instead of a light]: declares its OWN minimal
// per-draw StructuredBuffer (`RxDepthPrepassDrawData`) rather than reusing
// material.slang's `RxDrawData` -- avoids pulling this pass's own pipeline
// layout into material.slang's full bindless surface (gTextures/gSamplers/
// gShadowCompareSamplers/gTexturesCube/gClusterBuffers/gClusterLights) for
// a pass that only ever needs a position transform. `model`/`viewProj` are
// duplicated per row (NOT read from the SAME RxDrawData row forward_entry.
// slang's vertexMain reads) -- the same "simpler than sharing a buffer
// across two independently-scheduled pipelines" precedent
// `rx::shadow::ShadowDrawData` already establishes for the light-space
// case (that struct ALSO duplicates `model`, which DrawDataGpu already
// carries too).
//
// REVERSED-Z [D13's MAIN-camera convention, unlike the shadow caster's own
// STANDARD-Z]: this file itself does not encode the convention (a vertex
// shader's SV_Position output is just clip space either way) -- the
// REVERSED depth compare op (VK_COMPARE_OP_GREATER_OR_EQUAL) and clear
// value (0.0) are pipeline/graph-side choices (samples/09_scene's own
// buildDepthPrepassPipeline()/declareGraph(), matching MaterialSystem's
// own forward-pipeline convention exactly -- see material_system.cpp's own
// "GREATER_OR_EQUAL, not LESS... every pass from Phase 4 Stage 2 on"
// comment).
//
// D26.1-STYLE BINDLESS ADDRESSING, NOT A PUSH-CONSTANT INDEX:
// `instanceId` binds to `SV_VulkanInstanceID`, never `SV_InstanceID` --
// see material.slang's own `RxDrawData`/`gDrawData` header comment for the
// full, verified Slang/SPIR-V/D3D semantic mismatch this sidesteps.
public struct RxDepthPrepassDrawData {
    public float4x4 model;
    public float4x4 viewProj;
}

[[vk::binding(2, 0)]]
StructuredBuffer<RxDepthPrepassDrawData> gDepthPrepassDrawData[];

struct PushConstants {
    uint drawDataBufferIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

// VERTEX INPUT -- only `position` (location 0) is read; the pipeline's own
// VkVertexInputBindingDescription still declares the FULL D8 pooled-vertex
// stride (`rx::asset::PoolVertex`, 48 bytes: pos3/normal3/tangent4/uv2) so
// this shader can bind the SAME GeometryPool vertex buffers the main
// forward pass does, with no separate position-only copy -- see
// shadow_caster.vert.slang's own identical comment.
[shader("vertex")]
float4 vsMain(float3 position : POSITION, uint instanceId : SV_VulkanInstanceID) : SV_Position
{
    RxDepthPrepassDrawData draw = gDepthPrepassDrawData[gPush.drawDataBufferIndex][instanceId];
    float4 worldPos = mul(draw.model, float4(position, 1.0));
    return mul(draw.viewProj, worldPos);
}
