### Task 1: Render-graph history resources + PassSignature bounds check

**Files:** Modify `src/rx_graph/{render_graph.cpp,executor.cpp,transient_pool.{h,cpp}}`, `src/rx_graph/include/rx_graph/{render_graph.h,pass.h,resources.h}`; tests in both rx_graph targets.
**Interfaces (produces):**
```cpp
// Pass declaration additions:
Pass& addHistoryInput(std::string_view name);   // sampled read of the resource's PREVIOUS-frame contents
Pass& setHistoryOutput(std::string_view name, const AttachmentDesc& desc); // persistent (non-discard) written image
```
Semantics per spec item seed-15: history resources live in a pinned pool (never recycled/aliased/swapped between logical resources), ping-pong internally (read slot = last frame's write slot) under frames-in-flight; first-ever use initializes via UNDEFINED-discard + documented "history invalid on frame 0" contract (pass callback can query `PassContext::historyValid(name)`); subsequent frames load-preserve. Compile-time: history inputs read SHADER_READ_ONLY_OPTIMAL with FRAGMENT|COMPUTE stages per pass kind; barrier state machine initialized from tracked last-frame layout instead of UNDEFINED. Executor: pinned entries track lastFrameFinalStages/Access exactly like pooled ones.
**Also in this task (final-review carry):** `compile()` throws when a pass declares more color outputs than `PassSignature::kMaxColorAttachments` (8) — loud, named error + device-free test.
**Steps:** device-free tests (declaration/compile semantics, bounds check) → GPU test: pass A writes frame-N pattern to history output, pass B reads history input and writes readback target; assert frame N+1 readback contains frame N's pattern, frame 1 handles invalid-history branch; zero validation errors (sync validation) → implement → both presets → commit `feat: add render-graph history resources and color-attachment bounds check`.

