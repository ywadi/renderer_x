// shaders/ibl/equirect_to_cubemap.slang -- Phase 5 Stage 1 Task 9 [#45,
// gate rulings T9/RC1]: equirectangular HDR environment -> cubemap
// projection, the first stage of the IBL bake chain. One compute pass per
// destination FACE -- rx_graph's rx::rhi::StorageImage exposes no writable
// 2D-ARRAY view spanning all 6 faces of a cube image at once (a full
// 6-layer subresource request on a cube-flagged image always resolves to
// a VK_IMAGE_VIEW_TYPE_CUBE view, per storage_image.h's own
// viewForSubresource() doc comment), and Slang/HLSL has no RWTextureCube
// write-target type at all -- so per-face 2D dispatch is this codebase's
// only available shape for a cube compute WRITE, confirmed directly
// against Task 2's own GPU test (rx_graph/tests/test_compute_gpu.cpp's
// "two compute passes writing DISJOINT faces of a cube storage image"
// case), not merely assumed.
//
// PORT PROVENANCE -- ported from google/filament v1.75.0 (tag; commit
// 0e58877c09afb1aacd09ff640f74d2adcd2a7e80, Apache-2.0):
//   - libs/ibl/include/ibl/Cubemap.h, Cubemap::getDirectionFor()
//     (:160-180) -- getDirectionForFace() below is this formula
//     re-derived byte-for-byte (x/y -> [-1,1] via `cx = u*2-1`,
//     `cy = 1-v*2`, then a per-face switch). Independently cross-checked
//     (this task's own report) against the standard Vulkan/OpenGL
//     hardware cube-face convention (VK_IMAGE_VIEW_TYPE_CUBE's own layer
//     order, +X=0,-X=1,+Y=2,-Y=3,+Z=4,-Z=5) and found algebraically
//     IDENTICAL -- Filament's CPU convention already matches hardware
//     cube addressing, which is why this port can safely feed a real
//     TextureCube sampled read downstream (irradiance_convolve.slang /
//     prefilter_specular.slang) without a face-order mismatch. A
//     dedicated GPU test (test_ibl_cube_face_convention_gpu.cpp) proves
//     this directly: writes 6 distinct known colors through this exact
//     per-face-index scheme, then samples the resulting image as a real
//     hardware TextureCube at the 6 canonical axis directions and checks
//     each returns the texel written to the correspondingly-INDEXED face.
//   - libs/ibl/src/CubemapUtils.cpp, CubemapUtils::equirectangularToCubemap()
//     (:182-238) -- directionToEquirectUv() below is toRectilinear()
//     (:186-192) ported exactly (`atan2(x,z)/PI`, `asin(y)*2/PI`, remapped
//     to [0,1] UV). DELIBERATE SIMPLIFICATION vs the CPU original: the
//     original supersamples each destination texel with a Hammersley-
//     jittered box filter over its own footprint in equirect pixel space
//     (CubemapUtils.cpp:196-233), because ITS only read primitive is a
//     hand-indexed nearest-neighbour CPU buffer fetch (Cubemap::sampleAt()
//     there has no hardware filtering to lean on at all). This GPU port
//     instead takes ONE bilinear-filtered hardware sample per destination
//     texel (VkSampler linear filtering already anti-aliases the read
//     cheaply) -- a documented simplification, not a missed formula; the
//     underlying direction<->equirect-UV mapping itself is ported exactly.

struct PushConstants {
    uint faceIndex;  // 0=+X,1=-X,2=+Y,3=-Y,4=+Z,5=-Z (Vulkan/Filament-standard cube layer order)
    uint faceDim;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 1)]]
Texture2D<float4> gEquirect;

[[vk::binding(1, 1)]]
SamplerState gEquirectSampler;

[[vk::binding(2, 1)]]
RWTexture2D<float4> gOutFace;

static const float kPi = 3.14159265358979323846;

// [Cubemap.h:160-180] `u`/`v` are texel-center-normalized [0,1] (the
// caller applies the "+0.5" Cubemap::center() convention before calling).
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

// [CubemapUtils.cpp:186-192] toRectilinear(), ported exactly (the
// `(width-1)`/`(height-1)` pixel-index scaling in the CPU original is
// specific to its own hand-indexed buffer fetch -- this port's hardware
// sampler takes normalized [0,1] UV directly, so that scaling has no
// analog here; the direction<->longitude/latitude mapping itself is
// unchanged).
float2 directionToEquirectUv(float3 s) {
    float xf = atan2(s.x, s.z) * (1.0 / kPi);
    float yf = asin(clamp(s.y, -1.0, 1.0)) * (2.0 / kPi);
    float u = (xf + 1.0) * 0.5;
    float v = (1.0 - yf) * 0.5;
    return float2(u, v);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id: SV_DispatchThreadID) {
    if (id.x >= gPush.faceDim || id.y >= gPush.faceDim) {
        return;
    }
    float u = (float(id.x) + 0.5) / float(gPush.faceDim);
    float v = (float(id.y) + 0.5) / float(gPush.faceDim);
    float3 dir = getDirectionForFace(gPush.faceIndex, u, v);
    float2 eq = directionToEquirectUv(dir);
    float4 color = gEquirect.SampleLevel(gEquirectSampler, eq, 0.0);
    gOutFace[id.xy] = float4(color.rgb, 1.0);
}
