# Phase 3: Render Graph + Material System — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `rx_graph` (dynamic-rendering-native render graph with automatic sync2 barriers, ported from Granite's algorithms) and `rx_material` (Slang-module materials with a COM-lite public API surface), exiting with samples 05_multipass and 06_materials deployed and release v0.3.0-phase3.

**Spec:** `docs/superpowers/specs/2026-08-10-phase3-render-graph-materials-design.md` (decisions cited below as D1-D12). Research: `.superpowers/sdd/2026-08-10-phase3-render-graph-materials/research-{rendergraph,abi-materials}.md`.

**Architecture:** `rx_graph` (depends on rx_rhi_vk only): declarative passes → compile (cull, order, lifetimes, barriers — device-free) → realize/execute (transient pool, dynamic rendering, sync2). `rx_material` (depends on rx_graph + rx_shader + rx_rhi_vk): Slang-module materials, lazy (module-hash, pass-signature, spec-bits)-keyed pipeline cache, COM-lite `IRx*` surface. Reference implementation for graph algorithms: Granite `renderer/render_graph.{hpp,cpp}` + Arntzen's 2017 deep-dive post — re-implement against rx types, do not vendor (D1).

**Tech Stack:** Existing only — Vulkan 1.3 (dynamic rendering + sync2, volk), VMA, GLM, spdlog, doctest, Slang v2026.14.1 prebuilt, SDL3. No new third-party dependencies.

## Global Constraints

- Vulkan 1.3 baseline: dynamic rendering + synchronization2 ONLY. No `VkRenderPass`, no legacy `vkCmdPipelineBarrier`, no legacy barrier structs anywhere in new code.
- Zero validation errors: every GPU test and headless gate runs with validation active (samples/gates pass `--validate`); a validation message is a test failure.
- Test binaries with GPU tests use the established `doctest_main.cpp` warm-up pattern (vk-bootstrap process-wide function-pointer landmine — see `Context::create` docs) — copy the pattern from `src/rx_rhi_vk/tests/doctest_main.cpp`.
- Matrices crossing host→Slang StructuredBuffer/ParameterBlock boundary are row-major on the shader side: hosts `glm::transpose` at the boundary (documented on `rx::shader::Compiler::create`).
- Descriptor slot rewrites and resource destruction while frames may be in flight go through the fence-gated `DeletionQueue` (see `bindless.h` RELEASE-SAFETY CONTRACT).
- Public repo, no AI attribution of ANY kind in commits, code, comments, or docs (CLAUDE.md). Commit author = local git config, short imperative commit subjects.
- Production grade only: no stubs, no TODO-laters inside delivered code. Deferrals happen in the plan/spec (D4/D5/D7), never as half-implemented code paths.
- Don't reinvent the wheel: graph algorithms follow the cited Granite reference; COM-lite surface mirrors Slang's `ISlangUnknown` shape; use existing rx_* infrastructure (BindlessTable, PipelineLayoutBuilder, Uploader, DeletionQueue, FrameSync, Compiler/reflect) rather than parallel implementations.
- CMake: new libs follow the existing `src/rx_shader/CMakeLists.txt` pattern (static lib, `rx::` alias, tests via doctest + ctest presets, both presets must build: `linux-native`, `windows-cross-zig`).
- Sub-1-minute incremental builds preserved: no header includes Vulkan implementation headers; keep volk usage in .cpp files where the existing code does.

---

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

### Task 4: Sample 05_multipass (shadow + forward + tonemap)

**Files:**
- Create: `samples/05_multipass/main.cpp`, `samples/05_multipass/CMakeLists.txt`, `shaders/multipass/shadow.vert.slang`, `shaders/multipass/lit.vert.slang`, `shaders/multipass/lit.frag.slang`, `shaders/multipass/tonemap.vert.slang`, `shaders/multipass/tonemap.frag.slang`
- Modify: `samples/CMakeLists.txt`, `tools/package_samples.sh` (add 05 dir), `.github/workflows/ci.yml` (gate list — follow how 04 was added), `samples/README.md`

