# Texture-path round review (#32) — runtime mips (Option A) + oversized-upload staging fix

Independent review of `dd1e110` (stb runtime mip generation), `5834114`
(chunked staging), `fcedd3c` (report), against `texture-path-round-brief.md`.
Reviewed by re-reading every changed line in
`review-9c76f01..fcedd3c.diff`, then independently re-deriving/re-running
every load-bearing claim in `texture-path-round-report.md` rather than
trusting it — see command tails below. All temporary probe edits were
restored byte-identically (`git diff`/`git status` confirmed empty on both
touched files after each probe) and rebuilt green before moving on. Machine:
same box as the implementer's — real NVIDIA GeForce RTX 2080, driver
580.82.07, plus `lvp_icd.json` lavapipe, both system-installed; `DISPLAY=:1`
a live X session.

## Verdict 1 — Spec compliance vs the round brief: PASS

Both release-gate items (Item A runtime mips, Item B chunked staging) meet
every binding requirement in the brief. Detail below; no compliance gaps
found.

## Verdict 2 — Code quality: APPROVED, no blocking findings

One LOW-severity test-coverage finding (not a functional defect — see
below). No MEDIUM/HIGH findings.

---

## Item A — runtime mip-chain generation

- **sRGB-role plumbing correctness (the "occlusion averaged in sRGB would
  be the inverted bug" concern)** — verified by reading the source, not
  just the report: `roleExpectsSrgb()` (`texture_decode.cpp`) returns true
  ONLY for `BaseColor`/`Emissive`; `generateStbMipChain()`'s `isSrgb` flag
  is derived directly from it, so `Occlusion`/`MetallicRoughness`/
  `GenericData`/`Normal` all take the plain `STBIR_4CHANNEL` linear path,
  never `STBIR_TYPE_UINT8_SRGB`. No inverted-bug risk — role selects the
  colorspace path correctly.
- **sRGB linear-space averaging, re-proved myself**: ran
  `rx_asset_gltf_tests --test-case="*generateStbMipChain*,*decodeTextureForUpload: a real 8x8*"`
  fresh off a from-source rebuild — 7/7 test cases, 58/58 assertions,
  matching the report exactly.
- **Revert-discrimination, re-proved myself (not re-run of the
  implementer's evidence — I performed the revert independently)**: edited
  `generateStbMipChain()` to force `isSrgb=false`/`isNormal=false`
  (naive-average simulation), rebuilt, re-ran the two discriminating tests.
  Got the identical failure signature: `CHECK( 128 > 176 )` /
  `CHECK( 128 > 150 )` for the sRGB case (correct answer 188, naive 128) and
  `CHECK( 0.803941 > 0.99 )` for normal renormalization (correct: unit
  length, naive: 0.804 — the same "two divergent normals average to a
  shorter vector" flattening case). Restored `texture_decode.cpp`
  byte-for-byte from a saved copy (`diff` confirmed identical, `git diff`
  on the file empty), rebuilt, re-ran green (7/7, 58/58 again). This is a
  real, non-vacuous discrimination — a naive revert fails both new tests by
  a wide, unambiguous margin.
- **Normal renormalization**: confirmed unit-length output (1.00002) and
  the degenerate zero-length fallback to tangent-space "straight up",
  matching D11's own flat-normal convention — verified in source.
- **Linear path for MR/occlusion**: confirmed via the "plain LINEAR box
  average" test (128, not sRGB-correct 188, on the identical input bytes
  used for the BaseColor test) — re-ran, passed.
- **NPOT floor-halving chain length**: `nextMipExtent()` uses
  `max(1, extent >> 1)` independently per axis, matching Vulkan's
  `floor(log2(max(w,h)))+1` total-level rule. Re-ran the 5x3 (NPOT, 3
  total levels) and 8x8 (POT, 4 total levels) chain-shape tests — pass,
  exact extents match.
- **Library-first adjudication**: verified directly, not just taken on
  faith — `stb_image_resize2.h` is present in
  `build/{linux-native,windows-cross-zig}/_deps/stb-src/`, the SAME
  FetchContent checkout `third_party/CMakeLists.txt` already pins via
  `RX_STB_TAG` for `stb_image.h`. No new pin, confirmed. Checked the
  vendored header directly: `STBIR_RGBA` is documented there as
  "alpha formats, where alpha is NOT premultiplied... the resizer will
  alpha weight the colors (effectively creating the premultiplied image),
  do the filtering, and then [un-premultiply]" — the report's technical
  characterization of the alpha-weighted-blend choice is accurate, not
  embellished. The ONLY hand-rolled numeric kernel is normal
  renormalization (`renormalizeNormalTexelsInPlace()`), correctly scoped
  to the one thing outside any general image resizer's domain — verified
  by reading the whole diff, no other hand-rolled averaging exists.
- **D17 regen provenance**: confirmed via `git log` that
  `samples/08_gltf_viewer/references/loaded_scene.png` and
  `samples/09_scene/references/grid_scene.png` were touched in exactly one
  commit (`dd1e110`, the Item A commit) and nowhere else; `loading_state.png`
  untouched (correct — pre-import frame, no texture loaded yet).
  `tools/regen_references.sh` hard-forces the system lavapipe ICD by
  design (read the script). Re-ran both headless gates myself, fresh
  build, lavapipe-forced:
  ```
  sample_08_gltf_viewer: D17 loading_state gate: failingPixels=0/65536 (0.0000%) pass=true
  sample_08_gltf_viewer: D17 loaded_scene gate: failingPixels=0/65536 (0.0000%) pass=true
  sample_09_scene: D17 grid_scene gate: failingPixels=0/65536 (0.0000%) pass=true
  ```
  Exact match to the report. `sample_06_materials`'s "no D17 gate at all"
  claim also verified directly (`find samples/06_materials -iname '*.png'`
  is empty) — that sample genuinely was never at risk.

## Item B — oversized texture uploads (chunked staging)

- **D25 ticket semantics / no host-wait-implies-device-visible slip**:
  traced this myself through `upload.cpp`. `reserveRingSpace()`'s
  transparent-auto-flush-on-wrap behavior (submits already-recorded work,
  does not wait) is PRE-EXISTING machinery, reused unmodified by the
  chunked-row loop — confirmed `registerRealTexture()`
  (`texture_cache.cpp`) is NOT in this diff at all, so its own
  flush()/wait() discipline around `uploadImageMips()` is byte-for-byte
  unchanged. Because every submission shares one monotonically-increasing
  timeline semaphore and the graphics queue processes/signals in submission
  order, waiting on a LATER ticket value is guaranteed (Vulkan timeline-
  semaphore ordering) to postdate every EARLIER auto-flushed chunk's
  completion too — so a caller awaiting only the final ticket still gets
  every chunk covered, exactly the report's own claim. This is
  structurally identical to how multi-level KTX2 uploads already worked
  before this round (a ring wrap between levels could already trigger the
  same internal auto-flush) — chunking a single oversized level is not a
  new hazard shape.
- **Ring-wrap test genuinely wraps 3x — read the test myself**: 32x32
  RGBA8 (4096 bytes) through a 1024-byte ring, `bytesPerRow=128`,
  `rowsPerChunk=1024/128=8` → exactly 4 chunks of 1024 bytes each, each
  chunk exactly filling the ring, so chunks 2/3/4 each start past the
  ring's end — genuinely forces >=3 wraps, not a contrived assertion. Also
  re-ran it myself (see revert-discrimination below) — real GPU test, byte-
  exact full-image readback against a non-repeating gradient, not a
  flat/degenerate fixture.
- **Chunked-staging revert-discrimination, re-proved myself
  independently**: edited `levelNeedsChunking()` to always report "cannot
  chunk" once a level exceeds ring capacity (simulating the pre-fix hard
  rejection), rebuilt `rx_rhi_vk_tests`/`rx_asset_tests`, re-ran both new
  tests on lavapipe. Got the exact documented failure signature: the
  low-level ring-wrap test hit `REQUIRE(...uploadImageMips...)` ==
  `false`; the real-shaped fixture test hit
  `REQUIRE_FALSE( record.isFallback ) is NOT correct! values: REQUIRE_FALSE( true )`
  — i.e. checkerboard fallback, exactly the original defect. Restored
  `upload.cpp` byte-for-byte (diff/`git status` empty), rebuilt, re-ran
  green (2/2, 23/23 and 1/1, 23/23 — exact match to the report).
- **Committed 4096 fixture through the FULL importGltf path against the
  production 16MiB ring**: verified directly in source —
  `TcTestFixture`'s `Uploader::create(*allocator, *device)` passes no
  ring-size override, and `Uploader::kDefaultRingBufferSize` is
  `16u*1024u*1024u` (`upload.h`) — this is genuinely the production
  default, not a test-only smaller ring.
- **Checkerboard fallback scoping**: confirmed `TextureCache`'s only
  fallback triggers are (1) stb/KTX2 decode failure, (2)
  `exceedsDimensionLimit()` — a genuinely different, pre-existing,
  legitimate "size" concern (device `maxImageDimension2D` hardware
  capability, unrelated to staging-ring capacity), and (3) the generic
  "GPU upload/bindless registration failed" catch-all, which still covers
  bindless-table exhaustion (pre-existing, unrelated to this round) and
  the residual "cannot chunk safely" case. The brief's "size is no longer
  a fallback reason" claim is scoped correctly to the staging-ring case
  this round actually fixes, and the report is honest about the residual
  theoretical case rather than overclaiming a universal guarantee.
- **Whole-row chunking limitation — documented AND fails loudly, mostly
  test-covered**: read both loud-failure branches in `uploadImageMips()`'s
  validation loop. Both are real `RX_LOG_ERROR` + `return false` (before
  any GPU work is recorded) — never a silent skip. I independently
  reproduced the "even a single row doesn't fit" branch via the revert
  probe above. However: `upload_test.cpp` has no DEDICATED test for the
  OTHER loud-failure branch (`level.extent.height == 0 ||
  level.size % level.extent.height != 0` — the actual "does not divide
  evenly" case, as opposed to "a whole row is still too big"). This is a
  minor test-coverage gap, not a functional defect — I confirmed by direct
  code read that the branch is loud and correct, and the report's own
  "Concerns for the coordinator" section already flags this as a
  currently-unexercised, hypothetical case (no current asset produces a
  non-block-aligned oversized level). See finding L1 below.
- **Composition with Item A**: the oversized-texture fixture test asserts
  `mipLevels == 13` (`floor(log2(4096))+1`), with mip 0 (64MB) chunked and
  mip 1 (exactly 16MiB) single-trip — genuinely exercises both this
  round's items together in one import. Independently corroborated on the
  real Workshop asset (see below).

## Composition / real-world proof — re-run myself on real NVIDIA

Rebuilt from source at `fcedd3c` (`cmake --build --preset linux-native`,
zero warnings on a forced touch-rebuild of every round-touched file, both
presets). `vulkaninfo` under the forced NVIDIA ICD confirms
`deviceName=NVIDIA GeForce RTX 2080`, `driverInfo=580.82.07` — same device
class the report used.

**Workshop** (`sample_09_scene --present --scene
assets/fetched/Workshop/workshop_render_scene.glb --validate`, real
`DISPLAY=:1`, NVIDIA-forced, sustained ~15s, two captures 5s apart,
SIGTERM): captured frames show a real garage interior — car, tool shelving,
tarps — no checkerboard/magenta anywhere. HUD read `Texture: 827444.0 KiB
(34 alloc(s))`, `316` renderables — exact match to the report's figures.
Log: 0 raw `[error]` lines, 0 `checkerboard`/`fallback` mentions, 0
`exceeding...staging`/`cannot chunk` occurrences, 33898 validation
lines all carrying this repo's pre-existing "known false positive" label
(0 unfiltered). `sample_09_scene: window closed cleanly` on SIGTERM.

**Sponza** (same run, `--scene sponza`): captured frames show real tiled
roof/wall texture content, no checkerboard. HUD read `Texture: 393339.5 KiB
(77 alloc(s))` — exact match to the report. 0 raw errors, 0 unfiltered
validation errors, clean SIGTERM shutdown.

## Verification bar — re-run myself, all green

- **Full serial lavapipe ctest**: 29/29 passed, 82.87s (report: 81.51s).
- **Full serial real-NVIDIA ctest** (forced `nvidia_icd.json`): 29/29
  passed, 139.95s (report: 137.23s).
- **windows-cross-zig build spot-check**: forced a touch-rebuild of every
  round-changed file under the `windows-cross-zig` preset — clean, zero
  warnings.
- **Wine ctest** (this repo's CI exclusion set,
  `-E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample'`):
  13/13 passed, 128.70s (report: 103.40s — the report's own tail didn't
  show a full end-to-end wall-clock difference is a concern; both are
  green, no failures either way).
- **Wine, bonus direct chunk test**
  (`rx_rhi_vk_tests.exe --test-case="*CHUNKED*"` under
  `wine` + `xvfb-run`, winevulkan→llvmpipe): 2/2 test cases, 23/23
  assertions — exact match to the report's bonus claim.
- **Zero warnings**: confirmed on a forced full rebuild of every
  round-touched file, both `linux-native` and `windows-cross-zig` presets
  — `grep -i warning` (excluding `_deps/`) returns nothing on either.

## Commit hygiene

- 3 commits (`dd1e110`, `5834114`, `fcedd3c`), each pathspec-scoped to its
  own item (verified file lists per commit via `git log --stat`): Item A
  commit touches only `texture_decode.{h,cpp}`, the new resize-impl TU,
  its CMakeLists entry, its own tests, and the two D17 reference PNGs;
  Item B commit touches only `upload.{h,cpp}`, its own tests, and its own
  fixture assets; the report commit touches only the report + its two
  evidence PNGs.
- Author on every commit: `Yousef Wadi <ywadi85@gmail.com>`, matching
  local `git config user.name`/`user.email` exactly.
- No AI attribution anywhere: grepped commit messages and every touched
  file for `claude|anthropic|co-authored|chatgpt|copilot` — the only hit
  is a reference to this repo's own `CLAUDE.md` conventions file by name
  (`stb_image_resize_impl.cpp`'s "prefer ready-made libraries" citation),
  not attribution.
- Nothing pushed: `git status -sb` shows `main...origin/main [ahead 3]` —
  exactly the 3 local commits, no more, nothing pushed.
- The pre-existing `progress.md` modification was left untouched by both
  the round and this review (confirmed still the only uncommitted change
  at the end of this review).

## Findings

- **L1 (low, test coverage, non-blocking)**: `uploadImageMips()`'s
  "cannot chunk safely" contract has two distinct loud-failure branches —
  "a single row still exceeds the ring" (tested,
  `upload_test.cpp`'s 64-byte-ring case) and "bytes don't divide evenly
  into whole rows" (documented, correctly implemented as a loud
  `RX_LOG_ERROR`+`return false` per direct code read, but has no dedicated
  test exercising it). Not a defect — the code path is verified correct by
  inspection and is honestly flagged as a currently-unexercised
  hypothetical in the report's own "Concerns" section — but a small
  synthetic `ImageMipLevel` with a non-divisible size would have closed
  this gap cheaply and matches this codebase's own "test every branch"
  discipline elsewhere in the same file.

No other findings. No MEDIUM/HIGH-severity issues found in either item.

## Not independently verifiable in this review

- The report's Wine-ctest wall-clock figure (103.40s) versus my own
  128.70s re-run — both green, difference attributed to machine load
  variance, not investigated further (immaterial to correctness).
- Native-Windows behavior (no native Windows machine available to this
  review; windows-cross-zig + Wine are the closest available proxies, both
  re-run green).

## Conclusion

Both release-gate items are correctly implemented, match the brief's
binding requirements, and hold up under independent re-derivation of every
load-bearing claim (revert-discrimination for both items, real-NVIDIA
Workshop + Sponza reproduction, full lavapipe + real-driver ctest, Wine
ctest, windows-cross build spot-check). Spec compliance: PASS. Code
quality: APPROVED, one low-severity test-coverage note (L1) for the
coordinator's awareness, not blocking.
