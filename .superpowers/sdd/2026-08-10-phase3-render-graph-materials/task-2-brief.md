### Task 2: Barrier derivation (sync2 invalidate/flush accounting)

**Files:**
- Create: `src/rx_graph/barriers.cpp`, `src/rx_graph/include/rx_graph/barriers.h`
- Modify: `src/rx_graph/render_graph.cpp` (compile() calls barrier build as its last phase), `src/rx_graph/include/rx_graph/render_graph.h` (CompiledGraph::passBarriers)
- Create: `src/rx_graph/tests/test_barriers.cpp`

**Interfaces (produces):**

```cpp
namespace rx::graph {
struct ImageBarrier {                  // 1:1 payload for VkImageMemoryBarrier2, minus handles (device-free)
    uint32_t physicalIndex;
    VkPipelineStageFlags2 srcStage; VkAccessFlags2 srcAccess;
    VkPipelineStageFlags2 dstStage; VkAccessFlags2 dstAccess;
    VkImageLayout oldLayout; VkImageLayout newLayout;
};
struct BufferBarrier {
    uint32_t physicalIndex;
    VkPipelineStageFlags2 srcStage; VkAccessFlags2 srcAccess;
    VkPipelineStageFlags2 dstStage; VkAccessFlags2 dstAccess;
};
struct PassBarriers {
    std::vector<ImageBarrier> imageBarriers;    // emitted immediately before the pass
    std::vector<BufferBarrier> bufferBarriers;
};
// CompiledGraph additions:
//   std::span<const PassBarriers> passBarriers() const;           // index = position in executionOrder
//   const PassBarriers& finalBarriers() const;                    // backbuffer → PRESENT_SRC_KHR transition
}
```

**Algorithm (Granite's per-resource state machine, D3; reference: render_graph.cpp barrier build ~lines 1998-2038 — re-express, don't copy):** per physical resource track `currentLayout` (starts UNDEFINED each frame — transients are discard-on-frame-start, D4), `pendingFlushStages/Access` (last unflushed write), `invalidatedStages/Access` (visibility already established). Walk executionOrder; for each declared access:
- Need barrier if: layout differs; or access includes writes and any prior access exists (WAW/WAR need execution dependency; WAR emits srcAccess=0); or access is a read whose (stage,access) is not covered by `invalidated*` while a `pendingFlush*` exists.
- Emit with srcStage = pendingFlushStages (or `VK_PIPELINE_STAGE_2_NONE` + srcAccess=0 on first use), dst = the declared access. After a write: set pendingFlush to the write's (stage,access), clear invalidated. After a barrier that makes a write visible: clear pendingFlush, accumulate (dstStage,dstAccess) into invalidated. Reads with no layout change and already-covered visibility emit nothing.
- After the walk: `finalBarriers()` = backbuffer `currentLayout → PRESENT_SRC_KHR`, src = its last write, dst = `VK_PIPELINE_STAGE_2_NONE`/0.

**Steps:**
- [ ] **1. Failing tests** — `test_barriers.cpp`, asserting EXACT full barrier structs (all six mask/layout fields), doctest cases:
  - `shadow-then-sample`: depth write → fragment sample: exactly one ImageBarrier before forward: src `LATE_FRAGMENT_TESTS|EARLY_FRAGMENT_TESTS`/`DEPTH_STENCIL_ATTACHMENT_WRITE`, dst `FRAGMENT_SHADER`/`SHADER_SAMPLED_READ`, `DEPTH_ATTACHMENT_OPTIMAL→SHADER_READ_ONLY_OPTIMAL`. And before the shadow pass itself: `UNDEFINED→DEPTH_ATTACHMENT_OPTIMAL`, srcStage NONE, srcAccess 0.
  - `no-redundant-read`: two consecutive passes sampling the same texture → second pass has zero barriers for it.
  - `hdr-tonemap`: color write → fragment sample: src `COLOR_ATTACHMENT_OUTPUT`/`COLOR_ATTACHMENT_WRITE`, dst `FRAGMENT_SHADER`/`SHADER_SAMPLED_READ`, `COLOR_ATTACHMENT_OPTIMAL→SHADER_READ_ONLY_OPTIMAL`.
  - `war-execution-only`: pass reads T (sampled), later pass depth-writes T → barrier with srcAccess=0 (execution-only + layout change `SHADER_READ_ONLY_OPTIMAL→DEPTH_ATTACHMENT_OPTIMAL`).
  - `compute-to-draw-buffer`: compute storage write → graphics storage read: BufferBarrier `COMPUTE_SHADER`/`SHADER_STORAGE_WRITE` → `VERTEX_SHADER|FRAGMENT_SHADER`/`SHADER_STORAGE_READ`.
  - `present-final`: finalBarriers = exactly one ImageBarrier on backbuffer `COLOR_ATTACHMENT_OPTIMAL→PRESENT_SRC_KHR`.
  - `culled-contributes-nothing`: culled pass's writes leave no trace in any surviving pass's barriers.
- [ ] **2. Verify failure.**
- [ ] **3. Implement.**
- [ ] **4. Green + both presets build.**
- [ ] **5. Commit** `feat: derive sync2 barriers in rx_graph compile`.

