# Task 16 report — StandardPBR + Unlit material library, D28, sample 08_gltf_viewer (card #8)

Base commit: `f48ba57`. Implementer commits (this task, in order):

1. `869789b` — `feat(rx_material): D28 fixed-function pipeline-state axis + D26.1 bindless per-draw addressing`
2. `4cb7562` — `feat(rx_material): ship StandardPBR + Unlit material library`
3. `47ffda8` — `fix(samples): 06_materials tangent field for the grown pooled vertex layout`
4. `9d74a86` — `feat(samples): add a reusable D17 reference-PNG tolerance gate`
5. `3afcf05` — `feat(samples): add 08_gltf_viewer -- the Stage 1 async-import + StandardPBR showcase sample`
6. `f7138ae` — `chore(tools,docs): package/regen/document 08_gltf_viewer`

No AI attribution in any commit; author is local git config (`Yousef Wadi <ywadi85@gmail.com>`); nothing pushed; no board/issue/plan/spec/ledger files touched. `.superpowers/sdd/2026-08-11-phase4-scene-assets/progress.md` and `docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md` show as modified in `git status` but were **not** touched or committed by me (not mine per the brief); `.superpowers/sdd/2026-08-11-phase4-scene-assets/task-17-brief.md` is an untracked file I also did not create and did not commit. `git status --short` after the final commit shows only those three paths outstanding, exactly as expected.

## 1. Files delivered

