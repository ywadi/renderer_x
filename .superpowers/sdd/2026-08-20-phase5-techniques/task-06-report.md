# Task 6 report — Cubemap/array KTX2 loading + HDR image input (issue #42)

Implementer round. Base: main `c2f9eae` (post T5). Order of authority
followed: rulings (`rulings-2026-08-20.md`, "T6 (#42)" + RC7e) > plan
(`2026-08-20-phase5-techniques.md`, "Task 6", Stage 0) > gate matrix
(`matrix-p5t06-ktx2-cubemap-hdr.md`) > ticket (#42). Last Stage 0 task.

## Status: COMPLETE

Cubemap KTX2 now decodes/uploads for real (flat-array and cube-array
stay explicitly rejected, each with its own regression fixture+test);
equirect Radiance `.hdr` loads as float and uploads as
`VK_FORMAT_R16G16B16A16_SFLOAT`; `TextureRole::Environment` lands with
its own real mid-gray D11 fallback; the byte-source invariant's
"grep-enforced" wording is now literally true (`tools/
check_byte_source_invariant.sh`, wired into both CI jobs). T2's stray
`rx_compute_pipeline_test*.cache` leak is fixed in the same round, as
directed. Full suite green on linux-native (real NVIDIA AND lavapipe,
both driver-labeled) and windows-cross-zig (build clean; Wine ctest
green except one pre-existing, diff-unrelated `rx_core_tests` timeout —
see Concerns).

## Ruling followed (T6, #42, `rulings-2026-08-20.md:106-109`)

> T6 (#42): cubemap-only (arrays/cube-arrays stay explicitly rejected
> with regression tests); environment upload format
> R16G16B16A16_SFLOAT; environment-role D11 fallback = uniform
> mid-gray; byte-source grep becomes a real CI check (RC7e).

Followed exactly — see per-row proof below.

## What shipped

**`isUnsupportedLayoutFor()` narrowed** (`src/rx_asset/texture_decode.cpp`)
from Phase 4's blanket `isArray || isCubemap || numFaces>1 || numLayers>1
|| numDimensions!=2` to `numDimensions != 2 || (isArray && !isCubemap) ||
(isCubemap && numLayers > 1)` — a plain single-layer cubemap is now
supported; flat 2D-arrays and cube-arrays stay explicitly rejected by
their own independent clauses.

**`DecodedKtx2Texture` gains `isCube()`/`numFaces()`/a face-parametrized
`levels(uint32_t face = 0)`** (default preserves every pre-existing call
site byte-for-byte). `ktxTexture_GetImageOffset()`'s previously-unused
`faceSlice` parameter now threads the real face index through.
`decodeKtx2ForUpload()` gathers all 6 faces' mip chains into
`TextureDecodeResult::levels` (new `isCube`/`DecodedTextureLevel::
faceIndex` fields, both defaulting to false/0 for every non-cube
producer).

**`TextureRole::Environment`** appended last (index-stable — every
pre-existing `byRole`/`roleFallback_` index is unchanged; both arrays
grow 6→7). `roleFormatTable()`'s new case is documented dead code for
this task's actual HDR path (reused `kUnormData`, linear/non-sRGB) —
kept only so the exhaustive switch stays total against a conceivable
future Basis-encoded environment cubemap.

**Radiance `.hdr` float decode path** (`decodeStbImageHdr()`,
`stbi_loadf_from_memory`) — `decodeTextureForUpload()` now checks
`stbi_is_hdr_from_memory()` **before** the 8-bit stb branch. This closes
a real, previously-silent bug the matrix's own research surfaced: an
`.hdr` file handed to the OLD `decodeStbImage()` path decoded
*successfully* through stb's own internal `stbi__hdr_to_ldr()` tonemap,
silently clamping every super-unity texel to 8-bit LDR with zero
warning. HDR content uploads as `VK_FORMAT_R16G16B16A16_SFLOAT`
(`glm::packHalf4x16` — GLM's own reference half-float packer, already
used process-wide for this exact format by `rx_graph/scene_color.h`'s
readback tests; no hand-rolled float-to-half bit-twiddling written).

**`Texture2D::createCubeForPresuppliedMips()`** (`src/rx_rhi_vk/`) — a
real 6-layer, `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` image with a
`VK_IMAGE_VIEW_TYPE_CUBE` view, mirroring this project's own
`StorageImage::create()` cube-flag/view-type precedent for the
sampled-image case (not reinvented). `Uploader::ImageMipLevel` gains
`baseArrayLayer` (default 0, byte-identical for every existing caller),
threaded into both the single-copy and chunked-row `VkBufferImageCopy`
regions.

**`TextureCache::registerRealTexture()`** branches on `isCube` to call
the new cube factory, and now requests `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`
in addition to `SAMPLED_BIT` for every real texture it builds (see
Design decisions below). `TextureRole::Environment`'s own D11 fallback
(`uploadEnvironmentFallback()`) is a real, uniform mid-gray
`R16G16B16A16_SFLOAT` texture built through the SAME float-packing path
real HDR content uses — never a black texture, never an 8-bit-cast
shortcut (Open Question 3's own recommendation, followed exactly).

**Fixtures** (`assets/test/textures/`, `generate_fixtures.sh`, all
regenerated with the pinned `toktx` v4.4.2 release binary + system
ImageMagick): `cubemap.ktx2` regenerated with `--target_type RGBA
--assign_oetf srgb` (a real uploadable format now — the SAME file the
old rejection test used, now the discrimination-flip fixture);
`cubemap_mips.ktx2` (32×32 base, 6-level per-face mip chain, the primary
value-asserted GPU fixture); `array2d_rejected.ktx2`/
`cubearray_rejected.ktx2` (still-rejected regression fixtures);
`equirect_test.hdr`/`corrupt.hdr` (hand-authored Radiance RGBE — this
sandbox's ImageMagick is Q16 without HDRI and cannot express genuine
super-unity pixels, so the bytes are computed by hand against
`stb_image.h`'s own RGBE decode formula, documented inline).

**`tools/check_byte_source_invariant.sh`** (RC7e) — the real grep: zero
`fopen(`/`std::ifstream`/`std::ofstream` and zero `std::filesystem::`
members other than `path` in `texture_decode.cpp`/`texture_cache.cpp`.
Wired into both CI jobs immediately after checkout.

**Bundled fix**: `ComputePipelineCache::create()`'s three test call
sites (`compute_pipeline_test.cpp`) now write to
`std::filesystem::temp_directory_path()` instead of a bare relative
filename — closes the `rx_compute_pipeline_test*.cache`-into-CWD leak
directed by the coordinator's dispatch note.

## Matrix row disposition (selected — full matrix in `gate/
matrix-p5t06-ktx2-cubemap-hdr.md`)

| Row | Item | Disposition | Delivered |
|---|---|---|---|
| 1/2 | Cube rejection baseline + discrimination flip | consume-now / flip required | `isUnsupportedLayoutFor()` narrowed; `cubemap.ktx2`'s two old rejection tests DELETED, replaced by flip tests (device-free + GPU) asserting `handle != checkerboardHandle()` + exact per-face bytes |
| 3 | Scope: cubemap-only | needs-coordinator-decision → **ruled** cubemap-only | Narrowed predicate implements exactly the ruled formula; array2d/cubearray fixtures+tests prove the two excluded shapes stay rejected |
| 4 | Non-cube regression byte-identical | consume-now | Full pre-existing `rx_asset_tests`/`rx_asset_gltf_tests` suites re-run unmodified (minus the two deliberately-flipped cases) and pass; `TextureRole::Environment` appended at the end, verified index-stable |
| 5 | `cube_basisu_misleading_normal.ktx2` disambiguation | N/A | Untouched — confirmed unrelated lineage (Phase 4 Task 14), no action taken |
| 6 | New mip'd cubemap fixture | consume-now | `cubemap_mips.ktx2`, 32×32→1×1, 6 levels, `--genmipmap` |
| 7 | `Texture2D`/`Uploader` array-layer plumbing | consume-now, promoted from conditional to required | `createCubeForPresuppliedMips()` + `ImageMipLevel::baseArrayLayer` |
| 8 | `TextureRole::Environment` blast radius | consume-now | Appended last; `byRole`/`roleFallback_` grow 6→7; import_gltf.cpp's 5 material-slot call sites untouched (glTF has no environment slot) |
| 9 | Equirect HDR decode path | consume-now, closes a live silent-tonemap bug | `decodeStbImageHdr()` + dispatch-order fix |
| 10 | HDR is flat 2D, not cube | consume-now | Loads through the existing single-layer 2D path unmodified; zero cube-plumbing collision |
| 11 | GPU test asserts exact per-face/per-mip values | consume-now | `cubemap_mips.ktx2` GPU test: every face × {mip 0, mip 5} exact byte match, both drivers |
| 12 | Chunked-staging × per-face interaction | consume-now | `upload_test.cpp`'s new cube-chunked test forces the ring small enough to require chunking per face, asserts no cross-face corruption |
| 13 | HDR value-readback (>1.0 survives) | consume-now | Device-free (`decodeStbImageHdr`/`decodeTextureForUpload`) AND GPU (real upload+readback) tests both assert `4.0F` exactly |
| 14 | Byte-source invariant "grep-enforced" | needs-coordinator-ruling → **ruled**: add the real grep | `tools/check_byte_source_invariant.sh`, revert-proven, wired into both CI jobs |

Open Questions 1-5 (matrix) all resolved per their own stated
recommendation, matching the ruling: cubemap-only (1), `R16G16B16A16_
SFLOAT` (2), mid-gray fallback via the real float path (3),
`cubemap_mips.ktx2` naming/generation via the existing script (4), a
real CI grep script (5).

## Design decisions beyond the ticket's literal text

**`registerRealTexture()` now requests `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`
for every real texture, not just cubes.** Neither `Texture2D::
createForPresuppliedMips()` nor `createCubeForPresuppliedMips()` had
their own documented usage contract changed (`usage | TRANSFER_DST_BIT`
stays their only unconditional addition) — this is `registerRealTexture
()`'s own caller-supplied `usage` argument gaining a bit, which was
always an open, additive parameter. Needed because a TextureCache-
resident image is sampled-only by design; the matrix's own row 11 names
"direct face-indexed readback" (a raw `vkCmdCopyImageToBuffer`) as the
GPU test technique for per-face cube values and row 13's HDR
`>1.0`-survives bar needs the identical mechanism — neither is possible
against a `SAMPLED_BIT`-only image. Cost is zero on any conformant
Vulkan implementation (`TRANSFER_SRC_BIT` is mandatory format-feature
support for every optimal-tiling sampled format this engine uses); a new
`rawImageForTesting()` diagnostic accessor exposes the raw `VkImage`
(matching this class's existing `*ForTesting()` convention) so the
readback tests never need bindless/shader machinery to prove a value.
One pre-existing test's bindless-capacity fixture (`rx_asset_tc_
capexhaust`) needed its `sampledImages` capacity bumped 4→5 to match the
new Environment fallback's own slot consumption at `TextureCache::
create()` time — a real, expected, mechanically-derived adjustment, not
a design compromise.

## Verification

Device-free (`rx_asset_gltf_tests`, home of `texture_decode_test.cpp`):
```
[doctest] test cases:  60 |  60 passed | 0 failed | 0 skipped
[doctest] assertions: 806 | 806 passed | 0 failed |
```

GPU, **real NVIDIA** (RTX 2080, driver 580.82.07, `VK_ICD_FILENAMES=
nvidia_icd.json`, driver-labeled):
```
$ rx_asset_tests    -> 44/44 cases, 25314/25314 assertions, 0 failed, 0 unfiltered validation errors
$ rx_rhi_vk_tests   -> 97/97 cases,  2398/2398 assertions, 0 failed, 0 unfiltered validation errors
```

GPU, **lavapipe** (pixel-reference driver, `VK_ICD_FILENAMES=
lvp_icd.json`, driver-labeled):
```
$ rx_asset_tests    -> 44/44 cases, 25314/25314 assertions, 0 failed
$ rx_rhi_vk_tests   -> 97/97 cases,  2387/2387 assertions, 0 failed
```
(Assertion-count delta between drivers is pre-existing conditional
coverage, e.g. `samplerAnisotropy` support branching — not new this
round; both runs are 100% pass.)

Full project suite, linux-native, default device selection:
```
$ ctest --preset linux-native --output-on-failure
100% tests passed, 0 tests failed out of 31
Total Test time (real) = 198.89 sec
```

`tools/check_byte_source_invariant.sh` revert-proven: injected
`fopen(`/`std::ifstream`/`std::filesystem::exists` into
`texture_decode.cpp`, script failed loudly naming the exact line;
reverted, script passes clean.

windows-cross-zig: clean configure + full build (74/74 targets, zero
errors/warnings). Wine ctest (CI's own GPU-exclusion pattern):
```
13/14 tests passed (rx_core_tests: Timeout)
```
`rx_asset_tests` (home of every new cube/HDR GPU test) and
`rx_asset_gltf_gpu_tests` both PASS under Wine — the actual surface this
task touches is proven there too, not just build-clean.

Zero compiler warnings on a forced rebuild of every touched `.cpp`
(`-v` ninja output grepped for `warning`/`error`: no hits).

## Revert-discrimination

**`isUnsupportedLayoutFor()`'s narrowed predicate** — the two OLD
Phase-4 cube-rejection tests were deleted, not left alongside new
passing ones; their replacements assert the literal flip (`isUnsupported
Layout()` false where it used to be true), which is itself the
discrimination proof the matrix's row 2 requires — a no-op
implementation could not make these new tests pass.

**`ImageMipLevel::baseArrayLayer` threading** — `upload_test.cpp`'s new
cube-chunked test uses two per-face buffers that are bitwise-NOT of each
other (maximally distinguishable at every byte), forced through the
SAME small-ring chunking path the pre-existing plain-2D chunked test
uses; a face-swap or a misplaced chunk both fail its `memcmp`.

**`tools/check_byte_source_invariant.sh`** — proven both directions live
this round (see Verification): fails loudly on injected violations,
passes clean on the real, unmodified files.

## Self-review

- **TDD discipline**: fixtures generated and `ktx2check`/`ktxinfo`-
  verified (`isCubemap`/`isArray`/`numLayers`/`numFaces`/`vkFormat`
  fields confirmed matching the intended container shape) before any
  production code changed; the HDR corrupt-fixture design itself went
  through one real TDD cycle (the first truncated-scanline design
  "passed" decode instead of failing, root-caused via the vendored
  `stb_image.h` source directly, then fixed to a genuinely-corrupt
  resolution line — documented inline, not silently patched over).
- **No deferred fixes**: every matrix row closes in this round; the
  live-bindless-capacity-off-by-one this task's own Environment fallback
  addition caused in a pre-existing test was found (real-driver run)
  and fixed in-round, not deferred.
- **Value-asserted GPU tests**: every new GPU test asserts real decoded
  bytes/floats (per-face RGBA8 colors, half-float texel values via
  `glm::unpackHalf4x16`), never "it loaded without crashing."
- **Real-GPU verification**: both binaries this task touches run clean
  on real NVIDIA hardware, driver-labeled, not lavapipe-only.
- **No AI attribution**: verified directly against all 5 commits this
  round (`git log` + grep for claude/anthropic/co-authored/generated-by/
  ai-assistant) — none found; author identity is the user's own
  configured git identity throughout.
- **Commit scope**: 5 pathspec-scoped commits (rx_rhi_vk primitives,
  fixtures, rx_asset decode/cache layer, CI grep, the bundled T2
  cache-leak fix) — see SHAs below.
  `.superpowers/sdd/2026-08-20-phase5-techniques/progress.md` carries
  the coordinator's own concurrent edits throughout this round
  (confirmed via `git status`/`git diff --stat` before every commit) and
  is deliberately excluded from all of them.
- **Scope discipline**: no push performed; no board/plan/spec/ledger
  file edited; the `TRANSFER_SRC_BIT` widening (see Design decisions) is
  the one choice beyond the ticket's literal text — flagged explicitly
  above rather than silently folded in.

## Concerns for the coordinator

1. **Wine `rx_core_tests` timeout, windows-cross-zig** — reproducible
   (2/2 runs, identical hang point: right after the log-forward-
   callback-throws test sequence, before the suite's own teardown).
   `rx_core` is untouched by this diff (`git diff --stat -- src/rx_core/`
   is empty) and the target that DOES matter for this task
   (`rx_asset_tests`, which carries every new cube/HDR test) passes
   clean under Wine both times. This matches the same category of
   Wine-teardown flake T5's own ledger entry already documented for a
   *different* binary (`rx_platform_tests`, "Subprocess killed... after
   passing 34/365") — recommend treating as the same watch-item class
   rather than a new incident, but flagging since it is a second,
   independent occurrence against a third binary.
2. **`TRANSFER_SRC_BIT` widening** (see Design decisions above) is a
   real, if small, capability change to every TextureCache-resident
   texture — reviewed here as clearly in-scope and low-cost, but it is
   the one place this round's implementation reached slightly past the
   ticket's literal "loading" framing into a production RHI usage-flag
   change; flagging explicitly for the review round.
3. **libktx v5 tool-suite removal** (matrix's own "New gaps," unrelated
   to this task's scope) — `generate_fixtures.sh` still targets `toktx`
   (v4.4.2, the current pin); a future pin-refresh task will need to
   migrate its invocations to `ktx create`'s different flag surface.
   Not touched this round (out of scope), reproduced here only because
   this task fetched v4.4.2 `toktx` fresh from the pinned release URL
   and confirmed it still works exactly as documented.

## Commit SHAs (base `c2f9eae`, all local, none pushed)

1. `0d26462` — feat(rx_rhi_vk): cube-aware Texture2D + per-face Uploader::ImageMipLevel (#42)
2. `db99857` — test(assets): cubemap/array/HDR KTX2+Radiance fixtures (#42)
3. `aa7e3e1` — feat(rx_asset): cubemap KTX2 + equirect HDR loading, Environment role (#42)
4. `2badf74` — ci(tools): real byte-source-invariant grep, both CI jobs (RC7e) (#42)
5. `44ae638` — fix(rx_rhi_vk): stop leaking rx_compute_pipeline_test*.cache into CWD
