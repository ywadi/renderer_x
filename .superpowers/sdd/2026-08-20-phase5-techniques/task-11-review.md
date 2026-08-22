# Task 11 review — glTF PBR conformance harness vs Khronos Sample Viewer (issue #47)

Independent reviewer round. Commit under review: `e77b376`
(`feat(tests/conformance): glTF PBR conformance harness vs Khronos Sample
Viewer (#47)`), base `24794e7`, branch `task/t11-conformance`, worktree
`/media/ywadi/second/renderer_x-worktrees/t11-conformance`. Reviewer did not
write this code. Every claim below was independently re-derived: a fresh
fetch of the pinned `glTF-Sample-Renderer` source at commit
`863b981fb755359063e370ff7b6e956bda0716e2` (not trusted from the report), a
full read of the 180KB diff, and live re-execution on both drivers,
including a temporary, byte-identically-restored historical-bug probe.

## Verdict 1 — Spec compliance: **PASS** (matrix-p5t11-conformance-harness.md)

All matrix rows are met: ≥6 conformance models gated at Stage 1 close (6
mandatory + 1 optional = 7, exceeding the bar), MetalRoughSpheres passes on
the real driver with a working discrimination proof, ground-truth
provenance is committed next to every reference, per-model license
verification is real and correctly disposed (Adobe Stock content
correctly kept out of the public repo), and the reference-generation
mechanism matches gate ruling T11 (headless-browser automation of
`GltfView`/`GltfState`) exactly. See Finding 1 below for the one gap this
verdict does not cover — a CI-wiring omission, not a matrix-row failure.

## Verdict 2 — Code quality: **NOT Approved — 1 MAJOR finding, 2 MINOR**

