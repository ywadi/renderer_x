### Task 3: Device descriptor-indexing enablement + BindlessTable

**Files:**
- Modify: `src/rx_rhi_vk/src/device.cpp` (+header if needed): chain `VkPhysicalDeviceVulkan12Features` via vk-bootstrap `set_required_features_12` enabling exactly: `descriptorIndexing`, `runtimeDescriptorArray`, `descriptorBindingPartiallyBound`, `descriptorBindingVariableDescriptorCount`, `descriptorBindingSampledImageUpdateAfterBind`, `descriptorBindingStorageImageUpdateAfterBind`, `descriptorBindingStorageBufferUpdateAfterBind`, `descriptorBindingUpdateUnusedWhilePending`, `shaderSampledImageArrayNonUniformIndexing`, `shaderStorageBufferArrayNonUniformIndexing` [R:B1/B2 — all confirmed on Deck RADV]. Selection failure → loud startup error naming the missing feature.
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/bindless.h`, `src/rx_rhi_vk/src/bindless.cpp`, `src/rx_rhi_vk/tests/bindless_test.cpp`

**Interfaces produced:**
- `rx::rhi::BindlessTable` — `static create(VkDevice, capacities{sampledImages, samplers, storageBuffers}) -> std::optional<BindlessTable>`: ONE descriptor set (set 0) from an update-after-bind pool, bindings 0/1/2 as runtime arrays with `PARTIALLY_BOUND | UPDATE_AFTER_BIND | VARIABLE_DESCRIPTOR_COUNT` (last binding) per [R:B3]; `descriptorSetLayout()`, `descriptorSet()`; `registerSampledImage(VkImageView, VkImageLayout) -> BindlessHandle` (generational — reuse `rx::core::HandlePool` for index allocation), `registerSampler(VkSampler)`, `registerStorageBuffer(VkBuffer, range)`, `release(BindlessHandle)`; `BindlessHandle::index() -> uint32_t` (the shader-visible index).
- Writes happen immediately via `vkUpdateDescriptorSets` (update-after-bind makes this legal while bound, except for descriptors referenced by executing commands without `UPDATE_UNUSED_WHILE_PENDING` semantics — released slots are only rewritten after release, and release safety versus in-flight frames is Task 4's DeletionQueue's job; document this contract in the header).

**Tests (headless device):** create table (capacities 1024/16/256) → valid handles, zero validation errors; register/release/re-register cycles reuse indices with bumped generations; write a real image view + sampler + buffer and bind the set in a trivial dispatch-free command buffer (bind + no draw) → validation clean; feature-enablement failure path unit-tested by requesting an absurd capacity beyond `maxDescriptorSetUpdateAfterBindSampledImages` → clean error, no crash. Existing Phase 1 tests still green (Device change is additive).

**Verify:** full ctest green; both presets build; commit clean.

---

