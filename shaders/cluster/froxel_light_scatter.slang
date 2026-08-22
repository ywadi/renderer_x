// shaders/cluster/froxel_light_scatter.slang -- Phase 5 Stage 2 Task 14
// [#50], PASS 3 (final) of the count/prefix-sum/scatter chain.
//
// DETERMINISM BY CONSTRUCTION [matrix's own named acceptance criterion:
// "dispatch the SAME light set + camera... TWICE... assert the per-froxel
// light-list buffer readback is BYTE-IDENTICAL both times"; explicitly
// flags "a naive atomic-counter-append implementation... is the design
// most likely to accidentally FAIL this test"]: this kernel uses **zero
// atomics**. Each thread owns exactly one froxel's own [offset,
// offset+writeCount) slice of gLightIndices (gOffsets/gWriteCounts, pass
// 2's own output, already partitions the buffer into disjoint per-froxel
// ranges with no overlap) and fills it by re-running the IDENTICAL
// light-index-ORDER loop pass 1 used to COUNT -- same thread, same
// sequential order, same lightIntersectsFroxel() test (imported from the
// SAME froxel_common.slang pass 1 uses, not a re-derived copy) -- so a
// given (grid, lights) input always produces the exact same output, on
// any run, on any dispatch/workgroup-size configuration, with no
// possibility of a race (no two threads ever write the same buffer
// element, and no thread's own local `written` counter depends on any
// other thread's progress).
import froxel_common;

[[vk::push_constant]]
ConstantBuffer<FroxelGridParamsGpu> gGrid;

[[vk::binding(0, 1)]]
StructuredBuffer<ClusterLightGpu> gLights;

[[vk::binding(1, 1)]]
StructuredBuffer<uint> gOffsets;

[[vk::binding(2, 1)]]
StructuredBuffer<uint> gWriteCounts;

// The global light-index buffer: gLightIndices[gOffsets[f] .. gOffsets[f]+gWriteCounts[f])
// holds froxel f's own light indices (into gLights), in ascending
// light-index order -- froxel f's [offset,count) pair (gOffsets[f],
// gWriteCounts[f]) IS its complete, standalone light list descriptor.
[[vk::binding(3, 1)]]
RWStructuredBuffer<uint> gLightIndices;

[shader("compute")]
[numthreads(64, 1, 1)]
void csScatter(uint3 id : SV_DispatchThreadID) {
    uint idx = id.x;
    uint total = froxelCountTotal(gGrid);
    if (idx >= total) {
        return;
    }

    uint3 coords = froxelCoordsFromIndex(idx, gGrid);
    FroxelBoundsGpu bounds = froxelViewSpaceBounds(coords.x, coords.y, coords.z, gGrid);

    uint offset = gOffsets[idx];
    uint capacity = gWriteCounts[idx];
    uint written = 0;
    for (uint i = 0; i < gGrid.totalLightCount && written < capacity; ++i) {
        if (lightIntersectsFroxel(gLights[i], bounds)) {
            gLightIndices[offset + written] = i;
            written = written + 1;
        }
    }
}
