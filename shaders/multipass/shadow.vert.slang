// Sample 05_multipass: shadow pass vertex shader.
//
// Depth-only pass -- there is deliberately no shadow.frag.slang at all:
// with no color attachments and rasterizerDiscardEnable = VK_FALSE, Vulkan
// needs no fragment shader stage to write depth at all (the depth test/
// write consumes SV_Position.z's interpolated value); samples/05_multipass/
// main.cpp's shadow pipeline has exactly one shader stage (VERTEX) because
// of this, not by omission.
//
// Reads the SAME bindless object-transform storage buffer
// lit.vert.slang/lit.frag.slang read (row layout must match byte-for-byte)
// via rx::rhi::BindlessTable's fixed binding 2 (kStorageBufferBinding),
// but only ever consumes `.lightMvp` here: this pass renders the scene
// from the light's own point of view, so a caster's CAMERA-space MVP is
// irrelevant to it. `transformIndex` (a push constant, uniform across the
// whole draw call) selects which object's row to read -- never wrapped in
// NonUniformResourceIndex() [spec Fixed decision #6, matching every other
// sample's push-constant-indexed bindless read].
//
// `ObjectTransform` is concatenated ahead of this file at compile time
// from scene_types.slang -- see samples/05_multipass/main.cpp's
// buildShadowPipeline() and scene_types.slang's own header comment for
// why (and for the postmortem of the real bug this fixes: this file used
// to declare its OWN separate copy of this struct, since it is compiled
// as its own Slang module -- never concatenated with the lit files -- and
// that copy silently drifted out of sync with lit.vert.slang's when the
// latter grew two more fields). [fix round 1]
struct PushConstants {
    uint transformIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(2, 0)]]
StructuredBuffer<ObjectTransform> gTransforms[];

struct VSIn {
    float3 position : POSITION;
};

[shader("vertex")]
float4 vsMain(VSIn input) : SV_Position
{
    // gTransforms[0]: this sample registers exactly ONE bindless storage
    // buffer (the shared, double-buffered-by-row transform table), exactly
    // like samples/03_bindless_mesh's own gTransforms[0] -- a compile-time-
    // constant descriptor-array index, uniform by construction.
    float4x4 lightMvp = gTransforms[0][gPush.transformIndex].lightMvp;
    return mul(lightMvp, float4(input.position, 1.0));
}
