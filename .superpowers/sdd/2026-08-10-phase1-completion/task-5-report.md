# Task 5 report: Triangle correctness gate — offscreen render + pixel readback

Branch: `main` (worked directly, as authorized)

## What was built

- `samples/01_triangle/main.cpp` — headless-mode triangle correctness gate.
  Builds the full stack (hidden 256x256 `rx::platform::Window` →
  `Context::create` with real window extensions + validation →
  `window->createVulkanSurface` → `Device::create` → `Allocator::create` →
  `CommandContext::create`), then creates a **dedicated offscreen
  `VkImage`** (256x256, `device->swapchainFormat()`,
  `COLOR_ATTACHMENT_BIT | TRANSFER_SRC_BIT`, device-local, raw
  `vkCreateImage`/`vkAllocateMemory`/`vkBindImageMemory`/`vkCreateImageView`
  — the swapchain itself is built and queried but never written to, per the
  brief's core correctness rule). Loads `RX_TRIANGLE_VERT_SPV`/`_FRAG_SPV`,
  builds a dynamic-rendering pipeline (empty layout, no vertex input,
  dynamic viewport/scissor, no cull, no blend, 1 sample,
  `VkPipelineRenderingCreateInfo` with the target format), records via
  `runOnce` (transition → clear-black `vkCmdBeginRendering` → set
  viewport/scissor → bind → `vkCmdDraw(3)` → end → transition to
  `TRANSFER_SRC_OPTIMAL`), reads back via a second `runOnce` +
  `Allocator::createHostVisibleBuffer`, asserts center `(128,150)` white
  (`>200`/channel), corner `(10,10)` black (`<20`/channel), and
  `!ctx->hasValidationErrors()`. Logs `triangle readback PASSED`/`FAILED`
  and returns 0/1. All raw (non-RAII) handles — image, memory, view, both
  shader modules, pipeline layout, pipeline — are torn down explicitly via
  a local `destroyRawResources()` lambda (called on every early-failure
  path and at the natural end) while `device` is still alive; every RAII
  object (Buffer, CommandContext, Allocator, Device, Context, Window) is a
  function-scope local, destroyed automatically in reverse declaration
  order at the end of `main()` — no `exit()`/`std::exit()` anywhere in this
  file, since that would skip C++ stack unwinding and leave Vulkan objects
  dangling when the instance/device tear down (which the validation layer
  does flag).
- `samples/01_triangle/CMakeLists.txt` — `add_executable(sample_01_triangle
  main.cpp)`, links `rx_rhi_vk` + `rx_platform`, injects
  `RX_TRIANGLE_VERT_SPV`/`_FRAG_SPV` via `target_compile_definitions`,
  `add_dependencies(... triangle_shaders)`, and
  `add_test(NAME sample_01_triangle_headless COMMAND sample_01_triangle)`.
- Root `CMakeLists.txt`: added `add_subdirectory(samples/01_triangle)` at
  the end (after `rx_rhi_vk`/`rx_platform`/`shaders` are already defined,
  so `target_link_libraries`/`add_dependencies` by name resolve correctly).

## Two real defects found and fixed (both driver-tolerated, not spec-valid — exactly this task's mandate)

Running the sample against this machine's real GPU + validation layer
surfaced two genuine spec-validity gaps, neither hypothetical:

1. **Missing `shaderDrawParameters` device feature.** Slang's SPIR-V
   backend translates HLSL's `SV_VertexID` to `gl_VertexIndex -
   gl_BaseVertex` (to reproduce HLSL's zero-based semantics regardless of a
   nonzero `firstVertex`), which declares `OpCapability DrawParameters` and
   a `BaseVertex` `BuiltIn` input — confirmed directly via `spirv-dis` on
   the compiled `triangle.vert.spv`. `Device::create` (Task 1) requested
   Vulkan 1.3 core features only; without `shaderDrawParameters` (Vulkan
   1.1 core, promoted from `VK_KHR_shader_draw_parameters`) also requested,
   `vkCreateShaderModule` is a validation error
   (`VUID-VkShaderModuleCreateInfo-pCode-01091`). **Fix:** added
   `VkPhysicalDeviceVulkan11Features{shaderDrawParameters = VK_TRUE}` to
   `Device::create`'s existing `PhysicalDeviceSelector` chain
   (`src/rx_rhi_vk/src/device.cpp`) — one line of new state, no extension
   string needed (core since 1.1, and `set_minimum_version` is already
   ≥1.1), no behavior change for any existing consumer. Fully documented
   inline at the point of use.
2. **Unrecognized SPIR-V `SourceLanguage=Slang` in the installed validation
   layer.** slangc 2026.14.1 unconditionally emits `OpSource Slang 1` into
   every module (verified: recompiling with `-g0` — strip debug info —
   still emits it; this is base module-provenance metadata, not the
   optional embedded-source-text feature `-g`/`-debug-info-include-source`
   controls). `Slang = 11` is a legitimate, currently-registered SPIR-V
   `SourceLanguage` enum value (confirmed against a current
   SPIRV-Headers `spirv/unified1/spirv.h`), but this machine's
   apt-packaged `vulkan-validationlayers` (1.3.204.1 — the same package
   version already implicated in `context.cpp`'s existing portability
   -enumeration false-positive guard) bundles a SPIRV-Tools build from
   before that addition, so its `OpSource` operand-range check
   (`UNASSIGNED-CoreValidation-Shader-InconsistentSpirv`) rejects it. Zero
   effect on module semantics/execution — pure debug metadata. **Fix:**
   extended `context.cpp`'s existing narrowly-scoped false-positive-guard
   pattern with a second matcher,
   `isKnownUnrecognizedSlangSourceLanguageBug`, matched on both the
   check's own distinctive message text and its VUID-less `UNASSIGNED`
   category so a genuinely different "module not valid" failure is never
   silently swallowed. Logged as `RX_LOG_WARN`, not counted toward
   `hasValidationErrors()` — same treatment as the existing portability
   guard.

Both fixes are outside this task's originally-listed file set
(`src/rx_rhi_vk/src/device.cpp`, `src/rx_rhi_vk/src/context.cpp` — both
Task 1 files) but squarely inside the plan's own mandate ("Every Vulkan
usage must be spec-valid, not merely driver-tolerated") and this task's
specific charter (closing exactly this kind of driver-tolerated/spec-invalid
gap). Both are minimal, additive, fully documented at the point of change,
and independently verified against primary sources (a `spirv-dis` dump for
the first; a current SPIRV-Headers enum definition plus an empirical
`-g0` recompile for the second) rather than assumed.

## Verification performed

- `cmake --preset linux-native && cmake --build --preset linux-native`:
  clean, only 2 recompiled TUs on the last iteration (`context.cpp`,
  `device.cpp`) plus the new `sample_01_triangle`/relinked
  `rx_rhi_vk_tests`.
- `sample_01_triangle` run directly (`DISPLAY=:1`, real GPU) 4 times:
  every run logs `triangle readback PASSED` and exits 0. Validation output
  shows exactly the two documented, narrowly-matched known-false-positive
  `WARN` lines (portability enumeration, Slang source language) and
  nothing else — confirmed `!ctx->hasValidationErrors()` is genuinely true,
  not merely unchecked.
- `ctest --preset linux-native --output-on-failure`: **5/5 green**
  (`shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`,
  `rx_rhi_vk_tests`, `sample_01_triangle_headless`).
- `cmake --preset windows-cross-zig && cmake --build --preset
  windows-cross-zig`: clean, produces `sample_01_triangle.exe` and the
  updated `rx_rhi_vk_tests.exe`.
- Commit hygiene: `git log -1 --format='%B'` inspected before writing —
  message below contains no AI attribution, matches repo's `CLAUDE.md`
  policy.

## Notes / deviations from the brief worth flagging forward

- The two `device.cpp`/`context.cpp` fixes above (see rationale). Both are
  a natural extension of Task 1's/Task 1's own already-established defect
  -handling precedent in the same files, not a new pattern.
- Pipeline has no `pDepthStencilState` (left `nullptr`): per spec this is
  ignored when the dynamic-rendering `VkPipelineRenderingCreateInfo` has no
  depth/stencil attachment format set (both left `VK_FORMAT_UNDEFINED`
  here, matching "no depth" — this sample never uses one). Confirmed no
  validation complaint at runtime.
- `frontFace`/winding is set but irrelevant since `cullMode = NONE`; kept
  explicit for clarity rather than relying on the (also harmless) default.
- Added a coherence check on the readback path in `main.cpp` mirroring
  `clear_color_test.cpp`'s documented precondition (Buffer/Allocator expose
  no flush/invalidate — tracked for Phase 2's Uploader task per the
  ledger) rather than silently assuming it; on this machine it's a no-op
  pass-through, but it turns a silent assumption into a verified,
  fail-loud one.

## Files touched

- Created: `samples/01_triangle/main.cpp`, `samples/01_triangle/CMakeLists.txt`
- Modified: root `CMakeLists.txt` (`add_subdirectory(samples/01_triangle)`),
  `src/rx_rhi_vk/src/device.cpp` (shaderDrawParameters feature),
  `src/rx_rhi_vk/src/context.cpp` (second false-positive guard)
