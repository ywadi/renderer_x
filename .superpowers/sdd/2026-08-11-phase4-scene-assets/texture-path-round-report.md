# Texture-path round report — runtime mips (Option A) + oversized-upload staging fix

Brief: `texture-path-round-brief.md`. Repo `/media/ywadi/second/renderer_x`,
base `main` at `9c76f01` (D5-clean tree except the coordinator's own
`progress.md` append and pre-existing untracked SDD workspace files, neither
touched here). Both release-gate items closed in this round.

**Real device used throughout this report's "real-NVIDIA" evidence**:
`vulkaninfo` under `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json`
reports `deviceName = NVIDIA GeForce RTX 2080`, `driverInfo = 580.82.07`.
Lavapipe evidence used `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`
(mesa llvmpipe) explicitly, per this repo's own D17/regen-script convention.
Both ICDs are present system-wide on this machine; every command below
states which one it forced.

## Item A — runtime mip-chain generation for the stb decode path (D10 Option A)

### Design

`decodeStbForUpload()` (`src/rx_asset/texture_decode.cpp`) previously
uploaded mip 0 only. The fix adds `generateStbMipChain()` (new public
function, declared in `texture_decode.h` right after `DecodedTextureLevel`,
same device-free/thread-affinity-NONE header the KTX2 decode layer already
lives in — no new header, so no new D5 blanket note needed; the existing
file-level "pure CPU parse/transcode/decode logic... safe to call from any
thread" comment already covers it) that produces levels 1..N (floor-halving
down to 1x1, matching Vulkan's own `floor(log2(max(w,h)))+1` total-level
rule) from an already-decoded RGBA8 mip 0, then `decodeStbForUpload()`
appends them onto the same `result.levels` the KTX2 path already populates
generically. No changes were needed in `TextureCache::applyDecodeResult()`
or `Uploader::uploadImageMips()`'s registration path — confirmed exactly
what the prior investigation (`sponza-visual-investigation.md` §2.7) found:
that path was already mip-chain-generic.

**Library-first**: `stb_image_resize2.h` is already vendored — it lives in
the SAME `stb` GitHub checkout `third_party/CMakeLists.txt`'s existing
`FetchContent_Declare(stb ...)` pulls for `stb_image.h` (one repo, one pinned
commit, `RX_STB_TAG`), so no new dependency, no new pin, and no new
`target_include_directories` were needed — `rx_asset` already inherits
`stb_SOURCE_DIR` transitively via its existing `PUBLIC` link against
`rx_rhi_vk`. One new implementation TU was added,
`src/rx_asset/stb_image_resize_impl.cpp` (`#define
STB_IMAGE_RESIZE_IMPLEMENTATION`), mirroring `rx_rhi_vk/src/stb_impl.cpp`'s
own established single-TU-implementation discipline, and added to
`rx_asset`'s `add_library()` sources.

The library expresses both real kernels this task needs directly, via the
general `stbir_resize()` entry point (explicit `STBIR_FILTER_BOX` — "same
result as box for integer scale ratios" per the library's own enum comment
— not `STBIR_FILTER_DEFAULT`, so every power-of-two halving step is an
exact, unambiguous arithmetic mean, which is what this round's own
correctness tests pin against):

- **sRGB roles** (`BaseColor`/`Emissive`, `roleExpectsSrgb()==true`):
  `STBIR_TYPE_UINT8_SRGB` + `STBIR_RGBA` layout — sRGB-decode, box-average
  in linear space, sRGB-re-encode for RGB; alpha stays linear (glTF's own
  "baseColor alpha is coverage, never color" convention — `STBIR_TYPE_UINT8_SRGB`,
  not `_SRGB_ALPHA`, confirmed by reading the library's own convenience-
  wrapper source) and is alpha-weighted into the RGB blend (the library's
  documented anti-"black-fringing" behavior for RGBA + real alpha content —
  a genuine correctness bonus over a naive per-channel average, not just
  parity with it).
- **Every linear-data role** (`MetallicRoughness`/`Occlusion`/`GenericData`,
  and the pre-renormalize pass for `Normal`): `STBIR_TYPE_UINT8` +
  `STBIR_4CHANNEL` — plain per-channel box average, deliberately NO alpha
  weighting (`STBIR_4CHANNEL`, not `STBIR_RGBA` — channel 3 is not real
  alpha for any of these roles).

