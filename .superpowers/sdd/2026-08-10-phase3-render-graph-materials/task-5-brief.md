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

