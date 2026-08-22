// shaders/cluster/froxel_light_count.slang -- Phase 5 Stage 2 Task 14
// [#50], PASS 1 of the count/prefix-sum/scatter clustered light-assignment
// chain [gate ruling: "light lists via two-pass count+prefix-sum+scatter
// (NO 256-light bitset cap)"]. This is RendererX's OWN compute-shader
// EXPRESSION of Filament's froxelization ALGORITHM (gate matrix Open
// Question #1's licensed scope: Filament's own froxelizer is CPU-side, no
// GLSL compute source exists to translate literally -- see
// froxel_common.slang's own top comment for the per-formula port
// citations this file builds on).
//
// ONE THREAD PER FROXEL (1D dispatch over grid.froxelCount()) -- each
// thread independently loops over EVERY light and counts how many
// intersect ITS OWN froxel, writing exactly one uint. No cross-thread
// communication, no atomics: this is what makes the whole 3-pass chain
// deterministic BY CONSTRUCTION (see froxel_light_scatter.slang's own top
// comment for why this matters for the determinism acceptance criterion).
import froxel_common;

[[vk::push_constant]]
ConstantBuffer<FroxelGridParamsGpu> gGrid;

// set 1, binding 0: the frame's flat point/spot light list (view-space
// already-transformed -- rx::cluster::buildClusterLightList()'s own
// output), read-only.
[[vk::binding(0, 1)]]
StructuredBuffer<ClusterLightGpu> gLights;

// set 1, binding 1: gTrueCounts[froxelIndex] = the RAW (uncapped)
// intersecting-light count for that froxel -- pass 2 applies BOTH capacity
// axes (per-froxel maxLightsPerFroxel, global maxTotalLightIndices)
// against this true value, so the true count itself is never lost (needed
// to report an EXACT overflow amount, per the plan's own "counters exact
// and CI-gateable" acceptance criterion).
[[vk::binding(1, 1)]]
RWStructuredBuffer<uint> gTrueCounts;

[shader("compute")]
[numthreads(64, 1, 1)]
void csCount(uint3 id : SV_DispatchThreadID) {
    uint idx = id.x;
    uint total = froxelCountTotal(gGrid);
    if (idx >= total) {
        return;
    }

    uint3 coords = froxelCoordsFromIndex(idx, gGrid);
    FroxelBoundsGpu bounds = froxelViewSpaceBounds(coords.x, coords.y, coords.z, gGrid);

    uint count = 0;
    for (uint i = 0; i < gGrid.totalLightCount; ++i) {
        if (lightIntersectsFroxel(gLights[i], bounds)) {
            count = count + 1;
        }
    }
    gTrueCounts[idx] = count;
}
