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

