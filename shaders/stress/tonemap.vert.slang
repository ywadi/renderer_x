// Sample 07_stress: tonemap-pass vertex shader.
//
// Byte-for-byte the same fullscreen-triangle idiom as
// shaders/multipass/tonemap.vert.slang (samples/05_multipass's own
// tonemap pass) -- a distinct, per-sample copy rather than a shared file,
// matching this codebase's existing per-sample shader convention (no two
// samples share an on-disk .slang file outside rx_material's own
// shared material.slang/forward_entry.slang, which is a materials-system
// concern, not a per-pass one).
//
// COMPILATION NOTE: concatenated BEFORE tonemap.frag.slang at compile time
// -- every global declared below is visible to tonemap.frag.slang's fsMain.
struct PushConstants {
    uint hdrTextureIndex;
    uint hdrSamplerIndex;
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
