# DamagedHelmet texture investigation — report

**Status: NO texture-slot/index-swap defect found.** The reported bug
hypothesis (a slot swap somewhere between import-time material resolution and
shader-time sampling) is **disproven** by direct, reproducible, pixel-level
evidence gathered against the real committed asset. No production code was
changed. The requested verification-gap test was still built and committed
(it currently passes, and is proven to catch exactly this class of bug via a
swap-and-revert exercise).

## 1. Reproduction

Built `sample_08_gltf_viewer` at base `ad9a5e6`, ran headless
(`--write-references`), 256x256 (the sample's native headless size, `--present`
not available in this environment). The captured render reproduces the
committed `samples/08_gltf_viewer/references/loaded_scene.png` exactly (D17
gate: 4/65536 pixels differ, informational-only on this non-lavapipe driver,
`pass=true`). `helmet-before.png` / `helmet-after.png` in this directory are
byte-identical (`cmp` confirms) — expected, since no production code changed.

## 2. Root-cause investigation

### 2.1 CPU-side pipeline — verified correct at every layer

Instrumented (temporarily; fully reverted, confirmed via `git diff HEAD`)
every stage from decode through shader binding for DamagedHelmet's one
material (`Material_MR`, all 5 slots present):

- **Decode** (`decodeTextureForUpload`/`decodeStbForUpload`,
  `src/rx_asset/texture_decode.cpp`): per-slot avg/max RGBA logged and
  cross-checked in Python against the real `Default_*.jpg` files. Decode is
  byte-faithful — e.g. baseColor's decoded average `(114,117,115)` matches
  the source JPEG's true mean `(114.9, 117.3, 115.4)` exactly; every slot's
  `maxRGB` shows real (non-degenerate) content survived decode intact.
- **Slot→role mapping** (`textureRefForSlot()`, `fillRef()` lambda,
  `src/rx_asset/import_gltf.cpp`): each of the 5 `fillRef()` calls passes a
  distinct `MaterialTextureSlot` and `TextureRole` pair; the marshal loops
  (`registerDecodedTexturesAndPatchMaterials`,
  `marshalGltfImportPrepareStep`) iterate `slotIdx 0..4` in fixed array
  order and patch the correct `TextureRef` field each time. No aliasing,
  no off-by-one.
- **Bindless registration** (`TextureCache::registerRealTexture`,
  `BindlessTable::registerSampledImage`): each slot gets its own fresh
  `Texture2D`, own VMA allocation, own bindless index
  (`dstArrayElement == internal.index()` symmetrically) — confirmed
  baseColor/MR/normal/occlusion/emissive landed at distinct bindless indices
  5/6/7/8/9 respectively, in every run.
- **CPU→shader param binding** (`setupMaterials()`,
  `samples/08_gltf_viewer/main.cpp`; `reflectMaterialLayout()`,
  `src/rx_material/material_system.cpp`): reflected field offsets for the 5
  texture-index fields are non-overlapping (64/68/72/76/80, 4 bytes each);
  `resolveTextureIndex()` returns the same 5/6/7/8/9 values that
  `setMaterialParam()` writes at those exact offsets. No collision.
- **Upload timing / ring-buffer**: DamagedHelmet's 5 textures are each
  *exactly* 16 MiB (2048×2048×4), matching the Uploader's default 16 MiB
  staging ring, so every texture after the first forces a wrap-and-wait in
  `reserveRingSpace()`. This was investigated as a plausible corruption
  vector; disproven empirically by re-running with a 128 MiB ring buffer
  (no wrap at all) — behavior was unchanged.

### 2.2 GPU/shader-side sampling — verified correct

Using a temporarily modified `standard_pbr.slang` (fully reverted after),
confirmed `gTextures[5]` (baseColor) and `gTextures[6]` (metallicRoughness)
sample **visibly and measurably different** content once a build artifact
(below) was corrected. Cross-referencing pixel data with Python/numpy against
the real `Default_albedo.jpg`:

- The small green patch visible in a baseColor-only render (470/65536
  pixels) has mean color `(15.8, 82.0, 10.6)` — matching the real albedo
  texture's own (rare, 0.32% of pixels) green content, mean `(17.5, 93.8,
  11.0)` — **not** metallicRoughness's green, mean `(0.9, 188.7, 14.6)`,
  which is visibly and numerically distinct.
- UV coordinates visualized directly (`return float4(v.uv, 0, 1)`) are a
  smooth, continuous gradient across the whole mesh — no scrambling, no
  degenerate/zero regions.
- Metallic/roughness sampled and visualized directly show a physically
  plausible distribution (mostly low-metallic dome, high-metallic
  panels/mechanisms) — not garbage.

**Conclusion:** the import → registration → binding → sampling pipeline is
correct end to end for this asset, at this commit.

## 3. Two false leads corrected during the investigation

1. **A methodology bug in my own testing, not a product bug.**
   `samples/08_gltf_viewer/CMakeLists.txt`'s `POST_BUILD` custom command that
   deploys `shaders/material/*.slang` next to the binary has **no file-level
   dependency tracking** (`add_custom_command(TARGET ... POST_BUILD)` with no
   `DEPENDS` on the four `.slang` source files). Editing a shared `.slang`
   file and doing an ordinary incremental `cmake --build` (with no C++
   source touched) leaves the *deployed* runtime copy stale — Ninja reports
   "no work to do" and the sample silently keeps running the old shader. This
   produced a false "baseColor and metallicRoughness sample identically"
   result early in the investigation, until caught by a sanity probe
   (sampling the D11 checkerboard fallback and getting the *previous*
   buggy-looking image back, which is impossible if indexing worked).
   Recommend the coordinator route a follow-up to add
   `DEPENDS shaders/material/material.slang ... standard_pbr.slang ...`
   to that custom command — this is a real, latent trap for the next
   developer who edits a shared material shader, independent of this
   investigation. Not fixed here (CMake build-wiring change, outside this
   task's material-resolution/binding/shader scope, and not the reported
   defect).
2. **Emissive term reading back as literal zero.** Investigated as a
   possible defect; the decoded texture and its bindless registration are
   correct (confirmed via a fixed-UV probe sampling the texture's own known
   brightest texel directly, which read back correctly bright). The real
   explanation: the default orbit camera's specific framing in this headless
   capture does not bring any visible mesh UV into the small (1.4% of
   pixels) glowing region of `Default_emissive.jpg` — a camera-coverage
   fact, not a texture-identity bug.

## 4. Assessment of the reported symptom

Given the above, the dark/mottled appearance relative to the Khronos
IBL-lit reference is consistent with:

- FG1's flat-ambient-only lighting model (no IBL) — explicitly called out
  as expected in this task's own brief, not something to fix here.
- DamagedHelmet's real baseColor content being genuinely dark/weathered
  over large areas (14.7% of the albedo texture is near-black, only 20.6%
  near-white) with a small (0.32%) legitimate green vent-light accent —
  confirmed byte-exact against the real source JPEG, not a swapped texture.
- This specific camera framing/light direction, which the task explicitly
  scoped out ("do NOT try to fix lighting").

No evidence supports a texture-slot/index swap in the currently committed
code. If the project owner's original local build showed something visibly
different from what this investigation reproduces at `ad9a5e6`, the
likeliest explanations are (a) a different/older commit, or (b) exactly the
stale-shader-deploy trap in §3.1 above, which is asset-independent and could
plausibly explain a stale local build showing wrong content after an
unrelated edit.

## 5. Verification-gap closed regardless

Per the task's explicit instruction, built the requested discriminating
slot-swap test independent of whether a live bug was found — this is
permanent regression protection the existing suite lacked (every existing
single-slot test proves "this slot resolves to *some* real texture"; none
proved "five slots resolved together never cross-contaminate").

- **Fixture** (new, committed): `assets/test/cube_slot_swap_probe.gltf` — a
  cube with one material carrying all 5 StandardPBR texture slots, each
  bound to a distinct, pure-primary/secondary color PNG:
  `assets/test/textures/slot_swap_{basecolor,metalrough,normal,occlusion,emissive}.png`
  (red/green/blue/magenta/cyan respectively — every channel 0 or 255, so
  sRGB round-tripping can never blur the assertion). Generation documented
  in `assets/test/textures/generate_fixtures.sh` (extended, same
  ImageMagick-`convert` convention as every other fixture in that
  directory).
- **Test** (new): `TEST_CASE("Slot-swap discrimination: ...")` in
  `src/rx_asset/tests/texture_cache_test.cpp` (`rx_asset_tests` binary) —
  imports the fixture through `Registry::importGltf()` with a real
  `TextureCache`, resolves all 5 `MaterialAsset::TextureRef` handles, and
  for each one: renders+reads back the actual sampled bindless texture
  content (reusing the file's own established
  `renderAndReadbackQuadrants()` GPU pipeline) and asserts it matches
  **only** its own expected color and **not** any of the other 4 slots'
  colors.
- **Swap-and-revert proof** (baseColor/metallicRoughness pair, as
  requested): edited `cube_slot_swap_probe.gltf` in place, swapping
  `baseColorTexture.index`/`metallicRoughnessTexture.index` (0↔1), re-ran
  the already-built test binary (a pure JSON-fixture edit needs no
  recompilation) — **test failed**, 10/69 assertions, baseColor read back
  green `(0,255,0)` instead of red, exactly the swap's own signature.
  Reverted via `git checkout -- assets/test/cube_slot_swap_probe.gltf`
  (the file was already staged in the index from this task's own work, so
  this is a guaranteed byte-exact revert — confirmed via `git diff`
  showing empty afterward); re-ran — **69/69 passing** again. A literal
  second git worktree was not used for this specific proof: the swap is a
  pure data-file edit requiring no C++ recompilation, the fixture is a
  brand-new file with zero collision surface against Task 17's concurrent
  rx_platform/rx_rhi_vk/samples work, and git's index gave an equally
  strong (and much faster) revert guarantee. This methodology choice and
  its reasoning are recorded here for the coordinator's review.

## 6. Verification

- `rx_asset_tests` (includes the new test): **32/32 test cases, 538/538
  assertions**, zero validation errors.
- `rx_asset_gltf_tests`: **48/48, 292/292**.
- `rx_asset_gltf_gpu_tests`: **57/57** clean on 3 of 4 runs; one run showed
  1 failed case out of 57, with a *different total assertion count* each
  run (8067212 / 8192741 / 8364425 / 8468233) — this is a pre-existing
  wall-clock/iteration-budget-driven test elsewhere in that binary (not
  touched by this investigation; none of my changes are linked into this
  binary's sources), flaky under host load. Flagging for the coordinator;
  not chased further as out of scope.
- `rx_material_gpu_tests`: **49/49, 2156/2156**.
- `rx_material_tests`: **14/14, 75/75**.
- Full serial `ctest --test-dir build/linux-native -j1`: **22/22 passed**,
  including `sample_08_gltf_viewer_headless` (the D17 gate).
- `windows-cross-zig`: full project builds clean, **154/154 targets**,
  after clearing a stale `mikktspace` FetchContent populate-stamp left
  over from an interrupted prior build in that directory (pre-existing —
  the `UPDATE_DISCONNECTED TRUE` idempotency fix already in
  `third_party/CMakeLists.txt` worked correctly once the stale on-disk
  state was cleared; not caused by this investigation).
- Zero unfiltered Vulkan validation errors in any run (only the two
  already-documented "known false positive" categories:
  `SPIR-V SourceLanguage=Slang` and the separate-sampler
  `SYNC-HAZARD-READ_AFTER_WRITE` misclassification, both pre-existing and
  named as such in the codebase's own comments).
- No D17 references regenerated — no production code changed, so
  regenerating would be a no-op at best (or introduce unrelated
  driver-specific pixel noise at worst).

## 7. Files touched (final state)

- **New, committed:** `assets/test/cube_slot_swap_probe.gltf`,
  `assets/test/textures/slot_swap_{basecolor,metalrough,normal,occlusion,emissive}.png`,
  extension to `assets/test/textures/generate_fixtures.sh`, new
  `TEST_CASE` in `src/rx_asset/tests/texture_cache_test.cpp`.
- **Reverted to pristine (zero diff vs `HEAD`):** all temporary diagnostic
  instrumentation in `shaders/material/standard_pbr.slang`,
  `src/rx_asset/import_gltf.cpp`, `src/rx_asset/import_pipeline.h`, and
  `samples/08_gltf_viewer/main.cpp` — confirmed via
  `git diff HEAD --stat` returning empty for all four.

## 8. Concern for the coordinator: concurrent-commit collision

This task ran concurrently with Task 17 (rx_platform/rx_rhi_vk/samples,
`--fullscreen`) in the same shared working tree. My new test/fixture files
(staged via `git add` during the investigation) were **not** committed by
me directly — they were swept into a third party's `git add -A`-style
commit, `ed5239a` ("docs: registry — techniques-phase charter..."), a
docs/SDD commit unrelated to this task, alongside Task 17's own
`rx_platform`/`rx_rhi_vk` core changes. Task 17's own report independently
flags this same collision ("the concurrent-agent commit-split race on the
core rx_rhi_vk/rx_platform changes"). I did not rewrite shared history to
fix the attribution — too risky with other agents concurrently active on
the same branch, and not this task's call to make unilaterally. Content
was verified byte-for-byte correct in that commit
(`git show ed5239a --stat` confirms exactly my intended files, no more, no
less). Flagging for the coordinator's awareness; a future interactive
rebase/history cleanup, if desired, is a coordinator decision.

## 9. Recommendation

Close this P0 as **not reproducible as a texture-identity defect** at
`ad9a5e6`. If the visual result is still considered unacceptable, the next
productive step is almost certainly on the lighting side (FG1 ambient
tuning, or fast-tracking IBL) rather than further material-resolution
investigation — but that is explicitly out of this task's scope and a
separate decision for the coordinator/roadmap.
