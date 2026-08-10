# Task 6 Report: sample_03_bindless_mesh (reflection + bindless + upload integration proof)

Commit: `4f9aa12` on `main`.

## Summary

Implemented both required deliverables: the `PipelineLayoutBuilder`
external-set-0 substitution mechanism (with focused unit tests) and
`samples/03_bindless_mesh`, the Phase 2 integration sample proving
reflection-driven pipeline layouts, `BindlessTable`, and `Uploader` work
together end to end. Also closed the tracked `rx_shader_deploy_runtime_libs()`
directory-scope bug properly (fixed the function itself; migrated
`samples/02_hotreload` off its old workaround). Full `ctest` is green on
both presets (8/8; `rx_rhi_vk_tests` now 30 cases/732 assertions);
`windows-cross-zig`'s cross-compiled binaries run correctly under Wine;
`--present` mode was manually verified on real hardware (screenshot
attached to this report's verification trail, described below). Zero
compiler warnings on either preset for the new file.

## 1. PipelineLayoutBuilder external-set-0 substitution

**Files:** `src/rx_rhi_vk/include/rx_rhi_vk/pipeline_layout.h`,
`src/rx_rhi_vk/src/pipeline_layout.cpp`, `src/rx_rhi_vk/tests/pipeline_layout_test.cpp`.

`build()` gained a third parameter: `VkDescriptorSetLayout externalSet0 = VK_NULL_HANDLE`.
When non-null:

- `setLayouts[0]` becomes `externalSet0` verbatim — no `vkCreateDescriptorSetLayout`
  call for set 0 at all.
- `PipelineLayoutBundle` does **not** own that handle: a new private
  `externalSet0_` flag (carried through move ctor/assignment) makes
  `destroyAll()` skip index 0 whenever it's set, on every cleanup path
  (mid-loop set-layout-creation failure, pipeline-layout-creation failure,
  and the destructor). Documented explicitly in the header's class comment
  and `build()`'s own comment.
- `setCount` is forced to at least 1 when `externalSet0` is supplied, even
  if the shader reflects zero set-0 bindings (or zero bindings at all) —
  the caller explicitly wants set 0 wired to that handle regardless.
- **Shape validation** (host-side, before touching `externalSet0` at all):
  every reflected set-0 binding's number must be one of
  `BindlessTable::kSampledImageBinding/kSamplerBinding/kStorageBufferBinding`
  with **exactly** that slot's descriptor type
  (`SAMPLED_IMAGE`/`SAMPLER`/`STORAGE_BUFFER` respectively); a binding
  declaring a bounded (non-unbounded) count must stay within the existing
  `kUnboundedArrayDescriptorCapacity` ceiling. A shader may use a strict
  subset of the three slots (that's the "subset-compatible" the brief
  asks for) but any binding number/type outside that fixed table is
  rejected with a specific logged reason.
  - **Why a fixed table, not real introspection of `externalSet0`:** Vulkan
    exposes no API to query a `VkDescriptorSetLayout`'s bindings back from
    an opaque handle. `externalSet0` has exactly one real producer in this
    codebase (`BindlessTable::descriptorSetLayout()`), so validating
    against that scheme directly — via `pipeline_layout.cpp` including
    `bindless.h` (same library, no circular dependency, no Slang coupling)
    — is the correct, honest design, not a shortcut. This is a host-side
    pre-pipeline-creation sanity gate; it does not replace (and is
    narrower than) whatever `vkCreateGraphicsPipelines` validation itself
    checks against the SPIR-V's declared bindings.

**Tests added** (`pipeline_layout_test.cpp`, using a real `BindlessTable`
built from the same headless-device fixture the file already had, extended
with a `physicalDevice` field):
1. Happy path — a reflected shape using 2 of the 3 slots (images +
   samplers, a genuine subset) plus a push range → `build()` succeeds,
   `setLayouts[0]` is the *exact* `BindlessTable` handle (not a lookalike).
2. Mismatched-type rejection — binding 0 (the images slot) declared as
   `UNIFORM_BUFFER` instead of `SAMPLED_IMAGE` → rejected.
3. Unknown-binding rejection — binding 7 (no counterpart in the 0/1/2
   scheme) → rejected regardless of its declared type.
4. Zero-bindings-still-wires-set0 — a shader reflecting no bindings at
   all still gets `setLayouts[0] == externalSet0` (setCount forced to 1).

Live run: `rx_rhi_vk_tests` 30 cases / 732 assertions, zero validation
errors, on both linux-native (native) and windows-cross-zig (under Wine).

## 2. CMake fix: `rx_shader_deploy_runtime_libs()` directory-scope bug

**Files:** `src/rx_shader/CMakeLists.txt`, `samples/02_hotreload/CMakeLists.txt`.

Tracked since Task 5's review: the function referenced a plain,
non-CACHE `RX_SLANG_RUNTIME_LIBS` variable `file(GLOB)`'d in its own
*definition* directory scope; CMake resolves unqualified variables inside
a function body through the *calling* scope, so any sibling directory
calling the function got nothing to copy, forcing 02_hotreload to
re-glob the identical pattern itself as a workaround.

**Fix:** the function now re-globs internally, using the already-CACHE-INTERNAL
`RX_SLANG_TARGET_ROOT` (visible in any directory scope) — every caller,
in any directory, gets a correct glob with nothing to set up first. The
dead module-level glob in `rx_shader/CMakeLists.txt` was removed (now
redundant); `samples/02_hotreload/CMakeLists.txt`'s local workaround was
deleted and it now just calls the function directly, same as
`samples/03_bindless_mesh` does. Verified: both `rx_shader_link_smoketest`,
`rx_shader_tests`, `sample_02_hotreload`, and `sample_03_bindless_mesh`
all get their Slang runtime libs deployed correctly on a clean
reconfigure+rebuild, on both presets.

## 3. samples/03_bindless_mesh

**Files:** `samples/03_bindless_mesh/{CMakeLists.txt,main.cpp,texture.png}`,
`samples/README.md`, root `CMakeLists.txt`.

### Scene
Procedural cube (24 verts/36 indices, per-face UVs), UV sphere
(16 rings × 24 segments), and a single quad plane — all generated in code,
no importer. 5 object instances (2 cubes, 2 spheres, 1 plane) spaced along
X, each with its own texture: 4 procedurally generated (two checkerboards,
two gradients, distinct colors/cell sizes) plus one real PNG (a 64×64
orange/teal bullseye, generated via PIL and committed at
`samples/03_bindless_mesh/texture.png`) decoded through `stb_image` at
runtime and deployed next to the binary at build time (same
`SDL_GetBasePath()` + post-build-copy pattern 02_hotreload uses for
`hotreload.slang`).

### Reflection-driven layout (the actual point of this sample)
The shader (`kShaderSource` in `main.cpp`, compiled at runtime via
`rx::shader::Compiler::compileFromSource` — the runtime path, not
`slangc`, per the brief's requirement that reflection walk the *actual*
sample shader) declares set 0 with three unbounded arrays matching
`BindlessTable`'s fixed scheme exactly: `Texture2D gTextures[]` (binding
0), `SamplerState gSamplers[]` (binding 1), `StructuredBuffer<float4x4>
gTransforms[]` (binding 2). `reflect()` walks this into a
`ShaderLayoutInfo`; `PipelineLayoutBuilder::build(device, *layoutInfo,
bindlessTable.descriptorSetLayout())` is the substitution call this whole
sample exists to exercise. No `VkDescriptorSetLayoutBinding` is
hand-typed anywhere in this file for set 0.

