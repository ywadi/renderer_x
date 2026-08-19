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

---

## 10. SUPERSEDE ADDENDUM (fix round, sampler-wrap P0)

**This section corrects Section 4-9 above. The original text above is left
unedited (per instruction) — read it as the record of what the first
investigation could see, not as the current verdict.**

### 10.1 The no-bug verdict is REFUTED

Adversarial verification (a separate task, after this report) found the
real defect one layer below everything this investigation audited:
`shaders/material/material.slang`'s `rx_sampleTexture()` sampled **every**
material texture slot through **one hardcoded, process-wide
`CLAMP_TO_EDGE` sampler** (`material_system.cpp`'s `defaultSamplerInfo`,
the documented Task 4 seam-bleed choice), regardless of that texture's own
glTF wrap mode. DamagedHelmet's `TEXCOORD_0` V coordinate lies **entirely**
in `[1.0005, 1.9987]` — spec-legal, relying on glTF's own default REPEAT
wrap for UVs outside `[0,1]`. Under CLAMP_TO_EDGE every fragment's V
clamped to `1.0`, so the whole mesh sampled each texture's bottom edge
texel row: a near-black dome (the albedo's bottom-row content), a flat
green blob (the metallicRoughness bottom row bleeding through the visor
region), and an emissive term that read as **exactly** zero (the emissive
map's bottom row happens to be unlit) — not a camera-framing coincidence,
as §3.2 above concluded. `TextureCache::getOrCreateSampler()` had already
built the *correct* per-texture sampler, honoring each texture's real glTF
wrap mode — it was simply never wired to anything the shader could reach.

Proof chain (adversarial verifier, reproduced independently before this
fix round started): a build-tree `frac()` probe on the sampled UV flipped
the render to teal-visor/silver-panels matching an independent Blender
no-IBL render (`helmet-reference-noibl.png`, this directory); a `v -> 3-v`
flip test was byte-identical to the *bug* (clamp eats a flip the same way
it eats the real UVs — this is what disambiguated "sampler clamp" from
"someone flipped V somewhere," the coordinator's own alternate hypothesis);
no flip transform exists anywhere in the actual pipeline; the colorspace/
decode layer (already verified correct by §2.1 above) stayed clean; the
committed D17 reference PNG **encodes the bug** (it was generated against
the same broken code, so the self-referential D17 gate could never have
caught this); and no fixture in `assets/test/` had UVs outside `[0,1]`,
which is exactly why nothing in the existing suite (including this
report's own new slot-swap discriminator, §5 above) was structurally
capable of exercising this code path at all.

**Why this investigation's own methodology missed it, precisely**: the
UV-gradient probe (§2.2) visualizes UV *before* the sampler ever runs — a
smooth, correct UV gradient says nothing about what wrap mode the sampler
then applies to it. The color-mean/quadrant checks (§2.2, §5) validate
"this slot's content is present and distinct from its siblings" — a
content-exists check, not a content-is-at-the-right-screen-position check.
Both classes of check are still correct and still worth having; neither
was ever capable of seeing a sampler-address-mode defect, because both
sit either upstream or orthogonal to where the bug actually lived.

### 10.2 The fix

Per-slot sampler wiring, analogous to the existing per-slot texture-index
wiring:

- `shaders/material/material.slang`: `rx_sampleTexture()` gained a
  three-argument overload (`textureIndex, samplerIndex, uv`) that samples
  through an **explicit** bindless sampler index; the original two-argument
  form is now a thin wrapper over it using
  `gMaterialGlobals.defaultSamplerIndex` (unchanged shape, now genuinely
  just a fallback).
- `shaders/material/standard_pbr.slang` / `unlit.slang`: `StandardPbrParams`
  gained one bindless sampler index per texture slot (`baseColorSampler`,
  `metallicRoughnessSampler`, `normalSampler`, `occlusionSampler`,
  `emissiveSampler`); `UnlitParams` gained `baseColorSampler`. Every
  `rx_sampleTexture()` call site in both files switched to the new
  three-argument form.
- `src/rx_asset/texture_cache.{h,cpp}`: new
  `TextureCache::getOrCreateSamplerBindlessIndex(const SamplerDesc&)` —
  gets-or-creates the deduplicated `VkSampler` (via the pre-existing
  `getOrCreateSampler()`, unchanged), then gets-or-creates its OWN
  bindless-table registration (a second cache keyed by the already-
  deduplicated `VkSampler` handle, so the mapping logic is not
  duplicated). Released in `~TextureCache()`, symmetric with the existing
  `vkDestroySampler` loop.
- `samples/08_gltf_viewer/main.cpp`: new `resolveSamplerIndex()` (mirrors
  `resolveTextureIndex()`) resolves each slot's real sampler through
  `TextureCache`, falling back to the app's own default sampler only on a
  genuine `TextureCache` failure. `setupMaterials()` wires it for every
  StandardPBR/Unlit texture slot. `SamplerDesc`'s own default-constructed
  values (glTF's documented "sampler unspecified" default: REPEAT wrap,
  auto filtering) are passed straight through for both an ABSENT slot and
  a present slot whose glTF texture referenced no sampler object at all —
  no special-casing needed, since that default IS the spec-correct answer
  either way.
- **Absent-sampler default flipped from CLAMP to REPEAT**: both the app's
  own hand-rolled default sampler (`samples/08_gltf_viewer/main.cpp`) and
  `MaterialSystem`'s own internal default sampler
  (`material_system.cpp`'s `defaultSamplerInfo`) now default to
  `VK_SAMPLER_ADDRESS_MODE_REPEAT` — glTF's own documented default for
  "sampler unspecified," and the honest choice now that real per-slot
  sampling handles wrap-sensitive content; this default is reached only as
  a fallback (the two-argument `rx_sampleTexture()` overload; a genuine
  `getOrCreateSamplerBindlessIndex()` failure).
  `MaterialSystem::create()` gained a `defaultSamplerAddressMode`
  parameter (default `REPEAT`) precisely so a caller that still needs
  CLAMP_TO_EDGE can request it explicitly, rather than depending on a
  process-wide default that no longer defaults to it.
- **Task 4's own seam-bleed test updated honestly, not weakened**: the two
  `test_api_factory.cpp` `TEST_CASE`s that motivated CLAMP_TO_EDGE in the
  first place (`"IRxMaterialInstance::setTexture's bound texture actually
  changes the rendered image..."` and the hot-reload sibling) now call
  `MaterialSystem::create(..., VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE)`
  explicitly. Same byte-exact quadrant assertions as before — this test's
  own deliberately non-tiled 2x2 texture genuinely does need CLAMP_TO_EDGE
  to avoid REPEAT's wrong-neighbor bleed; it now says so explicitly instead
  of inheriting it silently.
- Every `StandardPbr`/`Unlit` param-blob builder in
  `src/rx_material/tests/test_standard_pbr_unlit.cpp` (one shared helper,
  `makeDefaultStandardPbrBlob()`, plus 7 manually-built Unlit blobs at
  individual `TEST_CASE`s) now populates the new sampler field(s), pointed
  at that file's own rig-local CLAMP_TO_EDGE sampler — byte-identical
  behavior to before this fix round, since no test in that suite exercises
  UVs outside `[0,1]`.

### 10.3 The missing regression class (item 2)

New, committed: `assets/test/sampler_wrap_probe.gltf` +
`assets/test/sampler_wrap_probe.bin` — a single quad whose one material's
`baseColorTexture` references the already-committed
`assets/test/textures/quadrant.png` (8x8, TL=red/TR=green/BL=blue/
BR=yellow) through `"samplers": [{}]` (an explicit-but-EMPTY glTF sampler
object) referenced by `"textures":[{"source":0,"sampler":0}]` —
byte-for-byte the same shape DamagedHelmet's own real glTF file uses.

New `TEST_CASE` in `src/rx_asset/tests/texture_cache_test.cpp`
("Sampler-wrap regression: ..."): imports the fixture through the real
`Registry::importGltf()` with a real `TextureCache`, asserts the imported
material's parsed `SamplerDesc` is REPEAT (`wrapS`/`wrapT == 10497`),
resolves the REAL bindless sampler index via
`TextureCache::getOrCreateSamplerBindlessIndex()` (the exact call
`resolveSamplerIndex()` makes in production), and renders two constant-UV
quads through a raw bindless-sampling shader (this file's own established
`buildQuadPipeline()`/`kQuadShaderSource` infrastructure, extended with a
new `renderCustomQuadAndReadbackQuadrants()` helper that accepts caller-
supplied vertices instead of the existing helper's fixed `[0,1]`
corners — added rather than modifying the existing helper, so every
pre-existing caller is unaffected). Both probe UVs sit **wholly outside
`[0,1]`** in V (`1.3125` and `1.6875`, matching DamagedHelmet's own "V
wholly outside `[0,1]`" shape) and land exactly on a `quadrant.png` texel
CENTER post-wrap, so the assertion is filter-mode-independent:

- `V=1.3125` (wraps to texel row 2, the texture's RED half): the
  discriminating probe. Expects RED under the fix; a CLAMP_TO_EDGE revert
  clamps V to exactly `1.0`, deterministically reading the LAST row
  (texture's BLUE half) instead — the identical failure *shape* the real
  DamagedHelmet defect had.
- `V=1.6875` (wraps to texel row 5, BLUE): non-discriminating sanity probe
  (both fix and bug read BLUE here) — proves the texture/pipeline are real
  and not degenerate.

**Revert-test evidence (mandatory, empirical, not asserted):** a literal
scratch `git worktree` (`git worktree add ... 715681b`) was attempted
first, per instruction; it hit a reproducible, DIAGNOSED, and unrelated
zig/mikktspace toolchain quirk — `mikktspace.c` compiled through the
project's `zig cc` wrapper links clean (0 `__ubsan_handle_*` references)
from the main tree's own absolute path, but the identical source, same
compiler flags, compiled from the worktree's own (necessarily different)
absolute path pulled in 11 unresolved `__ubsan_handle_*` symbols at link
time — confirmed NOT a path-alone effect (a manual, minimal `zig cc`
invocation against a plain copy at an alternate path linked clean), most
likely a `~/.cache/zig` content-addressed-cache interaction specific to
this environment, entirely unrelated to any file this fix round touched.
Given the project's own established precedent for exactly this situation
(§5 above, this same report: "A literal second git worktree was not used
for this specific proof... git's index gave an equally strong... revert
guarantee"), the proof was completed instead as an in-tree revert-and-
restore against the already-committed fix (`715681b`), which gives an
identical safety guarantee (the fix was safely committed before the revert
edit, and `git diff`/`git checkout --` confirm a byte-exact restore):

1. **Fixed code** (`715681b`, `getOrCreateSampler()` uses
   `mapWrap(desc.wrapS/wrapT)`): `rx_asset_tests
   --test-case="Sampler-wrap regression*"` → **1/1 test case, 26/26
   assertions, SUCCESS**.
2. **Re-hardcoded-clamp revert** (`getOrCreateSampler()`'s `key.wrapS`/
   `key.wrapT` forced to `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE`
   regardless of `desc`, in-tree, uncommitted): same test →
   **1/1 test case FAILED, 21/26 assertions passed, 5 FAILED** — every one
   of the 5 discriminating assertions (`topLeft`/`topRight`/`bottomLeft`/
   `bottomRight` all reading BLUE instead of RED, plus the explicit
   `CHECK_FALSE(...kBlue...)` also failing) failed exactly as predicted.
3. **Restore** (`git checkout -- src/rx_asset/texture_cache.cpp`,
   confirmed byte-exact via empty `git diff --stat`): same test →
   **1/1 test case, 26/26 assertions, SUCCESS** again.

### 10.4 Shader-deploy DEPENDS (item 3)

Confirmed in-session: a clean-HEAD `sample_08_gltf_viewer_headless` run
produced `UNASSIGNED-CoreValidation-Shader-OutputNotConsumed` against
`standard_pbr.slang`'s vertex outputs — a stale DEPLOYED shader copy left
over from this fix round's own in-place shader edits, exactly the
class of bug this report's own §3.1 flagged as a follow-up and never
fixed. Root cause confirmed: every sample's shader-deploy step used a bare
`add_custom_command(TARGET ... POST_BUILD ...)`, which CMake only re-runs
when the TARGET itself relinks — `add_custom_command(TARGET ...)` has no
`DEPENDS` keyword at all (only the `OUTPUT` form does), so a `.slang`-only
edit left the deployed copy silently stale.

Fixed by converting every such step to the `OUTPUT` + stamp-file +
`add_custom_target` + `add_dependencies` pattern this same file
(`samples/08_gltf_viewer/CMakeLists.txt`) already used for its own D17
reference-PNG deploy — across every sample that had the gap:
`samples/01_triangle` (the precompiled-SPIR-V deploy — same staleness
class, one layer removed: editing the `.slang` source regenerates the
`.spv` via its own tracked custom command, but the COPY next to the binary
was still untracked), `02_hotreload`, `05_multipass`, `06_materials`
(both its own `materials/*.slang` deploy and the shared
`material.slang`/`forward_entry.slang` deploy), `07_stress`, and
`08_gltf_viewer` (both the `material_shaders/` deploy and the tonemap-
shader deploy — the two the brief named explicitly). `04_streaming` and
`03_bindless_mesh`'s own `texture.png` deploy (not a shader) were out of
scope. Verified: a full clean rebuild (§10.6 below) shows every one of
these stamp-generating custom commands firing exactly once, and
`sample_08_gltf_viewer_headless` passes clean from that same clean build.

### 10.5 D17 references regenerated (item 4)

Regenerated via the documented, never-auto-run `tools/regen_references.sh
linux-native`, from the CLEAN rebuilt (§10.6) + freshly redeployed build
directory (per that script's own header comment: lavapipe-only,
`VK_ICD_FILENAMES` forced to the real system ICD). Only
`samples/08_gltf_viewer/references/loaded_scene.png` changed
(`loading_state.png` — the pre-import placeholder frame — is untouched by
this fix, as expected). Rebuilt `sample_08_gltf_viewer` (picks up the new
PNG via its own tracked deploy step) and re-ran the headless gate:

```
sample_08_gltf_viewer: D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true
sample_08_gltf_viewer: D17 loaded_scene gate: failingPixels=0/65536 (0.0000%) pass=true
sample_08_gltf_viewer: headless gate PASSED
```

`helmet-after.png` (this directory) refreshed: captured via
`--write-references` from the same clean build (confirmed byte-identical
to the newly committed D17 `loaded_scene.png` reference via `cmp`), then
upscaled 256x256 → 512x512 (Lanczos, presentation-only — this sample's
headless capture has no resolution flag, so there is no native 512px
render; the upscale is documented here rather than silently presented as
one). Visually: teal visor, silver/white panels, gold accent at the
mouth-guard, and the dark-green dome pattern with cyan vent lights —
matching `helmet-reference-noibl.png`'s own independent Blender no-IBL
render. `helmet-before.png` (the original bug capture) is left untouched
for comparison.

### 10.6 Verification, clean rebuild (item 6)

`rm -rf build/linux-native`, then `cmake --preset linux-native && cmake
--build --preset linux-native` (full project, all 164 targets, ~57s) —
genuinely clean, not incremental; this is the build the D17 regen and
every suite below ran from.

- `rx_asset_tests`: **33/33 test cases, 564/564 assertions** (was 32/32,
  538/538 before this fix round — +1 case for the new sampler-wrap
  regression test, +26 assertions).
- `rx_asset_gltf_tests`: **48/48, 292/292** (unchanged).
- `rx_asset_gltf_gpu_tests`: **57/57, 8,401,735/8,401,735** — clean this
  run (the wall-clock/iteration-budget flake §6 flagged before is
  pre-existing and unrelated to this fix; not chased further, same as
  before).
- `rx_material_tests`: **14/14, 75/75**.
- `rx_material_gpu_tests`: **49/49, 2265/2265** (was 49/49, 2156/2156
  before — the new per-slot sampler `setParam()` calls this fix round
  added to every StandardPbr/Unlit blob-builder account for the increase;
  zero behavior change to any existing assertion).
- Full serial `ctest --preset linux-native -j1`: **22/22 passed**
  (73.56s), including `sample_08_gltf_viewer_headless` — the
  OutputNotConsumed contamination from this fix round's own in-session
  shader edits is gone, confirming §10.4's fix.
- `windows-cross-zig`: full incremental build (existing build directory,
  40 targets touched/relinked) succeeds clean; full serial `ctest
  --preset windows-cross-zig -j1` under Wine: **22/22 passed** (139.77s),
  including `sample_08_gltf_viewer_headless`.
- Zero unfiltered Vulkan validation errors in any run — only the two
  already-documented, pre-existing false-positive categories fire
  (`SPIR-V SourceLanguage=Slang`, and the separate-sampler
  `SYNC-HAZARD-READ_AFTER_WRITE` misclassification), both named as such
  in-code before this fix round and unrelated to it.

### 10.7 Files touched (fix round, final state)

Pathspec-scoped commits (author = local git config, no AI attribution, no
push):

- Production fix: `shaders/material/{material,standard_pbr,unlit}.slang`;
  `src/rx_asset/{texture_cache.h,texture_cache.cpp}`;
  `src/rx_material/include/rx_material/{material_system.h,draw_data.h}`;
  `src/rx_material/material_system.cpp`;
  `samples/08_gltf_viewer/{main.cpp,CMakeLists.txt}`.
- Shader-deploy DEPENDS sweep:
  `samples/{01_triangle,02_hotreload,05_multipass,06_materials,07_stress}/CMakeLists.txt`.
- New regression class: `assets/test/sampler_wrap_probe.{gltf,bin}`;
  `src/rx_asset/tests/texture_cache_test.cpp`.
- Test-suite updates for the new per-slot sampler fields:
  `src/rx_material/tests/{test_api_factory.cpp,test_standard_pbr_unlit.cpp}`.
- D17 regen: `samples/08_gltf_viewer/references/loaded_scene.png`.
- This addendum: `helmet-texture-fix-report.md`.
- `.superpowers/sdd/2026-08-11-phase4-scene-assets/helmet-after.png`
  (SDD workspace artifact, not production code).

No board/plan/spec/ledger file was edited by this fix round.

---

## 11. ROUND 2 (independent review response)

Independent review (`helmet-fix-review.md`, scope: commits `715681b`,
`2f584c4`, `787f978`) verdict: **spec PASS, code quality Approved**, with
one Important and three Minor findings, none blocking. Per this project's
"no deferrals, close all findings in-round" policy, the two findings
assigned to this implementer are closed here; the review's own
worktree-fallback closure (Concern 1) and the pre-existing `gltf_gpu`
flake (Concern 3, tracked separately by the coordinator) needed no further
action from this round.

### 11.1 [Important] StandardPbr-level per-slot sampler coverage gap — CLOSED

**The gap, precisely**: the round-1 regression test
(`texture_cache_test.cpp`, "Sampler-wrap regression...") proves
`TextureCache::getOrCreateSamplerBindlessIndex()` builds and registers the
correct `VkSampler` for a given glTF sampler object — the sampler-
creation/resolution layer — but samples it through a raw bindless test
shader, never through the actual shipped `standard_pbr.slang`/
`StandardPbrParams`. A future bug in how a caller maps each slot's own
resolved sampler index into the *correct* named `gParams` field (e.g.
`setupMaterials()` accidentally writing `metallicRoughnessSampler`'s
resolved value into `normalSampler`) would not have been caught by any
test below the coarse, whole-image D17 pixel gate.

**Closure**: a new GPU `TEST_CASE` in
`src/rx_material/tests/test_standard_pbr_unlit.cpp` ("StandardPBR
per-slot sampler wiring: ..."), through the real
`MaterialSystem`/`standard_pbr.slang`/`StandardPbrParams` path:

- One 1x2 "striped" texture (top texel RED, bottom texel BLUE) bound to
  BOTH `baseColorTexture` and `emissiveTexture`.
- Sampled at the IDENTICAL out-of-`[0,1]` UV (`V=1.125`, reached via the
  same `KHR_texture_transform` offset/scale mechanism the neighboring
  "KHR_texture_transform..." `TEST_CASE` already proves works — a texel-
  center-exact coordinate, filter-mode-independent, matching the round-1
  test's own discipline) through TWO DIFFERENT real samplers:
  `baseColorSampler=REPEAT`, `emissiveSampler=CLAMP_TO_EDGE` — "distinct
  wrap modes on different slots of one material," per the review's own
  requested shape.
- **Isolation, no BRDF entanglement**: `ambientColor=(1,1,1)` +
  `lightColor=(0,0,0)` makes `directLight` identically zero
  (`standard_pbr.slang`'s own `directLight = (diffuse+specular) *
  v.lightColor * NdotL` — a zero `lightColor` factor zeroes it
  unconditionally, regardless of geometry/metallic/roughness), so `color
  = ambient + emissive = ambientColor*occlusion*baseColor.rgb +
  emissiveFactor*emissiveSample` exactly (`occlusion==1.0` via the rig's
  own default white occlusion texture, the same fact this file's own "FG1
  ambient closed-form" `TEST_CASE` already relies on). Two draws each zero
  the OTHER term via its own `*Factor`, so each draw's final pixel is a
  direct, unblended read of exactly one slot's own sampled texel — since
  both slots read the SAME texture at the SAME coordinate, the only thing
  that can make their results differ is which sampler each slot's own
  shader code actually names.
- Since both slots read the same texture at the same coordinate, the
  discriminating assertions are: `baseColorPixel` reads RED
  (`baseColorSampler=REPEAT`: `frac(1.125)=0.125`, row 0); `emissivePixel`
  reads BLUE (`emissiveSampler=CLAMP_TO_EDGE`: clamps to `1.0`, row 1 — the
  edge texel, same reasoning as the round-1 test).
- `BindlessTable::Capacities::samplers` in this file's shared `makeFixture()`
  bumped `2 -> 4` (empirically required: the rig's own default sampler
  plus this test's two new ones is 3 live samplers, exceeding the old
  capacity of 2 — hit directly as a `registerSampler` capacity-exhausted
  rejection before the fix). Purely permissive for every other `TEST_CASE`
  in the file (none registers more than 1 sampler).
- Explicit teardown added (`vkDestroySampler` for both new raw samplers,
  before `destroyRig()`) — the first version of this test leaked them,
  caught directly by this project's own teardown-time validation-error
  gate (`VUID-vkDestroyDevice-device-00378`) before being fixed.

**Revert-test evidence (mandatory, same rigor as round 1, done in a
genuine scratch worktree this time)**: per the review's own FYI (symlink
`toolchain/`/`.deps-cache`/`third_party/slang-prebuilt` into a scratch
worktree — the convention already visible in this repo's own sibling
agent worktrees under `.claude/worktrees/`), `git worktree add` at
`026186f` (round 2's own commit, below) configured and built
`rx_material_gpu_tests` clean on the first attempt — no toolchain issue
this time.

1. **Fixed code** (`026186f`): `rx_material_gpu_tests
   --test-case="StandardPBR per-slot sampler wiring*"` → **1/1 test case,
   114/114 assertions, SUCCESS**.
2. **Field-swap revert** (in the scratch worktree only, never committed):
   `makeBlob(repeatSampler, clampSampler, ...)` at both call sites edited
   to `makeBlob(clampSampler, repeatSampler, ...)` — swapping exactly
   which resolved sampler value feeds which named field, simulating the
   precise "`setupMaterials()` wrote the wrong resolved index into the
   wrong slot" bug class this test exists to catch. Same test → **1/1
   test case FAILED, 110/114 assertions passed, 4 FAILED**:
   `baseColorPixel` read `(0, _, 255)` (BLUE, was RED — `r>200` and
   `b<50` both failed) and `emissivePixel` read `(255, _, 0)` (RED, was
   BLUE — `b>200` and `r<50` both failed) — the exact inversion predicted.
3. Worktree removed (`git worktree remove --force` + branch deleted); main
   tree confirmed untouched throughout (`git diff --stat` on the real
   source file, empty).

### 11.2 [Minor] `renderCustomQuadAndReadbackQuadrants()` duplication — CLOSED

`texture_cache_test.cpp`'s `renderAndReadbackQuadrants()` gained one
defaulted trailing parameter, `const std::array<QuadVertex, 4>& vertices
= kQuadVertices` — every pre-existing caller (7 call sites across 5
`TEST_CASE`s) compiles and behaves byte-identically without passing it.
The near-identical duplicate function (`renderCustomQuadAndReadbackQuadrants()`,
~90 lines) and its one call site (the round-1 sampler-wrap regression
test) are removed; that call site now passes its own custom vertex arrays
as the new trailing argument to the single shared function. Net: -201/+that
same body once, not twice (see the round-2 commit's own diffstat).

### 11.3 Verification (round 2)

- `rx_material_gpu_tests`: **50/50, 2379/2379** (was 49/49, 2265/2265 —
  +1 case/+114 assertions for the new coverage test; zero regressions in
  the other 49).
- `rx_asset_tests`: **33/33, 564/564** (unchanged — the dedup is
  behavior-preserving by construction, confirmed).
- Zero unfiltered Vulkan validation errors (including the sampler-leak
  teardown error caught and fixed during this round's own development,
  before commit).
- Full serial `ctest --preset linux-native -j1`: **22/22 passed**
  (86.34s).
- `windows-cross-zig`: incremental build (9 targets touched) succeeds
  clean; full serial `ctest --preset windows-cross-zig -j1` under Wine:
  **22/22 passed** (139.32s).

### 11.4 Files touched (round 2)

Pathspec-scoped commit `026186f` (author = local git config, no AI
attribution, no push): `src/rx_material/tests/test_standard_pbr_unlit.cpp`,
`src/rx_asset/tests/texture_cache_test.cpp`. This addendum section:
`helmet-texture-fix-report.md`. No board/plan/spec/ledger file edited.
