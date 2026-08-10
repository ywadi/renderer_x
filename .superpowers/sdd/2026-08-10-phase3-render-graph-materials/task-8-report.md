# Task 8 report: sample 06_materials

## Summary

`samples/06_materials` drives `rx_material` exclusively through its public
COM-lite API (`rx_api.h`): loading two materials, creating two
differently-parameterized instances of each, creating a real texture via
`createTexture2D()`/`setTexture()`, and hot-reload polling via
`reloadChanged()`. Internal `rx_material` headers are confined to one
clearly marked bridge section. The headless gate passes with 4 analytic,
channel-order-exact pixel assertions and zero real Vulkan validation
errors. Both presets build; the packaged (unzipped, outside the build
tree) layout runs correctly on native Linux and under Wine.

Two real bugs were found and fixed along the way, both load-bearing for
correctness (not just this sample's own polish): a `Scene`-returned-by-value
dangling-pointer bug in `MaterialSystem::create()`'s caller (this sample's
own code), and a mesh-winding bug exposed by `MaterialSystem`'s fixed
`VK_CULL_MODE_BACK_BIT` rasterization state (also this sample's own code,
not `rx_material`'s).

## Bridge-section design

Per the brief: system creation and draw-time binding have no public
equivalent in Phase 3 (D5/D11). `samples/06_materials/main.cpp` confines
every reference to an internal `rx::material::` type to two explicitly
delimited regions:

1. **`#include` section** (top of file, right after the public
   `rx_api.h` include): the only two non-`rx_api.h` `rx_material` headers
   this file includes (`material_system.h`, `rx_api_detail.h`), bracketed
   by `BEGIN`/`END` banner comments.
2. **`namespace materialBridge { ... }`** (also bracketed by banner
   comments): every actual use of an internal type lives here, behind four
   functions:
   - `createSystem()` — builds the real `rx::material::MaterialSystem` via
     `create()`, then bridges it into a public `IRxMaterialSystem` via
     `RxMaterialSystemDesc` + `rxCreateMaterialSystem()`.
   - `destroySystem()` — releases the public wrapper, then destroys the
     internal system (ordering matters: see "Bug #1" below).
   - `beginFrame()` / `onFrameCompleted()` — thin wrappers with no public
     equivalent (per-frame arena advance).
   - `bindAndGetLayout()` — resolves `materialHandle()`/
     `materialInstanceBlobData()`/`materialInstanceBlobSize()` (via
     `rx_api_detail.h`) from a public `IRxMaterial*`/`IRxMaterialInstance*`,
     builds an `InstanceBinding`, calls `bindInstance()`, and returns
     `pipelineLayout()` so the caller can separately bind descriptor set 0
     (the bindless table) — `bindInstance()` never touches set 0 by design.

Stricter than the letter of the acceptance criterion: `materialBridge`'s own
`MaterialSystemHandle` struct (the only thing code outside the bridge ever
touches) stores the internal `MaterialSystem*` as `void*`, not as its real
type — so no internal type NAME appears anywhere outside the two marked
regions, not just no *cast*. Verified directly: `grep -n "#include
<rx_material/" main.cpp` returns exactly 4 lines (the doc comment + 2 real
includes + 1 more doc comment, all inside/adjacent to the marked
`#include` block); every real `rx::material::` usage falls between the
`namespace materialBridge {` / `}  // namespace materialBridge` lines
(verified by grep + line-range inspection during this task).

## Shared-shader path mechanism chosen

Per the carried ledger item: `MaterialSystem::create()`
(`src/rx_material/include/rx_material/material_system.h`) gained a new,
optional trailing parameter:

```cpp
static std::unique_ptr<MaterialSystem> create(rx::rhi::Device& device, rx::rhi::BindlessTable& bindless,
                                                const std::filesystem::path& pipelineCachePath,
                                                const std::filesystem::path& sharedShaderDir = {});
```

Empty (the default) means "use `RX_MATERIAL_SHADER_DIR`" — every existing
caller (`test_material_system.cpp`, `test_api_factory.cpp`,
`test_api_contract.cpp`) keeps compiling and behaving identically, unchanged.
Internally, `create()` resolves `effectiveShaderDir` once and threads it
through `createMaterialSession()` (now takes the directory as a parameter
instead of reading the macro itself) and `loadForwardEntry()`; the resolved
value is stashed on `Impl::sharedShaderDir` so `reloadChanged()`'s own
fresh-session path reuses the SAME directory rather than re-reading the
macro. This is an additive, minimal change — no existing test needed
updating, and all of them still pass (`rx_material_tests`,
`rx_material_gpu_tests`, both green after the change).

`samples/06_materials` resolves a real runtime directory
(`SDL_GetBasePath()`-relative, exactly like `02_hotreload`'s own
`resolveShaderPath()`) and passes it explicitly: shared files
(`material.slang`/`forward_entry.slang`) deploy into a `material_shaders/`
subdirectory sibling to the sample's own `materials/` subdirectory
(`checker.slang`/`rim.slang`), both via `POST_BUILD` copy steps in
`samples/06_materials/CMakeLists.txt`. `tools/package_samples.sh` ships both
subdirectories. Proven end to end: the packaged `.zip`, unzipped to a
directory with zero relationship to the build tree, passes its own headless
gate unmodified (see "Packaging evidence" below).

## Scene, materials, and pixel derivations

4 objects, 1 forward pass: cube A/B use `checker.slang` (tint = warm orange
/ cool teal), sphere A/B use `rim.slang` (rimColor = magenta / golden
yellow). Placed in a 2x2 grid in the XY plane; a fixed-radius,
azimuth-only-orbiting orthographic camera (`makeCameraPose()`) sits at
`(0, 0, kOrbitRadius)` at headless azimuth 0, looking straight down -Z.

**Central engineering problem**: `forward_entry.slang`'s shared vertex
stage has no model/view/projection transform at all — clip position and
world position are the literal same attribute
(`float4(position, 1.0)`/`position`). No material-parameter mechanism
exists to inject one either (Phase 3's reflection supports exactly one
`gParams` block, nothing else). Resolved by pre-transforming every vertex's
position into clip space and every vertex's normal into VIEW space on the
CPU (`transformAndUploadObjectVertices()`), before upload — exact (not
approximate) specifically because the camera is orthographic: its
projection matrix's last row is always `(0,0,0,1)`, so the transformed
`w` is always exactly `1.0`, matching `forward_entry.slang`'s own hardcoded
assumption bit-for-bit. A perspective camera could not do this.

**Checker derivation**: orthographic projection is invariant to a shift
along the view axis, so probing at an object's world-space CENTER lands on
the same pixel as whatever surface the camera actually sees there. Every
cube face shares the identical UV parameterization
`(0,0)-(1,0)-(1,1)-(0,1)`, so the face's own geometric center is always
`uv = (0.5, 0.5)`. `checker.slang`'s `kCellsPerAxis = 3` (odd, deliberately
not 2 or 4) puts that point in the grid's middle cell, safely inside a cell
boundary — parity 0 ("light", lightness 1.0). Expected color: `tint * 1.0`,
exactly.