**Scene/pipeline spec:** ground plane + one cube + one sphere (reuse the procedural mesh generation approach from `samples/03_bindless_mesh`), directional light at fixed elevation. Graph: `shadow` (depth-only, Absolute 1024×1024 D32_SFLOAT "shadowmap") → `forward` (reads "shadowmap" sampled with compare or manual PCF-1 tap; writes SwapchainRelative R16G16B16A16_SFLOAT "hdr" + D32 depth "depth"; Lambert × shadow factor, light-space matrix via push constants/bindless buffer per the 03 pattern) → `tonemap` (reads "hdr", writes backbuffer; Reinhard `c/(1+c)` then linear→sRGB is handled by the UNORM/sRGB swapchain as in existing samples — output plain Reinhard). Zero hand-written barriers/transitions in the sample (D10) — grep-able acceptance: `vkCmdPipelineBarrier2` must not appear in `samples/05_multipass/`.

**Steps:**
- [ ] **1. Shaders + sample** wired through `RenderGraph`/`Executor` + existing `FrameSync` loop (acquire → executor.execute into frame cmd → submit/present). Headless mode: fixed light/camera, 3 frames, readback, assert: (a) a specified ground-plane pixel inside the cube's shadow is darker than a specified lit ground pixel by >2×; (b) lit pixel is non-trivially bright (>0.2); (c) all readback values ≤ 1.0 (tonemapped). `--present` animates the light azimuth; `--validate` opt-in flag identical to samples 01-04.
- [ ] **2. ctest gate** (headless + `--validate`) green locally, zero validation errors.
- [ ] **3. Packaging + CI**: extend `tools/package_samples.sh` and ci.yml gate lists; per-sample dir ships the `shaders/multipass/*.slang` files + Slang runtime libs exactly as 02 does.
- [ ] **4. Both presets build; run packaged binary from an unzipped layout locally.**
- [ ] **5. Commit** `feat: add sample 05_multipass driving shadow+forward+tonemap through rx_graph`.

### Task 5: rx_material core (modules, pass signatures, variant cache)

**Files:**
- Create: `src/rx_material/CMakeLists.txt`, `src/rx_material/include/rx_material/material_system.h`, `src/rx_material/material_system.cpp`, `shaders/material/material.slang`
- Create: `src/rx_graph/include/rx_graph/pass_signature.h` — PassSignature lives in `rx::graph` (it is derived purely from graph pass state; putting it in rx_material would create a dependency cycle since PassContext exposes it). rx_material aliases it: `namespace rx::material { using PassSignature = rx::graph::PassSignature; }`.
- Create: `src/rx_material/tests/CMakeLists.txt`, `src/rx_material/tests/doctest_main.cpp` (warm-up pattern), `src/rx_material/tests/test_material_system.cpp`, `src/rx_material/tests/data/test_unlit.slang`
- Modify: `src/rx_graph/include/rx_graph/pass.h` (+`PassSignature PassContext::passSignature() const`), `src/rx_graph/executor.cpp` (populate it), `src/CMakeLists.txt`

**Interfaces (produces):**

```cpp
namespace rx::graph {                            // pass_signature.h
struct PassSignature {
    std::array<VkFormat, 8> colorFormats{};      // VK_FORMAT_UNDEFINED-padded
    uint32_t colorCount = 0;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    [[nodiscard]] uint64_t hash() const;         // FNV-1a over all fields
    bool operator==(const PassSignature&) const = default;
};
} // namespace rx::graph

namespace rx::material {
using PassSignature = rx::graph::PassSignature;

using MaterialHandle = rx::core::Handle<struct MaterialTag>;   // existing generational handle template (adapt to the actual rx_core template signature)

struct PipelineRequest {
    MaterialHandle material;
    PassSignature pass;
    uint32_t specializationBits = 0;             // D8: reserved axes; 0 in Phase 3 core
};

class MaterialSystem {
public:
    static std::unique_ptr<MaterialSystem> create(rhi::Device& device, rhi::BindlessTable& bindless,
                                                  const std::filesystem::path& pipelineCachePath);
    ~MaterialSystem();                           // saves VkPipelineCache to pipelineCachePath
    MaterialHandle loadMaterial(const std::filesystem::path& slangModulePath);  // throws on compile error w/ diagnostics
    [[nodiscard]] uint64_t moduleHash(MaterialHandle) const;                    // content hash (FNV-1a over source bytes)
    // Lazy: compiles+links Slang, builds VkPipeline on first request for the key
    // (moduleHash, pass.hash(), specializationBits); cached thereafter (D7).
    VkPipeline getPipeline(const PipelineRequest& req);
    [[nodiscard]] VkPipelineLayout pipelineLayout(MaterialHandle) const;         // reflection-driven, external set 0 = bindless
    [[nodiscard]] const shader::ShaderLayoutInfo& layoutInfo(MaterialHandle) const;
};
}
```