- `src/rx_material/include/rx_material/material_system.h` / `material_system.cpp` — `MaterialFixedFunctionState`/`AlphaMode`, the `loadMaterial(path, fixedFunctionState)` overload, `fixedFunctionState()` accessor, D28's fixed-function pipeline-cache-key axis, `gDrawData` binding recognition in `reflectMaterialLayout()`, the default identity draw-data buffer, `MaterialGlobalsPush` push (now 3 fields, not 1).
- `src/rx_material/include/rx_material/draw_data.h` (new) — `DrawDataGpu`/`MaterialGlobalsPush`, the C++ mirror of `material.slang`'s `RxDrawData`/`RxMaterialGlobals`.
- `shaders/material/material.slang` — `MaterialVertex` grows `tangent`/lighting-surface fields; `RxDrawData`/`gDrawData` (D26.1's bindless per-draw StructuredBuffer); `RxMaterialGlobals` grows `drawDataBufferIndex`/`exposure`.
- `shaders/material/forward_entry.slang` — reads `gDrawData[...][SV_VulkanInstanceID]` for a real model/view/projection transform and lighting surface; `safeNormalize()` hardening; `--exposure`'s `2^exposure` multiply in `fragmentMain`.
- `shaders/material/standard_pbr.slang` (new) — full glTF metallic-roughness StandardPBR.
- `shaders/material/unlit.slang` (new) — KHR_materials_unlit.
- `src/rx_material/tests/test_standard_pbr_unlit.cpp` (new, 47 `TEST_CASE`s) + `src/rx_material/tests/CMakeLists.txt` — GPU coverage for every matrix row plus D28/D26.1 mechanism tests.
- `src/rx_material/tests/test_api_factory.cpp`, `test_material_system.cpp`, `tests/data/test_{solid,textured,textured_sample,unlit}.slang` — updated for the grown vertex layout / reflected bindings / `MaterialGlobalsPush` size.
- `samples/06_materials/main.cpp`, `materials/{checker,rim}.slang` — tangent-field fix (a real NaN bug, see §5) + epsilon-read updates for the grown `MaterialVertex`.
- `samples/common/{CMakeLists.txt,reference_gate.h,reference_gate.cpp}` (new) — the reusable D17 tolerance gate.
- `samples/08_gltf_viewer/{CMakeLists.txt,main.cpp,references/*.png}` (new) — the sample itself.
- `CMakeLists.txt` (root) — `add_subdirectory(samples/common)` / `add_subdirectory(samples/08_gltf_viewer)`.
- `tools/package_samples.sh`, `tools/regen_references.sh` (new), `.github/workflows/ci.yml`, `MANUAL_VERIFICATION.md`, `samples/README.md` — packaging/regen/docs.

## 2. Per-criterion proof vs the completeness matrix

Every StandardPBR/Unlit feature row in `gate/matrix-issue08-standard-pbr.md` is covered by a dedicated `TEST_CASE` in `test_standard_pbr_unlit.cpp` (47 cases / 1982 assertions, all passing — see §3):

| Matrix row | Test case (abbreviated) |
|---|---|
| `baseColorFactor`/`baseColorTexture` | flat-lit factor-only probe + factor×texture multiply |
| `metallicFactor`/`roughnessFactor` + MR texture (G=roughness, B=metalness) | synthetic 1×1 MR texel readback |
| Normal mapping, BC5 two-channel + Z reconstruction | X=Y=0 flat (bright), X=0.6/Y=0.8 grazing (dark), out-of-unit-circle radicand clamp (no NaN) |
| MikkTSpace tangent/bitangent sign convention | `bitangent = cross(normal, tangent.xyz) * tangent.w` sign-flip probe |
| Occlusion, core-spec closed form `1+strength*(occ-1)` | synthetic occlusion texel + strength scaling |
| Emissive | factor×texture, additive, independent of lighting |
| Lambertian diffuse (÷π) | flat-lit diffuse-only probe against the analytic 1/π value |
| GGX/Smith-correlated/Schlick specular | grazing-angle Fresnel + roughness-controlled highlight width probes |
| FG1 flat ambient, metal-safe | fully-metallic (metallic=1, zero diffuse) still reads non-black |
| `alphaMode=MASK` | discard below cutoff / opaque above it (checker alpha texture) |
| `alphaMode=BLEND` | composites over an opaque background, does not write/occlude depth (three-draw depth-discrimination probe) |
| `doubleSided` | back-face of a single-sided quad culled vs. rendered |
| `KHR_texture_transform` (offset/scale) | per-slot UV remap probe |
| `alphaCutoff` uniform, always-present discard | OPAQUE/BLEND bind 0.0 (never discards) |
| D28 fixed-function axis | two `loadMaterial()` calls, same bytes, different `AlphaMode`/`doubleSided` → independently-cached `VkPipeline`s (cache-counter test) |
| D26.1 bindless per-draw addressing | two draws in one command buffer, second at `firstInstance>0`, read distinct `DrawDataGpu` rows via `SV_VulkanInstanceID` |
| `--exposure` neutral guard | `exposure=0` byte-identical no-op (±6/255 of the independently-computed Lambertian value); `exposure=1` measurably brighter |
| Unlit exactness + zero lighting dependence | `evaluate() == baseColorFactor × baseColorTexture` exactly; light-flip zero-delta probe |

Sample `08_gltf_viewer` itself exercises the same mechanisms end-to-end against a real glTF asset (DamagedHelmet: 5 textures, 1 StandardPBR material, doubleSided=false, alphaMode=OPAQUE) rather than synthetic single-quad probes — see §4/§6.

## 3. Suite status (both presets)

**linux-native**, full `ctest` (`VK_ICD_FILENAMES` forced to the system's real lavapipe ICD, `/usr/share/vulkan/icd.d/lvp_icd.json`, matching CI's own only-available-driver posture — see §7 for why this had to be located/forced explicitly in this dev environment):

```
100% tests passed, 0 tests failed out of 22
Total Test time (real) =  69.87 sec
```

Includes the two new `sample_08_gltf_viewer_headless` / `sample_08_gltf_viewer_quit_during_load` ctest cases (both `Passed`), `rx_material_gpu_tests` (47 new cases among its total, `1982/1982` assertions in the new file alone, 0 failed), `rx_material_tests`, and every pre-existing sample/library test unchanged.

**windows-cross-zig**: full `cmake --build --preset windows-cross-zig` (94/94 targets, including `sample_08_gltf_viewer.exe`) exits 0. `ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'` (CI's own exact filter) under Wine: `10/10 tests passed` (`rx_core_tests` through `rx_material_tests`). Sample tests are excluded from the Wine run by that same pre-existing filter (no Vulkan under Wine) — unchanged by this task, confirmed the filter still matches `sample_08_gltf_viewer_*` by name.

Environment note (not a code defect): both build directories needed a one-time `rm -rf .../_deps/mikktspace-{src,subbuild,build}` before a clean reconfigure — a pre-existing, already-documented (task-15/prior-session) FetchContent PATCH_COMMAND idempotency hazard tied to this dev machine having two same-inode paths to the checkout (`/media/ywadi/second/renderer_x` and `/home/ywadi/d2/renderer_x`), unrelated to this task's own changes.

## 4. Standalone/packaged verification

- `tools/package_samples.sh linux-native linux-x86_64 <zip>` produces a `08_gltf_viewer/` subdirectory with the binary, `material_shaders/{material,forward_entry,standard_pbr,unlit}.slang`, `tonemap.{vert,frag}.slang`, `references/{loading_state,loaded_scene}.png`, the Slang runtime libs + `LICENSE`, and a pre-staged `assets/DamagedHelmet/glTF/*` + `assets/DamagedHelmet/LICENSE.txt`.
- Unzipped to `/tmp/rx_pkg_test` (outside the build tree, no `RX_REPO_ROOT_DIR` dev-tree fallback reachable), `./sample_08_gltf_viewer --validate` runs standalone and passes: `D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true`, `D17 loaded_scene gate: failingPixels=0/65536 (0.0000%) pass=true`, `headless gate PASSED`.
- `tools/regen_references.sh` run end-to-end: forces lavapipe, renders both frames, copies them into `samples/08_gltf_viewer/references/`, and prints the rebuild+re-verify instructions. A second run reproduced byte-identical PNGs (deterministic default framing/lighting, per `frameCameraToScene()`'s/`updateDrawDataPerPassFields()`'s own fixed, asset-derived-but-content-independent constants).

## 5. Real bugs found and fixed in the course of this task

1. **06_materials NaN corruption** (pre-existing latent bug, surfaced by the vertex-layout growth this task's own D8/D26.1 work required): `transformAndUploadObjectVertices()` value-initialized (zeroed) the destination vertex buffer and copied position/normal/uv per vertex but never copied the newly-added `tangent` field, leaving every tangent at `(0,0,0,0)`. `forward_entry.slang`'s own `normalize(mul(modelMat3, tangent.xyz))` turned that into a NaN (`normalize` of the zero vector), which propagated into the rendered pixel through `checker.slang`/`rim.slang`'s own dead-code-elimination-avoidance epsilon reads (`color.z += v.tangent.x * 1e-6`). Root-caused via binary-search elimination of each epsilon term in a scratch copy of `checker.slang`. Fixed by copying the field explicitly in both `transformAndUploadObjectVertices()` and `generateSphere()`; hardened `forward_entry.slang`'s own `normalize()` calls with a `safeNormalize()` fallback as defense-in-depth against a future omission of the same kind.
2. **Dangling-pointer crash in the GPU test rig**: `MaterialSystem::create()` was being called against a `std::optional<Fixture>` local *before* it got moved into the returned rig struct — `MaterialSystem::Impl` stores a raw `Device&`/`BindlessTable&`, so the move invalidated them. Fixed by heap-allocating `Fixture` (`std::unique_ptr<Fixture>`) *before* calling `MaterialSystem::create()` against its now-stable address, matching `async_import_test.cpp`'s own established `AsyncTestFixture` two-step pattern.
3. **Missing Vulkan Y-flip in the test camera**: `glm::orthoZO()` is written for OpenGL's Y-up clip convention; without `proj[1][1] *= -1.0F`, every single-sided quad in the whole test file was silently back-face-culled (no validation error — culling is spec-legal), reading back the executor's own fixed clear color. Took the pass rate from ~29% to 95%+ once found; the same fix was baked into `08_gltf_viewer/main.cpp`'s own `vulkanPerspective()` from the start to avoid repeating it.
4. **`cmdCtx`/`frameSync` outliving the `VkDevice` they were built against** (found *this* task, in `samples/08_gltf_viewer/main.cpp` itself, both in `runHeadless()` and `runPresent()`): `rx::rhi::CommandContext`/`rx::rhi::FrameSync` own real command pools/fences/semaphores; `destroyApp()` destroys the `VkDevice`, but these two locals were declared *before* the point `destroyApp()` was called and never explicitly torn down first, so their own destructors ran *after* the device was gone. Reproduced directly: `VUID-vkDestroyDevice-device-00378` ("child object not destroyed") validation errors followed by a segfault at process exit, on **both** an artificial `SIGTERM`-under-`timeout` kill *and* a real, SDL-delivered clean quit (`SIGINT` under Xvfb, which SDL3 translates into a normal `SDL_EVENT_QUIT` — confirmed via the log line `sample_08_gltf_viewer: window closed cleanly` appearing *before* the crash). Fixed by explicitly resetting `cmdCtx`/`frameSync` immediately before every `destroyApp()` call site in both functions (3 sites in `runHeadless()` were N/A — only one existed there — and 3 sites in `runPresent()`). Re-verified clean afterward on the same SIGINT-under-Xvfb repro: zero `VUID-vkDestroyDevice-*` errors, `window closed cleanly`, no crash.
5. **`references/*.png` POST_BUILD copy not re-triggered by editing just the PNGs**: the original `add_custom_command(TARGET ... POST_BUILD ...)` recipe only re-runs when the *target itself* is relinked, not when its own declared inputs change on their own — running `tools/regen_references.sh` (which only rewrites the two PNGs) left an incrementally-rebuilt binary's *deployed* copy stale until something else forced a relink. Caught via an `md5sum` diff between the freshly regenerated source PNG and the still-stale deployed one. Fixed by converting the deploy step to a real `OUTPUT`/`DEPENDS`-tracked `add_custom_command` + `add_custom_target`, so ninja/make re-runs the copy on the two PNGs' own mtime alone.

## 6. Discrimination / revert-testing evidence

Five load-bearing mechanisms, each empirically defeated in place (never committed) and confirmed to fail the specific `TEST_CASE` that claims to discriminate it, then restored and re-verified green:

| Mechanism | Revert applied | Result before fix | Restored, re-verified |
|---|---|---|---|
| MASK cutoff discard | `if (false && baseColor.a < gParams.alphaCutoff)` in `standard_pbr.slang` | `1/96 assertions failed` | yes |
| BC5 Z-reconstruction + radicand clamp | dropped the `max(0.0, ...)` clamp and hardcoded `Z=0.0` | `2/127 assertions failed` | yes |
| D28 pipeline-cache-key axis | `PipelineKey` construction forced to `{AlphaMode::Opaque, false}` regardless of the material's real `fixedFunctionState` (C++, `material_system.cpp`, rebuilt) | `1/16 assertions failed` (the two `loadMaterial()` calls collapsed onto one cached `VkPipeline`) | yes |
| `--exposure` neutral-at-zero guard | `color.rgb *= exp2(exposure) + 1.0` in `forward_entry.slang` | `1/65 assertions failed` | yes |
| `SV_VulkanInstanceID` (D26.1 addressing) | `SV_VulkanInstanceID` → `SV_InstanceID` in `forward_entry.slang`'s `vertexMain` | **did not fail** — see below | restored regardless |

**Genuine, unresolved empirical finding on the last row**: swapping `SV_VulkanInstanceID` for `SV_InstanceID` did **not** make the D26.1 two-draw test fail (`40/40 assertions passed`, unchanged), on this project's pinned Slang `2026.14.1` targeting SPIR-V, against lavapipe. This directly contradicts the documented rationale repeated in three places in this codebase (`material.slang`, `forward_entry.slang`, `draw_data.h` header comments): *"Slang SUBTRACTS `firstInstance` back out of `SV_InstanceID` to preserve D3D-compatible semantics... verified against Slang's own SPIR-V-target docs before relying on this."* I confirmed the revert was real and complete (`grep` showed the edit as the only `instanceId`-binding occurrence in the file) and that the C++ side genuinely issues two `vkCmdDrawIndexed` calls with distinct `firstInstance` (0 and 1) — `renderQuad()`'s own `/*firstInstance=*/draw.drawDataRow`. I did **not** go further (SPIR-V disassembly of both variants) to find the mechanistic reason, given this task's own scope and budget; I did **not** change the shipped code (`SV_VulkanInstanceID` is restored and is still the objectively-safer, more-explicit choice regardless of whether `SV_InstanceID` also happens to work here) or silently rewrite the three existing header comments' factual claim without independent re-verification. **Flagging this explicitly, not deferring it silently**: the "Slang subtracts `firstInstance` for `SV_InstanceID`" claim, as currently documented in three places in this codebase, is not reproducible with the evidence I gathered this task, and deserves either a from-source re-verification against Slang's own SPIR-V backend or a correction to those three comments in a follow-up.

## 7. Environment note: locating lavapipe locally

This dev machine's default Vulkan device is a real (NVIDIA) GPU, not lavapipe, so `--validate` runs and the D17 gate both needed `VK_ICD_FILENAMES` forced to the system's real lavapipe ICD to match CI's own only-driver-available posture. `mesa-vulkan-drivers` was already installed system-wide (`/usr/share/vulkan/icd.d/lvp_icd.json` + `/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so`) — used directly; a separately-extracted, glibc-incompatible `.deb` payload found in this session's own scratchpad directory (apparently left over from an earlier, abandoned attempt) was not usable and was not relied on. `tools/regen_references.sh` bakes this same ICD path in and fails loudly if it is not present, rather than silently regenerating against whatever driver happens to be default.

## 8. Deviations from a literal reading of the brief

- **`MaterialSystem::bindInstance()` is not used by `08_gltf_viewer`.** The brief's plan text does not explicitly forbid it, but `material_system.cpp`'s own `bindInstance()` doc comment (already present before this task touched it, extended in commit 1) states in so many words that a real D26.1 caller drives its own draw-data buffer and pushes the range itself, "never through this method" — I followed that documented contract rather than the more superficially-convenient `bindInstance()` call, and built the sample's own manual bind sequence (pipeline resolve via the same `getPipeline()`, a real `MaterialGlobalsPush`, a hand-built but pipeline-layout-*compatible* set-1 descriptor layout per the Vulkan spec's own "identically defined" rule). This is the only way the sample's own `--exposure` and real per-scene draw-data buffer can reach the shader at all.
- **Light/ambient tuning is a sample-level judgment call, not a spec value.** D22/FG1 fix the *mechanism* (Lambertian + GGX + a flat ambient term) but not specific brightness constants. The shipped defaults (`lightColor=(5,5,5)`, `ambientColor=0.18`) were chosen empirically against DamagedHelmet's own genuinely-dark base-color texture so the showcase sample's default render is legible without IBL (deferred to the techniques phase per D22's own scope) — documented in `updateDrawDataPerPassFields()`'s own comment, including the precedent (`shaders/multipass/lit.frag.slang`'s `kAmbient=0.08`) these values were nudged brighter from.
- **Pre-existing `samples/README.md`/`.github/workflows/ci.yml` staleness beyond sample 08 itself was also fixed while already touching those files** (07_stress had never been added to `samples/README.md`'s top-of-file bundle listing or its "Running the automated test suite" section) — a small, low-risk, in-scope-adjacent cleanup rather than leaving a doubly-stale list behind.

## 9. Self-review

- Every acceptance-criterion row in the completeness matrix has a dedicated, passing GPU test (§2); the five most safety-critical mechanisms additionally have empirical revert-testing evidence (§6), with one genuine unresolved discrepancy flagged rather than hidden.
- Both presets build clean; both presets' full applicable test suites are green; the packaged, standalone `.zip` output was actually unzipped and run outside the build tree.
- Four real, non-trivial bugs were root-caused and fixed during this task (§5), one of them (the `cmdCtx`/`frameSync` teardown-ordering bug) directly matching this stage's own standing "abandon/teardown paths need real-GPU-resource tests" review lesson — found specifically *because* I insisted on driving a real SDL-delivered quit signal under Xvfb rather than accepting the sample's own headless-mode-only green ctest run as sufficient evidence for present-mode's own teardown path.
- No AI attribution anywhere in history; only my own files committed, verified directly via `git status --short` after the final commit (not merely assumed).
- Known gaps for a future task/follow-up: the `SV_InstanceID` documentation-vs-observed-behavior discrepancy (§6) is unresolved; `--present` mode's own mouse-drag orbit and visual quality have not been watched by a human on real display hardware (only functionally verified under Xvfb+lavapipe, `MANUAL_VERIFICATION.md`'s own checkboxes left unchecked pending that).
