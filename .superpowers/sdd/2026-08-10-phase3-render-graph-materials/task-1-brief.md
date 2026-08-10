### Task 1: rx_graph declarations + compile front-half (cull, order, lifetimes)

**Files:**
- Create: `src/rx_graph/CMakeLists.txt`, `src/rx_graph/include/rx_graph/render_graph.h`, `src/rx_graph/include/rx_graph/pass.h`, `src/rx_graph/include/rx_graph/resources.h`, `src/rx_graph/render_graph.cpp`
- Create: `src/rx_graph/tests/CMakeLists.txt`, `src/rx_graph/tests/doctest_main.cpp` (NO warm-up needed — this test binary is device-free), `src/rx_graph/tests/test_compile.cpp`
- Modify: `src/CMakeLists.txt` (add subdirectory), `CMakePresets.json` only if a new test target needs wiring (follow existing test registration pattern)

**Interfaces (produces — exact, later tasks depend on these):**

```cpp
namespace rx::graph {

enum class QueueClass : uint8_t { Graphics, AsyncCompute }; // hint only in Phase 3 (D4): scheduler maps all to Graphics

enum class SizeClass : uint8_t { SwapchainRelative, Absolute };

struct AttachmentDesc {
    VkFormat format = VK_FORMAT_UNDEFINED;
    SizeClass sizeClass = SizeClass::SwapchainRelative;
    float width = 1.0F;   // multiplier (SwapchainRelative) or pixels (Absolute)
    float height = 1.0F;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

struct BufferDesc {
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
};

class Pass {
public:
    Pass& addColorOutput(std::string_view name, const AttachmentDesc& desc);
    Pass& setDepthStencilOutput(std::string_view name, const AttachmentDesc& desc);
    Pass& addTextureInput(std::string_view name);          // sampled read (any prior attachment)
    Pass& addStorageBufferOutput(std::string_view name, const BufferDesc& desc);
    Pass& addStorageBufferInput(std::string_view name);
    Pass& setSideEffect();                                  // exempt from culling
    Pass& setExecute(std::function<void(PassContext&)> fn); // PassContext defined in Task 3; forward-declare now
    [[nodiscard]] std::string_view name() const;
    [[nodiscard]] QueueClass queueClass() const;
};

struct CompileInfo {
    uint32_t swapchainWidth = 0;
    uint32_t swapchainHeight = 0;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
};

struct ResourceAccess {                 // one declared access, resolved during compile
    uint32_t physicalIndex;             // index into CompiledGraph::resources
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 access;
    VkImageLayout layout;               // images only; UNDEFINED for buffers
};

struct PhysicalResource {
    std::string name;
    bool isBuffer;
    AttachmentDesc attachment;          // valid if !isBuffer (absolute pixel size resolved at compile)
    BufferDesc buffer;                  // valid if isBuffer
    VkImageUsageFlags imageUsage;       // union of all declared uses (+ TRANSFER_SRC for readback targets)
    uint32_t firstUsePass;              // indices into executionOrder positions
    uint32_t lastUsePass;
    bool isBackbuffer;
};

class CompiledGraph {
public:
    [[nodiscard]] std::span<const uint32_t> executionOrder() const; // pass indices, submission order
    [[nodiscard]] bool isCulled(uint32_t passIndex) const;
    [[nodiscard]] std::span<const PhysicalResource> resources() const;
    [[nodiscard]] std::span<const ResourceAccess> passAccesses(uint32_t passIndex) const;
    // Task 2 adds: passBarriers(uint32_t orderPosition)
};

class RenderGraph {
public:
    RenderGraph();
    ~RenderGraph();
    Pass& addPass(std::string_view name, QueueClass queue = QueueClass::Graphics);
    void setBackbufferSource(std::string_view name);   // resource presented at frame end
    void compile(const CompileInfo& info);              // device-free: cull, order, lifetimes (+Task 2 barriers)
    [[nodiscard]] const CompiledGraph& compiled() const;
    void reset();                                       // clear all passes/resources for re-declaration
};

} // namespace rx::graph
```

**Compile algorithm (Granite's front half, D1; reference: `traverse_dependencies`/`depend_passes_recursive` in render_graph.cpp — re-implement, don't copy):**
1. Build name→virtual-resource table; record each pass's reads/writes with the stage/access/layout implied by the declaration kind (color output → `COLOR_ATTACHMENT_OUTPUT` / `COLOR_ATTACHMENT_WRITE` / `COLOR_ATTACHMENT_OPTIMAL`; depth output → `EARLY_FRAGMENT_TESTS|LATE_FRAGMENT_TESTS` / `DEPTH_STENCIL_ATTACHMENT_READ|WRITE` / `DEPTH_ATTACHMENT_OPTIMAL`; texture input → `FRAGMENT_SHADER` / `SHADER_SAMPLED_READ` / `SHADER_READ_ONLY_OPTIMAL`; storage buffer out → `COMPUTE_SHADER` / `SHADER_STORAGE_WRITE|READ` for Compute-class passes, `VERTEX_SHADER|FRAGMENT_SHADER` / same for Graphics-class; storage buffer in → same stages / `SHADER_STORAGE_READ`).
2. Cull: start from the backbuffer source pass + all side-effect passes; walk producer edges recursively (a pass reading resource R depends on the last writer of R declared before it; write-after-write chains order by declaration order). Passes never reached are culled.
3. Order: topological order of surviving passes; preserve declaration order among independents (stable, deterministic — no reordering heuristics in Phase 3).
4. Lifetimes: firstUsePass/lastUsePass per physical resource over the execution order (D4 — this data feeds the future aliasing allocator; pooling uses it now).
5. Validation errors are exceptions (`std::runtime_error` with the offending pass/resource name): reading a resource nobody wrote, backbuffer source never written, duplicate pass names, compile() without setBackbufferSource.

**Steps:**
- [ ] **1. Failing tests first** — `test_compile.cpp` with doctest cases (exact expectations):
  - `culling`: passes A(writes "x", no side effect, nothing reads "x"), B(writes "bb"), backbuffer="bb" → A culled, B survives; `resources()` contains no physical entry for "x".
  - `ordering`: shadow(writes "sm") → forward(reads "sm", writes "hdr") → tonemap(reads "hdr", writes "bb"), declared in scrambled order → executionOrder = shadow, forward, tonemap.
  - `lifetimes`: same graph → "sm" first=0 last=1; "hdr" first=1 last=2; "bb" first=2 last=2, isBackbuffer=true.
  - `usage-union`: "sm" declared depth output + texture input → imageUsage contains `DEPTH_STENCIL_ATTACHMENT_BIT|SAMPLED_BIT`.
  - `diamond`: A writes "a"; B reads "a" writes "b"; C reads "a" writes "c"; D reads "b","c" writes "bb" → order A,(B,C in declaration order),D.
  - `errors`: read of never-written resource throws; no backbuffer source throws; message contains the resource name.
- [ ] **2. Run tests, verify they fail to compile/link** (`ctest --preset linux-native -R rx_graph` after wiring the target).
- [ ] **3. Implement** headers + `render_graph.cpp` per the algorithm above. Header hygiene: only `vulkan/vulkan_core.h` types + std; no volk include in public headers.
- [ ] **4. All tests green, both presets build:** `cmake --build --preset linux-native && ctest --preset linux-native -R rx_graph --output-on-failure` and `cmake --build --preset windows-cross-zig`.
- [ ] **5. Commit** `feat: add rx_graph pass declarations and compile front-half`.

