# Task 1 Report: rx_rhi_vk::Device (device, queues, swapchain, acquire/present/recreate)

Status: **DONE**

**Update (post-review fix pass):** see "Fix pass: review findings addressed" at the bottom of this report for the three findings from the Approved-with-findings review and how each was resolved.

## What was built

### Step A — `Context` extended to store `vkb::Instance`

- `src/rx_rhi_vk/include/rx_rhi_vk/context.h`: now `#include <VkBootstrap.h>` and stores `vkb::Instance vkbInstance_{}` by value instead of raw `VkInstance`/`VkDebugUtilsMessengerEXT`. Added `const vkb::Instance& vkbInstance() const`. `instance()`/`debugMessenger()` now read through `vkbInstance_.instance`/`.debug_messenger`. Private constructor is now `Context(vkb::Instance, std::shared_ptr<int>)`.
- `src/rx_rhi_vk/src/context.cpp`: dtor and move-assign now guard on `vkbInstance_.instance != VK_NULL_HANDLE` and tear down via a single `vkb::destroy_instance(vkbInstance_)` call (destroys messenger + instance together), matching the brief. Move ctor nulls the moved-from instance handle. The `isKnownPortabilityEnumerationLayerBug` false-positive guard and the `errorCount_` mechanism are untouched.
- Verified in isolation before moving to Step B: rebuilt `rx_rhi_vk_tests`, reran just the context test — passed, no regression — then confirmed both presets (`linux-native` build+test, `windows-cross-zig` build) were clean before starting Step B.

### Step B — `Device`

