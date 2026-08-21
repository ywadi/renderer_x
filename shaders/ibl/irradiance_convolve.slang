// shaders/ibl/irradiance_convolve.slang -- Phase 5 Stage 1 Task 9 [#45,
// gate rulings T9 per-ticket ruling: irradiance-CUBEMAP baseline, SH9
// recorded as a later optional optimization -- see rulings-2026-08-20.md].
// Diffuse irradiance convolution: cosine-weighted importance-sampled
// average of the source (base) cubemap over the hemisphere around each
// destination texel's own direction.
//
// PORT PROVENANCE -- google/filament v1.75.0 (tag; commit
// 0e58877c09afb1aacd09ff640f74d2adcd2a7e80, Apache-2.0), libs/ibl/src/
// CubemapIBL.cpp, CubemapIBL::diffuseIrradiance() (:554-634):
// hemisphereCosSample() (:59-65) + the per-texel accumulation loop
// (:610-633) ported exactly -- `Ed() = (1/N) * sum L(l)`, the SAME
// "avoid the 1/PI multiply in the shader" pre-division convention the
// original's own header comment documents (CubemapIBL.cpp:544-551): the
// PI cancellation from the cosine-weighted-importance-sampling estimator
// is already folded into the `(1/N)` normalization below, so a
// downstream Lambertian consumer multiplies this cubemap's own sampled
// value by `albedo` alone, no extra `/PI`. This is the analytic ground
// truth this task's tests key on: convolving a spatially UNIFORM
// environment of radiance L0 with ANY normalized weighting (this
// includes cosine-importance-sampling, since the per-sample weights are
// implicitly uniform for a uniform L) yields exactly L0, independent of
// sample count -- verified in this port's own test suite
// (test_ibl_analytic_gpu.cpp).
//
// DELIBERATE SIMPLIFICATION vs the CPU original (documented, not a missed
// formula -- see equirect_to_cubemap.slang's own header comment for the
// same class of simplification applied there): the CPU original reads
// from a PRE-DOWNSAMPLED mip chain (`levels`, CubemapIBL.h's own
// `Slice<const Cubemap> levels` parameter) to bias each importance sample
// toward a coarser LOD for noise reduction ("filtered importance
// sampling", Krivanek/Colbert -- a variance-reduction/perf optimization
// layered on TOP of the core unbiased Monte-Carlo estimator, not a
// correctness requirement of the estimator itself, per the CPU source's
// own header comment at CubemapIBL.cpp:497-552). This port reads every
// sample from the SOURCE cubemap's own single mip 0 via hardware
// TRILINEAR-filtered (seamless cross-face) TextureCube sampling instead
// -- still a convergent, unbiased cosine-weighted estimator of the same
// integral, just noisier at a fixed sample count; offset here by a
// generous sample budget since this bake runs once at load time, never
// per-frame.

struct PushConstants {
    uint faceIndex;
    uint faceDim;
    uint numSamples;
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

// Duplicated from equirect_to_cubemap.slang -- this codebase's own
// established per-file-duplicated-helper idiom for small standalone
// compute kernels (see e.g. test_standard_pbr_energy_compensation_gpu.cpp's
// own header comment on why its synthetic kernel duplicates
// hammersley()/hemisphereImportanceSampleDggx() rather than importing a
// shared module: rx::shader::Compiler's compileFromFile() has no
// configured cross-file Slang import search path in this codebase, so
// keeping each standalone kernel single-file avoids a fragile,
// unverified import-resolution dependency).
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

// [utilities.h, standard low-discrepancy radical-inverse sequence -- same
// formula this codebase's own test_standard_pbr_energy_compensation_gpu.cpp
// already duplicates for the identical reason.]
float2 hammersley(uint i, float iN) {
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float2(float(i) * iN, float(bits) * (1.0 / 4294967296.0));
}

// [CubemapIBL.cpp:59-65] hemisphereCosSample(), ported exactly.
float3 hemisphereCosSample(float2 u) {
    float phi = 2.0 * kPi * u.x;
    float cosTheta2 = 1.0 - u.y;
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

    // [CubemapIBL.cpp:616-620] Tangent basis centered on N -- "handle case
    // of normal close to up" ported exactly.
    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangentX = normalize(cross(up, N));
    float3 tangentY = cross(N, tangentX);

    float3 accum = float3(0.0, 0.0, 0.0);
    float invN = 1.0 / float(gPush.numSamples);
    for (uint i = 0; i < gPush.numSamples; i++) {
        float2 xi = hammersley(i, invN);
        float3 ls = hemisphereCosSample(xi);
        float3 dir = tangentX * ls.x + tangentY * ls.y + N * ls.z;
        accum += gSourceCube.SampleLevel(gSourceSampler, dir, 0.0).rgb;
    }
    gOutFace[id.xy] = float4(accum * invN, 1.0);
}
