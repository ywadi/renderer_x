// shaders/material/cluster_lighting.slang -- Phase 5 Stage 2 Task 15 [#51,
// gate rulings-2026-08-20.md's RC5 + T15's own matrix rows]: the
// shading-side per-pixel froxel lookup that turns T14's own compute-
// produced per-froxel light lists (rx_cluster, shaders/cluster/*.slang)
// into an ADDITIVE clustered Point/Spot lighting term. standard_pbr.slang's
// evaluate() calls `rx_evaluateClusteredLights()` below ALONGSIDE its own
// existing single-slot punctual term (RxDrawData::lightType/
// lightPositionWorld/...) -- that term is left COMPLETELY UNTOUCHED (see
// this file's own "ADDITIVE, NEVER REPLACING" note below and
// standard_pbr.slang's own header comment): a scene with only a
// directional light (clusterEnabled == 0, the default) reproduces
// byte-identical pre-Task-15 output.
//
// PER-PIXEL COST SCALES WITH LIGHTS-IN-FROXEL, NEVER LIGHTS-IN-SCENE
// [CLAUDE.md's own binding performance-first rule -- "per-pixel cost must
// scale with lights-in-froxel, not lights-in-scene"]: the loop below
// iterates EXACTLY `gClusterBuffers[...][froxelIndex]`'s own [offset,
// offset+count) range -- T14's own compute-produced, per-froxel-capped
// light-index slice -- never the scene's full light list. This is the
// entire point of clustering: `standard_pbr.slang`'s existing single-slot
// term already proves the ATTENUATION FORMULAS are correct (T13, #49);
// this file's own job is PURELY the per-pixel light-SELECTION mechanism.
//
// FROXEL-INDEX DERIVATION -- ported from shaders/cluster/froxel_common.slang
// (rx_cluster's own Slang mirror of rx::scene::froxel, itself citing
// google/filament v1.75.0's Froxelizer.{h,cpp}, commit
// 0e58877c09afb1aacd09ff640f74d2adcd2a7e80 -- see that file's own top
// comment for the full port-provenance chain, cited once there rather than
// repeated here). Cross-directory `import` is not available here (rx_material's
// own Slang session is built with search path shaders/material/ ONLY --
// material_system.cpp's own createMaterialSession(), verified directly
// before writing this file), so the two pieces of math this lookup needs
// (findSliceZ's own exponential Z-slicing formula; the XY-from-view-space-
// position derivation, the algebraic INVERSE of froxel_common.slang's own
// froxelViewSpaceBounds() corner formula) are DUPLICATED here, following
// the SAME "kept in sync manually, cross-checked bit-for-bit by tests"
// convention froxel_common.slang's own top comment already establishes for
// its C++/Slang pair -- src/rx_material/tests/test_cluster_shading_gpu.cpp
// cross-checks this copy's froxel-index output against
// rx::scene::froxel::findSliceZ()'s own C++ oracle for the SAME grid/
// position inputs (the "bit-for-bit, not just close" acceptance
// discipline matrix-p5t14 established).
import material;
import brdf;

// GPU mirror of rx::cluster::ClusterLightGpu (rx_cluster/cluster_lighting.h)
// -- SEVEN consecutive float4s (112 bytes), kept manually in sync with
// BOTH that C++ struct AND shaders/cluster/froxel_common.slang's own
// identically-shaped mirror (three independent copies of the SAME layout,
// per this toolchain's own "no shared C++/Slang codegen" standing
// constraint -- froxel_common.slang's own top comment). Only the
// SHADING-side four float4s (the tail) are actually read below; the three
// CULLING float4s at the head are declared (never renamed/reordered) so
// this struct's own byte OFFSETS match the other two copies exactly, even
// though this file's own code never reads them.
struct ClusterLightGpu {
    float4 viewPositionRadius;
    float4 viewAxisSinInverse;
    float4 cosSquaredFlags;
    // x=lightType [1=Point,2=Spot], y=lightRange, z=lightAngleScale, w=lightAngleOffset.
    float4 shadingTypeRangeAngle;
    float4 shadingPositionWorld;  // xyz = world-space position.
    float4 shadingColorCandela;   // rgb = color*intensity, candela.
    float4 shadingSpotDirWorld;   // xyz = world-space spot facing direction.
};

