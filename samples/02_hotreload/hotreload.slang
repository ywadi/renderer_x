// The live-reloadable half of sample_02_hotreload.
//
// samples/02_hotreload/main.cpp's --present mode polls THIS EXACT FILE's
// mtime on disk (~4Hz) and, whenever it changes, recompiles it in-process
// via rx::shader::Compiler::compileFromFile, walks the result with
// rx::shader::reflect(), builds a fresh VkPipelineLayout from that
// reflection with rx::rhi::PipelineLayoutBuilder, and swaps the running
// pipeline for a new one built from all of that -- no restart, no
// hand-typed descriptor/push-constant layout anywhere in the host code.
//
// EDIT ME while --present is running: change the returned color, the
// stripe math, the frequency -- anything you like -- and save. The
// running window updates within about 250ms. A syntax error is fine too:
// the console logs Slang's diagnostics and the window keeps rendering the
// last-good version until the file compiles again.
//
// Fullscreen triangle, no vertex buffer: vsMain emits one oversized
// clip-space triangle (two corners land outside NDC [-1,1], so only the
// portion inside the viewport ever rasterizes) purely from SV_VertexID --
// the same trick samples/01_triangle and rx_shader's own tests use.
struct PushConstants {
    float time;
};

// The host (main.cpp) writes elapsed seconds here every frame in --present
// mode (0 in the headless ctest gate, which uses its own embedded sources
// without this push constant at all -- see main.cpp's kEmbeddedSourceA/B).
// This is real, non-trivial reflect() output flowing through
// PipelineLayoutBuilder::build() into a real VkPipelineLayout -- not the
// hand-crafted ShaderLayoutInfo values rx_rhi_vk's own unit tests use.
[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

struct VSOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vertexID : SV_VertexID)
{
    float2 positions[3] = float2[3](
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    );
    VSOut o;
    o.position = float4(positions[vertexID], 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fsMain(float4 fragCoord : SV_Position) : SV_Target
{
    // <-- EDIT HERE. Try a different base color, a checkerboard from
    // fragCoord.xy, or dropping gPush.time entirely for a static image.
    float stripe = 0.5 + 0.5 * sin(fragCoord.x * 0.05 + gPush.time);
    return float4(stripe, 0.2, 1.0 - stripe, 1.0);
}