### Push constants (12 bytes)
`{ transformIndex, textureIndex, samplerIndex }`, all `uint32`, all
**uniform** across a draw call — `NonUniformResourceIndex()` is never
used, matching the spec's discipline (a per-draw push-constant index is
uniform by construction; RDNA2's `...Native=false` cost is the reason
this matters, per the research file).

### Double-buffered transforms without re-registration
Rather than reallocating a fresh bindless storage-buffer slot every frame
or racing a single-buffered transform buffer against a frame still
in-flight (`FrameSync` keeps 2 frames in flight), the sample allocates
**one** storage buffer sized for `framesInFlight() * objectCount` `mat4`
rows, registers it into the bindless table **once**, and each frame
writes only the row range belonging to the frame-in-flight slot currently
being recorded (`transformIndex = frameSync.currentFrameIndex() *
objectCount + objectIndex`). That row range was last read by a draw using
this exact slot, and the present loop already waits on that slot's own
fence before reaching this point — so the write is provably safe with no
extra synchronization beyond what the standard frames-in-flight loop
already provides. Headless mode always uses slot 0. This is documented at
length in `main.cpp`'s file header comment.

### Depth buffer
`rx::rhi::Texture2D`, `VK_FORMAT_D32_SFLOAT`, `requestedMipLevels=1`,
never touched by `Uploader` (created, transitioned once via a **new local
helper**, used purely as a depth attachment) — per the brief's explicit
guidance to sidestep `texture.cpp`'s blit-path color-aspect assumption
entirely by not requesting mips on the depth image.

### Camera
Orbits the origin in the XZ plane; `--present` mode drives the orbit
angle from elapsed time, headless mode evaluates the same function at
`t=0` (a fixed, well-known front view) — one orbit function, not two
camera paths.

## 4. Two real bugs found and fixed via direct empirical verification

Both were caught by actually running the sample and inspecting output —
not by reasoning alone — per the standing verification discipline.

1. **Matrix layout mismatch.** The first working build passed compilation
   but rendered severely distorted, near-degenerate geometry (confirmed by
   dumping the offscreen buffer to a raw file and converting to a viewable
   PNG). Root cause: GLM stores `mat4` column-major; Slang's default
   `float4x4` read out of a `StructuredBuffer<float4x4>` element (no
   `row_major`/`column_major` qualifier) interprets the same 16 floats as
   row-major. Fix: `glm::transpose()` the MVP once on the host before
   upload (`updateTransforms()`), documented at the call site with the
   empirical evidence, not just the theory.
