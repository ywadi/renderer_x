// shaders/ibl/dfg_lut.slang -- Phase 5 Stage 1 Task 9 [#45, gate rulings
// T9/RC1]: the DFG (BRDF-integration) LUT -- a flat 2D image, NOT a
// cubemap, no source environment read needed at all (purely procedural,
// a function of (NoV, roughness) alone). This is what activates Task 8's
// waiting `energyCompensation(f0, dfgY)` [shaders/material/brdf.slang:
// 219-221] with a REAL, backed value: `dfgY` is this LUT's G channel.
//
// PORT PROVENANCE -- google/filament v1.75.0 (tag; commit
// 0e58877c09afb1aacd09ff640f74d2adcd2a7e80, Apache-2.0), libs/ibl/src/
// CubemapIBL.cpp:
//   - DFV_Multiscatter() (:790-833) -- ported exactly below as
//     dfvMultiscatter(). multiscatter=true is used UNCONDITIONALLY (never
//     the plain, non-multiscatter DFV() at :748-788) -- this is not a
//     free choice: Task 8's energyCompensation(f0, dfgY) formula (`1 +
//     f0*(1/dfgY - 1)`) is the Kulla-Conty multiscatter identity keyed
//     EXACTLY on DFV_Multiscatter's own `.y` channel (`Ess`, the
//     single-scatter energy sum with no Fresnel term -- see that
//     function's own header comment: "Er() = (1-f0)*DFV.x + f0*DFV.y").
//     Feeding the plain (non-multiscatter) DFV()'s own `.y` channel into
//     that exact formula would be a DIFFERENT, WRONG quantity (DFV's own
//     comment: "Er() = f0*DFV.x + f90*DFV.y", not the same identity at
//     all) -- Task 8's own calling contract (brdf.slang's header comment)
//     is written against the multiscatter formulation specifically.
//   - Visibility() (CubemapIBL.cpp:170-177) -- the SAME height-correlated
//     Smith visibility term shaders/material/brdf.slang's own
//     V_SmithGGXCorrelated() already ports (Task 7) -- duplicated here
//     rather than imported, matching this codebase's own established
//     per-file-duplicated-helper idiom for standalone compute kernels
//     (see irradiance_convolve.slang's header comment).
//   - CubemapIBL::DFG() (:1008-1037) -- the (x,y) texel -> (NoV,
//     linearRoughness) parameterization ported exactly: `coord =
//     saturate((height - y + 0.5) / height)`, `linearRoughness = coord *
//     coord` (row 0 = roughest, the LAST row = smoothest/mirror -- see
//     this task's own analytic test for the closed-form low-roughness
//     row this parameterization makes exact); `NoV = saturate((x + 0.5) /
//     width)`. `cloth`=false always (unconditionally) -- no cloth/sheen
//     BRDF exists in this codebase yet (Task 27/34 registry pointers);
//     the CPU original's own `cloth` branch (DFV_Charlie_Uniform into the
//     B channel) is simply never taken, matching this LUT's own 2-channel
//     (R16G16) storage shape, which has no third channel to carry it in
//     anyway.

struct PushConstants {
    uint width;
    uint height;
    uint numSamples;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 1)]]
RWTexture2D<float2> gOutLut;

static const float kPi = 3.14159265358979323846;

float2 hammersley(uint i, float iN) {
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float2(float(i) * iN, float(bits) * (1.0 / 4294967296.0));
}

float3 hemisphereImportanceSampleDggx(float2 u, float a) {
    float phi = 2.0 * kPi * u.x;
    float cosTheta2 = (1.0 - u.y) / (1.0 + (a + 1.0) * ((a - 1.0) * u.y));
    float cosTheta = sqrt(max(0.0, cosTheta2));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta2));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// [CubemapIBL.cpp:170-177] Visibility(), ported exactly, with an added
// fp32 div-by-zero guard (1e-5) matching brdf.slang's own kDiv0Epsilon
// precedent (Task 7) -- the CPU original has no such guard (a CPU double-
// precision divide-by-a-tiny-nonzero-number is merely imprecise, never a
// GPU-fp32 NaN/Inf risk the way a genuine zero denominator would be at an
// exact grazing angle).
float visibilityTerm(float noV, float noL, float a) {
    float a2 = a * a;
    float ggxL = noV * sqrt(max(0.0, (noL - noL * a2) * noL + a2));
    float ggxV = noL * sqrt(max(0.0, (noV - noV * a2) * noV + a2));
    return 0.5 / max(ggxV + ggxL, 1e-5);
}

// [CubemapIBL.cpp:790-833] DFV_Multiscatter(), ported exactly.
float2 dfvMultiscatter(float noV, float linearRoughness, uint numSamples) {
    float2 r = float2(0.0, 0.0);
    float3 v = float3(sqrt(max(0.0, 1.0 - noV * noV)), 0.0, noV);
    float invN = 1.0 / float(numSamples);
    for (uint i = 0; i < numSamples; i++) {
        float2 xi = hammersley(i, invN);
        float3 h = hemisphereImportanceSampleDggx(xi, linearRoughness);
        float dotVH = dot(v, h);
        float3 l = 2.0 * dotVH * h - v;
        float voH = saturate(dotVH);
        float noL = saturate(l.z);
        float noH = saturate(h.z);
        if (noL > 0.0) {
            float term = visibilityTerm(noV, noL, linearRoughness) * noL * (voH / max(noH, 1e-6));
            float fc = pow(clamp(1.0 - voH, 0.0, 1.0), 5.0);
            r.x += term * fc;
            r.y += term;
        }
    }
    return r * (4.0 / float(numSamples));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id: SV_DispatchThreadID) {
    if (id.x >= gPush.width || id.y >= gPush.height) {
        return;
    }
    float h = float(gPush.height);
    float coord = saturate((h - float(id.y) + 0.5) / h);
    float linearRoughness = coord * coord;
    float noV = saturate((float(id.x) + 0.5) / float(gPush.width));
    float2 dfg = dfvMultiscatter(noV, linearRoughness, gPush.numSamples);
    gOutLut[id.xy] = dfg;
}
