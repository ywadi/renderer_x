### Task 7: Parallel command recording + sample 07_stress

**Files:** Modify `src/rx_graph/include/rx_graph/pass.h` + `executor.cpp` (opt-in parallel recording per D4): 
```cpp
// No opt-in flag and no caller-chosen count: providing a chunked callback IS the parallel path
// (executor derives chunk count from the scheduler; grain scaling handles small workloads).
Pass& setExecuteChunked(std::function<void(PassContext&, uint32_t chunkIndex, uint32_t chunkCount)> fn);
// PassContext gains: VkCommandBuffer chunkCommandBuffer() — the secondary this chunk records into (workers)
```
Executor: per-thread × per-frames-in-flight command pools (created lazily per scheduler worker count); for parallel passes: begin rendering with SECONDARY_COMMAND_BUFFERS contents, secondaries begun with VkCommandBufferInheritanceRenderingInfo matching the pass's attachment formats/samples [R:threading], chunks fanned out via rx_task parallelFor, vkCmdExecuteCommands in chunk order, pools reset per frame slot. Whole-pass-callback passes byte-identical to today (they are the hand-written simple case, not a disabled mode). **Samples 05 and 06 migrate to chunked callbacks in this task** (user-directed): auto-grain collapses them to one chunk (no perf change, headless pixel gates must stay byte-identical) but their CI gates then exercise the parallel recording path on every commit. Samples 01-04 stay pre-graph by design — they document the layers below the executor.
Create `samples/07_stress/` + `shaders/stress/*.slang`: procedural instanced field (default 30,000 draws — cubes/spheres mix, per-instance transform+color via bindless arena, 4 pipeline/material variations to make sorting/state non-trivial), flags: `--draws N --threads N --vsync on|off --validate` (threads default = scheduler default, i.e. parallel recording ON; `--threads 1` is the A/B baseline), forward+tonemap through the graph with the forward pass parallel-recorded; ImGui NOT yet (Stage 2) — stats to stdout each second + Tracy zones; headless gate: fixed 3 frames, counter assertions (exact draws submitted, chunk count = threads, pool allocations within budget) + tolerance probe on 4 analytic pixels; **CI counter gate** (D18) + wall-clock printed and uploaded as artifact `stress-numbers.txt`; report publishes single-vs-multi-thread record timings (Tracy evidence) on the dev machine.
**Steps:** TDD gate → implement executor path → sample → measurements → packaging/CI wiring → commit(s).

