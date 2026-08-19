### Task 19: DrawListBuilder — parallel culling + sort keys (D14/D15, D26, D27)

**Files:** Create `src/rx_scene/draw_list.{h,cpp}` + tests.
**Interfaces (amended 2026-08-18 per D26 — SoA, indirect-compatible, caller-owned storage):**
```cpp
// Geometry fields packed VkDrawIndexedIndirectCommand-compatible; per-draw payload in a
// parallel array (SoA). Grouped by GeometryPool blockId — the recorded per-block indirect
// submission bound (one future MDI call per block).
struct DrawCommand { uint32_t indexCount, instanceCount, firstIndex; int32_t vertexOffset; uint32_t firstInstance; };
struct DrawPayload { uint32_t materialIndex; uint32_t instanceDataIndex; };  // resolved from asset::MaterialHandle (T13 Registry, residency-tolerant per D24)
struct ViewLists { std::vector<DrawCommand> commands; std::vector<DrawPayload> payloads; std::vector<BlockRange> blocks; CullCounters counters; /* opaque FtB by u64 key pipeline|material|depth, then instancing-collapsed; blend BtF, uncollapsed */ };
class DrawListBuilder { // caller-owned reused storage: zero net allocations in steady state
  void build(const Scene&, const Camera&, task::Scheduler&, ViewLists& out);
  void buildShadow(const Scene&, const DirectionalLight&, const Camera&, task::Scheduler&, ShadowLists& out);
};
// Engine-provided chunked submit helper (parallel recording is the DEFAULT for scene-driven passes):
// rx::scene::recordDrawList(PassContext&, chunkIndex, chunkCount, const ViewLists&, ...) — sample 09 hand-chunks nothing.
```
Frustum cull: planes from reversed-Z viewProj; AABB-vs-planes batched in parallelFor chunks (grain ~512); layer masks (camera cullMask, light channels incl. caster filtering); shadow ortho frustum fitted to camera-visible bounds + conservative caster extrusion along light dir (D15). Sort per D14. Counters exact (CI-gateable).
**Amendments (2026-08-18):**
- **Instancing collapse (D26.3, seed 9c):** after sorting, adjacent
  identical (pipeline, material, mesh range, block) runs collapse into
  one `DrawCommand` with `instanceCount > 1`; counters report
  records-in vs draws-submitted (CI-gated in sample 09's stress-v2).
- **Per-draw addressing (D26.1):** `recordDrawList` drives materials via
  `firstInstance` indexing into the bindless per-draw buffer — zero
  per-draw push constants in the scene path.
- **Main-thread pre-resolution (D27):** before fan-out, the helper
  pre-resolves every distinct (material, pass-signature, specialization)
  pipeline + parameter offsets on the main thread from the sorted list;
  worker chunks consume only pre-resolved plain data — `getPipeline`/
  `bindInstance` are main-thread-guarded and MUST NOT be called from
  chunks ≥ 1 (the sample-06 collision, resolved here by design).
- **Eviction invariant (D24):** material/mesh handle resolution at
  list-build time is residency-tolerant (fallback substitution, never a
  crash or raw-pointer escape).
- **Zero-alloc invariant:** `build()` into reused storage performs zero
  net heap allocations across steady-state frames — asserted by test.
**Steps:** device-free tests with synthetic scenes: known in/out AABB sets (exact counters), mask filtering matrices, sort-order assertions (opaque key monotonic, blend depth descending), instancing-collapse assertions (identical-run scene → 1 command with instanceCount=N; counters match), off-screen-caster-still-casts case, determinism across thread counts (same lists any --threads), steady-state zero-allocation assertion, pre-resolution unit test (worker chunks never hit the main-thread guard — assert under a debug hook) → implement → commit.
**Gate hardening (2026-08-18, BINDING):** criteria per
`gate/matrix-issue06-drawlists-culling.md` +
`gate/matrix-issue07-layer-masks.md` as amended by
`gate/rulings-2026-08-18.md` §#6/#7 + RC3/RC5. Key deltas: sort-key
bit layout documented with named constants + decode() round-trip test
(bgfx discipline); depth bucket = truncated monotonic float32 bit
pattern (never linear rescale); deterministic low-bit tie-break =
stable creation index; partition sort-DIRECTION test on one shared
fixture (opaque decreasing / blend increasing under reversed-Z);
priority tier above pipeline bits, blendOrder bits reserved
unpopulated; fixed index-range chunks + chunk-index-ordered
concatenation → byte-identical output across --threads; **D26.3
lockstep criterion**: commands and payloads sort/collapse as ONE unit
— the interleaved-scene test that catches silent payload
desynchronization is mandatory (collapsed `[firstInstance,
firstInstance+instanceCount)` ranges cross-checked against source
renderables); `CullCounters` = {totalCandidates, culledByLayerMask,
culledByFrustum, visible, recordsIn, drawsSubmitted,
shadowCastersConsidered, shadowCastersVisible} — exact, CI-gated;
`ShadowLists` = ViewLists shape, single partition, sorted (pipeline,
mesh range, block), BLEND excluded (RC3); culling planes from
`Camera::cullingProj()` (finite) + the extreme-depth never-culled
test; degenerate/zero-extent AABB + ground-slab cases; per-block
contiguous `BlockRange` grouping test; D27 worker-guard test reuses
`setViolationHookForTests` + the rendezvous-barrier pattern from
test_material_system.cpp:854-1042 verbatim; layer/channel getters +
`setChannels` added; the five-case mask CI matrix.