- `src/rx_rhi_vk/include/rx_rhi_vk/device.h` / `src/rx_rhi_vk/src/device.cpp` (new).
- `SwapchainStatus{Ok, NeedsRecreate, DeviceLost}` and `AcquireResult{status, imageIndex}` as specified.
- `Device::create(Context&, VkSurfaceKHR)`: `VkPhysicalDeviceVulkan13Features` (dynamicRendering + synchronization2) → `vkb::PhysicalDeviceSelector` (surface, min version 1.3, features13) → `vkb::DeviceBuilder(...).build()` → `volkLoadDevice` → `get_queue`/`get_queue_index` for graphics, `get_queue` for present → `vkb::SwapchainBuilder(vkbDevice, surface).build()` → `get_images()`. Every failure path logs the vkb error message via `RX_LOG_ERROR` and unwinds everything already created (swapchain → device → surface, using `vkb::destroy_swapchain`/`vkb::destroy_device`/`vkb::destroy_surface` while the vkb wrapper objects are still in scope) before returning `std::nullopt`.
- Ownership: `Device::create` takes ownership of the surface unconditionally — on success the returned `Device` owns and destroys swapchain → device → surface (in that order, via raw `vkDestroySwapchainKHR`/`vkDestroyDevice`/`vkDestroySurfaceKHR`, since `Device` stores raw handles, not vkb wrapper types, per the required accessor signatures); on failure `Device::create` destroys the surface itself. Documented on the class in `device.h`.
- Move-only: private default ctor relies on the members' default initializers (`VK_NULL_HANDLE`/`0`/`VK_FORMAT_UNDEFINED`/`{0,0}`); move ctor delegates to move-assign (`: Device() { *this = std::move(other); }`); move-assign destroys its own current resources, takes over `other`'s state, then nulls `other`.
- `acquireNextImage`/`present`/`recreateSwapchain` implemented exactly per the brief's status-mapping table (`VK_SUBOPTIMAL_KHR` on acquire → `Ok`, on present → `NeedsRecreate`; `VK_ERROR_DEVICE_LOST` → logged + `DeviceLost`; anything else → logged + `NeedsRecreate`/`false`).
- `recreateSwapchain`: `vkDeviceWaitIdle` → destroy old swapchain → `vkb::SwapchainBuilder(physicalDevice_, device_, surface)` (raw-handle ctor, confirmed to match the brief exactly against the pinned commit's `VkBootstrap.h`) → refresh images/format/extent/surface. On `get_images()` failure after a successful rebuild, the just-built swapchain is destroyed via `vkb::destroy_swapchain` before returning `false`, to avoid leaking it.

### Step C — tests

- `src/rx_rhi_vk/tests/device_test.cpp` (new): a shared `makeFixture()` helper (hidden window → extensions → validated `Context::create` → `createVulkanSurface`) used by both cases, with the same skip-guard pattern as `rx_platform`'s tests (but it does not skip on this machine — see Verify below).
  1. `Device::create` test: asserts device/queues/swapchain non-null, `swapchainImages().size() > 0`, format != `VK_FORMAT_UNDEFINED`, extent non-zero, `!hasValidationErrors()`.
  2. Acquire/present round-trip: one throwaway `VkCommandPool`/buffer, `acquireNextImage` → `vkCmdPipelineBarrier` (`UNDEFINED → PRESENT_SRC_KHR`, classic sync1 barrier, `VK_QUEUE_FAMILY_IGNORED` on both sides since the swapchain's sharing mode already covers cross-queue-family use if graphics/present differ) → `vkQueueSubmit` waiting on the acquire semaphore at `COLOR_ATTACHMENT_OUTPUT`, signaling a second semaphore + a fence → `vkWaitForFences` → `present()` (asserted `!= DeviceLost`) → `vkDeviceWaitIdle` **before** destroying the semaphores/fence/pool (the fence only covers the submit, not the present itself, so the extra idle-wait is what actually guarantees the presentation engine is done with the semaphore before it's destroyed).
- `src/rx_rhi_vk/tests/doctest_main.cpp` (new) + `context_test.cpp` trimmed to drop `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`: with two test files now, `rx_rhi_vk_tests` follows the same shared-`doctest_main.cpp` pattern as `rx_core_tests`.
- `src/rx_rhi_vk/CMakeLists.txt`: added `src/device.cpp` to the library; `rx_rhi_vk_tests` now built from `doctest_main.cpp` + `context_test.cpp` + `device_test.cpp`, linking `rx_platform` in addition to `rx_rhi_vk`/`doctest::doctest`.

## A real bug found and worked around: vk-bootstrap's process-wide function-pointer cache

While bringing up the two new test cases, `Device::create`'s very first `PhysicalDeviceSelector::select()` call **segfaulted** whenever it ran after `context_test.cpp`'s existing headless `Context::create({}, true)` test had already run in the same process. Root-caused with gdb (not guessed):

```
#0  0x0000000000000000 in ?? ()
#1  vkb::detail::get_present_queue_index (...) at VkBootstrap.cpp:1103
#2  vkb::PhysicalDeviceSelector::is_device_suitable (...) at VkBootstrap.cpp:1187
#3  vkb::PhysicalDeviceSelector::select_devices (...) at VkBootstrap.cpp:1342
#4  vkb::PhysicalDeviceSelector::select (...) at VkBootstrap.cpp:1372
#5  rx::rhi::Device::create (...) at device.cpp:26
```

`VkBootstrap.cpp:1103` calls `detail::vulkan_functions().fp_vkGetPhysicalDeviceSurfaceSupportKHR(...)` — a **null** function pointer. Read through `VkBootstrap.cpp`: `detail::vulkan_functions()` is a function-local `static` singleton, and `VulkanFunctions::init_instance_funcs(VkInstance inst)` is guarded by `if (instance_functions_initialized) return;` — it resolves every instance-level function pointer (via `vkGetInstanceProcAddr(instance, name)`) exactly once per process, from whichever `vkb::Instance` happens to be built first, and never refreshes them. `vkb::destroy_instance` does not reset this cache either (there is a `deinit_all()` that would, but nothing in vk-bootstrap's public API calls it). Since `context_test.cpp`'s Context is headless (`Context::create({}, true)` → `requiredExtensions.empty()` → `set_headless(true)`), it never enables `VK_KHR_surface`, so `vkGetInstanceProcAddr` on that instance legitimately returns null for `vkGetPhysicalDeviceSurfaceSupportKHR` — and that null gets cached **permanently for the whole process**, poisoning every later, fully-windowed `PhysicalDeviceSelector`/`DeviceBuilder`/`SwapchainBuilder` call, regardless of what extensions *they* enable. Confirmed empirically: running only the two Device tests (no headless Context ever built first) passes cleanly every time; interleaving the headless Context test before them reliably segfaults; doctest's default test order is `--order-by=file`, which sorts purely by `__FILE__` string (`context_test.cpp` < `device_test.cpp`), independent of link order — so this wasn't something reordering `CMakeLists.txt`'s source list could have fixed.

This is a genuine defect/limitation of the pinned vk-bootstrap commit (`556b79b165386f6c1a18362d30f2a076fdaa2778`), not something fixable from our side without patching vk-bootstrap itself. The fix applied is a deliberate, self-documenting test-ordering constraint, not a silent hack:
- `context_test.cpp`'s existing `TEST_CASE` (assertions unchanged) is decorated with `* doctest::test_suite("zz_run_after_windowed_device_tests")`; `device_test.cpp`'s two cases stay in the default (`""`) suite, which always sorts first under `--order-by=suite`.
- `src/rx_rhi_vk/CMakeLists.txt`'s `add_test` now passes `--order-by=suite` explicitly, so `ctest` reliably builds a fully-extensioned windowed `Context`/`Device` first, warming the process-wide cache with valid pointers, before the headless test ever runs.
- Both files carry comments pointing at this report's root cause (file/line in `VkBootstrap.cpp`, the exact null function pointer) so a future maintainer hitting something similar doesn't have to re-derive it.

This is a real constraint worth flagging forward: **any process that builds more than one `vkb::Instance` across its lifetime, where an earlier one enables a narrower extension set than a later one needs, will hit this.** It's not just a test artifact — a real application recreating `rx::rhi::Context` (e.g. device-lost recovery, or a tool that legitimately runs headless before going windowed) could hit the identical null-function-pointer crash. Flagging for whoever picks up later device-loss/recreation work.

## Verify

`cmake --build --preset linux-native` (clean, from a removed `build/linux-native`) — all targets built without errors or warnings-as-issues.

`ctest --preset linux-native --output-on-failure`:
```
Test project /media/ywadi/second/renderer_x/build/linux-native
    Start 1: rx_core_tests
1/3 Test #1: rx_core_tests ....................   Passed    0.00 sec
    Start 2: rx_platform_tests
2/3 Test #2: rx_platform_tests ................   Passed    0.06 sec
    Start 3: rx_rhi_vk_tests
3/3 Test #3: rx_rhi_vk_tests ..................   Passed    0.86 sec

100% tests passed, 0 tests failed out of 3
```
Repeated 3x in a row (plus once more after the clean rebuild) with identical results — no flakiness observed. Direct run of `rx_rhi_vk_tests --order-by=suite --success` shows all 3 cases passing with 31/31 assertions, including `swapchainImages().size() == 4`, `swapchainFormat() == 50` (`VK_FORMAT_B8G8R8A8_SRGB`), `64x64` extent, and `!hasValidationErrors()` in every case. The only validation-layer output throughout is the pre-existing, documented `isKnownPortabilityEnumerationLayerBug` false positive (logged as a warning, not counted as an error) — confirmed this machine is genuinely running against a real display (`DISPLAY=:1`) and a real Vulkan 1.4 NVIDIA RTX 2080 driver (`vulkaninfo` confirms `apiVersion 1.4.312`, `DRIVER_ID_NVIDIA_PROPRIETARY`), not skipping.

`cmake --build --preset windows-cross-zig` (clean, from a removed `build/windows-cross-zig`) — configures and builds all targets (including `rx_rhi_vk_tests.exe`) without errors.

## Deviations from the brief, and why

1. **vk-bootstrap process-wide function-pointer cache workaround** (see above) — not mentioned in the brief because the brief couldn't have anticipated a latent defect in the pinned dependency; discovered only by actually running the two new tests together with the pre-existing one. Addressed via explicit, documented test-suite ordering rather than any change to `Context`/`Device`'s production logic.
2. Everything else matches the brief's assumed vk-bootstrap API exactly as checked against `VkBootstrap.h` at the pinned commit (`556b79b165386f6c1a18362d30f2a076fdaa2778`): the raw-handle `SwapchainBuilder(VkPhysicalDevice, VkDevice, VkSurfaceKHR, uint32_t = QUEUE_INDEX_MAX_VALUE, uint32_t = QUEUE_INDEX_MAX_VALUE)` constructor exists verbatim, `Device::get_queue`/`get_queue_index`, `Swapchain::get_images()`, `destroy_surface`/`destroy_device`/`destroy_swapchain`/`destroy_instance` all match what the brief assumed — no other adaptation was needed.

## Self-review

- Ownership/lifetime: verified by inspection and by the round-trip test's explicit teardown order (pool/fence/semaphores destroyed, then `vkDeviceWaitIdle`'d, before the `Device`/`Context` optionals go out of scope in reverse declaration order — `Device` before `Context`, so the surface is destroyed via a still-valid `VkInstance`).
- Move semantics: `Device`'s move-assign is self-move-guarded and nulls every field of the moved-from object; `Context`'s Step A refactor keeps the exact same guard/teardown shape it had before, just through `vkb::destroy_instance`.
- No leaks on failure paths in `Device::create`: each failure step destroys everything successfully created so far (traced through all 6 failure branches).
- Zero validation errors confirmed on every test run, not just asserted in code.
- Did not touch `.superpowers/sdd/.../progress.md`, `task-4-*`, `.claude/`, or other files touched by concurrent work in this repo — staged and committed only the files this task owns.

## Concerns

- The vk-bootstrap caching defect (above) is real and will resurface if: (a) a future test binary in this project ever builds a headless-then-windowed (or otherwise extension-mismatched) sequence of `vkb::Instance`s without the same `--order-by=suite` discipline, or (b) real runtime code (not just tests) ever recreates `rx::rhi::Context` with a different extension set mid-process. Worth a forward note for whoever designs device-lost/context-recreation handling.
- `recreateSwapchain`'s surface parameter does not take ownership (per the brief's wording, which only discusses destroying the *old swapchain*, not the surface) — in the intended usage the same surface handle is passed back in on every call. This is documented on the header but is a slightly implicit contract worth double-checking against Task 3/4's actual call site once written.

