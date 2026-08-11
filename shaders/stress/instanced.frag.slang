// Sample 07_stress: instanced forward-pass fragment shader.
//
// Concatenated AFTER instanced.vert.slang at compile time -- gPush/
// gInstances are declared there (see that file's own header comment).
//
// Fixed-direction Lambert, ambient + diffuse -- deliberately the simplest
// possible shading model this sample's own headless correctness gate can
// still probe analytically (samples/05_multipass's own "no per-object
// rotation" rationale, extended one step further: this sample additionally
// needs its light direction FIXED, not orbit-animated, so the same
// dominance-based probe math main.cpp computes for each of the four
// mesh/pipeline variants holds for every frame the headless gate renders,
// not just one). Writes into the "hdr" pooled attachment
// (R16G16B16A16_SFLOAT, unclamped) -- the "tonemap" pass (whole-pass,
// unchanged in shape from samples/05_multipass's own tonemap.frag.slang)
// maps it down for the backbuffer.
static const float3 kLightDirWorld = normalize(float3(0.3, 1.0, 0.2));

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    float3 n = normalize(input.worldNormal);
    float ndotl = max(dot(n, kLightDirWorld), 0.0);
    float3 shaded = input.color * (0.2 + 0.8 * ndotl);
    return float4(shaded, 1.0);
}
