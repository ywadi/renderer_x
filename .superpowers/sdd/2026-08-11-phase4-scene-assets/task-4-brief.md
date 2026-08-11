### Task 4: Material texture sampling wiring (seed 10 / carried)

**Files:** Modify `shaders/material/material.slang` + `forward_entry.slang` (bindless texture array + sampler array access for materials: `float4 rx_sampleTexture(uint index, float2 uv)` helper), `src/rx_material/material_system.cpp` (`reflectMaterialLayout()` allow-list accepts the material-side bindless references), tests (+`tests/data/test_textured_sample.slang`).
**Acceptance (the carried bar):** a `createTexture2D`-created texture bound via `setTexture` VISIBLY changes rendered output — GPU test renders a quad with a 2×2 texture through the public API and asserts the four quadrant colors; hot-reload of a textured material keeps working. GUID regen on any ABI-visible change per documented policy.
**Steps:** failing GPU test → implement → suite green both presets, zero validation errors → commit.