// set 0, binding 5 -- generic uint[] bindless array
// (rx::rhi::BindlessTable::kGenericStorageBufferBinding). Serves T14's own
// `clusterOffsets`/`clusterWriteCounts`/`clusterLightIndices` render-graph
// buffers -- `MaterialVertex::cluster{Offsets,WriteCounts,LightIndices}
// BufferIndex` selects WHICH of the three this row's own producer
// registered at, per-buffer (three independent bindless indices into this
// ONE array, matching BindlessTable's own "one binding per distinct
// element TYPE, many independently-addressed logical buffers within it"
// idiom `gDrawData`/`gTextures` already establish).
[[vk::binding(5, 0)]]
StructuredBuffer<uint> gClusterBuffers[];

// set 0, binding 6 -- ClusterLightGpu[] bindless array
// (rx::rhi::BindlessTable::kClusterLightBufferBinding).
[[vk::binding(6, 0)]]
StructuredBuffer<ClusterLightGpu> gClusterLights[];

// --- Froxel-index derivation [ported/duplicated, see this file's own top
// comment] ------------------------------------------------------------

// Port of rx::scene::froxel::findSliceZ() / shaders/cluster/
// froxel_common.slang's own findSliceZ() -- BIT-FOR-BIT identical formula
// (Froxelizer.cpp:580-598, google/filament v1.75.0).
int clusterFindSliceZ(float viewSpaceZ, MaterialVertex v) {
    int s = int(log2(-viewSpaceZ / v.clusterZLightFar) * v.clusterInvLinearizer + float(v.clusterCountZ));
    s = viewSpaceZ < 0.0 ? s : 0;
    s = clamp(s, 0, int(v.clusterCountZ) - 1);
    return s;
}

// Inverse of shaders/cluster/froxel_common.slang's own
// froxelViewSpaceBounds() corner formula (`corners[idx] = float3(ndcX*d*
// tanHalfFovX, ndcY*d*tanHalfFovY, -d)`, d = -viewSpaceZ): given a
// view-space position, recovers its NDC.xy, then the [0,countX)/[0,countY)
// froxel column/row -- the SAME symmetric-perspective frustum this
// engine's froxel grid is built against (froxel_grid.h's own
// "SYMMETRIC-PERSPECTIVE SIMPLIFICATION" comment, D13: `rx::scene::Camera`
// is a plain symmetric perspective by construction).
uint3 clusterFroxelCoords(float3 viewPos, MaterialVertex v) {
    float d = max(-viewPos.z, 1e-4);
    float tanHalfFovX = v.clusterTanHalfFovY * v.clusterAspectRatio;
    float ndcX = viewPos.x / (d * tanHalfFovX);
    float ndcY = viewPos.y / (d * v.clusterTanHalfFovY);
    uint x = uint(clamp(int((ndcX * 0.5 + 0.5) * float(v.clusterCountX)), 0, int(v.clusterCountX) - 1));
    uint y = uint(clamp(int((ndcY * 0.5 + 0.5) * float(v.clusterCountY)), 0, int(v.clusterCountY) - 1));
    uint z = uint(clusterFindSliceZ(viewPos.z, v));
    return uint3(x, y, z);
}

// Public ONLY for this file's own GPU cross-check test (test_cluster_
// shading_gpu.cpp probes this exact function's output against the C++
// oracle) -- standard_pbr.slang itself never calls this directly, only
// rx_evaluateClusteredLights() below.
public uint clusterFroxelIndex(float3 worldPos, MaterialVertex v) {
    float3 viewPos = mul(v.clusterView, float4(worldPos, 1.0)).xyz;
    uint3 coords = clusterFroxelCoords(viewPos, v);
    uint plane = v.clusterCountX * v.clusterCountY;
    return coords.z * plane + coords.y * v.clusterCountX + coords.x;
}

