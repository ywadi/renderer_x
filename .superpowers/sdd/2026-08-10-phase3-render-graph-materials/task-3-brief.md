### Task 3: Physical realization + execution (transient pool, dynamic rendering)

**Files:**
- Create: `src/rx_graph/executor.cpp`, `src/rx_graph/include/rx_graph/executor.h`, `src/rx_graph/transient_pool.cpp` (+ header)
- Create: `src/rx_graph/tests/test_execute_gpu.cpp`, replace tests binary main with warm-up `doctest_main.cpp` variant for a NEW second test target `rx_graph_gpu_tests` (keep compile/barrier tests device-free in the existing target)
- Modify: `src/rx_graph/CMakeLists.txt`, `src/rx_graph/include/rx_graph/pass.h` (PassContext definition)

**Interfaces (produces):**

```cpp
namespace rx::graph {
struct PassContext {
    VkCommandBuffer cmd;
    VkExtent2D renderArea;                       // extent of this pass's attachments
    [[nodiscard]] VkImageView imageView(std::string_view name) const;  // resolved physical view
    [[nodiscard]] VkImage     image(std::string_view name) const;
    [[nodiscard]] VkBuffer    buffer(std::string_view name) const;
    [[nodiscard]] VkFormat    imageFormat(std::string_view name) const;
    // Task 5 adds passSignature()
};

class Executor {                                  // owns physical resources; graph stays declarative
public:
    static std::unique_ptr<Executor> create(rhi::Device& device);
    ~Executor();                                  // waits idle via existing Device teardown conventions
    // Realize physical resources for a compiled graph (idempotent per compile;
    // re-realizes on swapchain resize). Backbuffer supplied per frame.
    void realize(const RenderGraph& graph);
    // Record one frame: emits per-pass sync2 barriers, begins/ends dynamic rendering
    // per graphics pass (color LOAD_OP_CLEAR on first use, LOAD_OP_LOAD after;
    // STORE_OP_STORE), invokes pass execute callbacks, emits finalBarriers.
    void execute(const RenderGraph& graph, VkCommandBuffer cmd,
                 VkImage backbufferImage, VkImageView backbufferView, VkExtent2D backbufferExtent);
};
}
```

Transient pool: images/buffers keyed by (format, extent, usage, samples)/(size, usage) via VMA (`DEVICE_LOCAL`), reused across frames and across compiles when descriptors match; unused entries destroyed through `DeletionQueue` after `kFramesInFlight` frames. First-use-of-frame image barriers use oldLayout=UNDEFINED with srcStage = the resource's last-frame final-use stages (tracked in the pool entry; `ALL_COMMANDS` on its very first frame) — same-queue execution dependency makes cross-frame reuse safe with frames-in-flight=2. Depth attachments: `LOAD_OP_CLEAR`(1.0)/`STORE_OP_STORE`. Per-pass debug labels via `vkCmdBeginDebugUtilsLabelEXT` when the extension is present (query once at Executor::create).

**Steps:**
- [ ] **1. Failing GPU test** `test_execute_gpu.cpp` (headless, uses warm-up main + `--validate` in the ctest invocation): graph = pass "draw" (writes 256×256 R8G8B8A8_UNORM "color", side-effect-free) → pass "invert" (textureInput "color", writes "bb" absolute 256×256; execute callback registers "color"'s view in the BindlessTable, draws fullscreen triangle sampling+inverting it) → backbuffer "bb" imported as an offscreen target image created by the test. Readback via existing Uploader/readback path: assert corner pixel of "draw"'s clear color inverted correctly in "bb" (exact RGBA values). Bindless registration in the callback defers release per the DeletionQueue contract.
  - Second case `resize-rerealize`: compile at 128×128, realize, compile at 256×256, realize, execute — no validation errors, output extent honored.
- [ ] **2. Verify failure.** ctest gate registered as `rx_graph_gpu_tests` with `--validate` (follow the existing GPU-gate registration pattern in `src/rx_rhi_vk/tests/CMakeLists.txt`; CI excludes GPU tests on windows-cross the same way existing ones are excluded).
- [ ] **3. Implement** executor + transient pool.
- [ ] **4. Green** (`ctest --preset linux-native -R rx_graph --output-on-failure`), both presets build, zero validation errors.
- [ ] **5. Commit** `feat: add rx_graph executor with transient pool and dynamic rendering`.