2. **Hardcoded color aspect on the depth transition.** The first run with
   correct matrices still logged a real validation error:
   `rx::rhi::transitionImage()` (`command.cpp`) hardcodes
   `VK_IMAGE_ASPECT_COLOR_BIT`, which is wrong for a depth-only format —
   caught applying that shared helper to the depth image. Fix: a local
   `transitionDepthImage()` twin in `main.cpp` (same barrier shape,
   `VK_IMAGE_ASPECT_DEPTH_BIT`), used at all 4 depth-transition call sites
   (initial transition + swapchain-resize recreation, both run modes).

After both fixes: `vkCreateGraphicsPipelines`/render/readback path is
validation-clean, and a direct screenshot of `--present` mode (captured
via `import -window` against the real X display during this task, RTX
2080) shows all 5 objects rendering correctly and distinctly — red/white
checkerboard cube, blue→green gradient cube, yellow/blue checkerboard
sphere, magenta→cyan gradient sphere, and the real-PNG orange/teal
bullseye plane, viewed under a mid-orbit camera angle with visible
perspective foreshortening on the plane.

## 5. Verification performed

- `ctest --preset linux-native`: **8/8 green** (`shader_spirv_test`,
  `rx_core_tests`, `rx_platform_tests`, `rx_shader_tests`,
  `rx_rhi_vk_tests` — 30/30, 732 assertions, `sample_01_triangle_headless`,
  `sample_02_hotreload_headless`, `sample_03_bindless_mesh_headless`).
- `cmake --preset windows-cross-zig` configures; `cmake --build` builds
  all 9 changed/new targets clean, zero warnings.
- `ctest --preset windows-cross-zig` (via Wine, `CMAKE_CROSSCOMPILING_EMULATOR`):
  **8/8 green**, including `sample_03_bindless_mesh_headless` reporting
  byte-identical probe values to the native Linux run.
- `--present` mode manually run on this machine's real hardware (NVIDIA
  RTX 2080, driver 580.82.07): window opens, camera orbits, survived 3
  live resizes via `xdotool` (500×400 → 1100×800 → 700×700), closed
  cleanly via `SIGTERM` (exit 0), zero unexpected validation errors (only
  the two pre-existing documented false positives — the
  `VK_KHR_portability_enumeration` layer warning and the Slang
  `SourceLanguage` operand warning both already covered by
  `context.cpp`'s known-false-positive guards). A screenshot was captured
  and visually confirmed correct.
- Build budget: `tools/check_build_budget.sh linux-native 60` → incremental
  build **2s**, well within budget (informational; Task 8 owns the formal
  CI gate).
- `git log --format='%B' -1 | grep -i` for AI-attribution strings: no
  matches.

## 6. Deviations / notes for the coordinator

- **No `MANUAL_VERIFICATION.md` section added** for this sample, despite
  having manually verified `--present` mode on real hardware this task.
  Deliberate: that file is scoped entirely to `sample_01_triangle` (three
  per-platform checklists, no other sample present) — `samples/02_hotreload`
  did not get a section either despite also having a `--present` mode.
  Followed that same precedent rather than introduce an inconsistent
  one-off; `samples/README.md`'s new "03_bindless_mesh" section documents
  the expected `--present` behavior instead, matching 02_hotreload's own
  documentation split.
- **Debug-dump code was added temporarily during development** (an
  env-var-gated raw pixel dump used to diagnose the matrix-layout bug)
  and **removed** before finalizing — not present in the committed file.
- Capacities for this sample's `BindlessTable` (32 images / 8 samplers /
  8 storage buffers) are deliberately generous small round numbers, not
  tuned to the exact 5/2/1 real usage — cheap headroom, no correctness
  implication.
- `rx::rhi::PipelineLayoutBuilder::kUnboundedArrayDescriptorCapacity` is
  reused as the generic ceiling for a *bounded* set-0 binding's count in
  the new shape check; this is separate from any specific `BindlessTable`
  instance's real capacities (which aren't retrievable from a
  `VkDescriptorSetLayout` handle at all) — documented explicitly in the
  header comment so a future reader doesn't assume it's checking the
  real, live capacity.

## Files touched

- `src/rx_rhi_vk/include/rx_rhi_vk/pipeline_layout.h` — `externalSet0`
  parameter + ownership contract documentation.
- `src/rx_rhi_vk/src/pipeline_layout.cpp` — substitution logic + shape
  validation against `BindlessTable`'s fixed scheme.
- `src/rx_rhi_vk/tests/pipeline_layout_test.cpp` — 4 new tests + fixture
  extension (`physicalDevice`).
- `src/rx_shader/CMakeLists.txt` — `rx_shader_deploy_runtime_libs()` fixed
  to re-glob internally; dead module-level glob removed.
- `samples/02_hotreload/CMakeLists.txt` — migrated off its re-glob
  workaround.
- `samples/03_bindless_mesh/CMakeLists.txt`, `main.cpp`, `texture.png` —
  new sample.
- `samples/README.md` — new "03_bindless_mesh" section + updated
  build/run/test instructions.
- `CMakeLists.txt` (root) — `add_subdirectory(samples/03_bindless_mesh)`.