// --- Per-fragment clustered Point/Spot accumulation --------------------
//
// ADDITIVE, NEVER REPLACING [matrix-p5t15's own row 1: "Directional light
// stays DIRECT (never froxel-clustered)"; standard_pbr.slang's own
// single-slot term stays byte-identical]: returns RAW (pre-energy-
// -compensation) diffuse/specular sums via `outDiffuse`/`outSpecular` --
// energy compensation is a PURE multiplicative scalar depending only on
// (F0, dfgY) (brdf.slang's own `energyCompensation()`/`EnergyCompensationOn
// ::apply()`: `specular * energyCompensation(f0, dfgY)`, no dependence on
// `specular`'s own per-light scale), so `sum(apply(spec_i)) ==
// apply(sum(spec_i))` EXACTLY -- standard_pbr.slang applies it ONCE, to
// this function's own accumulated total, rather than this file needing the
// link-time-specialized `EnergyComp` type at all (which would require
// importing standard_pbr.slang here -- circular, since standard_pbr.slang
// itself imports this file).
//
// Attenuation formulas [KHR_lights_punctual, ported -- the SAME citations
// standard_pbr.slang's own single-slot term already documents in full
// (README.md's range-property/spot-cone reference code,
// rx_scene/light_math.h's own derivation) -- NOT re-derived here, restated
// only where the formula itself appears, per this project's own citation
// discipline].
public void rx_evaluateClusteredLights(MaterialVertex v, float3 N, float3 viewDir, float NdotV, float3 F0, float F90,
                                        float3 diffuseColor, float roughness, float alpha, out float3 outDiffuse,
                                        out float3 outSpecular) {
    outDiffuse = float3(0.0, 0.0, 0.0);
    outSpecular = float3(0.0, 0.0, 0.0);

    uint froxelIndex = clusterFroxelIndex(v.worldPos, v);
    uint offset = gClusterBuffers[v.clusterOffsetsBufferIndex][froxelIndex];
    uint count = gClusterBuffers[v.clusterWriteCountsBufferIndex][froxelIndex];

    for (uint i = 0; i < count; ++i) {
        uint lightIdx = gClusterBuffers[v.clusterLightIndicesBufferIndex][offset + i];
        ClusterLightGpu light = gClusterLights[v.clusterLightsBufferIndex][lightIdx];

        float3 lightPos = light.shadingPositionWorld.xyz;
        float3 toLight = lightPos - v.worldPos;
        float distSq = max(dot(toLight, toLight), 1e-8);
        float dist = sqrt(distSq);
        float3 lightDir = toLight / dist;

        float NdotL = max(dot(N, lightDir), 0.0);
        // [Filament precedent, cited by matrix-p5t15's own row 3:
        // surface_light_punctual.fs:180-225 -- "early-out on NoL<=0...
        // shadow lookups are the expensive step, gated last"] This engine
        // has no per-clustered-light shadow test yet (T18's own future
        // scope, cubemap/atlas point-and-spot shadows) -- the early-out
        // still saves the D/V/Fresnel evaluation below for a
        // back-facing-relative-to-this-light fragment, the SAME
        // performance shape the cited precedent establishes.
        if (NdotL <= 0.0) {
            continue;
        }

        // KHR_lights_punctual range-property window [README.md, quoted in
        // full by standard_pbr.slang's own comment] -- the LITERAL,
        // un-squared form (this project's own resolved Open Question,
        // matching the spec text/reference code/glTF-Sample-Renderer all
        // three).
        float lightRange = light.shadingTypeRangeAngle.y;
        float rangeWindow = 1.0;
        if (lightRange > 0.0) {
            float ratio = dist / lightRange;
            float ratio4 = ratio * ratio;
            ratio4 = ratio4 * ratio4;
            rangeWindow = saturate(1.0 - ratio4);
        }
        float lightAttenuation = rangeWindow / distSq;

        // x > 1.5 selects Spot (2.0) over Point (1.0) -- see
        // rx_cluster::buildClusterLightList()'s own `shadingTypeRangeAngle.x`
        // comment (cluster_lighting.h); Directional lights never reach this
        // buffer at all (buildClusterLightList() skips them, matrix-p5t15's
        // own row 1).
        if (light.shadingTypeRangeAngle.x > 1.5) {
            float angleScale = light.shadingTypeRangeAngle.z;
            float angleOffset = light.shadingTypeRangeAngle.w;
            float3 spotDir = light.shadingSpotDirWorld.xyz;
            float cd = dot(normalize(spotDir), -lightDir);
            float angularAttenuation = saturate(cd * angleScale + angleOffset);
            angularAttenuation *= angularAttenuation;
            lightAttenuation *= angularAttenuation;
        }

        float3 halfVec = normalize(viewDir + lightDir);
        float NdotH = max(dot(N, halfVec), 0.0);
        float VdotH = max(dot(viewDir, halfVec), 0.0);

        float D = D_GGX(alpha, NdotH);
        float vis = V_SmithGGXCorrelated(alpha, NdotV, NdotL);
        float3 fresnel = F_Schlick(F0, F90, VdotH);

        // [standard_pbr.slang's own punctual-term comment, restated]
        // `shadingColorCandela` is CANDELA -- multiplying by the
        // inverse-square/window/cone attenuation term converts it into the
        // SAME lux-equivalent-irradiance space the diffuse/specular BRDF
        // terms already assume, matching Filament's own `light.
        // colorIntensity.rgb * light.attenuation * NoL` composition.
        float3 radiance = light.shadingColorCandela.rgb * lightAttenuation * NdotL;

        outDiffuse += diffuseColor * Fd_Lambert() * radiance;
        outSpecular += D * vis * fresnel * radiance;
    }
}
