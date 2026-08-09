// Vertex shader for the triangle demo: no vertex buffer, positions are
// hardcoded NDC coordinates selected by SV_VertexID (the classic
// "fullscreen/triangle without a vertex buffer" trick).
struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
VertexOutput main(uint vertexID : SV_VertexID)
{
    float2 positions[3] = float2[3](
        float2(0.0, -0.5),
        float2(0.5, 0.5),
        float2(-0.5, 0.5)
    );

    VertexOutput output;
    output.position = float4(positions[vertexID], 0.0, 1.0);
    output.color = float4(1.0, 1.0, 1.0, 1.0);
    return output;
}
