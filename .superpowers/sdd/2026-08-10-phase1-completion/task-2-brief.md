### Task 2: VMA integration done right (Allocator + host-visible Buffer)

**Files:**
- Modify: `third_party/CMakeLists.txt` (VMA v3.4.0 via guarded `FetchContent_Populate` + `set(vma_SOURCE_DIR ... PARENT_SCOPE)` — mirror the volk block and its comment style; VMA's own CMakeLists must NOT be added)
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/buffer.h`, `src/rx_rhi_vk/src/buffer.cpp`, `src/rx_rhi_vk/src/vma_impl.cpp`
- Create: `src/rx_rhi_vk/tests/buffer_test.cpp`
- Modify: `src/rx_rhi_vk/CMakeLists.txt` (add sources; add `${vma_SOURCE_DIR}/include` to PUBLIC include dirs)

**Interfaces:**
- Consumes: `Context`, `Device` (Task 1).
- Produces: `rx::rhi::Allocator` with `static create(Context&, Device&) -> std::optional<Allocator>`, `static createRaw(VkPhysicalDevice, VkDevice, VkInstance) -> std::optional<Allocator>` (shared impl; `create` delegates to `createRaw`), `createHostVisibleBuffer(VkDeviceSize, VkBufferUsageFlags) -> std::optional<Buffer>`; `rx::rhi::Buffer` with `handle()`, `mappedData()`, `size()`. Both move-only RAII.

**The three defects this task must not reproduce (the original plan had all three):**
1. **Function pointers:** use `#define VMA_STATIC_VULKAN_FUNCTIONS 0` and `#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1` (both defined before every `#include <vk_mem_alloc.h>` — put them in `buffer.h` above the include). Fill `VmaVulkanFunctions` with ONLY `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` (volk provides both globals) and let VMA fetch everything else itself. Do NOT hand-fill a partial 1.0-era table: with `vulkanApiVersion = VK_API_VERSION_1_3`, VMA requires the `*2` variants (`vkGetBufferMemoryRequirements2`, `vkBindBufferMemory2`, `vkGetPhysicalDeviceMemoryProperties2`, …) and asserts/fails if they're missing.
2. **Implementation TU:** `src/rx_rhi_vk/src/vma_impl.cpp` contains exactly:
   ```cpp
   #define VMA_STATIC_VULKAN_FUNCTIONS 0
   #define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
   #define VMA_IMPLEMENTATION
   #include <volk.h>
   #include <vk_mem_alloc.h>
   ```
   Nothing anywhere else defines `VMA_IMPLEMENTATION`.
3. **CMake scope:** `vma_SOURCE_DIR` must be explicitly propagated `PARENT_SCOPE` in third_party/CMakeLists.txt (same sibling-scope reason as volk — copy that block's comment rationale).

Buffer creation: `VMA_MEMORY_USAGE_AUTO` + `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT`; store `allocationInfo.pMappedData`. All failures `RX_LOG_ERROR` + nullopt. RAII: Buffer destroys via `vmaDestroyBuffer`; Allocator via `vmaDestroyAllocator`; move-only, nulled moved-from.

**Test** (window → context → surface → device → allocator, all RAII in one scope so destruction order is automatically inverse — allocator/buffer die before Device): write a 3-vertex float pattern into a host-visible vertex buffer, `memcmp` round-trip, size check, zero validation errors.

**Verify:** both presets build; full ctest green on linux-native; commit clean.

---

