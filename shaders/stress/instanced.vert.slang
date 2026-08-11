// Sample 07_stress: instanced forward-pass vertex shader.
//
// COMPILATION NOTE: concatenated BEFORE instanced.frag.slang at compile
// time (same reasoning as samples/05_multipass's lit.vert.slang -- one
// linked Slang translation unit per pass, so rx::shader::reflect() merges
// both stages' bindings/push-constants into one layout) -- every global
// declared below is visible to instanced.frag.slang's fsMain.
//
// PER-INSTANCE DATA: one bindless storage buffer, one row per instance,
// addressed by a push-constant `instanceIndex` -- samples/03_bindless_mesh/
// 05_multipass's own bindless-storage-buffer-plus-push-constant-index
// pattern, extended to the "many small, cheap draws" shape this sample
// stresses (task-7-brief.md: "per-instance transform+color in a bindless
// storage arena per the 03/05 idiom"). Each instance's own vkCmdDrawIndexed
// call is genuinely separate (this sample measures CPU-side per-draw
// recording cost at scale, not GPU-side hardware instancing) -- see
// samples/07_stress/main.cpp's own header comment for why a single
// instanceCount-N draw call would defeat the entire point of this sample.
//
// TRANSFORM: position + uniform scale only, no rotation -- same convention
// samples/05_multipass's lit.vert.slang already established, and for the
// identical reason: a translation-plus-uniform-scale model transform never
// needs a normal-matrix multiply at all (a uniform scale changes a normal's
// LENGTH but never its DIRECTION -- the vertex shader passes the local
// normal straight through, and instanced.frag.slang normalizes it before
// use, which cancels the scale factor exactly). Reintroducing rotation
// and/or non-uniform scale without also reintroducing a normal-matrix
// transform here would silently break lighting.
//
// CAMERA: `viewProj` travels as a push constant (not baked into each
// instance's own storage row) -- this sample's instance data is uploaded
// ONCE, at scene setup, and never re-touched per frame [main.cpp's own
// header comment: recomputing/re-uploading 30000 rows every frame would
// conflate CPU-side "prepare instance data" cost with the CPU-side
// "record the forward pass" cost this sample exists to measure]. A shared
// per-frame value in a push constant, re-sent on every one of this
// pass's many draws, is simpler than a second one-row bindless buffer
// just for it -- matches samples/05_multipass's lit.vert.slang's own
// `lightDirWorld` precedent for a per-pass (not per-object) constant.
struct InstanceData {
    float4 positionAndScale;  // xyz = world position, w = uniform scale
    float4 color;              // rgb = albedo, a unused (padding)
};

struct PushConstants {
    float4x4 viewProj;
    uint instanceIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(2, 0)]]
StructuredBuffer<InstanceData> gInstances[];

struct VSIn {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct VSOut {
    float4 clipPosition : SV_Position;
    float3 worldNormal : NORMAL;
    float3 color : COLOR0;
};

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    InstanceData inst = gInstances[0][gPush.instanceIndex];

    float3 worldPos = input.position * inst.positionAndScale.w + inst.positionAndScale.xyz;

    VSOut o;
    o.clipPosition = mul(gPush.viewProj, float4(worldPos, 1.0));
    o.worldNormal = input.normal;
    o.color = inst.color.rgb;
    return o;
}
