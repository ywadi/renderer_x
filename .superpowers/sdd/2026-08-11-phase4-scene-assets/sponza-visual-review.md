# Independent review: Sponza visual-defect fix round (58abe48..59f60b6)

Reviewer: independent, did not write this code. Commit under review:
`59f60b6` (local, on `main`, not pushed). Inputs: `sponza-visual-investigation.md`
and `review-58abe48..59f60b6.diff`. All empirical work below was re-run by
this review from scratch, on this machine, driver-labeled per the standing
rule (lavapipe-only is not verification).

## Verdicts

- **Spec compliance (round mandate): PASS.** Both reported P1 defects
  (Sponza texture-to-mesh misassignment, fly-camera W/S inversion) are
  root-caused correctly, fixed correctly, covered by new discriminating
  tests that are ctest-registered and pass, and independently reproduced
  fixed on real NVIDIA hardware by this review (not just re-reading the
  report's own artifacts).
- **Code quality: Approved**, with one trivial documentation nit (below).
  No correctness, safety, or thread-affinity findings of any severity.

## 1. Texture fix (`materialIndexForSpan` / `recordForwardChunk`)

### 1a. Span-to-materialIndex mapping across instancing collapse and block boundaries

Traced the invariant to its source rather than trusting the header comment:

- `rx::scene::resolveDrawGroups()` (`src/rx_scene/draw_list.cpp:855-893`)
  groups `ViewLists::commands` into contiguous runs **strictly by
  materialIndex adjacency** — `materialOf(i) != currentMaterial` is the
  *only* condition that starts a new `ResolvedDrawGroup` (line 883). A
  `ResolvedDrawGroup` therefore has exactly one materialIndex across its
  whole `[firstCommand, firstCommand+commandCount)` range, by construction,
  independent of how many distinct `pipelineToken` values happen to
  coincide across groups (Sponza: 22 distinct materialIndex groups, 1
  shared pipelineToken).
- D26.3 instancing collapse (`sameDrawIdentity()`, `draw_list.cpp:337-348`)
  includes `a.materialIndex == b.materialIndex` in its identity tuple, so a
  single collapsed `DrawCommand` (`instanceCount > 1`) can never span two
  materialIndex values either — every instance folded into one draw shares
  one materialIndex.
- `splitByBlockAndGroup()` (`draw_recording.cpp:7-49`) only ever
  *subdivides* at group/block boundaries (`spanEnd = min(groupEnd, blockEnd,
  rangeEnd)`); it never merges two groups. Every emitted `RecordSpan`
  therefore lies within exactly one `ResolvedDrawGroup`, and reading
  `payloads[commands[span.commandOffset].firstInstance].materialIndex` off
  the span's first command is exact for the whole span — including a span
  that starts mid-group (a worker chunk's `[begin,end)` slice need not
  align to a group boundary), since every command in that group shares the
  same materialIndex regardless of where within it a span starts.

"Impossible by construction" is verified, not merely asserted by the
comment.

### 1b. Pipeline-rebind vs. material-rebind decoupling

Read the current `recordForwardChunk()` directly (`samples/09_scene/main.cpp:2160-2206`):
`lastToken`/`havePipeline` (pipeline) and `lastMaterialIndex`/`haveMaterial`
(material) are two fully independent state machines over the same span
loop — the material branch's condition (`!haveMaterial || materialIndex !=
lastMaterialIndex`) does not read `lastToken` at all. Two consecutive spans
sharing one `pipelineToken` but differing `materialIndex` correctly skip
the (redundant) pipeline rebind but still take the material rebind branch.
Confirmed by direct code reading, not diff-reading alone.

### 1c. Same defect class elsewhere?

- **Sample 08 (`recordSceneDraws()`, `samples/08_gltf_viewer/main.cpp:1508-1565`)**:
  does not use `resolveDrawGroups()`/pipeline-token grouping at all — it
  iterates `app.draws` directly (one `DrawItem` per submesh-instance, never
  collapsed) and rebinds off `draw.materialBindingIndex`/`material.handle`
  (a real material identity) directly, never off a pipeline token. No
  defect of this class exists there; if anything its state-sort is less
  aggressive than the fixed sample 09 (rebinds pipeline whenever material
  changes, even same-pipeline cases) — a minor efficiency delta only, not
  a correctness issue, and out of this round's scope.
- **Shadow recorder (`recordShadowPass()`, `samples/09_scene/main.cpp:2038-2069`)**:
  binds only the bindless set (set 0) and a push constant; it never binds
  a material-params descriptor set at all (RC3: single-pipeline, depth-only
  pass). The defect class (material identity resolved via pipeline token)
  cannot exist there because no per-material descriptor selection happens
  in that pass at all.

No finding: the defect class is fully scoped to the one call site that had it.

## 2. Empirical verification — real NVIDIA driver

**Driver: NVIDIA GeForce RTX 2080, proprietary driver 580.82.07**
(`VK_ICD_FILENAMES` explicitly forced to
`/usr/share/vulkan/icd.d/nvidia_icd.json` — this machine also has lavapipe
registered; cross-checked the same forced ICD against `vulkaninfo --summary`,
which reports `deviceName = NVIDIA GeForce RTX 2080`, confirming the loader
was not silently falling back to lavapipe).

- **`sample_09_scene --present --scene assets/fetched/Sponza/glTF/Sponza.gltf --validate`**,
  ~24s clean run (X11 `DISPLAY=:1`): 25 materials / 1 renderable loaded,
  ran the full duration, **0 unfiltered Vulkan validation errors** (23,319
  filtered "known false positive" lines, 0 outside that filter), window
  closed cleanly.
- **Default startup pose, captured live** (`import -window`): roof-tile
  texture appears ONLY on the roof, a distinct stone-wall texture below it
  — byte-for-byte-matching qualitative content of the committed
  `sponza-after-default.png`.
- **Interior pose, captured live**: reintroduced the investigation's own
  `RX_DEBUG_CAMERA_POSE` diagnostic hook (temporarily, same mechanism
  described in the report's §2.5 — the original exact coordinates are not
  recorded in the report's prose, only in ImageMagick captures, so this
  review picked its own interior vantage point rather than reproducing the
  original pixel-for-pixel) at `(-10, 1.6, -0.31)`, yaw -90°: a long nave
  corridor with correctly-differentiated stone arches/columns, four
  distinct banner cloth textures (red/green/blue), and the lion medallion
  visible at the far wall — matching the qualitative subject and material
  layout of the committed `sponza-after-interior.png`/`sponza-gt-interior.png`.
  **No roof-tile smearing anywhere** — column shafts and arches show their
  own stone texture, not the roof's tile pattern. The hook was reverted
  byte-identically (`git diff --stat` empty) before continuing.
- **Workshop scene** (`assets/fetched/Workshop/workshop_render_scene.glb`),
  run headless under a dedicated `Xvfb :199` (real NVIDIA driver, same
  forced ICD, no visible display): loaded cleanly — 316 renderables, 31
  materials — ran the full 25s, **0 unfiltered Vulkan validation errors**
  (48/48 filtered-only). Live capture shows correctly per-material
  differentiated objects (distinct floor, ceiling beams, barrels, tarps) —
  no uniform-texture smearing, the same qualitative signature the Sponza
  fix targets. A second, independent multi-material asset exercising the
  same code path with no regression.
  - **Unrelated pre-existing issue observed, out of this round's scope**:
    5 of Workshop's textures (`s_car baseColor/metallicRoughness/normal/occlusion`,
    `Material.047 normal`, all 4096×4096) fail to upload —
    `Uploader::uploadImageMips: level 0 is 67108864 bytes, exceeding the
    16777216-byte staging ring buffer's total capacity` — and fall back to
    the D11 checkerboard (visible as the magenta/purple car in the
    captured screenshot). This is a staging-buffer-sizing limitation in
    `Uploader`/`TextureCache`, untouched by this diff, unrelated to the
    materialIndex fix under review. Flagging for the coordinator's
    registry, not a finding against this round.

## 3. Fly-camera W/S fix

- `Camera::forward()` (`src/rx_scene/include/rx_scene/camera.h:144`) is
  `orientation * (0,0,-1)` — confirmed directly, matching the report's
  premise. `FlyCamera::moveLocal()` (`fly_camera.h:29-31`) is a plain
  `position += right()*x + up()*y + forward()*z`. `flyCameraLocalMoveDelta()`
  (`fly_camera.h:73-75`) returns `(strafe, vertical, forward)` — unnegated.
  The chain is consistent end to end: no second negation anywhere.
- `updateFlyCamera()` (`main.cpp:2409-2423`): keyboard (`forward += 1.0F`
  on W, `-= 1.0F` on S) and gamepad (`forward += -pad.leftStick.y`) both
  accumulate into the **same** local `float forward` variable before the
  single `flyCameraLocalMoveDelta(forward, strafe, vertical)` call —
  confirmed directly from source. Gamepad forward/back rides the identical
  fixed path; no separate gamepad-specific code exists to have missed.
- **Revert-and-reprove, done independently by this review** (not just
  re-reading the report's own numbers): reintroduced the historical bug
  (`glm::vec3(strafe, vertical, -forward)`) in `fly_camera.h`, rebuilt
  `sample_09_scene_tests`, reran on lavapipe:
  `16 test cases | 11 passed | 5 failed`, `49 assertions | 41 passed | 8 failed`.
  **The 5 failures were exactly the forward-axis cases**: W, S,
  W-after-rotation, gamepad-forward, and the axis-assignment structural
  check — A/D, Space/Ctrl, both mouse-look cases, and all 7
  `splitByBlockAndGroup`/`materialIndexForSpan` cases stayed green.
  Restored the fix byte-identically (`git diff --stat` empty), rebuilt,
  reran: `16/16 passed, 49/49 assertions`.
  - Note: the report's own §1.3/§1.4 prose states "14 total... 42
    assertions" for this same discrimination. The actual registered binary
    has 16 cases / 49 assertions (7 `draw_recording` + 9 `fly_camera`) —
    the report's count appears to predate the 2 new `materialIndexForSpan`
    cases being folded into the same binary. The **qualitative claim**
    (exactly the 5 forward-axis cases fail, nothing else) is exactly
    reproduced; only the absolute total in the prose is stale. See finding
    below.

## 4. Test suite

- **Lavapipe, full serial**: `VK_ICD_FILENAMES` forced to `lvp_icd.json`,
  `ctest --test-dir build/linux-native -j1` → **29/29 passed**, 74.57s.
  `sample_09_scene_tests` (test #29) confirmed ctest-registered and
  includes both the fly-camera and materialIndexForSpan cases.
- **windows-cross-zig**: `cmake --build build/windows-cross-zig` — clean,
  incremental (only `main.cpp`/`test_fly_camera.cpp` relinked from a prior
  build). Full serial `ctest --test-dir build/windows-cross-zig -j1` under
  Wine → **29/29 passed**, 171.04s.

Both match the report's claimed counts; both re-run from scratch by this
review, not taken on faith.

## 5. Commit hygiene

- Single commit: `59f60b6`, exactly the commits between `58abe48..59f60b6`.
- Author/committer: `Yousef Wadi <ywadi85@gmail.com>` on both — matches
  local `git config user.name`/`user.email` exactly.
- Commit message: no `Co-Authored-By`, no AI attribution of any kind.
- File scope: exactly `samples/09_scene/{main.cpp,draw_recording.{h,cpp},
  fly_camera.h,tests/*}` plus the investigation's own report + 6 PNGs under
  `.superpowers/sdd/2026-08-11-phase4-scene-assets/`. No plan/spec/ledger/
  board file touched.
- Push status: `git log origin/main..HEAD` shows `59f60b6` is 1 commit
  ahead of `origin/main` — not pushed.
- Working tree after this review's own temporary edits (camera-pose probe
  in `main.cpp`, bug-reintroduction in `fly_camera.h`): both restored
  byte-identically (`git diff --stat` empty for both files). Only
  pre-existing change left in the tree: `.superpowers/sdd/.../progress.md`
  (left untouched, as instructed).

## 6. Adjudications

### 6a. D10 mip-generation deferral

Verified the technical premises directly against source, not just the
report's prose:
- `decodeStbForUpload()` (`src/rx_asset/texture_decode.cpp:443-459`) does
  upload mip 0 only, with an explicit recorded-limitation log line.
- `TextureCache::applyDecodeResult()` (`src/rx_asset/texture_cache.cpp:386-412`)
  is genuinely generic over `decoded.levels.size()` (a `reserve()` +
  loop over however many levels are present) — confirmed, not "possibly
  generic."
- `getOrCreateSampler()` (`texture_cache.cpp:519-556`) does set
  `maxLod = VK_LOD_CLAMP_NONE` for every `MIPMAP`-named `minFilter`
  (`singleLevelOnly=false` for those enum values) — confirmed, so a real
  mip chain would be sampled the moment one exists, no sampler-side change
  needed.
- Option B's blocker (`toktx`/`ktx` not installed in this environment) —
  confirmed: neither binary is on `PATH` here.

**Gamma-correctness concern: real, not overstated.** Box-averaging mip
levels directly in sRGB-encoded byte space (rather than converting to
linear light, averaging, then re-encoding) is a well-established,
non-hypothetical energy-loss/darkening artifact — the sRGB transfer
function's convexity means a naive arithmetic mean of encoded values is
systematically darker than the correct linear-space mean once re-encoded.
This is standard, textbook graphics-pipeline knowledge (every
production-grade mip generator — stb_image_resize2's own linear-colorspace
mode, DirectXTex, Filament's tools, etc. — treats this as a required step
for sRGB-role textures, not an optional nicety), so the report's
characterization of this as a real cost (not a shortcut-able detail) is
sound engineering judgment, not manufactured caution to justify deferral.

**Adjudication: the deferral is sound.** D10 is a pre-existing, already-
recorded limitation (not introduced by this round's fix), the two viable
closing paths both carry genuine, verified costs (Option A: real
correctness work + a multi-sample D17 reference-image regression-baseline
risk; Option B: a missing toolchain dependency this investigation
correctly declined to add unilaterally), and the report flags it
explicitly for the coordinator rather than silently leaving it recorded —
consistent with this project's "no deferred fixes... only feature
phase-fits go to the registry" posture (this is a phase-fit scheduling
call, not a swept-under-the-rug in-round finding).

### 6b. NonUniformResourceIndex assessment

Traced the actual shader-side data path end to end, not just the report's
narrative:
- `rx_sampleTexture()` (`shaders/material/material.slang:360-362`) reads
  `gTextures[textureIndex]` where `textureIndex` comes from
  `gParams.baseColorTexture` etc. — `gParams` is
  `[[vk::binding(0,1)]] ParameterBlock<StandardPbrParams> gParams`
  (`shaders/material/standard_pbr.slang:98-99`), bound via a single
  `vkCmdBindDescriptorSets(..., firstSet=1, 1, &mb->paramSet, ...)` call
  per material change (`main.cpp:2196-2197`) — **one descriptor set for
  the entire draw**, never indexed by any per-instance/per-invocation
  value.
- `RxDrawData::materialIndex` (`material.slang:253`) — the field the task
  prompt's own "materialIndex via firstInstance → per-draw buffer" framing
  refers to — is written into the per-instance GPU buffer
  (`main.cpp:1953`, `row.materialIndex = payload.materialIndex`) but is
  **never read by any shader** (grep across every `.slang` source file
  confirms it is declared once and never referenced in
  `forward_entry.slang`, `standard_pbr.slang`, or `unlit.slang`). It exists
  purely as CPU-side bookkeeping ground truth for
  `resolveMaterialIndexToBinding()`'s descriptor-set selection — it plays
  no role in shader-side texture indexing at all.
- D26.3 instancing collapse (already verified in §1a) only ever merges
  instances sharing one materialIndex, so even a collapsed multi-instance
  draw's `gTextures[textureIndex]` reference is invariant across every
  `SV_VulkanInstanceID` in that draw.

**Adjudication: the reasoning holds, and understates its own case.** Every
texture index in this path is not merely "dynamically uniform" in the
weaker SPIR-V-spec sense (uniform across a draw but sourced from data that
could in principle vary) — it is a genuine bind-time constant, sourced
from a per-material UBO rebound only on an actual material change, with no
reachable code path (including under instancing collapse) that could make
it vary within one draw's invocations. `NonUniformResourceIndex` is
correctly assessed as unnecessary here. The report's own hedge (flagging
it as a speculative, unverified Windows/AMD hardening candidate rather
than shipping a speculative shader edit) is the right level of caution —
nothing found in this review changes that recommendation.

## Findings

1. **Trivial / documentation nit** — `sponza-visual-investigation.md`
   §1.3/§1.4 states "14 total in `sample_09_scene_tests`, 42 assertions"
   for the fly-camera revert-proof; the actual registered binary (which
   also contains the same round's 2 new `materialIndexForSpan` cases) has
   16 cases / 49 assertions. The qualitative discrimination claim (exactly
   5 forward-axis failures, 8 assertion failures, everything else green)
   is exactly correct and was independently reproduced by this review —
   only the absolute totals in the prose are stale. No code change
   needed; cosmetic only.

No other findings of any severity (correctness, thread-safety, style, or
otherwise) surfaced in this review.
