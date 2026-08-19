// Phase 4 Stage 2 Task 22 [spec D21, gate ruling #23]: the scene-path
// shadow-caster pass's vertex shader -- depth-only, no shadow_caster.frag
// at all (the same "no fragment stage needed" reasoning
// shaders/multipass/shadow.vert.slang's own header comment documents:
// with no color attachments and rasterizerDiscardEnable=VK_FALSE, Vulkan
// writes depth from SV_Position.z alone).
//
// D26.1 bindless addressing, NOT sample 05's push-constant
// `transformIndex` [gate matrix-issue23's own "3x3 PCF -- shadow map read
// path" row, mirroring issue #8's D26.1 finding]: `instanceId` binds to
// `SV_VulkanInstanceID`, never `SV_InstanceID` -- Slang subtracts
// `firstInstance` back out of `SV_InstanceID` to preserve D3D-compatible
// semantics (the same verified Slang/SPIR-V pitfall
// shaders/material/forward_entry.slang's own vertexMain documents in
// full) -- an ordinary `SV_InstanceID` read would silently give a
// per-draw-RELATIVE (always-0-on-a-single-draw) index instead of the
// absolute bindless-buffer row this shader needs.
//
// This is built OUTSIDE MaterialSystem [D28/RC3 -- gate ruling RC3:
// "the scene-path shadow-caster pass is a depth-only pipeline built
// OUTSIDE MaterialSystem"], so it declares its OWN minimal per-draw
// StructuredBuffer (`RxShadowDrawData`) rather than reusing
// shaders/material/material.slang's `RxDrawData` -- the two live in
// entirely separate SPIR-V modules/pipelines and never share a binding.
// `RxShadowDrawData` carries the light's OWN view-proj matrix PER ROW
// (repeated per instance, same "simpler than a second bindless buffer"
// precedent RxDrawData's own header comment establishes for its
// per-pass fields) rather than as a push constant -- this project
// deliberately avoids mat4/vec3 fields inside a push-constant block
// (lit.vert.slang's own empirically-verified reason); only the
// STORAGE-BUFFER INDEX itself (a scalar) travels via push constant, per
// gMaterialGlobals.drawDataBufferIndex's own established idiom.
//
// Field layout mirrors `rx::shadow::ShadowDrawData`
// (rx_shadow/include/rx_shadow/shadow_caster_pipeline.h) EXACTLY -- see
// that header's own comment for the cross-file drift risk.
public struct RxShadowDrawData {
    public float4x4 model;
    public float4x4 lightViewProj;
}

[[vk::binding(2, 0)]]
StructuredBuffer<RxShadowDrawData> gShadowDrawData[];

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
// rx::shadow::ShadowCasterPipeline's own vertex-input-state comment for
// the exact VkVertexInputAttributeDescription this assumes.
[shader("vertex")]
float4 vsMain(float3 position : POSITION, uint instanceId : SV_VulkanInstanceID) : SV_Position
{
    RxShadowDrawData draw = gShadowDrawData[gPush.drawDataBufferIndex][instanceId];
    float4 worldPos = mul(draw.model, float4(position, 1.0));
    return mul(draw.lightViewProj, worldPos);
}
