### Task 2: Reflection → descriptor set layouts + pipeline layouts

**Files:**
- Create: `src/rx_shader/include/rx_shader/reflection.h`, `src/rx_shader/src/reflection.cpp`, `src/rx_shader/tests/reflection_test.cpp`
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/pipeline_layout.h`, `src/rx_rhi_vk/src/pipeline_layout.cpp`, `src/rx_rhi_vk/tests/pipeline_layout_test.cpp`
- Modify: both CMakeLists

**Interfaces produced:**
- `rx::shader::ShaderLayoutInfo { struct Binding { uint32_t set, binding, count; VkDescriptorType type; VkShaderStageFlags stages; bool unboundedArray; }; std::vector<Binding> bindings; struct PushRange { VkShaderStageFlags stages; uint32_t offset, size; }; std::vector<PushRange> pushRanges; }`
- `rx::shader::reflect(const CompileResult&) -> std::optional<ShaderLayoutInfo>` — walks `ProgramLayout`/`TypeLayoutReflection::getDescriptorSet*` + `BindingType::PushConstant` per [R:A3]; maps `slang::BindingType` → `VkDescriptorType`; merges per-entry-point stage flags.
- `rx::rhi::PipelineLayoutBuilder` — `build(VkDevice, const ShaderLayoutInfo&) -> std::optional<PipelineLayoutBundle { std::vector<VkDescriptorSetLayout> setLayouts; VkPipelineLayout layout; }>`; unbounded-array bindings get `UPDATE_AFTER_BIND | PARTIALLY_BOUND` flags + update-after-bind layout flag (consumed by Task 3's bindless set). RAII bundle owns its handles.

**Tests:** reflect a shader with a `Texture2D[] ` unbounded array + a sampler + a `ConstantBuffer` + push constants → assert exact set/binding/type/count/stage/range values (hand-computed expected table in the test); build the pipeline layout on a headless device → non-null handles, zero validation errors; a shader with >128-byte push constants → reflection succeeds but `PipelineLayoutBuilder` rejects with a logged error (enforces the budget [R:B2]).

**Verify:** full ctest green; both presets build; commit clean.

---

