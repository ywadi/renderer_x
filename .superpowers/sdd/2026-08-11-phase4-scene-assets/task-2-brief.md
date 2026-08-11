### Task 2: enkiTS adoption (`rx_task`) + threading contract doc

**Files:** Create `src/rx_task/{CMakeLists.txt,include/rx_task/scheduler.h,scheduler.cpp,tests/...}`, `docs/threading.md`; modify `cmake` dep wiring for enkiTS (pinned tag, DepCache), root CMakeLists.
**Interfaces (produces):**
```cpp
namespace rx::task {
class Scheduler {  // owns enki::TaskScheduler; one per app; main thread participates
 public:
  static std::unique_ptr<Scheduler> create(uint32_t workerCount /*0 = hw-1*/);
  void parallelFor(uint32_t itemCount, uint32_t grainSize, std::function<void(uint32_t begin, uint32_t end, uint32_t workerIndex)> fn); // blocking fan-out
  void runOnIoThread(std::function<void()> fn);         // pinned IO thread, FIFO
  void postToMain(std::function<void()> fn);            // queued; drained by pumpMain()
  void pumpMain();                                      // main-thread drain point (frame loop calls once per frame)
  uint32_t workerCount() const;                          // includes main participation semantics documented
};}
```
`docs/threading.md`: the D5 contract (main-thread-only list, worker-allowed list, handoff pattern, per-thread command-pool rule forward-referencing Task 7). Thread-affinity one-liners added to bindless.h/upload.h/material_system.h/deletion_queue.h headers (doc-only edits).
**Steps:** vendor enkiTS (pin tag; license recorded) → tests: parallelFor sums ranges exactly once (counters per worker), postToMain executes on main thread id, IO-thread ordering FIFO, nested parallelFor safe → both presets (windows-gnu build verified) → commit.

