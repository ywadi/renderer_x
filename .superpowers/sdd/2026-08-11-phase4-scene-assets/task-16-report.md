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

## 9. Fix round (coordinator review, two independent reviewers)

Commits (in order, after the base report's own `734497e`):

1. `9a53922` — `fix(rx_material): occlusion applies to ambient only, not direct light (fix round item 5)`
2. `00cd0ff` — `test(rx_material): fix-round test corrections (coordinator review items 1, 2, 5b)`
3. `23837ca` — `chore(samples): regenerate 08_gltf_viewer D17 reference after the occlusion fix (fix round item 5a)`
4. `9e0fcf8` — `feat(samples): 08_gltf_viewer perf instrumentation + orbit-camera comment fix (fix round items 6, 7)`
5. `8334803` — `chore(tools,samples): vendor full CC-BY-4.0/CC-BY-NC-4.0 legal texts into the redistribution zip (fix round item 4)`

Same rules as the base report: no AI attribution, local commits only (not pushed), only files this fix round produced were committed.

### Item 1 (Important) — D26.1 two-draw test was depth-tie-masked; reworked and revert-proven

**Root cause, confirmed by a second, independent reviewer's own SPIR-V disassembly**: `SV_InstanceID` genuinely emits `OpISub InstanceIndex, BaseInstance` on this project's pinned Slang/SPIR-V target (my own earlier flagged "contradiction" in the base report was wrong — settled with ground truth, not touched further; the three header comments asserting this were correct all along). The ORIGINAL test's own failure to catch this was a **test bug**, not a false claim about Slang: row 1's quad was placed fully OFF the ortho frustum, so under the `SV_InstanceID` revert bug, draw 2's mis-addressed fragments (reading row 0's IDENTITY transform) landed exactly on top of row 0's own already-written, identical-depth quad — this project's fixed `VK_COMPARE_OP_LESS` (strict) depth-compare state then rejected every one of those fragments as a depth tie, so the framebuffer read back identical to the CORRECT-behavior case either way. The single-center-pixel probe could not distinguish "row 1 correctly off-frustum, nothing to see" from "row 1 wrongly on top of row 0, rejected by a depth tie."

**Fix**: `makeHeadOnRow()` gained an optional `orthoHalfExtent` parameter (default `0.5F`, unchanged for all 46 other call sites); the D26.1 test now widens the frustum to `3.0` and places row 1's quad at a genuinely separate, on-screen, non-overlapping world location (`+2` in X) instead of off-frustum. `renderQuad()`'s single-pixel-readback body was split into a shared `renderQuadPixels()` (returns the full raw RGBA8 buffer for an arbitrary extent) + `pixelAt()` (extracts one texel) — `renderQuad()` itself is now a thin wrapper, unchanged in signature/behavior for its other 46 callers. The test now probes row 0's location (must read RED) and row 1's own, separate location (must read GREEN — the real discriminator).

**Revert evidence** (scratch, in place, never committed):

```
$ # baseline (SV_VulkanInstanceID, correct):
[doctest] test cases:  1 |  1 passed | 0 failed | 46 skipped
[doctest] assertions: 42 | 42 passed | 0 failed |

$ # reverted forward_entry.slang: SV_VulkanInstanceID -> SV_InstanceID
.../test_standard_pbr_unlit.cpp:773: ERROR: CHECK( row1Pixel.g > 200 ) is NOT correct!
  values: CHECK( 0 >  200 )
[doctest] test cases:  1 |  0 passed | 1 failed | 46 skipped
[doctest] assertions: 42 | 41 passed | 1 failed |

$ # restored SV_VulkanInstanceID -> full suite green again (see §3 below)
```

`row1Pixel.g == 0` under the revert is exactly the predicted mechanism: the buggy second draw never reaches row 1's own real, separate screen location at all (its fragments land on row 0's, where the depth tie silently swallows them), so that location reads back the plain background clear color instead of green — unambiguous, with no depth-tie possible between two draws that no longer even hit the same pixels.

### Item 2 (Important) — dedicated emissive probe added; report erratum corrected