## Fix pass: review findings addressed

The review came back **Approved-with-findings**: the vk-bootstrap diagnosis was independently verified against upstream source and confirmed exact, but the reviewer reproduced a SIGSEGV running `rx_rhi_vk_tests` *without* `--order-by=suite`, and ruled the `--order-by=suite` workaround too fragile (doctest's suite sort falls back to filename order on ties, so any future test file alphabetically before `device_test.cpp` that builds a narrower-capability Context would silently reintroduce the crash). Three findings, all addressed:

**1. (IMPORTANT) No documentation of the defect in production code.** Added a WARNING doc comment directly on `Context::create()` in `src/rx_rhi_vk/include/rx_rhi_vk/context.h`, describing the process-wide vk-bootstrap function-pointer cache, which specific function pointers are affected (surface-support functions for `PhysicalDeviceSelector`, debug-messenger create/destroy for validation — see finding 2 below for why both matter), what the observable symptoms are (segfault vs. a clean `Context::create` failure), and what a future implementer of Context/Device recreation needs to do about it. This is the first place a future device-lost-recovery or multi-Context implementer would actually look.

**2. (IMPORTANT) Replaced the filename-fragile ordering with a structural fix.** `src/rx_rhi_vk/tests/doctest_main.cpp` no longer uses `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`; it now uses `DOCTEST_CONFIG_IMPLEMENT` plus an explicit `main()` that calls a `warmUpVkBootstrapInstanceFunctionCache()` helper *before* constructing/running `doctest::Context`. That helper builds (and immediately lets go out of scope, destroying) one throwaway `rx::rhi::Context` using the broadest capability set any test in this binary needs: real window instance extensions via `rx::platform::Window::create(...)->requiredVulkanInstanceExtensions()` when a display is available (falling back to headless when not — the windowed Device tests skip themselves on such machines anyway), **and `enableValidation=true`**.
   - The `enableValidation=true` part was not incidental — it was a second instance of the *exact same defect class*, caught only by actually running the fix: my first attempt warmed up with `enableValidation=false` (reasoning: "just get surface support cached"), which instead cached a **null `fp_vkCreateDebugUtilsMessengerEXT`** (that function is only resolved non-null when the first instance in the process actually requests validation layers / a debug callback). That broke every subsequent validated `Context::create()` in the process with a clean-looking `vkb::InstanceBuilder::build failed: failed_create_debug_messenger` — caught immediately by `ctest --preset linux-native` going from 100% to 25% passing. Fixed by warming up with `enableValidation=true` too, and documented both angles (surface support *and* debug messenger) in the warm-up function's own comment and in `Context::create`'s doc comment, so "broadest instance" is understood to mean broadest along every axis vk-bootstrap caches per-process, not just extensions.
   - Removed the now-unnecessary `doctest::test_suite("zz_run_after_windowed_device_tests")` decorator from `context_test.cpp` and the `--order-by=suite` flag from `CMakeLists.txt`'s `add_test` line; replaced their comments with pointers to the real, structural explanation in `doctest_main.cpp`.
   - Proved the fix is order-independent, not just re-passing under the same lucky default order: ran `./rx_rhi_vk_tests --order-by=rand --rand-seed=N` for `N` swept from 1–40, found several seeds (4, 5, 17, 21, 28, 29, 30, 33, 35, 37, 40) where the random order puts the headless `Context::create` test **first** — the exact original crash precondition — and confirmed all of them still pass (31/31 assertions, 0 failed). Also ran plain `./rx_rhi_vk_tests` with no flags (doctest's own default, `--order-by=file`) — passes.

**3. (MINOR) `Device::recreateSwapchain` surface-mismatch guard.** Added a check at the top of `recreateSwapchain` in `src/rx_rhi_vk/src/device.cpp`: if the passed `surface` differs from the `Device`'s currently-owned `surface_`, logs `RX_LOG_WARN` naming both handles (as `void*`, since `VkSurfaceKHR` isn't directly formattable) and explaining that the previous surface will not be destroyed by this call. Does not change behavior (still overwrites `surface_` — that's the documented contract), just makes the mismatch visible instead of silent.

### Verification commands + actual output (this fix pass)

Clean incremental rebuild (note: the build tree picked up unrelated concurrent Task 4/Phase 2 changes mid-session — a `shader_spirv_test` target and Slang shader compilation now appear in the same build; not this task's work, left untouched):

```
$ cmake --build --preset linux-native
[... 7/7 ...] Linking CXX executable src/rx_rhi_vk/rx_rhi_vk_tests

$ ctest --preset linux-native --output-on-failure
Test project .../build/linux-native
    Start 1: shader_spirv_test
1/4 Test #1: shader_spirv_test ................   Passed    0.00 sec
    Start 2: rx_core_tests
2/4 Test #2: rx_core_tests ....................   Passed    0.00 sec
    Start 3: rx_platform_tests
3/4 Test #3: rx_platform_tests ................   Passed    0.06 sec
    Start 4: rx_rhi_vk_tests
4/4 Test #4: rx_rhi_vk_tests ..................   Passed    0.92 sec
100% tests passed, 0 tests failed out of 4
```
Repeated 3x — stable, no flakiness.

```
$ ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests            # default order (doctest's own --order-by=file)
[doctest] test cases:  3 |  3 passed | 0 failed | 0 skipped
[doctest] assertions: 31 | 31 passed | 0 failed |
[doctest] Status: SUCCESS!

$ for seed in 1 2 42 12345 4 5 17 40; do
    ./build/linux-native/src/rx_rhi_vk/rx_rhi_vk_tests --order-by=rand --rand-seed=$seed
  done
# every seed: 3 | 3 passed | 0 failed | 0 skipped; 31 | 31 passed | 0 failed; SUCCESS
# seeds 4, 5, 17, 40 specifically confirmed (via --list-test-cases first) to run
# "Context::create ..." (the headless one) FIRST -- the original crash precondition --
# and still pass cleanly.
```

```
$ cmake --build --preset windows-cross-zig
[... 11/11 ...] Linking CXX executable src/rx_rhi_vk/rx_rhi_vk_tests.exe
```

### Files touched in this fix pass

- `src/rx_rhi_vk/include/rx_rhi_vk/context.h` — WARNING doc comment on `Context::create`.
- `src/rx_rhi_vk/tests/doctest_main.cpp` — switched to `DOCTEST_CONFIG_IMPLEMENT` + explicit `main()` with the warm-up.
- `src/rx_rhi_vk/tests/context_test.cpp` — dropped the suite decorator/comment, replaced with a one-line pointer to the structural fix.
- `src/rx_rhi_vk/CMakeLists.txt` — dropped `--order-by=suite`; updated comment.
- `src/rx_rhi_vk/src/device.cpp` — `recreateSwapchain` surface-mismatch `RX_LOG_WARN`.

### Self-review of this fix pass

- Confirmed the fix addresses the *actual* mechanism (structural, order-independent) rather than papering over the reviewer's specific repro — proved with a seed sweep specifically targeting the original failure precondition, not just re-running the previously-passing case.
- Caught my own regression (the `enableValidation=false` warm-up breaking validated contexts) via the same verification discipline — full `ctest`, not just the happy path — before it could have shipped.
- Did not touch `.superpowers/sdd/.../progress.md`, `docs/superpowers/plans/2026-08-10-phase2-shader-resource.md`, or any other file modified by concurrent work in this shared working tree during this session; staged and committed only the five files this fix pass owns.
