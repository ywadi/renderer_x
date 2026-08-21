// tests/conformance: linear-clamp "tonemap" fragment shader -- Phase 5
// Stage 1 Task 11 (#47), gate ruling T11.
//
// NOT shaders/multipass/tonemap.frag.slang (Reinhard, `c/(1+c)`, this
// project's own production default) -- a deliberately DIFFERENT, NEW file
// (this project's own binding constraint on OTHER tasks not to touch
// shaders/multipass/ does not even apply here, since this is a genuinely
// separate concern, not a modification of that file). See
// tools/gltf_conformance/harness.html's own header comment for the full
// methodology: the Khronos Sample Viewer offers no Reinhard tonemap option
// at all (its own choices are KHR_PBR_NEUTRAL/ACES-family/NONE --
// verified directly against source/Renderer/shaders/tonemapping.glsl,
// pinned commit), so comparing through either side's own preferred curve
// would make this conformance gate measure "do the tonemap curves agree"
// (an orthogonal, Phase-5-Task-32-owned question) instead of "do the
// underlying BRDF/lighting/IBL linear-radiance values agree". This
// shader instead replicates the Sample Viewer's `GltfState.ToneMaps.NONE`
// path bit-for-bit: `tonemapping.glsl`'s own `toneMap()`, with every
// `#ifdef TONEMAP_*` branch compiled out, reduces to exactly
// `color *= u_Exposure; return linearTosRGB(color);` where
// `linearTosRGB(c) = pow(c, 1/2.2)` (that file's own definition) and the
// final hard clip to [0,1] happens implicitly at the 8-bit backbuffer
// write, on BOTH sides (WebGL2's own default framebuffer there; this
// engine's own SRGB-format swapchain attachment write here) -- so this
// shader's own job is just `clamp(hdr, 0, 1)` PLUS `pow(x, 1/2.2)`: the
// same "linear -> sRGB encoding... handled by the GPU's fixed-function
// attachment write path" trick tonemap.frag.slang's own header comment
// already documents does NOT apply to a plain `pow()` call this shader
// makes explicit itself, since relying on hardware sRGB write here would
// silently apply a SECOND, different (piecewise, not pure-gamma) curve on
// top of this shader's own already-encoded output -- see this file's own
// CMakeLists.txt caller for the backbuffer format this assumes (UNORM,
// not _SRGB, specifically so this shader's own explicit pow() is the ONLY
// encode step, matching the reference side's own single explicit
// linearTosRGB() call exactly).
//
// Concatenated AFTER shaders/multipass/tonemap.vert.slang at compile time
// (same idiom that file's own header comment documents) -- gPush/
// gTextures/gSamplers are declared there.
[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float3 hdr = gTextures[gPush.hdrTextureIndex].Sample(gSamplers[gPush.hdrSamplerIndex], input.uv).rgb;
    float3 clamped = clamp(hdr, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
    float3 srgb = pow(clamped, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    return float4(srgb, 1.0);
}