Two new `TEST_CASE`s: a factor-only probe (`emissiveFactorAndPad=(0.2,0.4,0.6)`, light+ambient zeroed on the row, expects an exact `(51,102,153)` byte readback) and a textured variant (`emissiveFactor=(1,1,1)` × a `(100,150,200)` texel, same isolation technique, expects an exact product). Both isolate emissive by zeroing `lightColor`/`ambientColor` (not `baseColor`, which would still leave a real, nonzero dielectric `F0=0.04` specular term reachable) — `color = directLight + ambient + emissive` collapses to exactly `emissive` regardless of `baseColor`/`metallic`/`roughness`, which are deliberately left at their ordinary neutral defaults rather than special-cased, to prove independence from them too.

**§2 table erratum** (this report, base version): the row claiming emissive coverage was added post-hoc and is now actually true; at the time it was written, `emissiveFactorAndPad` was set to `{0,0,0,0}` at exactly one call site in the whole file (the shared neutral-default blob builder) and never overridden anywhere — no test exercised a nonzero value. Corrected by this fix round's own new test cases, not by editing the prose of an already-committed report (this section is the correction of record).

### Item 3 (Minor) — report erratum: TEST_CASE count

The base report's own file list said "47 TEST_CASEs / 1982 assertions" for `test_standard_pbr_unlit.cpp` specifically. That was **the whole `rx_material_gpu_tests` binary's own total** (across `test_material_system.cpp`, `test_api_factory.cpp`, `test_param_arena.cpp`, and this file combined), not this file's own count. Corrected, counted directly (`grep -c '^TEST_CASE'` per file, summed and cross-checked against the doctest-reported binary total):

