# Task 4 report: sample 05_multipass (shadow + forward + tonemap through rx_graph)

**Worktree**: `/media/ywadi/second/renderer_x/.claude/worktrees/agent-a0257a83d60b1accb`
**Branch**: `worktree-agent-a0257a83d60b1accb`
**Base commit before this task's work**: `5c4aa23` (fast-forwarded from an earlier point on this branch onto local `main`, which was missing rx_graph Tasks 1-3 — see "Setup note" below)

## Setup note (environment gap found and fixed)

This worktree's branch was originally created from a point on `main` **before** rx_graph's Tasks 1-3 (render graph pass declarations, sync2 barrier derivation, Executor) had landed — `src/rx_graph/` did not exist in this worktree's initial history at all, even though the task brief assumed it was "in-tree." Confirmed via `git log --oneline HEAD..main` (8 commits, exactly the rx_graph Tasks 1-3 commits plus the SDD docs commit) and `git merge-base --is-ancestor`. Local `main` (8 commits ahead of `origin/main`, i.e. unpushed local work, not yet on the public remote) already had this work. Fixed with a plain fast-forward merge (`git merge main --ff-only`) — no rebasing, no destructive operation, zero unique commits lost (the worktree branch had none). This should be flagged to the coordinator: worktrees for tasks depending on recent in-tree work should be cut from the latest local `main`, not an earlier point.

## What was built

`samples/05_multipass/main.cpp` + `samples/05_multipass/CMakeLists.txt`, plus five new shader files under `shaders/multipass/`:
- `shadow.vert.slang` — depth-only shadow-pass vertex shader (no fragment stage at all — a graphics pipeline with zero fragment shader stages, valid Vulkan for a pass with no color output).
- `lit.vert.slang` / `lit.frag.slang` — forward pass: Lambert diffuse + manual single-tap shadow-map comparison (no `VK_COMPARE_OP` sampler).
- `tonemap.vert.slang` / `tonemap.frag.slang` — fullscreen triangle + plain Reinhard (`c/(1+c)`).