This is a strong, unusually well-verified round: the camera-fit port and
the tonemap-NONE reproduction are both bit-for-bit correct against the
actual pinned reference source (independently confirmed below, not merely
re-read from the implementer's own comments), the tolerance derivation is
real and empirically justified with wide margins, and the discrimination
mechanism required three genuine iterations to find a perturbation with no
content-specific fixed point — all honestly documented in code, not just
in the report. The MAJOR finding is a real CI-wiring gap (conformance
tests are not excluded from the Wine job, unlike every other GPU-backed
test binary in this repository) that should close before this is
considered done, not a defect in the harness's own logic.

---

## Finding 1 (MAJOR) — `rx_conformance_*` ctest targets are not excluded
from the Wine (`windows-cross-zig`) CI job, unlike every other GPU-backed
test binary

**Where:** `.github/workflows/ci.yml`, `windows-cross-zig` job, step "Test
under wine" (line 707-710):
```
ctest --preset windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_material_brdf_gpu|rx_debug_ui_gpu|rx_frame_loop_gpu|rx_ibl_gpu|sample' --output-on-failure
```

**What's wrong:** `tests/conformance/` is `add_subdirectory`'d
unconditionally in the top-level `CMakeLists.txt` (same as every sample),
so `rx_conformance_render` cross-compiles for Windows and its 8 ctest
entries (`rx_conformance_MetalRoughSpheres`, ...,
`rx_conformance_MetalRoughSpheres_discrimination_proof`) get registered
under the `windows-cross-zig` preset too. None of them match the `-E`
exclusion pattern above — verified mechanically (simulated the regex
against all 8 new test names; zero matches). Both CI jobs' asset-fetch
step calls `tools/fetch_assets.sh --sponza`, and since the six mandatory
conformance models are now fetched unconditionally inside that script (not
behind a flag), the six mandatory models' assets AND their committed
references will be present under Wine too — so 7 of the 8 new tests (all
but `EnvironmentTest`, which gracefully skips since `--environment-test`
is never passed) will attempt full execution: real `VkDevice` creation, a
full IBL bake, a forward render pass, an offscreen readback, and a pixel
comparison.

This is exactly the class of work this same file's own, extensively
documented policy excludes for every *other* GPU-backed binary
(`rx_rhi_vk_tests`, `rx_graph_gpu_tests`, `rx_material_gpu_tests`,
`rx_material_brdf_gpu_tests`, `rx_debug_ui_gpu_tests`,
`rx_frame_loop_gpu_tests`, `rx_ibl_gpu_tests`, every `sample_*_headless`
gate) — the file's own comment states the rationale in so many words:
*"exercising this project's actual device/swapchain/synchronization2
pipeline through Wine's Vulkan passthrough is a materially bigger claim
than 'window creation succeeds' and is not verified here."*
`rx_conformance_render` is squarely in that excluded class (arguably a
heavier GPU workload than several of the excluded binaries: a full IBL
bake chain plus a real forward+tonemap render graph, not just pipeline
creation). Its omission from the exclusion list reads as a genuine miss,
not a considered inclusion — there is no comment anywhere in the diff
discussing Wine at all.

**Consequence:** on the next CI run, the Wine job will either fail
outright (if Wine's winevulkan-over-lavapipe passthrough can't actually
carry a real render+readback, which this file's own policy treats as
unverified) or silently exercise a code path this project has explicitly
never trusted — either way, this is not a decision anyone made on purpose.

**Not empirically reproduced under Wine this round:** the `windows-cross-zig`
build dir was not warmed in the worktree (only `linux-native` was), and a
cold cross-build + Wine bring-up was outside this review's time budget.
The finding rests on a full read of `ci.yml` plus a mechanical regex
simulation against the new test names, both conclusive on their own.

**Suggested fix:** add `rx_conformance` (or `conformance`) to the `-E`
pattern, mirroring the existing GPU-tests convention exactly.

## Finding 2 (MINOR) — stale measured-divergence figures in
`tests/conformance/models.h`'s own top comment

**Where:** `tests/conformance/models.h`, the comment above `kModels`:
> "Measured lavapipe divergence against the committed references, all six
> mandatory models: MetalRoughSpheres 0.43%, MetalRoughSpheresNoTextures
> 0.58%, EmissiveStrengthTest 0.03%, CompareEmissiveStrength 0.30%,
> TextureTransformTest 1.62% ..., AlphaBlendModeTest 0.47%"

**What's wrong:** these numbers do not match the actual measured values.
Independently re-run twice this round on lavapipe against the exact
committed references in this commit:
MetalRoughSpheres 0.6748%, MetalRoughSpheresNoTextures 1.1482%,
EmissiveStrengthTest 0.2201%, CompareEmissiveStrength 0.8198%,
TextureTransformTest 1.6758%, AlphaBlendModeTest 0.9972% — which match
`task-11-report.md`'s own table exactly (also independently confirmed).
The header comment appears to be a stale snapshot from an earlier
development iteration (likely before the two camera-fit bugs were fully
fixed and references finalized) that was never refreshed. It does not
affect gate correctness — the shipped `±32/255`, `<2.5%` constants remain
well-justified against the *actual* numbers (see the tolerance
adjudication below) — but it will mislead a future reader who trusts the
in-code comment over the report.

## Finding 3 (MINOR) — `App::surface` (`VkSurfaceKHR`) is never destroyed
in `tests/conformance/main.cpp`

**Where:** `destroyApp()` in `main.cpp` carefully tears down every other
`App` member (device, allocator, bindless, samplers, material param arena,
draw-data buffer, environment, tonemap pipeline, registry, etc.) but never
calls `vkDestroySurfaceKHR` on `app.surface`, created earlier via
`window->createVulkanSurface(...)`. `app.context` (the `VkInstance`) and
`app.window` are also left for the `App` destructor to clean up implicitly
via their own RAII wrappers when `main()` returns, but the raw
`VkSurfaceKHR` has no such wrapper and is simply never freed. Since this
is a one-shot CLI tool that exits immediately after each ctest invocation,
the leak is reclaimed by process exit and has no observable effect today
— flagged because it is inconsistent with the otherwise-careful teardown
in the same function, not because it changes any measured result.

---

## Reference-harness correctness (the attention lens's primary ask)

Verified the camera-fit port **directly against the actual pinned source**
(fetched `source/gltf/user_camera.js` and `source/gltf/gltf_utils.js` at
commit `863b981fb755359063e370ff7b6e956bda0716e2` via `gh api`, not read
from the diff's own citations):

- `fitDistanceToExtents`/`fitCameraTargetToExtents`/
  `fitCameraPlanesToExtents` in `user_camera.js` are quoted verbatim and
  correctly in `camera_fit.cpp`'s header comments and correctly translated
  into the C++ port — confirmed line-by-line against the fetched source.
- **Bug #1 (transform-hierarchy ordering) is real**, confirmed directly:
  `node.js` shows `worldTransform` initialized to identity at construction
  and `getRenderedWorldTransform()` returning it unmodified; `scene.js`
  shows `applyTransformHierarchy()` is the only writer; `gltf_view.js`
  shows `GltfView.renderFrame()` calls `scene.applyTransformHierarchy()`
  internally. Calling `resetView()` before any `renderFrame()` genuinely
  reads stale identity transforms, exactly as claimed.
- **Bug #2 (sphere-inscribing-cube extent step) is real**, confirmed
  directly: `gltf_utils.js`'s `getExtentsFromAccessor()` computes a tight
  AABB from the 8 transformed corners, then explicitly replaces it with
  `center ± length(boxMax-center)` (the bounding-sphere radius) before
  `getSceneExtents()` unions it — exactly what `camera_fit.h`'s
  `sphereInscribingCubeOf()`/`accumulateReferenceStyleSceneExtents()`
  reproduce, at the same per-primitive granularity.
- **Numerically verified, not just structurally**: hand-derived
  `MetalRoughSpheres/provenance.json`'s committed camera
  (`target = 0.5*(max+min)`, `distance` from `fitDistanceToExtents`,
  `position = target + (0,0,distance)`) directly from its own committed
  `sceneExtents.min/max` and reproduced the exact committed
  `position`/`target`/`distance` figures by hand, confirming both the JS
  reference's own internal consistency and that the ported C++ formula
  (read directly in `camera_fit.cpp`) computes the identical closed form.
- **Tonemap-NONE reproduction is bit-for-bit correct**: fetched
  `source/Renderer/shaders/tonemapping.glsl` at the pinned commit — with no
  `TONEMAP_*` define active, `toneMap()` reduces to exactly
  `color *= u_Exposure; return linearTosRGB(color)` where
  `linearTosRGB(c) = pow(c, 1/2.2)`, matching
  `tonemap_linear.frag.slang` exactly. The clamp-before-pow (RendererX
  side) vs. clamp-at-8-bit-store-after-pow (reference side, implicit)
  ordering difference is mathematically inconsequential: `pow(x, 1/2.2)`
  is monotonic increasing with fixed points at 0 and 1, so clamping before
  or after produces identical results.

This is a well-executed, independently-verifiable port — the report's
claim that "our C++ side and the reference side compute the same camera"
holds up under direct scrutiny of the actual pinned source, not just the
implementer's own citations of it.

## Tolerance adjudication: justified, not overfit

Reused `samples/common/reference_gate.h`'s existing tolerance-gate
primitive (confirmed: its default is `±4/255`, `<0.5%` — the D17
same-renderer regression figure the report contrasts against). The
conformance gate's `±32/255`/`<2.5%` is wider, and the report's own
justification (independently spot-checked, not merely re-read) holds:

- **Why the divergence exists at all**: both sides deliberately avoid
  MSAA (`antialias: false` in `harness.html`'s WebGL2 context creation;
  RendererX's own forward pass declares no multisampled attachment), so
  MSAA-vs-none is ruled out as a cause. The worst real-content case
  (TextureTransformTest, ~1.68%) is attributed to UV-transform edge
  antialiasing/filtering differences between two independently-written
  texture-sampling paths — a plausible, specific mechanism, not a vague
  "cross-renderer noise" hand-wave.
- **Discrimination margin, independently re-measured** (both drivers, this
  round): mandatory-model worst case 1.6758% (TextureTransformTest,
  lavapipe and NVIDIA identical) vs. the shipped roughness-perturbation
  discrimination proof at 3.8685% (lavapipe) / 3.9616% (NVIDIA). The 2.5%
  budget sits ~49% above the worst real case and ~35% below the
  discrimination value on both drivers — a real, comfortable margin on
  both sides, not a hair's-width pass.

## The DFG-formula historical-bug probe — the most informative result

Per the review brief, temporarily reinstated the OLD, wrong pre-fix-round
DFG formula in `shaders/material/brdf.slang`'s `iblSpecularReflectance()`
(`f0*dfg.x + dfg.y`, the exact bug class from `task-10-review.md` Finding
1) in place of the current, correct `lerp(dfg.x, dfg.y, f0)`, then re-ran
the conformance gate without rebuilding the C++ binary (shaders compile
in-process from source at runtime). Restored byte-identically afterward
(md5 and `git status` both confirm zero residual diff).

**Result: the gate catches this bug class dramatically, far more easily
than the shipped roughness-perturbation proof does.**

| Model | Correct formula (lavapipe) | Wrong DFG formula (lavapipe) |
|---|---:|---:|
| MetalRoughSpheres | 0.6748% (PASS) | **15.5807%** (FAIL, 6.2x the budget) |
| MetalRoughSpheresNoTextures | 1.1482% (PASS) | **36.9240%** (FAIL, 14.8x the budget) |

Both wrong-DFG runs fail with an enormous margin relative to the 2.5%
budget — far beyond even the shipped discrimination proof's 3.87–3.96%.
Re-ran the correct formula afterward on the same model to confirm the gate
returns to its original, byte-identical passing state (`failingPixels=1769`,
same as every prior run). **Direct answer to the brief's question**: yes,
a subtler-but-real error of exactly the class this project has already
shipped once (T10's DFG-channel-composition bug) would be caught by this
gate, and caught easily, not marginally — the tolerance is not overfit to
pass known-good renders; it has real discriminating power against a real
historical bug class.

## Model-swap adjudication: sound scope call

Independently verified (fresh fetch of the raw glTF JSON from
`glTF-Sample-Assets`, not from the diff's own citation) that
`TextureTransformMultiTest` declares `extensionsUsed: ["KHR_materials_clearcoat",
"KHR_materials_unlit", "KHR_texture_transform"]` with exactly 9 of its 29
materials using `KHR_materials_clearcoat` (matches the report's claim
exactly), and that `AlphaBlendModeTest` has no `extensionsUsed` at all
(`None`, confirmed). Per `rulings-2026-08-20.md`, `KHR_materials_clearcoat`
is explicitly Stage-3 scope (T21). Gating the whole image against a model
that uses an unsupported extension would fail for a reason outside this
ticket's scope, which the ticket's own binding text ("failures are
findings to fix, never tolerance widenings") forbids papering over. The
substitution is a sound, well-evidenced scope call under the matrix's own
authority order, not a finding.

## CI integration

- **`linux-native` job**: correctly wired, matches established pattern.
  `tools/fetch_assets.sh --sponza` fetches the six mandatory conformance
  models unconditionally (same as DamagedHelmet/BoomBox); `--environment-test`
  is never passed, so `EnvironmentTest` correctly stays unfetched in CI.
  `ctest --preset linux-native` (unfiltered) runs the full suite under
  `xvfb-run` against lavapipe (the runner's only ICD) — all 8
  `rx_conformance_*` tests genuinely execute there, not skip.
- **`windows-cross-zig`/Wine job**: **gap — see Finding 1.** Not excluded
  from the Wine ctest run, unlike every other GPU-backed test binary.
- **No Node/Playwright leakage into the build/test path**: confirmed —
  `tests/conformance/CMakeLists.txt` and the top-level `CMakeLists.txt`
  contain zero references to Node, npm, or `tools/gltf_conformance/`;
  `rx_conformance_render` reads only the committed PNG + provenance JSON
  at test time. The new JS toolchain dependency is correctly scoped to the
  offline, human-invoked `generate_reference.mjs` only.

## Pins + licenses

- `glTF-Sample-Renderer` commit `863b981fb755359063e370ff7b6e956bda0716e2`,
  Apache-2.0 — confirmed real and fetchable via `gh api`.
- Playwright `1.62.1`, Apache-2.0 — confirmed pinned exactly (no range) in
  both `package.json` and `package-lock.json`; both agree.
- Per-model licenses in `assets/test/ASSET-NOTES.md` and
  `tools/fetch_assets.sh`'s own comments: MetalRoughSpheres/EmissiveStrengthTest/
  AlphaBlendModeTest CC-BY-4.0, MetalRoughSpheresNoTextures/CompareEmissiveStrength/
  TextureTransformTest CC0-1.0 (with the CompareEmissiveStrength Khronos-trademark
  nuance correctly called out), EnvironmentTest correctly identified and
  kept out of the repo as a proprietary Adobe Stock license — follows the
  established `fetch_assets.sh`/Sponza precedent exactly. Reference PNGs'
  provenance (generator script, renderer commit+license, camera, environment,
  rendering parameters) is committed in full per-model as
  `provenance.json`, satisfying the ticket's literal requirement.

## Empirical verification performed this round

All runs offscreen/headless (`Window::create(..., visible=false)`
confirmed in source), niced, serialized, no forks, driver-labeled:

- **lavapipe** (`llvmpipe (LLVM 15.0.7, 256 bits)`, forced ICD, under
  `xvfb-run`, `nice -n19`): full repo suite **42/42 passed**, zero
  regressions against the pre-existing 34. All 6 mandatory models +
  EnvironmentTest + discrimination proof independently re-measured; every
  percentage matches `task-11-report.md`'s table exactly.
- **Determinism**: re-ran all 6 mandatory models on lavapipe twice;
  `failingPixels`/percentages byte-identical across both runs for every
  model. Not flaky.
- **Real NVIDIA** (`NVIDIA GeForce RTX 2080`, driver `580.82.07`, real
  `DISPLAY=:1`, forced ICD, invisible offscreen window, `nice -n19`,
  serialized single runs, owner's seat undisturbed): all 6 mandatory
  models + discrimination proof independently re-measured; every
  percentage matches the report's table exactly
  (MetalRoughSpheres 0.7664%, MetalRoughSpheresNoTextures 1.1482%,
  EmissiveStrengthTest 0.2121%, CompareEmissiveStrength 0.8179%,
  TextureTransformTest 1.6758%, AlphaBlendModeTest 1.0181%;
  discrimination proof 3.9616%).
- **DFG historical-bug probe**: see above — real, temporary, restored
  byte-identically (md5 `d0672289b159f873032cc4ee3047b1d0` before and
  after; `git status` clean).
- **Hygiene**: single commit (`e77b376`), author `Yousef Wadi
  <ywadi85@gmail.com>` (confirmed via `git log --format='%an <%ae>'`), full
  commit body scanned for AI-attribution markers (clean), not pushed
  (`git ls-remote --heads origin task/t11-conformance` returns nothing),
  `EnvironmentTest` confirmed absent from the commit tree, main checkout
  untouched except the pre-existing `progress.md` modification (left
  alone per instructions).

## Not independently verifiable this round

- The `windows-cross-zig`/Wine execution path itself (Finding 1) — the
  build dir wasn't warmed and a cold cross-build + Wine bring-up was
  outside this round's time budget; the finding rests on source analysis
  and mechanical regex simulation, both conclusive, but not an actual
  failing/passing Wine CI run.
- Full byte-for-byte visual inspection of all 6+1 reference PNGs (relied
  on the quantitative gate results — pass margins, discrimination-proof
  failure margins, and the DFG-probe result — as the substantive
  correctness signal instead).
