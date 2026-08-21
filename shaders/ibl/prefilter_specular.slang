// shaders/ibl/prefilter_specular.slang -- Phase 5 Stage 1 Task 9 [#45,
// gate rulings T9/RC1]. Prefiltered specular cubemap: GGX importance-
// sampled convolution of the source (base) cubemap around each
// destination texel's own reflection direction, one compute PASS per
// (mip, face) pair -- roughness varies per MIP (a single `linearRoughness`
// push-constant field per dispatch), face varies per dispatch within a
// mip exactly like equirect_to_cubemap.slang/irradiance_convolve.slang's
// own per-face dispatch shape (see equirect_to_cubemap.slang's header
// comment for why: no writable whole-cube 2D-ARRAY storage view exists in
// this codebase, no RWTextureCube type exists in Slang/HLSL at all).
//
// PORT PROVENANCE -- google/filament v1.75.0 (tag; commit
// 0e58877c09afb1aacd09ff640f74d2adcd2a7e80, Apache-2.0), libs/ibl/src/
// CubemapIBL.cpp, CubemapIBL::roughnessFilter() (:296-465):
//   - linearRoughness == 0 special case (:318-341): a LITERAL passthrough
//     (`Cubemap::writeAt(data, cm.sampleAt(N))`, no importance sampling at
//     all) -- ported EXACTLY below: mip 0 of the prefiltered chain is a
//     bilinear resample of the source cubemap at each destination texel's
//     own direction, matching the ticket's own "mip-0 ~= source"
//     acceptance line by construction, not by approximation.
//   - linearRoughness > 0 (:344-465): hemisphereImportanceSampleDggx()
//     (:50-57) + the accumulate-then-normalize-by-weight structure
//     (:397-411, `entry.brdf_NoL *= 1/weight`) ported as an ONLINE
//     running sum (accumulate numerator AND weight, divide once at the
//     end) -- algebraically identical to the CPU original's two-pass
//     cache-then-normalize structure, just without the intermediate
//     CacheEntry vector a GPU kernel has no use for. `DistributionGGX()`
//     (:140-145) is NOT ported into this file -- the CPU original calls
//     it only to compute `pdf`/`omegaS`, inputs to the mip-LOD-biasing
//     math (selecting which SOURCE MIP an importance sample reads from,
//     CubemapIBL.cpp:389-395) -- this port drops that entire mechanism
//     (simplification 1 below), so nothing in this file ever needs the
//     GGX NDF value itself, only the importance-sampled DIRECTION
//     hemisphereImportanceSampleDggx() already produces.
//     `V = N` (isotropic, view-independent prefiltering -- the standard
//     assumption every real-time prefiltered-environment method makes,
//     including this exact CPU original's own tangent-space derivation,
//     see roughnessFilter()'s own header comment at CubemapIBL.cpp:184-208:
//     "v = n").
//
// SIMPLIFICATIONS vs the CPU original (both documented, neither a missed
// formula -- see irradiance_convolve.slang's own header comment for the
// same class of simplification applied there):
//   1. No pre-filtered "levels" mip-LOD-biasing input (Krivanek/Colbert's
//      variance-reduction trick) -- every importance sample reads the
//      SOURCE cubemap's own single mip 0 via hardware trilinear-filtered
//      TextureCube sampling. Still a convergent, unbiased estimator of
//      the same integral (offset by a generous per-mip sample budget,
//      this bake runs once at load time).
//   2. The CPU original's own `prefilter` bool / `mirror` parameter and
//      random per-scanline sample-set ROTATION (CubemapIBL.cpp:443,
//      "maybe blue-noise instead would look even better") are not
//      reproduced -- this port always applies the pre-filtered-importance-
//      sampling LOD-selection math (n/a here, since no mip-biasing input
//      exists per simplification 1 above) and always uses the SAME
//      (unrotated) Hammersley sample set per texel, exactly like
//      irradiance_convolve.slang's own DFV/irradiance kernels already do
//      -- consistent within this port, not a partial port of an option
//      this task's scope does not need.

