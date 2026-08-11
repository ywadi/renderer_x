// Sample 07_stress: tonemap-pass fragment shader.
//
// Concatenated AFTER tonemap.vert.slang at compile time -- gPush/gTextures/
// gSamplers are declared there (see that file's own header comment).
//
// Plain Reinhard (c / (1 + c), per channel) -- byte-for-byte the same
// formula as shaders/multipass/tonemap.frag.slang; see that file's own
// header comment for why the resulting [0,1) value is written straight to
// the backbuffer with no explicit sRGB encode here (the GPU's
// fixed-function attachment write path handles it when the backbuffer
// format itself carries an _SRGB suffix).
[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float3 hdr = gTextures[gPush.hdrTextureIndex].Sample(gSamplers[gPush.hdrSamplerIndex], input.uv).rgb;
    float3 mapped = hdr / (1.0 + hdr);
    return float4(mapped, 1.0);
}