**Shader-side contract** (`shaders/material/material.slang`, D6): `interface IMaterialShader { float4 evaluate(MaterialVertex v); }` with `struct MaterialVertex { float3 worldPos; float3 normal; float2 uv; }`; the shared entry-point module `shaders/material/forward_entry.slang` declares vertex+fragment entry points generic over `IMaterialShader` and its `ParameterBlock<...>`, composed + linked with the material module via `createCompositeComponentType`+`link` per unique key (Slang's documented specialization path — reference `rx_shader::Compiler` retained-component pattern from Phase 2). A material module = struct conforming to `IMaterialShader` + `ParameterBlock<TParams> gParams` at set 1 (set 0 remains the external bindless set via `PipelineLayoutBuilder`).

**Steps:**
- [ ] **1. Failing tests** (`test_material_system.cpp`, GPU target with warm-up):
  - `load-reflect`: `test_unlit.slang` (flat color from `gParams.tint`) loads; `layoutInfo` reports the set-1 param block; `pipelineLayout` != VK_NULL_HANDLE.
  - `cache-hit`: two `getPipeline` calls, same request → same `VkPipeline` handle; second call performs no Slang compilation (assert via a compile counter exposed for tests or spdlog capture — pick the existing project idiom).
  - `cache-key-pass`: same material, different depthFormat in signature → different pipeline handles.
  - `cache-key-material`: two different modules, same signature → different handles.
  - `bad-module`: loading a module with a syntax error throws; message contains Slang diagnostic text.
  - `pipeline-cache-persists`: destroy system, recreate with same path → cache file exists and loads (assert file non-empty; VkPipelineCache load path logged).
- [ ] **2. Verify failure.**
- [ ] **3. Implement** (fresh `Compiler` usage rules per compiler.h caveat; PassSignature populated by executor from the current pass's attachment formats).
- [ ] **4. Green, both presets build, zero validation errors.**
- [ ] **5. Commit** `feat: add rx_material core with pass-signature-keyed pipeline cache`.

### Task 6: COM-lite public surface (rx_api.h)

**Files:**
- Create: `src/rx_material/include/rx_material/rx_api.h` (the ONLY header a public consumer needs; self-contained: no rx_*, STL, or Vulkan includes — C types + pure-virtual interfaces only), `src/rx_material/api_impl.cpp`
- Create: `src/rx_material/tests/test_api_contract.cpp` (device-free target where possible; factory tests in the GPU target)
- Modify: `src/rx_material/CMakeLists.txt`

**Interfaces (produces — the ABI surface, D5; mirror Slang's `ISlangUnknown` shape [R:M§1.5]):**

```cpp
// rx_api.h — ABI rules (D5, R:M§1.3): pure-virtual single-inheritance only, no overloads,
// no data members, no exceptions/RTTI/STL across the boundary, renderer-side allocation only,
// explicit GUID per interface version, POD structs static_assert-pinned.
#include <cstdint>
#define RX_CALL /* empty on x64; kept for documentation of the boundary */

typedef int32_t RxResult;
enum : RxResult { RX_OK = 0, RX_E_FAIL = -1, RX_E_INVALIDARG = -2, RX_E_NOTFOUND = -3,
                  RX_E_COMPILE = -4, RX_E_NOINTERFACE = -5 };
struct RxGuid { uint32_t data1; uint16_t data2, data3; uint8_t data4[8]; };

struct IRxUnknown {
    virtual RxResult RX_CALL queryInterface(const RxGuid& iid, void** outObject) = 0;
    virtual uint32_t RX_CALL addRef() = 0;
    virtual uint32_t RX_CALL release() = 0;
};
struct IRxTexture : IRxUnknown { /* opaque: wraps rhi::Texture2D + bindless handle */ };
struct IRxMaterialInstance : IRxUnknown {
    virtual RxResult RX_CALL setFloat(const char* name, float value) = 0;
    virtual RxResult RX_CALL setFloat4(const char* name, const float value[4]) = 0;
    virtual RxResult RX_CALL setTexture(const char* name, IRxTexture* texture) = 0;
};
struct IRxMaterial : IRxUnknown {
    virtual RxResult RX_CALL createInstance(IRxMaterialInstance** outInstance) = 0;
    virtual const char* RX_CALL name() = 0;                    // valid for material lifetime
};
struct IRxMaterialSystem : IRxUnknown {
    virtual RxResult RX_CALL loadMaterial(const char* slangModulePath, IRxMaterial** outMaterial) = 0;
    virtual RxResult RX_CALL reloadChanged() = 0;              // Task 7 wires this; declared now (GUID stability)
};
// In-process bridge factory (standalone-DLL packaging deferred, D5): desc carries
// pointers to live internal objects; a future rx.dll adds public device creation.
struct RxMaterialSystemDesc { void* internalMaterialSystem; };  // rx::material::MaterialSystem*
extern "C" RxResult rxCreateMaterialSystem(const RxMaterialSystemDesc* desc, IRxMaterialSystem** outSystem);
```

GUIDs: one `static constexpr RxGuid kIID_IRx...` per interface (generate with `uuidgen`, embed literal values). Implementation classes in `api_impl.cpp` hold `std::atomic<uint32_t>` refcounts; `queryInterface` supports each interface's own IID + `IRxUnknown` (COM identity rule: same object pointer for IRxUnknown from any interface of the object). Errors are codes; internal exceptions caught at the boundary and mapped (`RX_E_COMPILE` carries diagnostics via `spdlog` — no strings across ABI in Phase 3).

**Steps:**
- [ ] **1. Failing tests** (`test_api_contract.cpp`): QI-identity (IRxUnknown* from IRxMaterial == from its IRxMaterialInstance's parent? NO — identity is per-object: assert IRxUnknown from the SAME object via different IIDs compares equal); refcount round-trip (create → addRef → release ×2 → destroyed exactly once, verified via instance counter for tests); unknown IID → `RX_E_NOINTERFACE` and `*outObject == nullptr`; null out-params → `RX_E_INVALIDARG`; `static_assert(sizeof(RxGuid) == 16)` and `sizeof(RxMaterialSystemDesc) == sizeof(void*)`; header self-containment (a test TU that includes ONLY rx_api.h and compiles). Factory + loadMaterial happy path in the GPU target using `test_unlit.slang`.
- [ ] **2. Verify failure. 3. Implement. 4. Green both presets.**
- [ ] **5. Commit** `feat: add COM-lite public material API surface`.

### Task 7: Material instances, parameter arena, hot reload

**Files:**
- Create: `src/rx_material/instance.cpp`, `src/rx_material/include/rx_material/instance.h`, `src/rx_rhi_vk/include/rx_rhi_vk/descriptor_arena.h`, `src/rx_rhi_vk/descriptor_arena.cpp`
- Modify: `src/rx_material/material_system.cpp` (+reload), `src/rx_material/api_impl.cpp` (wire setters + reloadChanged), tests in both rx_material targets, `src/rx_rhi_vk/tests/` (+ descriptor arena test)

**Design:**
- `rhi::DescriptorArena` (reusable RHI piece): per-frames-in-flight `VkDescriptorPool`s; `beginFrame(frameIndex)` resets that frame's pool; `VkDescriptorSet allocate(VkDescriptorSetLayout)`. Sized generously (spec constants; document limits).
- Instance parameter storage: CPU-side blob laid out by the reflected param block (offsets from `ShaderLayoutInfo`); `setFloat*`/`setTexture` write the blob by reflected member name (`RX_E_NOTFOUND` for unknown names, type-checked → `RX_E_INVALIDARG`). At record time (`MaterialSystem::bindInstance(cmd, PassContext&, instance)` — new internal API consumed by sample 06): copy blob into a per-frame host-visible arena buffer (persistently mapped, bump-allocated, flush if non-coherent), allocate set-1 from DescriptorArena, write UBO descriptor, bind. Texture params store the texture's bindless index in the blob (u32) — sampling stays bindless set-0, matching the established pattern.
- Hot reload (D9): `MaterialSystem::reloadChanged()` stats module files (mtime, 02 pattern); changed → recompile with a FRESH `Compiler`, rehash; success → erase cache entries whose key contains the old hash, retire their `VkPipeline`s via `DeletionQueue`, subsequent `getPipeline` re-links lazily; failure → keep last good, log diagnostics, return `RX_OK` with a logged warning (reload failure is not a caller error).

**Steps:**
- [ ] **1. Failing tests**: descriptor-arena (allocate across 3 simulated frames, reset reuse, no validation errors); instance param write→readback of arena blob at reflected offsets (exact bytes); unknown param name → `RX_E_NOTFOUND`; hot-reload test = regression pattern from Phase 2 (write module v1 to temp dir, load, getPipeline → P1; overwrite file with v2 (different tint math), `reloadChanged`, getPipeline → P2 ≠ P1; overwrite with syntactically broken v3, `reloadChanged`, getPipeline still returns P2).
- [ ] **2. Verify failure. 3. Implement. 4. Green both presets, zero validation errors.**
- [ ] **5. Commit** `feat: add material instances, parameter arena, and hot reload`.

### Task 8: Sample 06_materials

**Files:**
- Create: `samples/06_materials/main.cpp`, `samples/06_materials/CMakeLists.txt`, `samples/06_materials/materials/checker.slang`, `samples/06_materials/materials/rim.slang`
- Modify: `samples/CMakeLists.txt`, `tools/package_samples.sh`, `.github/workflows/ci.yml`, `samples/README.md`, `MANUAL_VERIFICATION.md` (05+06 present-mode rows)

**Spec:** 4 objects (2 cubes, 2 spheres; reuse 03's procedural meshes) drawn through a single-pass rx_graph forward pass; materials `checker.slang` (UV checker × `tint`) and `rim.slang` (rim-light × `rimColor`), each instanced twice with DIFFERENT per-instance parameter values — all material creation and parameter setting go **exclusively through `rx_api.h` interfaces** (factory → `IRxMaterialSystem` → `IRxMaterial` → `IRxMaterialInstance`). Internal rx_material headers are confined to a clearly-marked bridge section: system creation (the desc bridge) and the draw-time `bindInstance`/`getPipeline` calls, which have no public equivalent in Phase 3 (the public surface covers the material model, not draw submission — D5/D11). Grep-able acceptance: every `#include <rx_material/...>` other than `rx_api.h` sits in the bridge section, and no `IRx*` object is ever cast to an internal type outside it. Headless: readback, assert the 4 known object-center pixels match 4 distinct expected colors (per-instance overrides visible). Present: orbit camera + `reloadChanged()` polling both material files each second, keep-last-good.

**Steps:**
- [ ] **1. Materials + sample; headless gate green** (`--validate`, zero validation errors).
- [ ] **2. Packaging + CI** (materials dir ships next to the binary + Slang runtime libs, as 02 does).
- [ ] **3. Both presets build; packaged-layout run verified.**
- [ ] **4. Commit** `feat: add sample 06_materials on the public material API`.

### Task 9: Docs, deferred-minor fold-ins, roadmap

**Files:**
- Modify: `README.md` (Phase 3 → complete in Roadmap; project layout + samples list), `samples/README.md` (05/06 run instructions), `MANUAL_VERIFICATION.md` (if not finished in Task 8), `cmake/DepCache.cmake`, `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md` (tick delivered layers in the layer table)
- Create: `docs/abi.md` (the boundary rules of D5/R:M§1.3, for future contributors)

**Steps:**
- [ ] **1. DepCache fix** (carried deferred minor): include the dependency's `CMAKE_ARGS` in the cache-key hash (currently name|tag|triple|zig-version only) — changing CMAKE_ARGS must produce a new cache key; verify by reconfiguring with a changed arg and observing a rebuild, and document the key format in the file header comment.
- [ ] **2. Docs** listed above; keep hedged physical-hardware claims exactly as MANUAL_VERIFICATION.md does today.
- [ ] **3. Full local gate:** `ctest --preset linux-native --output-on-failure` all green; both presets build clean.
- [ ] **4. Commit** `docs: phase 3 documentation and dep-cache key hardening`.

---

## Execution notes (coordinator)

- Model assignment: Tasks 1-3, 5-7 Sonnet (multi-file/architecture); Task 4, 8 Sonnet (samples span shaders+cmake+CI); Task 9 Haiku (mechanical, fully specified). All reviews Sonnet.
- Parallelization: Task 4 (samples/05 + shaders/multipass) and Task 5 (src/rx_material + shaders/material) are file-disjoint after Task 3 lands → eligible for parallel worktree dispatch. Everything else is sequential.
- After Task 9: final whole-branch review (most capable model), at most one fix wave, then push, green CI, tag v0.3.0-phase3, attach CI packages, release notes with hedged hardware claims. Update this plan + ledger throughout.
