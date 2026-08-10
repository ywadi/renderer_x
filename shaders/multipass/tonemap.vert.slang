// Sample 05_multipass: tonemap-pass vertex shader.
//
// COMPILATION NOTE: concatenated BEFORE tonemap.frag.slang at compile time
// (see lit.vert.slang's identical header comment for the full rationale) --
// every global declared below is visible to tonemap.frag.slang's fsMain.
//
// Classic "one oversized triangle covers the whole clip-space square" trick
// (no vertex buffer, SV_VertexID-driven) -- the exact same idiom
// src/rx_graph/tests/test_execute_gpu.cpp's "invert" shader and this
// project's other fullscreen passes already use.
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
