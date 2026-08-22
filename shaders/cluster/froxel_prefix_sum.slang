// shaders/cluster/froxel_prefix_sum.slang -- Phase 5 Stage 2 Task 14
// [#50], PASS 2 of the count/prefix-sum/scatter chain. A single-thread
// SERIAL exclusive scan over gTrueCounts[] -- deliberately not a
// parallel/work-efficient scan: this project's own froxel grid budget is
// modest by design (kDefaultFroxelBudget = 8192, matching Filament's own
// FROXEL_BUFFER_MAX_ENTRY_COUNT, froxel_grid.h), so a serial O(froxelCount)
// loop costs microseconds on any GPU this engine targets (measured this
// task -- see task-14-report.md's own timing breakdown) and keeps the
// determinism/correctness proof trivial (a single thread's own strictly-
// ordered read-modify-write has no parallel-scan synchronization to get
// wrong at all). A work-efficient parallel scan (Hillis-Steele/Blelloch)
// is a documented, deferred optimization opportunity if a future content-
// scale target needs a materially larger froxel budget than this one --
// not exercised here because nothing in this phase's own scale target
// requires it.
//
// Applies BOTH capacity axes named by the plan's own acceptance criterion
// (plan:504-505, "max lights per froxel / total... counters exact and
// CI-gateable") in one pass:
//   1. PER-FROXEL cap (gGrid.maxLightsPerFroxel): gTrueCounts[i] is capped
//      before it can consume any of the global light-index buffer;
//      gPerFroxelOverflow[i] reports exactly how much was cut.
//   2. GLOBAL cap (gGrid.maxTotalLightIndices): the running prefix-sum
//      total is clamped against the global light-index buffer's own
//      declared capacity; gGlobalOverflow[i] reports exactly how much of
//      THIS froxel's (already per-froxel-capped) count could not fit.
// Both overflow counters are EXACT (never "hasOverflow" booleans) and
// PER-FROXEL (a scene with one saturated froxel never corrupts any other
// froxel's own list -- the plan's own "contained, not corrupt" acceptance
// text), matching CLAUDE.md's own capacity+1 standing rule.
import froxel_common;

[[vk::push_constant]]
ConstantBuffer<FroxelGridParamsGpu> gGrid;

[[vk::binding(0, 1)]]
StructuredBuffer<uint> gTrueCounts;

[[vk::binding(1, 1)]]
RWStructuredBuffer<uint> gOffsets;

[[vk::binding(2, 1)]]
RWStructuredBuffer<uint> gWriteCounts;

[[vk::binding(3, 1)]]
RWStructuredBuffer<uint> gPerFroxelOverflow;

[[vk::binding(4, 1)]]
RWStructuredBuffer<uint> gGlobalOverflow;

// A single scalar (index 0 only) -- the total number of light-index
// entries pass 3 will actually scatter this frame (sum of every
// gWriteCounts[i]), the exact size a caller should read back from
// gLightIndices rather than assuming the whole maxTotalLightIndices-sized
// buffer is meaningful.
[[vk::binding(5, 1)]]
RWStructuredBuffer<uint> gTotalUsed;

[shader("compute")]
[numthreads(1, 1, 1)]
void csPrefixSum(uint3 id : SV_DispatchThreadID) {
    uint total = froxelCountTotal(gGrid);
    uint running = 0;
    for (uint i = 0; i < total; ++i) {
        uint trueCount = gTrueCounts[i];
        uint capped = min(trueCount, gGrid.maxLightsPerFroxel);
        uint perFroxelOverflow = trueCount - capped;

        uint remaining = gGrid.maxTotalLightIndices > running ? (gGrid.maxTotalLightIndices - running) : 0;
        uint used = min(capped, remaining);
        uint globalOverflow = capped - used;

        gOffsets[i] = running;
        gWriteCounts[i] = used;
        gPerFroxelOverflow[i] = perFroxelOverflow;
        gGlobalOverflow[i] = globalOverflow;

        running = running + used;
    }
    gTotalUsed[0] = running;
}