| File | TEST_CASEs |
|---|---|
| `test_material_system.cpp` | 17 |
| `test_api_factory.cpp` | 13 |
| `test_param_arena.cpp` | 2 |
| `test_standard_pbr_unlit.cpp` | **17** (15 at the base report's own commit `4cb7562`; +2 this fix round, item 2) |
| **Binary total** | **49** (`49 | 49 passed | 0 failed`, `2156 | 2156 passed | 0 failed` — see §3 below) |

### Item 4 (Minor, coordinator-ruled) — full CC-BY-4.0/CC-BY-NC-4.0 legal texts now shipped

Fetched verbatim (`curl` against `creativecommons.org/licenses/{by,by-nc}/4.0/legalcode.txt`, 396 and 408 lines respectively, each a complete, well-formed legal document from its own title through "Creative Commons may be contacted at creativecommons.org.") and vendored as committed files (`samples/08_gltf_viewer/licenses/CC-BY-4.0.txt` / `CC-BY-NC-4.0.txt`) rather than fetched at package time — a small, static legal document is exactly the kind of third-party content this project already commits directly (unlike `assets/fetched/`'s own large, gitignored binary content), and vendoring keeps `tools/package_samples.sh` network-free, matching its own pre-existing posture. `LICENSE.txt` (the human-readable attribution notice) stays, now explicitly pointing at the two bundled full texts instead of external URLs.

**Verified**: re-ran `tools/package_samples.sh` for both `linux-native` and `windows-cross-zig`; `unzip -l` on both output `.zip`s shows all three files (`LICENSE.txt`, `CC-BY-4.0.txt` 18657 bytes, `CC-BY-NC-4.0.txt` 19347 bytes) under `08_gltf_viewer/assets/DamagedHelmet/`; extracted both license files from the `linux-native` zip to a scratch directory and `diff`'d them against the vendored source -- byte-identical (`CC-BY-4.0 identical` / `CC-BY-NC-4.0 identical`).

### Item 5 (Important, shipped-shader fix) — occlusion no longer attenuates direct light

**Fix**: `standard_pbr.slang`'s `color = directLight * occlusion + ambient + emissive` became `color = directLight + ambient + emissive` — occlusion now applies exclusively to the ambient term, matching the glTF spec's own occlusionTexture text, all three first-tier references this task's own matrix cites, and the gate matrix's own occlusion acceptance criterion (scoped to the ambient contribution alone). Prevents double-darkening a directly-lit, occluded surface today, and prevents double-counting against a real direct-light visibility term (shadow maps) once Task 22 lands one.

**Test extension + revert evidence**: the existing occlusion closed-form `TEST_CASE` gained a third case — a direct-lit (default head-on rig, `NdotL=1`), ambient-zeroed comparison between an unoccluded (`occlusion=1`) and heavily-occluded (`occlusion=0`, the same `blackOcclusionTex` the existing ambient-only cases already use) render of an otherwise-identical material, asserting the two read back identical (occlusion must not touch direct light at all when ambient contributes nothing).

```
$ # reverted: color = directLight * occlusion + ambient + emissive (the old, spec-incorrect form)
.../test_standard_pbr_unlit.cpp:1126: ERROR: CHECK( near8(directOccludedPixel.r, directUnoccludedPixel.r, 2) ) is NOT correct!
  values: CHECK( false )
.../test_standard_pbr_unlit.cpp:1127: ERROR: CHECK( near8(directOccludedPixel.g, directUnoccludedPixel.g, 2) ) is NOT correct!
  values: CHECK( false )
.../test_standard_pbr_unlit.cpp:1128: ERROR: CHECK( near8(directOccludedPixel.b, directUnoccludedPixel.b, 2) ) is NOT correct!
  values: CHECK( false )
[doctest] test cases:   1 |   0 passed | 1 failed | 48 skipped
[doctest] assertions: 162 | 159 passed | 3 failed |

$ # restored -> full suite green again (see §3 below)
```

The two PRE-EXISTING assertions in this same `TEST_CASE` (ambient-only closed-form, R=0/strength=1 and strength=0.5) were unaffected by either the fix or the revert either way: both already used a light pointed AWAY from the surface (`NdotL=0`), so `directLight` was already zero in both branches regardless of the occlusion multiply — confirmed by inspection (no regression risk) and by the full-suite pass count staying at 49/49 both before and after this item.

**D17 reference regeneration** (the sanctioned case, sequenced once, after every shader change in this fix round landed — commit `23837ca`): `tools/regen_references.sh linux-native` under the forced system lavapipe ICD. `loading_state.png` is byte-unchanged (that D17 frame never reaches the material shader). `loaded_scene.png` changed (21255 → 21400 bytes) — DamagedHelmet's own occlusion texture now legitimately brightens its directly-lit surface slightly. Visually inspected (still a recognizable, correctly-shaded helmet, no regression). Re-ran the headless gate against the new reference: `D17 loading_state gate: failingPixels=0/65536 pass=true`, `D17 loaded_scene gate: failingPixels=0/65536 pass=true`, `headless gate PASSED`.

### Item 6 (Important) — measured performance numbers

**Capture method** (disclosed per the measured-claims rule): direct `std::chrono::steady_clock` wall-clock instrumentation added to `runHeadless()` in `samples/08_gltf_viewer/main.cpp`, printed via a real, greppable `RX_LOG_INFO("sample08: perf ...")` line (matching `sample_07_stress`'s own `"stress:"` stats-line convention) in a real run under the system's own lavapipe ICD, Xvfb. **Not** a live Tracy GUI/network capture — this dev environment has no Tracy GUI attached, and this project's own build graph does not build Tracy's own separate `tracy-capture` CLI tool (a real, but materially larger, undertaking than this fix round's own scope called for). Task 15's own `RX_ZONE` Tracy zones around the async-import pipeline remain fully present, unaffected, and available for a real interactive Tracy GUI session (`RX_TRACY=ON` in the `linux-native` preset, `CMakePresets.json`) — this is a second, independent, always-on measurement, not a replacement for them.

`import_ms`: wall-clock from the `importGltfAsync()` kickoff to its completion callback confirming `setupMaterials()`/`buildDrawList()` both succeeded. `first_frame_ms`: wall-clock from `runHeadless()`'s own first instruction (before Window/Context/Device/Allocator/MaterialSystem/GeometryPool/TextureCache/Executor construction, the tonemap pipeline build, and the loading-state frame) through the first fully rendered, GPU-readback-confirmed POST-IMPORT frame -- genuine cold-start time to a real, correct pixel on screen, not import alone.

Three consecutive runs, DamagedHelmet, `linux-native`, system lavapipe (`/usr/share/vulkan/icd.d/lvp_icd.json`), Xvfb:

```
sample08: perf scene='.../DamagedHelmet.gltf' import_ms=349.872 first_frame_ms=855.580
sample08: perf scene='.../DamagedHelmet.gltf' import_ms=350.358 first_frame_ms=858.292
sample08: perf scene='.../DamagedHelmet.gltf' import_ms=356.609 first_frame_ms=852.581
```

`import_ms` ≈ 350ms, `first_frame_ms` ≈ 855ms, consistent across runs (< 2% spread). Both include lavapipe's own software-rasterizer/CPU-decode overhead (this is a software Vulkan implementation, not representative of a real GPU's own timing) and this specific dev machine's own CPU. **Steam Deck hardware was not available in this environment** — no number to report there; flagging rather than fabricating one, per this project's own measured-claims discipline. A real-GPU/Steam-Deck capture is follow-up work for whichever task/gate actually owns Phase 4's stage-exit performance-number requirement (this task-level ask was for `import_ms`/`first_frame_ms` numbers to exist in this report at all, which they now do).

### Item 7 (Minor) — stale comment corrected

Three comments in `samples/08_gltf_viewer/main.cpp` claimed the mouse-drag orbit camera reads state "via `Window::sdlWindow()`" — the actual code (unchanged, always correct) calls SDL3's global `SDL_GetMouseState()` directly, which takes no window argument at all in SDL3 (it queries OS-level cursor state, not scoped through any specific `SDL_Window*`). This already satisfies gate ruling #8's own underlying intent (sample-local, no `rx_platform` input surface pulled forward) without actually needing `Window::sdlWindow()` for this specific query. Comments corrected to describe the real call; no behavior change.

### Suite status after the full fix round

**linux-native**, full `ctest` (serial, lavapipe forced): `100% tests passed, 0 tests failed out of 22` (64.50s). `rx_material_gpu_tests` alone: `49 | 49 passed | 0 failed`, `2156 | 2156 passed | 0 failed`.

**windows-cross-zig**: full `cmake --build` (all targets) exits 0. `ctest -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|sample'` (CI's own exact filter) under Wine: `100% tests passed, 0 tests failed out of 10` (101.38s).

**Packaging**: `tools/package_samples.sh` re-run for both presets; both output `.zip`s carry the full license texts (§ item 4) and the regenerated D17 reference (§ item 5); the `linux-native` zip's own `08_gltf_viewer/` was unzipped to a scratch directory outside the build tree and its headless gate re-run standalone -- `headless gate PASSED`, 0 failing pixels on both frames.

## 10. Self-review (base report) / fix-round self-review addendum

Fix-round addendum: every one of the seven coordinator-review items above has direct empirical evidence (two real reverts with pasted failure output for items 1 and 5; a byte-identical diff for item 4; three consecutive measured runs for item 6; a direct file-count grep for item 3) rather than an assertion. The one open item from the base report (§ "Known gaps") — the `SV_InstanceID` documentation-vs-observed-behavior discrepancy — is now fully resolved by the coordinator's own independent SPIR-V disassembly and standalone Vulkan repro: the three header comments were correct, my own base-report flag was a false alarm caused by the depth-tie test bug fixed in item 1 above, and no code or comment changes were needed for that claim itself.

- Every acceptance-criterion row in the completeness matrix has a dedicated, passing GPU test (§2); the five most safety-critical mechanisms additionally have empirical revert-testing evidence (§6), with one genuine unresolved discrepancy flagged rather than hidden.
- Both presets build clean; both presets' full applicable test suites are green; the packaged, standalone `.zip` output was actually unzipped and run outside the build tree.
- Four real, non-trivial bugs were root-caused and fixed during this task (§5), one of them (the `cmdCtx`/`frameSync` teardown-ordering bug) directly matching this stage's own standing "abandon/teardown paths need real-GPU-resource tests" review lesson — found specifically *because* I insisted on driving a real SDL-delivered quit signal under Xvfb rather than accepting the sample's own headless-mode-only green ctest run as sufficient evidence for present-mode's own teardown path.
- No AI attribution anywhere in history; only my own files committed, verified directly via `git status --short` after the final commit (not merely assumed).
- Known gaps for a future task/follow-up: the `SV_InstanceID` documentation-vs-observed-behavior discrepancy (§6) is unresolved; `--present` mode's own mouse-drag orbit and visual quality have not been watched by a human on real display hardware (only functionally verified under Xvfb+lavapipe, `MANUAL_VERIFICATION.md`'s own checkboxes left unchecked pending that).
