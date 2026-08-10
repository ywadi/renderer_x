// Sample 05_multipass: tonemap-pass fragment shader.
//
// Concatenated AFTER tonemap.vert.slang at compile time -- gPush/gTextures/
// gSamplers are declared there (see that file's own header comment).
//
// Plain Reinhard (c / (1 + c), per channel) [task brief] -- the resulting
// [0,1) value is written straight to the swapchain's own UNORM/SRGB
// attachment; linear -> sRGB encoding (when the swapchain format carries
// an _SRGB suffix) is handled by the GPU's fixed-function attachment write
// path, exactly like every other sample in this codebase relies on for its
// own color output, not by this shader.
[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float3 hdr = gTextures[gPush.hdrTextureIndex].Sample(gSamplers[gPush.hdrSamplerIndex], input.uv).rgb;
    float3 mapped = hdr / (1.0 + hdr);
    return float4(mapped, 1.0);
}