struct PushConstants {
    uint faceIndex;
    uint faceDim;
    uint numSamples;
    float linearRoughness;  // alpha = perceptualRoughness^2; 0 at mip 0 (passthrough)
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 1)]]
TextureCube<float4> gSourceCube;

[[vk::binding(1, 1)]]
SamplerState gSourceSampler;

[[vk::binding(2, 1)]]
RWTexture2D<float4> gOutFace;

static const float kPi = 3.14159265358979323846;

// Duplicated -- see irradiance_convolve.slang's own header comment on
// this codebase's established per-file-duplicated-helper idiom.
float3 getDirectionForFace(uint face, float u, float v) {
    float cx = u * 2.0 - 1.0;
    float cy = 1.0 - v * 2.0;
    float3 dir;
    if (face == 0u) {
        dir = float3(1.0, cy, -cx);
    } else if (face == 1u) {
        dir = float3(-1.0, cy, cx);
    } else if (face == 2u) {
        dir = float3(cx, 1.0, -cy);
    } else if (face == 3u) {
        dir = float3(cx, -1.0, cy);
    } else if (face == 4u) {
        dir = float3(cx, cy, 1.0);
    } else {
        dir = float3(-cx, cy, -1.0);
    }
    return normalize(dir);
}

float2 hammersley(uint i, float iN) {
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float2(float(i) * iN, float(bits) * (1.0 / 4294967296.0));
}

// [CubemapIBL.cpp:50-57] hemisphereImportanceSampleDggx(), ported exactly
// (the "(aa-1) == (a-1)(a+1)" fp-accuracy rewrite preserved verbatim).
float3 hemisphereImportanceSampleDggx(float2 u, float a) {
    float phi = 2.0 * kPi * u.x;
    float cosTheta2 = (1.0 - u.y) / (1.0 + (a + 1.0) * ((a - 1.0) * u.y));
    float cosTheta = sqrt(max(0.0, cosTheta2));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta2));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id: SV_DispatchThreadID) {
    if (id.x >= gPush.faceDim || id.y >= gPush.faceDim) {
        return;
    }
    float u = (float(id.x) + 0.5) / float(gPush.faceDim);
    float v = (float(id.y) + 0.5) / float(gPush.faceDim);
    float3 N = getDirectionForFace(gPush.faceIndex, u, v);

    // [CubemapIBL.cpp:318-341] linearRoughness == 0: literal passthrough,
    // no importance sampling at all -- "mip-0 ~= source" by construction.
    if (gPush.linearRoughness <= 0.0) {
        float3 c = gSourceCube.SampleLevel(gSourceSampler, N, 0.0).rgb;
        gOutFace[id.xy] = float4(c, 1.0);
        return;
    }

    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangentX = normalize(cross(up, N));
    float3 tangentY = cross(N, tangentX);

    float3 accum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    float invN = 1.0 / float(gPush.numSamples);
    for (uint i = 0; i < gPush.numSamples; i++) {
        float2 xi = hammersley(i, invN);
        float3 hs = hemisphereImportanceSampleDggx(xi, gPush.linearRoughness);
        float3 h = tangentX * hs.x + tangentY * hs.y + N * hs.z;
        // [CubemapIBL.cpp:184-208 header comment: "v = n"] L = reflect(-N,
        // H) about N (the isotropic V=N assumption every real-time
        // prefiltered-environment method makes).
        float3 l = 2.0 * dot(N, h) * h - N;
        float noL = dot(N, l);
        if (noL > 0.0) {
            accum += gSourceCube.SampleLevel(gSourceSampler, l, 0.0).rgb * noL;
            weight += noL;
        }
    }
    float3 result = weight > 0.0 ? accum / weight : gSourceCube.SampleLevel(gSourceSampler, N, 0.0).rgb;
    gOutFace[id.xy] = float4(result, 1.0);
}