**Rim derivation**: a sphere's near point (closest to the camera) always
lies along the line from its center to the camera, by definition — its
local normal there equals the unit vector toward the camera, which, after
the same view-space normal pre-rotation, becomes exactly `(0,0,1)` in view
space (true for ANY azimuth on this sample's orbit, not just 0). So
`ndotv == 1.0` (up to the mesh's own finite-tessellation approximation of a
perfect sphere) at the probed pixel, giving `rimTerm = pow(1-1, 2.5) = 0`
and `color = kBaseAlbedo * 1.0 + rimColor * (0.5 + 0.0)`. A classic
"glow-only-at-the-edge" rim term would be indistinguishable between the two
rim instances at exactly this point — the `kAmbientFactor = 0.5` constant
term is what keeps `rimColor` visible there, which the file's own header
comment documents as the reason this formula isn't the textbook one.

**SRGB caveat** (found by `04_streaming`, cited not re-derived): this
project's swapchain/offscreen format resolves to an `_SRGB`-suffixed
`VkFormat` on at least one real driver, so a probed byte can be either the
raw linear value or its sRGB-encoded counterpart. The headless gate checks
both, at the REAL (format-resolved) R/G/B byte positions — channel-order-
exact per the brief, unlike `04_streaming`'s own order-agnostic check.

Observed headless values (this machine, `VK_FORMAT_B8G8R8A8_*`):

```
object 0 (checker) expected_linear=(255,140,38)  channels=(108,196,255,255) -- matched (B,G,R = 255,196,108... after sRGB decode)
object 1 (checker) expected_linear=(38,217,230)  channels=(243,237,108,255) -- matched
object 2 (rim)     expected_linear=(152,50,139)  channels=(195,122,203,255) -- matched
object 3 (rim)     expected_linear=(152,139,43)  channels=(115,195,203,255) -- matched
```

## Two real bugs found and fixed

### Bug #1: dangling `bindless` pointer via Scene-returned-by-value

`createScene()` originally built a local `Scene` and returned it by value
(`std::optional<Scene>`). `MaterialSystem::create()` stores a raw pointer to
whatever `BindlessTable&` it's given, for the instance's whole lifetime.
Load-time `loadMaterial()` calls (made from inside `createScene()`, before
the function returned) worked correctly — but the moment the by-value
return relocated `Scene::bindlessTable` to the caller's own storage, that
stored pointer went dangling. The read-back happened to come back as
all-zero bytes, so `bindless.descriptorSetLayout()` returned
`VK_NULL_HANDLE` — but ONLY on the first real `reloadChanged()`-triggered
rebuild (`PipelineLayoutBuilder::build()`'s `externalSet0` substitution
silently skipped), producing a real, repeating Vulkan validation error
(`VUID-vkCmdBindDescriptorSets-pDescriptorSets-00358`) the moment a
material was hot-reloaded in `--present` mode. Root-caused by
instrumenting `PipelineLayoutBuilder::build()` and the `recordDraws()` bind
call directly (temporary `RX_LOG_INFO` calls, removed before the final
commit) and observing `externalSet0=0x0` on the reload-triggered `build()`
call but not the load-time one.

Fixed by making `Scene::bindlessTable` a `std::optional<rx::rhi::
BindlessTable>` (mirroring `MaterialSystem::Impl`'s own pattern for members
with a private default constructor) and changing `createScene()` to
populate an already-existing `Scene&` in place (an out-parameter, returning
`bool`) rather than constructing-then-returning one — so the caller
declares `Scene scene;` once, in its own stack frame, and that address
never moves again for the rest of the program. Verified by reproducing the
exact validation error before the fix and its absence (3 consecutive
reload cycles, both files, zero real validation errors) after.

### Bug #2: mesh winding vs. `MaterialSystem`'s fixed backface culling

Every earlier sample (01-05) builds its own pipelines with
`VK_CULL_MODE_NONE`. `MaterialSystem`'s own fixed rasterization state
(`material_system.cpp`) uses `VK_CULL_MODE_BACK_BIT` with
`VK_FRONT_FACE_COUNTER_CLOCKWISE` — the first place in this codebase a
mesh's winding actually matters. Combined with this sample's own
`applyVulkanYFlip()` on the projection matrix (needed regardless, for the
Y-down NDC convention), the winding that reads as
"counter-clockwise, outward-facing" in a plain Y-up authoring sense
reversed to CLOCKWISE in Vulkan's own framebuffer-space sense — so every
object's FAR side survived culling, not the near side. The checker probe
still passed by coincidence (every cube face shares the identical UV
parameterization, front or back); the rim probe did not (caught it
immediately — a classic case of one test's own structural symmetry masking
a bug a differently-shaped test exposes). Fixed by reversing the index
winding in both `addQuad()` (cube generator) and the sphere generator, with
the mechanism documented in both places.

## Packaging evidence

`tools/package_samples.sh` extended: new `06_materials` case in the
runtime-libs loop, plus its own `materials/`/`material_shaders/`
subdirectory copies. Ran for real, both platforms:

- `linux-native`: `tools/package_samples.sh linux-native linux-x86_64
  /tmp/rendererx-samples-linux-x86_64.zip` — unzipped to a scratch
  directory with zero relationship to this repository's build tree, then
  ran `./sample_06_materials --validate` directly from there: headless gate
  PASSED, exit code 0.
- `windows-cross-zig`: `tools/package_samples.sh windows-cross-zig
  windows-x86_64 /tmp/rendererx-samples-windows-x86_64.zip` — unzipped to a
  separate scratch directory, ran `wine ./sample_06_materials.exe
  --validate` (under Xvfb, this machine's lavapipe software Vulkan
  implementation via winevulkan passthrough): headless gate PASSED, exit
  code 0.

Both runs used the packaged, already-unzipped copy exclusively — neither
referenced the build tree's own output directories, `RX_MATERIAL_SHADER_DIR`,
or any other build-tree-relative path.

## Test results

- `ctest --preset linux-native` (under `xvfb-run`): **15/15 passed**,
  including the new `sample_06_materials_headless` and every pre-existing
  test (`rx_core_tests`, `rx_platform_tests`, `rx_shader_tests`,
  `rx_rhi_vk_tests`, `rx_graph_tests`, `rx_graph_gpu_tests`,
  `rx_material_tests`, `rx_material_gpu_tests`, `shader_spirv_test`, and
  `sample_01`-`05`'s own headless gates) — confirming the
  `MaterialSystem::create()`/`PipelineLayoutBuilder` changes are safe.
- `ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|
  rx_material_gpu|sample'` (matching CI's own Wine-exclusion pattern):
  **6/6 passed**.
- `sample_06_materials --present --validate` (manual, under Xvfb):
  3 consecutive hot-reload cycles across both `checker.slang` and
  `rim.slang`, zero real validation errors, clean `SIGTERM` shutdown
  (`--present: window closed cleanly`, exit code 0) — both from the build
  tree and from the packaged/unzipped layout on both platforms (native +
  Wine).
- Both presets (`linux-native`, `windows-cross-zig`) build cleanly from a
  full rebuild.

## Files

- `samples/06_materials/main.cpp` (new)
- `samples/06_materials/CMakeLists.txt` (new)
- `samples/06_materials/materials/checker.slang` (new)
- `samples/06_materials/materials/rim.slang` (new)
- `CMakeLists.txt` (add_subdirectory)
- `tools/package_samples.sh`
- `.github/workflows/ci.yml` (comment/count accuracy only — no functional
  change: `linux-native`'s ctest has no filter, so the new test already ran
  there; `windows-cross-zig`'s existing `-E 'sample'` filter already covers
  the new sample's name)
- `samples/README.md`
- `MANUAL_VERIFICATION.md` (also adds the previously-missing 05_multipass
  present-mode rows, per this task's own ledger assignment)
- `src/rx_material/include/rx_material/material_system.h`
- `src/rx_material/material_system.cpp`
- `src/rx_material/CMakeLists.txt` (comment only)

## Commit

- `62611b5` — `feat: add sample 06_materials on the public material API`
  (all of the above, one commit; `main` branch, not pushed)

## Concerns

- **Manual, human-observed `--present` verification is still outstanding**
  for both 05_multipass and 06_materials (neither had a
  `MANUAL_VERIFICATION.md` row before this task). What this task DID
  verify for 06_materials is real functional correctness under an offscreen
  X server (Xvfb) — window creation, all 4 objects rendering at their
  analytically expected colors, camera orbit, 3 hot-reload cycles, clean
  shutdown, on both platforms — which is substantially more than a
  placeholder, but is explicitly NOT the same as a human watching a real
  window on real display hardware. Documented honestly in
  `MANUAL_VERIFICATION.md` rather than checked off.
- **`--present` mode's `SIGTERM`-based clean-shutdown test is somewhat
  environment-sensitive**: under this sandbox's job-control quirks, a
  `kill -TERM` sent via certain background-launch patterns (`setsid nohup
  ... &`) did not reliably reach the child process, while a plain `cmd &`
  backgrounding inside a single shell invocation did. This is almost
  certainly a shell/job-control artifact of the tooling used to drive the
  manual tests, not a bug in the sample itself (confirmed: the same binary,
  launched the reliable way, exits cleanly with status 0 every time,
  matching 02_hotreload's/05_multipass's own established `SIGTERM`→
  `SDL_EVENT_QUIT` mechanism).
- **Real bindless texture sampling from inside a material's own
  `evaluate()` is still not wired up** — `checker.slang`'s
  `checkerTexIndex` field is read numerically (never sampled), matching
  `test_textured.slang`'s own established Phase 3 scope boundary
  (`reflectMaterialLayout()` only supports one `gParams` block per
  material, nothing else at global scope). Documented in checker.slang's
  own header comment as deliberate, out-of-scope-for-this-task future work
  — wiring it up would mean changing `rx_material`'s core reflection rules,
  not just this sample.
- **`rx_material`'s own reflection/pipeline-building code (`material_system.
  cpp`, `pipeline_layout.cpp`) was NOT otherwise modified** beyond the
  `sharedShaderDir` threading — both bugs found and fixed during this task
  were bugs in THIS SAMPLE's own code (`main.cpp`), not in `rx_material`
  itself, though Bug #2 (backface culling) is worth flagging forward: this
  is the first real end-to-end draw call any sample or test in this
  codebase has issued against a `rx_material`-built pipeline with actual
  geometry, and it immediately surfaced a fixed-rasterization-state
  assumption (`VK_CULL_MODE_BACK_BIT`) that no earlier test exercised with
  real, winding-sensitive geometry. Worth a documentation note in
  `material_system.h`'s own `getPipeline()` comment for a future material
  author, though not something this task's own scope covers changing.
