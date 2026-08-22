// shaders/cluster/froxel_common.slang -- Phase 5 Stage 2 Task 14 [#50]:
// the GPU (Slang) mirror of rx_scene/froxel_grid.h's device-free camera-
// frustum froxel grid math. Every formula below is the BIT-FOR-BIT twin of
// its C++ counterpart in src/rx_scene/froxel_grid.cpp (which itself cites
// google/filament v1.75.0, commit 0e58877c09afb1aacd09ff640f74d2adcd2a7e80,
// Froxelizer.{h,cpp} + Intersections.h per-function -- see that file's own
// citations; not repeated here to avoid drift between two copies of the
// same citation text). Kept in sync MANUALLY -- this toolchain has no
// shared C++/Slang codegen -- which is exactly why rx_scene/tests/
// froxel_grid_test.cpp's device-free oracle and this file's own GPU
// membership tests (rx_cluster's test suite) cross-check the SAME grid
// parameters against BOTH derivations and assert byte/value equality, not
// mere resemblance [matrix row 1's own acceptance text: "the SAME formula
// ... matches the fragment-shader-side ... formula bit-for-bit in
// derivation, not just close"].
//
// COORDINATE CONVENTION: view space, camera looks down -Z (see froxel_grid.h's
// own top comment) -- identical here.

// GPU mirror of rx::scene::froxel::FroxelGridParams (froxel_grid.h). Field
// order/types chosen for a natural, unambiguous [[vk::push_constant]]
// layout (all 4-byte scalars, no vec3 padding ambiguity).
public struct FroxelGridParamsGpu {
    public uint countX;
    public uint countY;
    public uint countZ;
    public uint totalLightCount;
    public float tanHalfFovY;
    public float aspectRatio;
    public float zLightNear;
    public float zLightFar;
    public float linearizer;
    public float invLinearizer;
    public uint maxLightsPerFroxel;
    public uint maxTotalLightIndices;
};

public uint froxelCountTotal(FroxelGridParamsGpu grid) {
    return grid.countX * grid.countY * grid.countZ;
}

// Port of rx::scene::froxel::froxelCoords() [froxel_grid.cpp].
public uint3 froxelCoordsFromIndex(uint index, FroxelGridParamsGpu grid) {
    uint plane = grid.countX * grid.countY;
    uint z = plane != 0 ? index / plane : 0;
    uint rem = plane != 0 ? index % plane : 0;
    uint y = grid.countX != 0 ? rem / grid.countX : 0;
    uint x = grid.countX != 0 ? rem % grid.countX : 0;
    return uint3(x, y, z);
}

// Port of rx::scene::froxel::sliceZDistance() [froxel_grid.cpp -- see that
// function's own derivation comment for why i==1 resolves to EXACTLY
// zLightNear and i==countZ to EXACTLY zLightFar].
public float sliceZDistance(uint i, FroxelGridParamsGpu grid) {
    if (i == 0) {
        return 0.0;
    }
    float n = float(grid.countZ);
    float fi = float(i);
    return grid.zLightFar * exp2((fi - n) * grid.linearizer);
}

// Port of rx::scene::froxel::findSliceZ() [froxel_grid.cpp, itself a
// bit-for-bit port of Froxelizer::findSliceZ(), Froxelizer.cpp:580-598].
// Not used by this task's own compute kernels directly (they iterate every
// froxel by linear index, never needing to map a light's own view-space Z
// to a slice), but kept here so a FUTURE consumer (T15's shading pass,
// mapping a shaded fragment's own view-space Z to its froxel) uses the
// SAME derivation this file's own froxel-bounds math is built on, and so
// this task's own GPU test can dispatch this EXACT function and compare
// against the C++ oracle for the "bit-for-bit" acceptance criterion.
public uint findSliceZ(float viewSpaceZ, FroxelGridParamsGpu grid) {
    int s = int(log2(-viewSpaceZ / grid.zLightFar) * grid.invLinearizer + float(grid.countZ));
    s = viewSpaceZ < 0.0 ? s : 0;
    s = clamp(s, 0, int(grid.countZ) - 1);
    return uint(s);
}

public struct FroxelBoundsGpu {
    public float3 center;
    public float radius;
};

