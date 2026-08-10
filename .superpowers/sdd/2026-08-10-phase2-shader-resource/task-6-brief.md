### Task 6: sample_03_bindless_mesh

**Files:** `samples/03_bindless_mesh/**` (+ shader), README update, root wiring.

Procedural geometry (cube, sphere, plane — generated in code, no importer), 4 distinct generated textures (checkerboards/gradients via stb-independent procedural fill; stb_image still used to load one real PNG embedded in the sample dir to prove the path), uploaded via Uploader into BindlessTable; per-draw push constants = {mvp offset index or transform, textureIndex, samplerIndex} within 128 bytes; descriptor set layout + pipeline layout come from `reflect()` on the actual sample shader — hand-typed layouts are forbidden (that's the point of the sample).

**Required plumbing (from Task 3's review — the set-0 layouts are NOT compatible "by construction" today):** `PipelineLayoutBuilder` uses a fixed 4096 sentinel count for unbounded arrays and never emits `VARIABLE_DESCRIPTOR_COUNT_BIT`, while `BindlessTable` builds its real set-0 layout from caller capacities with the variable-count flag on the last binding — Vulkan's pipeline-layout compatibility rules make those two layouts incompatible. This task must add the substitution mechanism: extend `PipelineLayoutBuilder::build` with an optional external set-0 layout parameter (`build(VkDevice, const ShaderLayoutInfo&, VkDescriptorSetLayout externalSet0 = VK_NULL_HANDLE)`) that, when provided, uses the caller's layout handle for set 0 (skipping creation, NOT owning it in the bundle — document ownership) while still validating that the reflected set-0 bindings are a subset-compatible shape (binding numbers + descriptor types match; counts within capacity). The sample then passes `bindlessTable.descriptorSetLayout()` as the external layout. A focused unit test for the new parameter (mismatched-type rejection + happy path) is required alongside the sample. Uniform per-draw index → NO `NonUniformResourceIndex` [R:B3/E4]. Depth buffer via Texture2D. Camera orbits in present mode.
Headless gate: render one frame offscreen 256x256, assert ≥3 distinct texture samples appear at probe pixels (known geometry positions), validation clean, exit codes.

**Verify:** headless gate in ctest; present mode verified; both presets; commit clean.

---

