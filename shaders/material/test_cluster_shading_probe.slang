// shaders/material/test_cluster_shading_probe.slang -- Phase 5 Stage 2
// Task 15 [#51] TEST-ONLY fixture, NOT part of the production material
// set (nothing in material_system.cpp/MaterialSystem ever loads this
// file; it is compiled directly by src/rx_material/tests/
// test_cluster_shading_gpu.cpp via a plain rx::shader::Compiler, the SAME
// mechanism rx_cluster's own compute kernels use -- shaders/cluster/
// froxel_light_count.slang's own `import froxel_common;` resolves the
// identical way: rx::shader::Compiler configures no EXPLICIT session
// search path at all, relying on Slang's own "resolve a bare `import X;`
// relative to the loaded file's own directory" fallback). Lives here,
// alongside material.slang/cluster_lighting.slang (rather than
// src/rx_material/tests/data/, where this suite's other test fixtures --
// test_unlit.slang/test_textured.slang, loaded through MaterialSystem's
// OWN explicitly-search-pathed session instead -- live), specifically so
// THIS probe's own `import material;`/`import cluster_lighting;` resolve
// without needing either mechanism changed.
//
// A single compute entry point dispatching ONE thread per test position
// (`gTestPositions[id.x]`), calling the REAL PRODUCTION
// `rx_evaluateClusteredLights()` (cluster_lighting.slang) with a hand-fed
// `MaterialVertex` -- this is the exact function standard_pbr.slang's own
// evaluate() calls, exercised directly rather than through a full
// rasterized triangle, to isolate the shading-side per-pixel froxel
// lookup from BRDF/rasterization concerns already covered by
// test_standard_pbr_punctual_gpu.cpp's own single-slot punctual tests.
import material;
import cluster_lighting;

struct ProbeParams {
    uint clusterCountX;
    uint clusterCountY;
    uint clusterCountZ;
    float clusterTanHalfFovY;
    float clusterAspectRatio;
    float clusterZLightFar;
    float clusterInvLinearizer;
    uint clusterOffsetsBufferIndex;
    uint clusterWriteCountsBufferIndex;
    uint clusterLightIndicesBufferIndex;
    uint clusterLightsBufferIndex;
};

// set 1 (this probe's own dedicated set, alongside material.slang's
// external set-0 bindless substitution) -- test-owned I/O, never part of
// the production RxDrawData/gDrawData addressing. `gProbe` is a bound
// UNIFORM BUFFER here (NOT `[[vk::push_constant]]`): material.slang's own
// `import` unconditionally brings in its OWN push-constant global
// (`gMaterialGlobals`, `RxMaterialGlobals`), and Vulkan permits at most
// ONE push-constant range per shader stage -- a second `[[vk::push_
// constant]]` block in this SAME compiled program is a real pipeline-
// layout validation error (VUID-VkPipelineLayoutCreateInfo-
// pPushConstantRanges-00292, reproduced directly while writing this
// file), not merely redundant.
[[vk::binding(0, 1)]]
ConstantBuffer<ProbeParams> gProbe;

[[vk::binding(1, 1)]]
StructuredBuffer<float3> gTestPositions;

[[vk::binding(2, 1)]]
StructuredBuffer<float4x4> gClusterView;

[[vk::binding(3, 1)]]
RWStructuredBuffer<float3> gOutDiffuse;

[[vk::binding(4, 1)]]
RWStructuredBuffer<float3> gOutSpecular;

// Fixed BRDF/geometry inputs shared by every test position -- this probe
// isolates the LIGHT-SELECTION mechanism (already the point of this
// file), not the BRDF math itself (test_standard_pbr_punctual_gpu.cpp's
// own job) -- head-on view/normal, neutral dielectric F0, fully rough
// white diffuse.
static const float3 kN = float3(0.0, 0.0, 1.0);
static const float3 kViewDir = float3(0.0, 0.0, 1.0);
static const float kNdotV = 1.0;
static const float3 kF0 = float3(0.04, 0.04, 0.04);
static const float kF90 = 1.0;
static const float3 kDiffuseColor = float3(1.0, 1.0, 1.0);
static const float kRoughness = 1.0;
static const float kAlpha = 1.0;  // roughness^2 at roughness=1.

[shader("compute")]
[numthreads(1, 1, 1)]
void csProbe(uint3 id : SV_DispatchThreadID) {
    MaterialVertex v;
    v.worldPos = gTestPositions[id.x];
    v.normal = kN;
    v.tangent = float4(1.0, 0.0, 0.0, 1.0);
    v.uv = float2(0.0, 0.0);
    v.lightDirWorld = float3(0.0, 1.0, 0.0);
    v.lightColor = float3(0.0, 0.0, 0.0);
    v.ambientColor = float3(0.0, 0.0, 0.0);
    v.envIrradianceCubeIndex = 0xFFFFFFFFu;
    v.envPrefilteredCubeIndex = 0xFFFFFFFFu;
    v.envDfgLutIndex = 0xFFFFFFFFu;
    v.envCubeSamplerIndex = 0;
    v.envDfgSamplerIndex = 0;
    v.envMaxPrefilteredLod = 0.0;
    v.envIntensity = 0.0;
    v.lightType = 0;
    v.lightRange = 0.0;
    v.lightAngleScale = 0.0;
    v.lightAngleOffset = 0.0;
    v.lightPositionWorld = float3(0.0, 0.0, 0.0);
    v.lightSpotDirWorld = float3(0.0, 0.0, -1.0);
    v.cameraPosWorld = float3(0.0, 0.0, 0.0);
    v.clusterEnabled = 1;
    v.clusterCountX = gProbe.clusterCountX;
    v.clusterCountY = gProbe.clusterCountY;
    v.clusterCountZ = gProbe.clusterCountZ;
    v.clusterTanHalfFovY = gProbe.clusterTanHalfFovY;
    v.clusterAspectRatio = gProbe.clusterAspectRatio;
    v.clusterZLightFar = gProbe.clusterZLightFar;
    v.clusterInvLinearizer = gProbe.clusterInvLinearizer;
    v.clusterOffsetsBufferIndex = gProbe.clusterOffsetsBufferIndex;
    v.clusterWriteCountsBufferIndex = gProbe.clusterWriteCountsBufferIndex;
    v.clusterLightIndicesBufferIndex = gProbe.clusterLightIndicesBufferIndex;
    v.clusterLightsBufferIndex = gProbe.clusterLightsBufferIndex;
    v.clusterView = gClusterView[0];

    float3 diffuse;
    float3 specular;
    rx_evaluateClusteredLights(v, kN, kViewDir, kNdotV, kF0, kF90, kDiffuseColor, kRoughness, kAlpha, diffuse,
                                specular);
    gOutDiffuse[id.x] = diffuse;
    gOutSpecular[id.x] = specular;
}
