# Task 2 Report: VMA integration (Allocator + host-visible Buffer)

Status: **DONE**

## What was built

### Step A — VMA v3.4.0 fetched (`third_party/CMakeLists.txt`)

Added a block immediately after the existing volk block, mirroring it exactly: `FetchContent_Declare(vma GIT_REPOSITORY .../VulkanMemoryAllocator.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)` → `FetchContent_GetProperties`/`if(NOT vma_POPULATED) FetchContent_Populate(vma) endif()` → `set(vma_SOURCE_DIR "${vma_SOURCE_DIR}" PARENT_SCOPE)`. Comment above it explains both rationales in volk's own words adapted to VMA: (1) why `FetchContent_Populate` (source only) rather than `FetchContent_MakeAvailable` — VMA's own `CMakeLists.txt` only exists to build its sample application, which pulls in GLFW and other dependencies this project has no use for; there is no library target to add, since the implementation is a single TU already covered by `rx_rhi_vk`'s own sources; (2) the same CMake 3.22 sibling-scope reason as volk for why `PARENT_SCOPE` is required — `FetchContent_Populate()` is a function, so `vma_SOURCE_DIR` is scoped to this directory only, and `src/rx_rhi_vk` is a sibling of `third_party`, not a descendant.

Verified directly against the fetched source (not assumed): `vk_mem_alloc.h` lives at `include/vk_mem_alloc.h` (confirmed by inspecting `build/*/​_deps/vma-src/` after a real configure), so `${vma_SOURCE_DIR}/include` is the correct path added to `rx_rhi_vk`'s `target_include_directories`. VMA's `CMakeLists.txt`/`src/CMakeLists.txt` do indeed build a `VulkanSample` target (confirmed present: `src/VulkanSample.cpp`, `src/Tests.cpp`, GpuMemDumpVis tool) — never added via `add_subdirectory`, exactly as required.

### Step B — `rx::rhi::Allocator` / `rx::rhi::Buffer`

New `src/rx_rhi_vk/include/rx_rhi_vk/buffer.h`: defines `VMA_STATIC_VULKAN_FUNCTIONS 0` / `VMA_DYNAMIC_VULKAN_FUNCTIONS 1` immediately above the header's only `#include <vk_mem_alloc.h>`, with a comment explaining the defect this avoids (a hand-filled Vulkan-1.0-era `VmaVulkanFunctions` table omitting the `*2` variants VMA requires once `vulkanApiVersion = VK_API_VERSION_1_3`, which VMA's internal `ValidateVulkanFunctions()` asserts on). Declares `Buffer` first (move-only RAII: `handle()`, `mappedData()`, `size()`; private ctor + `friend class Allocator`), then `Allocator` (move-only RAII: `create(Context&, Device&)`, `createRaw(VkPhysicalDevice, VkDevice, VkInstance)`, `createHostVisibleBuffer(VkDeviceSize, VkBufferUsageFlags) -> std::optional<Buffer>`).