**Hand-rolled, explicitly (per the brief's own "otherwise hand-roll the
small kernels and say so" allowance)**: normal-map renormalization.  No
general-purpose image resizer has any reason to know a texel encodes a unit
tangent-space vector — `renormalizeNormalTexelsInPlace()`
(`texture_decode.cpp`, anonymous namespace) decodes each output texel
(`2*byte/255-1`), renormalizes to unit length (falling back to the
tangent-space "straight up" `(0,0,1)` — the same convention D11's own
flat-normal fallback texture uses — only in the fully-degenerate
zero-length case), and re-encodes. This is the ONLY hand-rolled numeric
kernel in the whole implementation; everything else routes through the
library.

Deviation worth naming explicitly: nothing was hand-rolled as a
"shortcut" — the library expresses both required kernels natively, so the
brief's own fallback clause ("prefer the library IF it can express the
requirements") resolved in the library's favor for the sRGB and plain-linear
kernels, with hand-rolling scoped to exactly the one thing outside any image
resizer's domain.

### Correctness tests (new, `src/rx_asset/tests/texture_decode_test.cpp`)

Every math test below constructs its own tiny in-memory RGBA8 buffer (not a
PNG round-trip) so expected values are exact arithmetic on known bytes, and
every 2x2→1x1 step is an EXACT box average by construction (no fractional-
coverage ambiguity for an integer 2x downsample).

**(a) sRGB linear-space averaging** — 2x2 checkerboard, two texels
`(255,255,255,255)` (white) and two `(0,0,0,255)` (black), role
`BaseColor`. Reference math (IEC 61966-2-1, implemented independently in
the test file — NOT calling the same production code path, so this is a
real cross-check): `srgbToLinear(255)=1.0`, `srgbToLinear(0)=0.0`, linear
average `=0.5`, `linearToSrgb(0.5) = 1.055*0.5^(1/2.4)-0.055 = 0.7354 ->
byte 188`. Measured production output: **R=G=B=188, A=255** — exact match.
A naive byte average of the same 4 bytes is `(255+0+255+0)/4 = 127.5 ->
127/128`, a ~60-value gap from the correct answer.

**(b) Normal-map renormalization** — 2x2, two symmetric unit tangent-space
normals `N1=(0.6,0.8,0)`/`N2=(-0.6,0.8,0)` byte-encoded and tiled, role
`Normal`. Their plain average is `(0,0.8,0)`, length 0.8 (a real, non-
contrived flattening case). Measured production output, decoded back to a
vector: `x=0.00392, y=1.0, z=0.00392`, **length = 1.00002** (unit, within
float rounding).

**(c) Plain linear average** (item 3) — the IDENTICAL (255/0) checkerboard
input as (a), role `MetallicRoughness`: measured **128** (plain arithmetic
mean, NOT the sRGB-correct 188) — proves the role selects the colorspace
path, not the input bytes.

**(d) Chain length/dimensions** (item 5/6c) — NPOT 5x3 source: 2 generated
levels, `(2,1)` then `(1,1)` (`floor(log2(5))+1=3` total, matches). POT 8x8
source (role `BaseColor`): 3 generated levels, `(4,4)`, `(2,2)`, `(1,1)`.

**(e) Degenerate input**: width/height 0, or fewer bytes than
`width*height*4`, returns an empty chain (never fabricates levels).

**(f) End-to-end integration** — `decodeTextureForUpload()` against the
real, committed `quadrant.png` fixture (8x8): now produces exactly 4 levels
(`8,4,2,1`), level 0 byte-identical (`256` bytes) to the pre-fix decode.

Command tail (`rx_asset_gltf_tests`, device-free binary,
`--test-case="*generateStbMipChain*,*decodeTextureForUpload: a real 8x8*"`):

```
[doctest] test cases:  7 |  7 passed | 0 failed | 48 skipped
[doctest] assertions: 58 | 58 passed | 0 failed |
[doctest] Status: SUCCESS!
```

### Revert-discrimination (empirical, not asserted)

`levelNeedsChunking` is Item B's own function — for Item A the probe was:
temporarily forced `isSrgb=false`/`isNormal=false` unconditionally in
`generateStbMipChain()` (simulating a naive byte-average-only revert),
rebuilt, re-ran the SAME two tests:

```
generateStbMipChain: BaseColor (sRGB role) box-averages in LINEAR space...
  CHECK( channel(0) > static_cast<uint8_t>(expectedCorrect - 12) ) is NOT correct!
    values: CHECK( 128 >  176 )
  CHECK( channel(0) > 150 ) is NOT correct!
    values: CHECK( 128 >  150 )

generateStbMipChain: role == Normal renormalizes...
  CHECK( length > 0.99F ) is NOT correct!
    values: CHECK( 0.803941 >  0.99 )
  CHECK( y > 0.9F ) is NOT correct!
    values: CHECK( 0.803922 >  0.9 )

[doctest] test cases:  6 |  4 passed | 2 failed | 49 skipped
```

Both correctness tests genuinely fail against the naive revert (128 vs. the
correct 188; length 0.804 vs. the required [0.99,1.01] band) — real
discrimination, not a vacuous assertion. The revert was then restored
byte-for-byte from a saved copy and re-verified green (58/58 assertions).

### D17 blast radius (item 7)

Full serial lavapipe ctest immediately after landing Item A alone showed
exactly the anticipated blast radius:

```
sample_08_gltf_viewer_headless ...........***Failed
sample_09_scene_headless .................***Failed
  [error] sample_09_scene: D17 grid_scene gate FAILED on lavapipe (first mismatch at (80,132))
  failingPixels=1154/65536 (1.7609%)
```

`sample_06_materials_headless` (the brief's third named risk) stayed green
throughout — checked directly: that sample has NO committed D17 reference
PNG / pixel gate at all (`find samples/06_materials -iname '*.png'` is
empty), so it was never actually at risk; the prior investigation's own
flag was conservative, not wrong to raise.

Regenerated via `tools/regen_references.sh linux-native 08_gltf_viewer` and
`... 09_scene` (lavapipe-forced, per that script's own hard-coded ICD path —
the one mechanism this repo has for updating D17 expectations). Only the
POST-texture-load frames changed:

```
$ git status --short samples/08_gltf_viewer/references/ samples/09_scene/references/
 M samples/08_gltf_viewer/references/loaded_scene.png
 M samples/09_scene/references/grid_scene.png
```

`loading_state.png` (08_gltf_viewer's pre-import loading-screen frame) is
untouched — correct, since no texture has loaded yet at that point. Both
regenerated PNGs were visually reviewed (helmet grid / single helmet render
correctly, no corruption) before committing. Re-run gates, both PASS:

```
sample_08_gltf_viewer: D17 loading_state gate: failingPixels=0/65536 pass=true
sample_08_gltf_viewer: D17 loaded_scene gate: failingPixels=0/65536 pass=true
sample_08_gltf_viewer: headless gate PASSED
sample_09_scene: headless gate PASSED
```

## Item B — oversized texture uploads (the 16MB staging cap)

### Design

`Uploader::uploadImageMips()` (`src/rx_rhi_vk/src/upload.cpp`) previously
rejected any level whose `size` exceeded the ring buffer's total capacity
outright, before recording anything — `TextureCache::registerRealTexture()`
mapped that failure to the D11 checkerboard fallback. Fixed via CHUNKED
STAGING TRIPS through the SAME fixed-size ring (the brief's own preferred
option — "preserves bounded memory", no unbounded ring growth, no separate
transient allocation):

- `levelNeedsChunking()` (new, anonymous-namespace helper): a level that
  fits in one trip (`size <= ringBufferSize_`) is untouched — byte-for-byte
  the SAME single-copy code path this method always had. An oversized level
  is split into consecutive row-groups (`bytesPerRow = size / extent.height`,
  requiring exact divisibility — true for every uncompressed RGBA8 level
  unconditionally, and every block-compressed level whose true height is
  block-aligned, i.e. every level actually large enough to need chunking in
  practice; a level that fails this check is refused loudly, before any GPU
  work is recorded, rather than mis-sliced).
- Each chunk gets its OWN `reserveRingSpace()` reservation, memcpy, and
  `vkCmdCopyBufferToImage` region (`imageOffset.y` advances to the right
  vertical strip; `bufferRowLength`/`bufferImageHeight` stay 0, "tightly
  packed within this chunk", the same Vulkan block-copy convention the
  existing single-copy path already relies on) — reusing
  `reserveRingSpace()`'s own existing flush/wrap/reclaim machinery
  unmodified, so D25's ticket semantics ("a batch's ticket covers every
  chunk recorded before the next `flush()`") apply with zero new completion-
  tracking code.
- Validation happens up front (loop over every level BEFORE recording
  anything) — same "no partial-batch corruption" contract the method
  always had: a level whose bytes don't divide evenly into whole rows, or
  where even a single row exceeds the ring, fails the whole call cleanly.

Checkerboard fallback now fires ONLY for genuine decode failures / an
un-chunkable level shape — never for size alone, closing the defect exactly
as specified.

### Tests

**Low-level, `src/rx_rhi_vk/tests/upload_test.cpp`** (deliberately tiny
rings so the chunked path is exercised cheaply/deterministically):

1. 32x32 RGBA8 (4096 bytes) through a 1024-byte ring — 4 exact 128-byte-row
   chunks (`rowsPerChunk = 1024/128 = 8`). Byte-exact FULL-IMAGE readback
   against a non-repeating gradient (`x*7+y`, `y*5+x`, `x^y`) — a
   misplaced/dropped chunk would corrupt a visible row range, not just
   "some pixels wrong":
   ```
   CHECK( uploader->ringWrapCount() >= 3 ) — values: CHECK( 3 >= 3 )
   CHECK( std::memcmp(readBackPixels.data(), pixels.data(), pixels.size()) == 0 )
   ```
2. A 64-byte ring (smaller than even one 128-byte row) — the "cannot chunk
   at all" branch: fails cleanly, `flush().value == 0` (nothing was ever
   recorded).

```
[doctest] test cases:  2 |  2 passed | 0 failed | 72 skipped
[doctest] assertions: 23 | 23 passed | 0 failed |
```

**Real-shaped, end-to-end GPU test, `src/rx_asset/tests/texture_cache_test.cpp`**:
a NEW committed fixture, `assets/test/textures/oversized_quadrant.png`
(4096x4096, nearest-neighbor upscale of the SAME `quadrant4x4.png` source
every other quadrant fixture in this suite uses — real quadrant content,
30KB on disk since flat regions compress trivially despite the huge pixel
count) referenced by a new glTF fixture,
`assets/test/oversized_texture_probe.gltf` (reuses the existing
`sampler_wrap_probe.bin` quad geometry — same `[0,1]`-UV textured quad
shape). Imported through the FULL `Registry::importGltf()` path (not just
`TextureCache::load()` in isolation) against the PRODUCTION default 16MiB
ring (`TcTestFixture`'s own `Uploader::create(*allocator, *device)`, no
override):

```
CHECK( record.width == 4096 )
CHECK( record.height == 4096 )
CHECK( record.mipLevels == 13 )     // floor(log2(4096))+1 -- item A composed with item B
REQUIRE_FALSE( record.isFallback )  // NOT checkerboard
CHECK( approxEqual(readback->topLeft, {255,0,0}, 2) )     // red
CHECK( approxEqual(readback->topRight, {0,255,0}, 2) )    // green
CHECK( approxEqual(readback->bottomLeft, {0,0,255}, 2) )  // blue
CHECK( approxEqual(readback->bottomRight, {255,255,0}, 2) ) // yellow
[doctest] test cases:  1 |  1 passed | 0 failed
[doctest] assertions: 23 | 23 passed | 0 failed
```

Mip 0 (64MB) needs chunking; mip 1 (2048x2048 = exactly 16MiB) is the
boundary case (fits in ONE trip, `size <= ringBufferSize_`); every smaller
level uses the pre-existing single-copy path — this one fixture exercises
both branches in one real import.

### Revert-discrimination (empirical)

`levelNeedsChunking()` temporarily forced to always report "cannot chunk"
for anything oversized (simulating the pre-fix hard rejection), rebuilt,
re-ran BOTH the new tests:

```
$ ./rx_asset_tests --test-case="*item B*" --validate
[error] Uploader::uploadImageMips: level 0 is 67108864 bytes (extent 4096x4096), exceeding
  the 16777216-byte staging ring buffer's total capacity, and its byte size does not divide
  evenly into whole image rows -- cannot chunk safely
[error] rx_asset: TextureCache: 'oversized_texture_probe_material baseColor' failed to load
  (GPU upload/bindless registration failed) -- falling back to the D11 checkerboard
FATAL ERROR: REQUIRE_FALSE( record.isFallback ) is NOT correct!
  values: REQUIRE_FALSE( true )
[doctest] test cases:  1 |  0 passed | 1 failed

$ ./rx_rhi_vk_tests --test-case="*CHUNKED*" --validate
FATAL ERROR: REQUIRE( uploader->uploadImageMips(...) ) is NOT correct!
  values: REQUIRE( false )
[doctest] test cases:  2 |  1 passed | 1 failed
```

Both fail exactly as the original defect describes (checkerboard fallback /
outright rejection). Restored byte-for-byte from a saved copy; re-verified
green.

### Real-world proof — the Workshop asset (real NVIDIA)

`sample_09_scene --present --scene assets/fetched/Workshop/workshop_render_scene.glb
--validate`, real display (`DISPLAY=:1`), `VK_ICD_FILENAMES` forced to
`nvidia_icd.json` (GeForce RTX 2080), sustained ~11s (two frames captured
5s apart via `import -window <id>`), then a real `SIGTERM`:

- `stb-recommend-ktx2` warnings fired 30 times (real stb decode + runtime
  mip generation engaged for real content).
- Zero `exceeding...staging`/`cannot chunk` errors.
- Zero occurrences of `checkerboard`/`fallback` anywhere in the log.
- Zero raw `[error]` lines; every `Validation Error`/`Validation Warning`
  line carries this codebase's own pre-existing "known false positive"
  label (the separate-sampler sync-validation misclassification class
  already documented elsewhere in this repo) — zero UNFILTERED validation
  errors.
- `sample_09_scene: window closed cleanly` logged on the real `SIGTERM`.
- HUD readout: `Texture: 827444.0 KiB (34 alloc(s))`, `316` renderables —
  real, large, multi-4K-material texture memory (a checkerboard-only
  scenario would be a handful of KB, not 827MB).

Frame captured as evidence:
`texture-path-round-workshop-nvidia.png` (this directory) — garage
interior, car, tool shelving, all real materials, no checkerboard/magenta
anywhere visible.

`sample_09_scene --present --scene sponza --validate` (same machine/ICD/
methodology, `Sponza`) run identically for corroboration: zero unfiltered
validation errors, `window closed cleanly`, `Texture: 393339.5 KiB (77
alloc(s))`. Frame: `texture-path-round-sponza-nvidia.png`.

## Verification bar — full results

**Full serial lavapipe ctest** (`VK_ICD_FILENAMES=lvp_icd.json xvfb-run -a
ctest --output-on-failure -j1`), after the D17 regen:

```
100% tests passed, 0 tests failed out of 29
Total Test time (real) =  81.51 sec
```

**Full serial ctest, real-NVIDIA forced** (`VK_ICD_FILENAMES=nvidia_icd.json
ctest --output-on-failure -j1`):

```
100% tests passed, 0 tests failed out of 29
Total Test time (real) = 137.23 sec
```

Every GPU-backed TEST_CASE in this suite asserts
`CHECK_FALSE(context.hasValidationErrors())` inline (the project's own
established pattern) — 29/29 green on the real driver is itself the "zero
unfiltered validation errors" proof for the whole suite, not just the two
samples captured above.

**`rx_asset_tests`/`rx_rhi_vk_tests` explicitly driver-labeled** (both
binaries, both ICDs, forced independently):

```
LAVAPIPE  rx_asset_tests:   39 test cases, 619 assertions, 0 failed
LAVAPIPE  rx_rhi_vk_tests:  74 test cases, 2164 assertions, 0 failed
NVIDIA    rx_asset_tests:   39 test cases, 619 assertions, 0 failed
NVIDIA    rx_rhi_vk_tests:  74 test cases, 2187 assertions, 0 failed
```

(The lavapipe/NVIDIA assertion-count difference, 2164 vs 2187, is a
pre-existing, unrelated window-resize-parity test that skips some checks
when the window manager doesn't actually resize an undecorated/unmapped
window on a fullscreen toggle — logged explicitly by that test itself, not
a regression here.)

**Zero warnings**: every touched file force-rebuilt (`touch` + full
rebuild) on BOTH presets; `grep -i warning` against both build logs
(excluding `_deps/`) returns nothing on either.

**windows-cross-zig build**: clean, 208/208 targets, zero warnings.

**Wine ctest** (this repo's own CI exclusion set,
`-E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_debug_ui_gpu|sample'`,
`WINEARCH=win64 xvfb-run -a ctest`):

```
100% tests passed, 0 tests failed out of 13
Total Test time (real) = 103.40 sec
```

Bonus (not required by the exclusion set, run directly as an extra check):
this machine's Wine install has a working winevulkan->llvmpipe passthrough,
so `rx_rhi_vk_tests.exe --test-case="*CHUNKED*"` was ALSO run directly under
Wine and passed (2/2, 23/23 assertions) — the chunked-staging path verified
under Wine too, not just excluded by policy.

## Files touched

- `src/rx_asset/texture_decode.{h,cpp}` — `generateStbMipChain()` +
  wiring into `decodeStbForUpload()`.
- `src/rx_asset/stb_image_resize_impl.cpp` (new) — the
  `STB_IMAGE_RESIZE_IMPLEMENTATION` TU.
- `src/rx_asset/CMakeLists.txt` — added the new TU.
- `src/rx_asset/tests/texture_decode_test.cpp` — 7 new device-free
  correctness/chain-length/degenerate-input tests.
- `src/rx_asset/tests/texture_cache_test.cpp` — updated the now-stale
  "PNG mip level 0 only" assertion (4 levels now); new end-to-end
  oversized-texture GPU test.
- `src/rx_rhi_vk/src/upload.cpp`, `include/rx_rhi_vk/upload.h` — chunked-
  row staging in `uploadImageMips()`; updated the D5 thread-affinity
  enumeration to name `uploadImageMips()` explicitly (pre-existing gap,
  fixed in passing since this round was already rewriting that exact
  method's doc comment).
- `src/rx_rhi_vk/tests/upload_test.cpp` — 2 new low-level chunked-staging
  tests.
- `assets/test/textures/generate_fixtures.sh`,
  `assets/test/textures/oversized_quadrant.png` (new),
  `assets/test/oversized_texture_probe.gltf` (new) — the Item B fixture.
- `samples/08_gltf_viewer/references/loaded_scene.png`,
  `samples/09_scene/references/grid_scene.png` — D17 regen (lavapipe,
  provenance above).
- This report + two evidence PNGs
  (`texture-path-round-{sponza,workshop}-nvidia.png`).

## Deviations from the brief

None material. Two things worth flagging as positive deviations rather than
gaps:

1. `stb_image_resize2` needed NO new vendoring/pinning at all (already
   present in the existing `stb` FetchContent checkout, same pinned
   commit) — better than the brief's own "check third_party for it or
   vendor the single header, pinned" anticipated.
2. The sRGB kernel's alpha-weighted RGB blend (`STBIR_RGBA`, not `_PM`)
   is a genuine correctness improvement over the brief's own literal
   "decode sRGB -> average -> re-encode" wording for content with varying
   alpha (avoids transparent-edge color bleed) — not exercised by this
   round's own opaque-alpha test fixtures, but real production behavior
   for any future cutout-alpha baseColor texture.

## Concerns for the coordinator

- Item B's chunking requires a level's bytes to divide evenly into whole
  image rows to chunk safely; this holds for every level this engine
  currently produces (uncompressed RGBA8 always; block-compressed KTX2
  levels large enough to need chunking always, since a level anywhere near
  the cap is never a 1-2-texel sub-block tail) — but a HYPOTHETICAL future
  KTX2 level with a genuinely non-block-aligned height AND large enough to
  need chunking would fail loudly rather than upload. Not exercised by any
  current asset; flagged for awareness, not a defect.
- Draco (#31), Workshop bundling, and the release build/rebuild remain the
  next items in the coordinator's own dispatch order per `progress.md`;
  untouched here.