**Graph shape** (declared once via `declareGraph()`, all sync derived by `rx::graph::RenderGraph::compile()`/`Executor`):
- `shadow` → writes `"shadowmap"` (Absolute 1024×1024 D32_SFLOAT, depth-only)
- `forward` → reads `"shadowmap"` (texture input); writes `"hdr"` (SwapchainRelative R16G16B16A16_SFLOAT) + `"depth"` (SwapchainRelative D32_SFLOAT, attachment-only, no reader — matches the coordinator's ambiguity resolution)
- `tonemap` → reads `"hdr"`; writes `"backbuffer"` (the graph's `setBackbufferSource()` target)

**Scene**: ground plane (XZ, `y=0`, half-size 6) + one cube (half-extent 1, sitting on the floor) + one sphere (radius 1, off to the side), reusing samples/03's procedural cube/sphere generators, extended with per-vertex normals (03 never needed normals — no lighting there). Objects only ever translate, never rotate, so the shader skips a normal-matrix transform entirely (documented in `lit.vert.slang`'s header comment). Directional light, fixed elevation (50°), azimuth 0 in headless / animated in `--present`.

**Camera**: fixed top-down **orthographic** camera (a deliberate ambiguity-resolution choice, not perspective) — this makes the headless probe pixels analytically derivable via `worldToPixel()`, the same closed-form-derivation discipline `samples/04_streaming`'s `cellProbePixel()` established, rather than empirically reverse-engineered against a perspective projection the way `samples/03_bindless_mesh`'s probes were.

**Bindless / push constants**: one shared `ObjectTransform` bindless storage buffer (double-buffered by frame-in-flight row, exactly `samples/03_bindless_mesh`'s convention), carrying `mvp`, `lightMvp`, `albedo`, `lightDirWorld` per object row. Push constants across all three passes are plain scalar `uint` fields only (see "Real bug found and fixed" below for why). `"shadowmap"`/`"hdr"` are registered into the bindless table lazily inside their reading pass's own `execute()` callback, cached by `VkImageView` value so re-registration only happens on an actual resize (never in headless mode) — documented in `main.cpp`'s header comment ("BINDLESS REGISTRATION FOR POOLED GRAPH RESOURCES").

**Zero hand-written barriers** (D10 acceptance criterion): `grep -r vkCmdPipelineBarrier2 samples/05_multipass/` returns nothing (verified — the file's own header comment had to be reworded once, since it originally *quoted* that exact grep command and would have self-matched). `rx::rhi::transitionImage()` is also never called. Every layout transition (including the offscreen headless backbuffer's own first-touch, and repeated reuse of that same offscreen image across 3 render frames) comes from the graph's own derived barriers — see `main.cpp`'s header comment ("BACKBUFFER REUSE ACROSS REPEATED execute() CALLS") for why that's safe given each headless frame is its own separately-`vkQueueWaitIdle()`'d `CommandContext::runOnce()` submission.

## A real bug found and fixed during implementation (not a false positive)

While debugging why the shadow/lit probes read identical brightness (no shadowing at all), I traced it to a genuine authoring bug, not a false positive: `shadow.vert.slang` declares its own **separate** `ObjectTransform` struct (it's compiled as its own Slang module, never concatenated with the lit files). When I extended `lit.vert.slang`'s copy of that struct with two extra fields (`albedo`, `lightDirWorld`) to work around a push-constant-packing surprise (see below), I forgot to make the identical change in `shadow.vert.slang`. Its struct stayed at 2 fields (128-byte stride) while the real C++-side buffer stride is 160 bytes. Row 0 (the floor) happened to read correctly regardless of stride (both conventions agree at offset 0), which is why the bug wasn't obvious immediately — but every other row (cube, sphere) read a garbled mix of adjacent rows for its `lightMvp`, producing a plausible-looking but wrong shadow-pass transform. Fixed by making `shadow.vert.slang`'s struct byte-for-byte identical to `lit.vert.slang`'s (documented in-place with the full diagnostic trail, since this is exactly the class of bug this sample's own shadow-vs-lit-probe assertion exists to catch). Diagnosed via a disciplined sequence of temporary instrumentation (decoded HDR debug channels through the tonemap pipeline, a raw shadow-map buffer dump via a temporary bare graph pass, and a direct `mul(xf.lightMvp, (0,0,0,1))` clip-space cross-check against the equivalent C++ computation) — all temporary debug code was removed before finalizing; the shader/main.cpp diffs in this commit contain none of it.

**Related, purely empirical finding** (documented in `lit.vert.slang`'s header comment): a push-constant block made entirely of scalar `uint` fields reflects at exactly `4 × fieldCount` bytes with **no** rounding to any fixed multiple — contradicting an assumption I initially carried over from `samples/04_streaming`'s own documented "rounds up to 16 bytes" finding (that finding was specific to a struct containing a `float4x4`, not evidence of a blanket rule). I resolved this by keeping every push-constant field a plain scalar and moving every vector value (`albedo`, `lightDirWorld`) into the already-proven-correct `StructuredBuffer` row layout instead of trying to reverse-engineer Slang's exact vector-in-push-constant packing rule from scratch.

## Sync-validation outcome (coordinator addition)

Enabled `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` in `src/rx_rhi_vk/src/context.cpp`'s `Context::create()`, guarded behind a `vkb::SystemInfo::get_system_info()` probe for `VK_EXT_validation_features` availability (so a machine without the validation layer degrades exactly as before, rather than hard-failing `InstanceBuilder::build()`). Per this repo's "don't reinvent the wheel" policy, the actual `VkValidationFeaturesEXT` struct/pNext chaining is done by vk-bootstrap's own `InstanceBuilder::add_validation_feature_enable()` (verified directly against the pinned vk-bootstrap source that it does chain a real `VkValidationFeaturesEXT`), not hand-rolled in this file.

**Turning it on surfaced two real-looking validation-error variants**, both traced to the **same confirmed false positive** in this machine's apt-packaged `VK_LAYER_KHRONOS_validation` (1.3.204.1) — not a real synchronization bug in rx_graph or this sample:

- The layer internally misclassifies a `Texture2D`+`SamplerState` (separate descriptors, the bindless convention every sample in this repo uses) `.Sample()` read as `SYNC_*_SHADER_STORAGE_READ`, then hazards that misclassified read against whatever `write_barriers` value is tracked for the resource's last real write (`SYNC_FRAGMENT_SHADER_SHADER_SAMPLED_READ` in one variant — its own correct derivation for the layout-transition barrier itself, disagreeing with its own misclassification of the read; `SYNC_COLOR_ATTACHMENT_OUTPUT_COLOR_ATTACHMENT_WRITE` in the other — the resource's last color-attachment write). Reproduces reliably for a texture written earlier in the **same command buffer** and read later with no intervening submission boundary — exactly the shape rx_graph's Executor produces for a pass reading another pass's output within one `execute()` call.
- **Verified, not assumed**: reproduced this exact hazard (both variants) against the apt-packaged 1.3.204.1 layer, then re-ran the identical binary with `VK_LAYER_PATH` pointed at a substantially newer `VK_LAYER_KHRONOS_validation` build (api_version 1.4.357, found already present on this machine at `/home/ywadi/sponza/vvl/`) — **zero** hazards reported there, for both `rx_graph_gpu_tests`' pre-existing "invert" test case and this sample's shadow-map/HDR reads. An actively-maintained implementation of the same validation feature agrees both accesses are correctly synchronized.
- Added a third documented false-positive guard in `context.cpp` (matching the file's existing two-guard convention exactly: narrow substring match on the hazard ID + descriptor type + misclassified usage + `prior_usage` + barrier command, deliberately **not** on the generic hazard ID alone, and deliberately **not** pinning the varying `write_barriers` value so both confirmed variants are covered without widening the match to anything else).
- **No real under-synchronization was found anywhere** — this task's scope did not require any barrier-logic code change in `rx_graph` or `rx_rhi_vk` beyond the guard itself.

## Validation evidence

`ctest --preset linux-native --output-on-failure` (all 12 tests, sync validation active):

```
100% tests passed, 0 tests failed out of 12
Total Test time (real) =  11.6-11.7 sec
```

Full test list: `shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`, `rx_shader_tests`, `rx_rhi_vk_tests`, `rx_graph_tests`, `rx_graph_gpu_tests`, `sample_01_triangle_headless`, `sample_02_hotreload_headless`, `sample_03_bindless_mesh_headless`, `sample_04_streaming_headless`, `sample_05_multipass_headless`.

Scanned the full verbose ctest log for any `[error]`-level line not covered by a "known false positive" tag or the sample's own final summary line — every remaining `[error]` line traced to a *different*, pre-existing test deliberately exercising an error path (e.g. `rx_shader_tests`' bad-module-path case, `rx_rhi_vk_tests`' `BindlessTable`/`PipelineLayoutBuilder` capacity-rejection cases, `rx_graph_tests`' cycle-detection/duplicate-name cases) — none are real failures; the `100%/0 failed` summary is corroborated line-by-line, not just trusted.

`sample_05_multipass --validate` (headless) on its own:
```
[info] shadow probe world=(0.0,2.0) pixel=(128,167) channels=(60,58,58,255) brightness_sum=176
[info] lit probe world=(-4.0,-4.0) pixel=(49,49) channels=(153,149,149,255) brightness_sum=451
[info] multipass headless gate PASSED
```
(176×2=352 < 451 → assertion (a); 451 > 153 (0.2 of full scale) → assertion (b); every UNORM/SRGB readback byte is inherently ≤255 → assertion (c), documented in-code as a weak-but-literal check per the brief's exact wording.)

`grep -r vkCmdPipelineBarrier2 samples/05_multipass/` → empty (exit 1, no matches).

`--present --validate` smoke-tested under `xvfb-run` for 6 seconds (light orbiting through multiple azimuth values) — zero unguarded `[error]` lines.

## Packaged-layout run evidence

- `tools/package_samples.sh linux-native linux-x86_64 <zip>` — succeeds, 47 files, `05_multipass/` subdirectory contains the binary + all 5 `.slang` sources + 4 Slang runtime `.so`s/symlinks + `LICENSE`.
- Unzipped `05_multipass/` alone into a fresh scratch directory (no other files from the build tree) and ran `./sample_05_multipass --validate` directly — **exit 0**, identical `PASSED` output, confirming the `$ORIGIN` RPATH + flat-deployed-shader-file mechanism works standalone.
- `tools/package_samples.sh windows-cross-zig windows-x86_64 <zip>` — succeeds, 39 files, `05_multipass/` subdirectory analogous to the Linux one (4 DLLs instead of `.so`s).
- **Bonus** (beyond what CI itself does — CI explicitly does not run sample gates under Wine, "no real Vulkan device"): unzipped the Windows `05_multipass/` alone and ran `wine ./sample_05_multipass.exe --validate` under `xvfb-run` on this machine (which has lavapipe/llvmpipe installed) — **exit 0**, identical `PASSED` output via Wine's winevulkan→llvmpipe passthrough. Validation layer itself isn't installed under Wine (logged, expected, matches this repo's own documented Wine-CI reasoning), so this is a functional-correctness proof, not a validation-cleanliness one.

## ctest summary

- `linux-native`: 12/12 passed (see above).
- `windows-cross-zig` under Wine with the CI's own exclusion regex (`-E 'rx_rhi_vk|rx_graph_gpu|sample'`): 5/5 passed (`shader_spirv_test`, `rx_core_tests`, `rx_platform_tests`, `rx_shader_tests`, `rx_graph_tests`) — confirmed the existing regex's bare `sample` substring already excludes `sample_05_multipass_headless` with **no ci.yml regex change needed** (verified directly, per the coordinator's instruction not to assume this).
- Both presets (`linux-native`, `windows-cross-zig`) build cleanly end to end, including the pre-existing rx_graph/rx_rhi_vk/samples 01-04 targets.

## Files

Created:
- `samples/05_multipass/main.cpp`
- `samples/05_multipass/CMakeLists.txt`
- `shaders/multipass/shadow.vert.slang`
- `shaders/multipass/lit.vert.slang`
- `shaders/multipass/lit.frag.slang`
- `shaders/multipass/tonemap.vert.slang`
- `shaders/multipass/tonemap.frag.slang`

Modified:
- `CMakeLists.txt` (added `add_subdirectory(samples/05_multipass)` — the brief said `samples/CMakeLists.txt`, but no such file exists in this repo; the root `CMakeLists.txt` lists every sample subdirectory directly, so that's the real file the brief meant)
- `tools/package_samples.sh` (five-sample header/comment updates, `05_multipass` added to the Slang-runtime-lib deployment loop, its five shader sources added to the per-sample asset copy list, `05_multipass` added to the final `zip` invocation)
- `.github/workflows/ci.yml` (comment/count updates only — linux job's descriptive comments now say 12 tests / five sample gates instead of 9/four; the windows job's actual exclusion regex already covered the new sample via its bare `sample` substring, verified, not changed)
- `samples/README.md` (new `## 05_multipass` section mirroring 01-04's structure; updated the top-of-file zip-contents tree, the Linux/Windows run-command blocks, and the "Running the automated test suite" section's test list/count)
- `src/rx_rhi_vk/src/context.cpp` (coordinator addition: `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` wiring + the third documented false-positive guard)

## Commit

One commit on this worktree's branch (`worktree-agent-a0257a83d60b1accb`), on top of the fast-forward merge described above.

## Concerns / things the coordinator should know

1. **Worktree base-point gap** (see "Setup note" above) — fixed locally via fast-forward, but worth checking whether other in-progress worktrees for this phase have the same gap.
2. **Sync validation false positives are specific to this machine's old apt-packaged validation layer (1.3.204.1)** — this is the third such guard in `context.cpp`, all attributable to the same stale-layer root cause the file's own comments already document for the first two. If CI's `ubuntu-latest` runner has a newer `vulkan-validationlayers` package than this dev machine, these hazards may simply never fire there at all (harmless either way — the guard only ever *documents and downgrades to a warning* an error this specific old layer version emits; it does not suppress a differently-worded real error). If a future validation-layer upgrade on this dev machine ever stops reproducing these, that's expected and the guard becomes dead code, not a rot risk (it would simply never match again).
3. **The brief's file list said `samples/CMakeLists.txt`**; no such file exists in this repo — I modified the root `CMakeLists.txt`'s `add_subdirectory(...)` list instead, which is where every other sample is wired in. Flagging in case this naming was intentional and I'm missing context, though I'm confident this is simply how the repo is actually laid out (verified directly, not assumed).
4. No existing-code sync bugs needed fixing — the one real bug found (shadow.vert.slang's stale struct) was in **this task's own new code**, not pre-existing rx_graph/rx_rhi_vk code, so it's a normal implementation fix, not a "BLOCKED, needs coordinator call" situation.

## Fix round 1 (post-review)

Coordinator review approved with one required finding: `ObjectTransform` was declared three times (`main.cpp`, `lit.vert.slang`, `shadow.vert.slang`) with only comments guarding drift — exactly the bug class the original implementation had already hit once (see "Real bug found and fixed" above). Addressed as follows.

### 1. Shader side: single source of truth

Created `shaders/multipass/scene_types.slang`, containing the one canonical `ObjectTransform` struct declaration. Removed the duplicated declarations from `lit.vert.slang` and `shadow.vert.slang`.

**Mechanism chosen: neither `import` nor `__include` — host-side textual concatenation, extending this sample's existing multi-file-compile mechanism.** Investigated both of the coordinator's suggested options concretely before deciding:

- **`import scene_types;`** (the coordinator's stated preference, matching `shaders/material/`'s `import material;` on `main`): verified directly against both `src/rx_material/material_system.cpp` (which sets `slang::SessionDesc::searchPaths` to `RX_MATERIAL_SHADER_DIR` before calling `import`) and `src/rx_shader/src/compiler.cpp` (`rx::shader::Compiler::create()`'s `slang::SessionDesc` has **zero** search paths configured — confirmed by reading the actual session-construction code, not assumed). This sample compiles exclusively through `rx::shader::Compiler` (never the raw Slang API `rx_material` uses), so a real `import` here needs either extending `Compiler` to accept search paths (explicitly forbidden this round: "do NOT extend rx_shader in this task") or reimplementing this sample's whole compile+reflect pipeline against the raw Slang API just to share one struct — disproportionate.
- **`__include "scene_types.slang"`**: reduces to the identical requirement. With no session search path, `__include` needs either a search path (same gap as `import`) or a literal, runtime-resolved absolute path baked into the compiled source text at the point of the directive — which on Windows means threading a backslash-bearing path through a Slang string-literal escape sequence, a mechanism with zero existing test coverage in this codebase.
- **What I actually did**: this sample's `compileAndReflect()` (present before this fix round, for pairing each pass's vertex+fragment files into one Slang translation unit) already reads named files from disk and concatenates their text into one combined source string before it ever reaches Slang. I added `scene_types.slang` as the first filename in both `buildShadowPipeline()`'s and `buildLitPipeline()`'s file lists — Slang never sees a directive, never needs a search path, and there is no path-string-escaping surface on any platform. Functionally and textually equivalent to `__include`, implemented with a mechanism this file already had and already shipped correctly (verified: `tonemap.vert.slang`+`tonemap.frag.slang` pairing worked identically before this fix round). Documented at length in `scene_types.slang`'s own header comment, `kSceneTypesFilename`'s comment in `main.cpp`, and at both call sites.

Packaging updated to ship the new file: `samples/05_multipass/CMakeLists.txt`'s `RX_MULTIPASS_SHADERS` list, `tools/package_samples.sh`'s per-sample asset list (now 6 files, was 5), `samples/README.md`'s three shader-file-count mentions (5→6, listing `scene_types.slang`).

### 2. Host side: strongest drift guard + position assertion

- Added `constexpr size_t kObjectTransformShaderStrideBytes = 64 + 64 + 16 + 16;` (hand-computed from `scene_types.slang`'s field list: two `float4x4` + two `float4`) and `static_assert(sizeof(ObjectTransform) == kObjectTransformShaderStrideBytes, ...)` immediately after the C++ `ObjectTransform` struct, with a comment pointing at `scene_types.slang` and explicitly noting why a `reflect()`-driven check isn't available (see item 3 below). This fails the **build**, not a runtime probe, the moment either side drifts in field count/size.
- Added headless-gate assertion (d): probes object index 1 (the cube, not index 0 — the floor's row starts at byte offset 0 under any stride assumption, which is exactly why the original bug went unnoticed at index 0) at its analytically-expected screen position (`worldToPixel(kCubePosition.xz, ...)`, the same fixed top-down camera every other probe uses) and asserts the pixel is clearly red-dominant (the cube's albedo direction, `(0.75, 0.25, 0.2)`), channel-order-agnostic (checks both an RGBA-shaped and a BGRA-shaped dominance condition, accepting either). This distinguishes the cube's expected pixel from the floor's near-neutral gray and the sphere's blue-leaning albedo — a stride/offset drift severe enough to substitute a neighboring row's data, or an `mvp`/`lightMvp` drift severe enough to move the cube off its footprint, would both make this probe land on non-red data instead.

### 3. rx_shader scope note

Per the coordinator's explicit instruction, did **not** extend `rx_shader` in this task. `rx::shader::ShaderLayoutInfo` (`shader_layout_info.h`) reports set/binding/type/count/stage/push-range shape only — it has no notion of a `StructuredBuffer` element's internal stride/size, so `reflect()`-based verification of `ObjectTransform`'s real GPU-side stride is not possible today. This is now noted directly in `main.cpp`'s comment next to the `static_assert`, and the coordinator is already ledgering it as an `rx_shader` enhancement candidate — no code change made here.

### Re-verification after the fix

- `sample_05_multipass --validate` (headless): `PASSED`, all four assertions (a)-(d) hold; sync validation active, zero unguarded validation errors (same three documented false-positive guards as before, nothing new).
- `grep -r vkCmdPipelineBarrier2 samples/05_multipass/` → still empty.
- `ctest --preset linux-native --output-on-failure`: **12/12 passed** (fresh run, post-fix).
- `windows-cross-zig`: builds cleanly (fresh full rebuild); `ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|sample'` under Wine: **5/5 passed**.
- Both presets repackaged via `tools/package_samples.sh`: Linux zip now 48 files (was 47 — `scene_types.slang` added), Windows zip now 40 files (was 39). Unzipped `05_multipass/` alone into a fresh scratch directory for **both** platforms and ran the binary directly (Linux natively; Windows via `wine` + `xvfb-run`, same bonus verification as the original implementation) — both exit 0, both `PASSED`, confirming `scene_types.slang` deploys and resolves correctly in a genuinely standalone layout on both platforms.

### Commit

New commit on this worktree's branch, on top of the original task-4 commit — see the STATUS reply for the hash.