New `src/rx_rhi_vk/src/buffer.cpp`:
- `Allocator::create` delegates to `createRaw(device.physicalDevice(), device.device(), context.instance())` — the one shared implementation, per the brief.
- `Allocator::createRaw` builds a `VmaVulkanFunctions` with **only** `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` set (volk's real global function pointers, populated by `Context::create`'s `volkLoadInstance` and `Device::create`'s `volkLoadDevice`), leaves every other field null, sets `vulkanApiVersion = VK_API_VERSION_1_3`, and calls `vmaCreateAllocator`. On failure: `RX_LOG_ERROR` + `std::nullopt`.
- `Allocator::createHostVisibleBuffer` builds a `VkBufferCreateInfo` (`VK_SHARING_MODE_EXCLUSIVE`) and a `VmaAllocationCreateInfo` with `usage = VMA_MEMORY_USAGE_AUTO`, `flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT`, calls `vmaCreateBuffer`, and on success constructs a `Buffer` from `allocationInfo.pMappedData`. On failure: `RX_LOG_ERROR` + `std::nullopt`.
- Both `Allocator` and `Buffer` move ctor/assign follow the exact same pattern as `Device` (Task 1): move ctor delegates to move-assign via a private default ctor; move-assign destroys its own current resource, takes over the other's fields, then nulls every field of the moved-from object (self-move guarded). Destructors call `destroyAll()`, which guards on the handle being non-null before calling `vmaDestroyAllocator`/`vmaDestroyBuffer`.

New `src/rx_rhi_vk/src/vma_impl.cpp` — exactly the five lines specified in the brief, no more, no less:
```cpp
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <volk.h>
#include <vk_mem_alloc.h>
```
This is the only translation unit anywhere in the codebase that defines `VMA_IMPLEMENTATION` (verified: `grep -r VMA_IMPLEMENTATION src/` returns only this file's own definition and the guard comment in `buffer.h`).

### Step C — `src/rx_rhi_vk/CMakeLists.txt`

Added `src/buffer.cpp` and `src/vma_impl.cpp` to the `rx_rhi_vk` library's sources; added `${vma_SOURCE_DIR}/include` to its `PUBLIC target_include_directories` (alongside the existing `include` and `${volk_SOURCE_DIR}`); added `tests/buffer_test.cpp` to the `rx_rhi_vk_tests` executable. Updated the header comment above the library target to describe both fetched sources (volk + VMA) and to note that `vma_impl.cpp` is the sole `VMA_IMPLEMENTATION` TU.

### Step D — test

New `src/rx_rhi_vk/tests/buffer_test.cpp`: a `BufferTestFixture` (window → validated `Context` → surface → `Device`) one layer deeper than `device_test.cpp`'s own fixture, using the identical skip-guard pattern (returns empty optional with `MESSAGE` when no display/Vulkan-surface backend is available; never silently skips on a real machine). Inside the one `TEST_CASE`:
- `Allocator::create(fixture->context, fixture->device)` — the higher-level convenience overload, exercising the `create`→`createRaw` delegation path.
- `createHostVisibleBuffer(sizeof(3 Vertex structs), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)`.
- Checks: `handle() != VK_NULL_HANDLE`, `mappedData() != nullptr`, `size() == kBufferSize` (36 bytes = 3 × 12-byte `{float x,y,z}`).
- `std::memcpy` a 3-vertex float pattern into `mappedData()`, then `std::memcmp` against the source pattern (`== 0`).
- `CHECK_FALSE(fixture->context.hasValidationErrors())`.

`Allocator`/`Buffer` locals are declared *after* the fixture (window/context/device) purely via ordinary C++ scope rules, so they are destroyed in exact reverse order — `Buffer`, then `Allocator`, both gone before `Device`/`Context`/`Window` tear down — with no manual teardown code, per the brief's requirement.

## Verify

Configured VMA fetch and inspected the real v3.4.0 header directly (not assumed) before writing any allocator code: confirmed `VmaVulkanFunctions`'s field names (`vkGetBufferMemoryRequirements2KHR`, `vkBindBufferMemory2KHR`, `vkGetPhysicalDeviceMemoryProperties2KHR`, `vkGetDeviceBufferMemoryRequirements`/`...Images...` for 1.3), `VmaAllocatorCreateInfo::vulkanApiVersion`, `VmaAllocationCreateInfo::{usage,flags}`, `VmaAllocationInfo::pMappedData`, and the exact `ImportVulkanFunctions_Dynamic()` implementation (VMA_FETCH_INSTANCE_FUNC/VMA_FETCH_DEVICE_FUNC macros conditioned on `m_VulkanApiVersion`) — confirms that supplying only `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` and leaving everything else null is sufficient and exactly what VMA's own dynamic-loading path expects; no adaptation from the brief's assumptions was needed.

Clean rebuild, `linux-native`:
```
$ rm -rf build/linux-native && cmake --preset linux-native      # clean configure, VMA fetched
$ cmake --build --preset linux-native                          # 11/11, zero warnings, zero errors
$ ctest --preset linux-native --output-on-failure
100% tests passed, 0 tests failed out of 4
```
Repeated 3× — stable, no flakiness. Direct run (`./rx_rhi_vk_tests --success`): 4 test cases, 41/41 assertions passed, 0 failed. The buffer test specifically: `size() == 36`, `memcmp(...) == 0`, `hasValidationErrors() == false`. The only logged output besides successes is the pre-existing, already-documented `isKnownPortabilityEnumerationLayerBug` false positive (a `RX_LOG_WARN`, not counted toward `hasValidationErrors()`) — zero real validation errors on every run.

Clean rebuild, `windows-cross-zig`:
```
$ rm -rf build/windows-cross-zig && cmake --preset windows-cross-zig   # clean configure, VMA fetched into its own _deps
$ cmake --build --preset windows-cross-zig                             # 34/34, zero warnings, zero errors
```
`rx_rhi_vk_tests.exe` (including `buffer_test.cpp.obj`, `buffer.cpp.obj`, `vma_impl.cpp.obj`) links successfully.

## The three named defects — how each was avoided

1. **Function pointers**: `buffer.h` defines `VMA_STATIC_VULKAN_FUNCTIONS 0`/`VMA_DYNAMIC_VULKAN_FUNCTIONS 1` above its only `#include <vk_mem_alloc.h>`. `Allocator::createRaw` fills `VmaVulkanFunctions` with only `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` (volk's real globals — verified present as `extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;` / `extern PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;` in `volk.h`), sets `vulkanApiVersion = VK_API_VERSION_1_3`, and lets `vmaCreateAllocator`'s own `ImportVulkanFunctions_Dynamic()` resolve everything else — including the `*2` variants — via those two entry points. No partial hand-filled table anywhere.
2. **Implementation TU**: `src/rx_rhi_vk/src/vma_impl.cpp` is exactly the five lines specified, and is the only place in the codebase defining `VMA_IMPLEMENTATION` (grepped to confirm).
3. **CMake scope**: `vma_SOURCE_DIR` is explicitly `PARENT_SCOPE`'d in `third_party/CMakeLists.txt`, identical in structure and rationale to volk's; confirmed working by a clean configure on both presets picking it up correctly from `src/rx_rhi_vk/CMakeLists.txt` (a sibling directory).

## Deviations from the brief, and why

None. Every VMA v3.4.0 header/API detail the brief assumed (struct field names, `vmaCreateAllocator`/`vmaCreateBuffer`/`vmaDestroyBuffer`/`vmaDestroyAllocator` signatures, `include/vk_mem_alloc.h` path, the sample-app-only `CMakeLists.txt`) was verified directly against the fetched source and matched exactly — no adaptation was required.

## Self-review

- Ownership/lifetime: `Buffer` never outlives its owning `Allocator` in the test (enforced by declaration order + C++ reverse-destruction, documented on the class); `Allocator` never outlives the `Device`/`Context` it was built from, for the same reason.
- Move semantics: both `Allocator` and `Buffer` follow `Device`'s established pattern exactly — self-move-guarded move-assign, full moved-from nulling, move-ctor delegating to move-assign via a private default ctor.
- No leaks on failure paths: `createRaw` returns `std::nullopt` before any resource is created if `vmaCreateAllocator` fails (nothing to clean up); `createHostVisibleBuffer` returns `std::nullopt` before constructing a `Buffer` if `vmaCreateBuffer` fails (same).
- Zero validation errors confirmed by direct test-binary run output, not just asserted in code.
- `git status`/`git diff --stat` checked before committing: only the six files this task owns were staged (2 modified CMakeLists, 4 new files) — no incidental changes to `.superpowers/sdd/.../progress.md` or any other concurrently-touched file.
- Commit message contains no AI attribution (verified via `git log -1 --format='%B'` before finishing, per repo `CLAUDE.md`); author identity untouched.

## Concerns

- None new. The pre-existing vk-bootstrap process-wide function-pointer cache landmine (documented on `Context::create`, worked around structurally in `doctest_main.cpp` since Task 1) is unaffected by this task — `Allocator`/`Buffer` don't touch vk-bootstrap at all, only raw Vulkan handles already resolved by `Context`/`Device`.
- `Buffer`/`Allocator` are currently only exercised end-to-end by one test case; future tasks building on top of `createHostVisibleBuffer` (e.g. a device-local/staged-upload path) will need their own defect-avoidance pass — this task only covers the host-visible, `VMA_MEMORY_USAGE_AUTO`-mapped path the brief scoped it to.