// Port of rx::scene::froxel::froxelViewSpaceBounds() [froxel_grid.cpp] --
// SAME 8-corner-centroid/max-corner-distance shape as Filament's own
// updateBoundingSpheres() (Froxelizer.cpp:324-382), corners derived from
// this engine's symmetric-FOV frustum math (see froxel_grid.h's own top
// comment on why).
public FroxelBoundsGpu froxelViewSpaceBounds(uint x, uint y, uint z, FroxelGridParamsGpu grid) {
    float ndcX0 = -1.0 + 2.0 * float(x) / float(grid.countX);
    float ndcX1 = -1.0 + 2.0 * float(x + 1) / float(grid.countX);
    float ndcY0 = -1.0 + 2.0 * float(y) / float(grid.countY);
    float ndcY1 = -1.0 + 2.0 * float(y + 1) / float(grid.countY);

    float dNear = sliceZDistance(z, grid);
    float dFar = sliceZDistance(z + 1, grid);

    float tanHalfFovX = grid.tanHalfFovY * grid.aspectRatio;

    float3 corners[8];
    uint idx = 0;
    [unroll]
    for (uint ix = 0; ix < 2; ++ix) {
        float ndcX = (ix == 0) ? ndcX0 : ndcX1;
        [unroll]
        for (uint iy = 0; iy < 2; ++iy) {
            float ndcY = (iy == 0) ? ndcY0 : ndcY1;
            [unroll]
            for (uint id = 0; id < 2; ++id) {
                float d = (id == 0) ? dNear : dFar;
                corners[idx] = float3(ndcX * d * tanHalfFovX, ndcY * d * grid.tanHalfFovY, -d);
                idx = idx + 1;
            }
        }
    }

    float3 center = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        center += corners[i];
    }
    center *= (1.0 / 8.0);

    float maxDist2 = 0.0;
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        float3 delta = corners[i] - center;
        maxDist2 = max(maxDist2, dot(delta, delta));
    }

    FroxelBoundsGpu result;
    result.center = center;
    result.radius = sqrt(maxDist2);
    return result;
}

// Port of rx::scene::froxel::sphereIntersectsFroxel() [froxel_grid.cpp].
public bool sphereIntersectsFroxel(float3 lightViewPos, float lightRadius, FroxelBoundsGpu froxel) {
    float3 d = lightViewPos - froxel.center;
    float rr = lightRadius + froxel.radius;
    return dot(d, d) <= rr * rr;
}

// Port of rx::scene::froxel::spotIntersectsFroxel() [froxel_grid.cpp,
// itself porting Intersections.h:50-62's sphereConeIntersectionFast()
// verbatim, FROXEL as the tested "sphere", LIGHT as the cone origin/axis].
public bool spotIntersectsFroxel(float3 lightViewPos, float lightRadius, float3 coneAxis, float coneSinInverse,
                                  float coneCosSquared, FroxelBoundsGpu froxel) {
    if (!sphereIntersectsFroxel(lightViewPos, lightRadius, froxel)) {
        return false;
    }
    float3 u = lightViewPos - (froxel.radius * coneSinInverse) * coneAxis;
    float3 d = froxel.center - u;
    float e = dot(coneAxis, d);
    float dd = dot(d, d);
    return (e * e >= dd * coneCosSquared) && (e > 0.0);
}

// One point/spot light's view-space-transformed data for froxel
// assignment -- GPU mirror of rx::cluster::ClusterLightGpu
// (rx_cluster/cluster_lighting.h, kept manually in sync). Every field is a
// float4 (never a bare float3) so std430-style StructuredBuffer packing
// needs no implicit padding reasoning on either the Slang or the C++ side
// -- both lay this struct out identically (three consecutive float4s, 48
// bytes) by construction.
public struct ClusterLightGpu {
    // xyz = view-space position, w = cull radius.
    public float4 viewPositionRadius;
    // xyz = spot cone axis in view space (unused, but always finite, for a
    // point light), w = coneSinInverse (spot only).
    public float4 viewAxisSinInverse;
    // x = coneCosSquared (spot only), y = isSpot (1.0 spot / 0.0 point), z/w unused.
    public float4 cosSquaredFlags;
};

// The SAME test either count or scatter kernel needs, factored once so
// both stay byte-for-byte identical to each other (a prerequisite for this
// task's own two-pass count-then-scatter determinism, since count decides
// CAPACITY and scatter decides WHICH lights fill it -- any divergence
// between the two tests would corrupt that contract).
public bool lightIntersectsFroxel(ClusterLightGpu light, FroxelBoundsGpu froxel) {
    float3 pos = light.viewPositionRadius.xyz;
    float radius = light.viewPositionRadius.w;
    if (light.cosSquaredFlags.y > 0.5) {
        return spotIntersectsFroxel(pos, radius, light.viewAxisSinInverse.xyz, light.viewAxisSinInverse.w,
                                     light.cosSquaredFlags.x, froxel);
    }
    return sphereIntersectsFroxel(pos, radius, froxel);
}
