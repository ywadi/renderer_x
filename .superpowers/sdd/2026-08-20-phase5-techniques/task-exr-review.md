# Review — OpenEXR (.exr) input support (issue #75)

Reviewer: independent review agent. Commit under review: `02346b9` on
`task/exr-support` (base `9d65db3`), worktree
`/media/ywadi/second/renderer_x-worktrees/exr-support`. Main checkout used
read-only for SDD docs only; all builds/tests ran in the worktree (`cd -P`),
`nice -n19`'d, offscreen only.

## Verdicts

**Spec compliance: PASS.** All brief/ticket requirements are met: tinyexr
vendored library-first, magic-number-gated EXR sibling added to the T6
float-image dispatch, identical `DecodedStbHdrImage` payload feeding the
unchanged Environment→T9 bake chain, `--env` routes `.exr` with zero new
sample logic, rejection envelope enforced with actionable messages, byte-
source invariant preserved, committed container-equivalent fixture with
provenance, full acceptance proof set present. Two scope calls the
implementer made independently are adjudicated below (both sound, neither
blocking).

**Code quality: Approved.** No correctness defects found. Two non-blocking
style nits noted below.

## Independent verification performed

**Vendoring pin.** Cloned `github.com/syoyo/tinyexr` tag `v3.2.0` fresh
(commit `6f470c9ab24bf3992bc512ce07e8ecb00d9bf105`) and diffed/sha256'd
against the worktree's own `FetchContent`-populated
`build/linux-native/_deps/tinyexr-src`: `tinyexr.h`, `exr_reader.hh`,
`streamreader.hh`, `LICENSE`, and `deps/miniz/*` are byte-identical (the
only difference found was `deps/ZFP/` submodule content present in the
build's copy but absent from my shallow non-recursive clone — expected,
irrelevant, `TINYEXR_USE_ZFP=0`). `LICENSE` independently read: genuinely
**BSD-3-Clause** — the implementer's correction of the brief's "zlib"
claim is correct. Vendoring pattern (`FetchContent_Declare` +
`FetchContent_Populate`, source-only, no `add_subdirectory`,
`PARENT_SCOPE`'d `_SOURCE_DIR`, single `TINYEXR_IMPLEMENTATION` TU,
`deps/miniz/miniz.c` compiled straight into the consumer) matches this
repo's established stb/mikktspace/VMA convention exactly — confirmed by
reading `third_party/CMakeLists.txt` for those entries side by side.

**Magic-byte / flag-bit offsets**, cross-checked directly against upstream
tinyexr source (`ParseEXRVersionWithReader`, `tinyexr.h:10700-10736`):
magic `0x76 0x2F 0x31 0x01` at offset 0-3, flags byte at offset 5, `tiled`
= bit `0x02`, `non_image` (deep) = bit `0x08`, `multipart` = bit `0x10` —
all match `looksLikeExr()` and `gen_exr_env_fixtures`'s fixture-patching
bytes exactly. `TINYEXR_COMPRESSIONTYPE_DWAA` confirmed `8` in the
upstream header, matching the DWAA-rejection fixture's patched byte.
`LoadEXRFromMemory`'s RGBA output buffer confirmed `malloc`-allocated
(`tinyexr.h:8108`), matching `decodeExrImage()`'s `free(rgba)`. No leaks
found on any `decodeExrImage()` return path (header freed on every route
out once allocated; early rejects return before `InitEXRHeader`).

**Orientation (flip) — verified empirically, not by reading code alone**,
via two independent cross-checks beyond the implementer's own round-trip
test:
1. Decoded both the committed `gate_test_env.hdr` and `gate_test_env.exr`
   with **ImageMagick + real `libopenexr`** (system package, NOT tinyexr)
   and ImageMagick's own separate Radiance reader — zero pixel
   differences across all 2048 texels row-for-row. Confirmed the fixture
   is genuinely **vertically asymmetric** (top-row corner texel ≈
   (0.375, 0.672, 1.0), bottom-row corner ≈ (0.060, 0.045, 0.030)) —
   so the container-equivalence test actually discriminates a flip; it is
   not vacuous. (Horizontal variation is minimal — this fixture is a
   row-banded vertical pattern, 65 distinct RGB tuples over 2048 texels —
   but vertical asymmetry, the flip-relevant axis, is strong and real.)
2. Built a standalone probe linking tinyexr's actual `LoadEXRFromMemory()`
   (the exact function `decodeExrImage()` calls, unmodified vendored
   code) against the coordinator's **real production 4K PIZ/HALF HDRI**
   (`DayEnvironmentHDRI020_4K_HDR.exr`, not the procedural fixture, not
   authored by tinyexr) and compared row 0 / mid-row / last-row against
   ImageMagick's independent real-OpenEXR decode of the identical file:
   matched to ~1e-6 (float-rounding-only difference). This closes the
   blind spot a tinyexr-encode→tinyexr-decode-only round trip could in
   principle hide (self-consistent-but-wrong convention) — it checks
   tinyexr's *reader* against ground truth on a file tinyexr never wrote.
   No flip bug found on either asset.

**Real-world input reproduction**, real NVIDIA, offscreen:
`sample_08_gltf_viewer --validate --env
/home/ywadi/Downloads/DayEnvironmentHDRI020_4K/DayEnvironmentHDRI020_4K_HDR.exr`
(read in place, not copied into the repo) → decode + bake
(`total_ms=223.319`, matching the report's `226.557` within run-to-run
variance) + bind + render, `headless gate PASSED`, exit 0, zero unfiltered
validation errors (the D17 `loaded_scene` pixel-diff line legitimately
shows `pass=false` against the *tiny fixture's* reference PNG — expected,
since this is a different environment, and explicitly informational-only
on non-lavapipe per the sample's own existing convention).

**Revert-discrimination**, reproduced independently: added the identical
class of sabotage (R/B channel swap immediately after
`decodeExrImage()`'s `rgba32.assign(...)`), rebuilt, ran the
container-equivalence test — **4097/8201 assertions failed, maxAbsDiff
1.5625** (matches the report's numbers exactly). Restored via
`git checkout -- src/rx_asset/texture_decode.cpp`; `git diff HEAD` on that
file = 0 lines (byte-identical); rebuilt; full device-free suite green
again (**69/69 test cases, 9041/9041 assertions**); `git status` clean
(only pre-existing untracked `.deps-cache/assets/fetched/toolchain`
build-artifact directories, nothing tracked left dirty).

**Byte-source grep check**: ran `tools/check_byte_source_invariant.sh`
directly — green. Confirmed it's the actual script CI's "byte-source
invariant" step runs (`.github/workflows/ci.yml:54,450`).

**Rejection tests**: all three (deep/tiled/DWAA) pass as part of the full
suite; message content spot-checked to contain the named variant and the
literal substring `"supported envelope"`, matching the acceptance
criterion.

## Empirical test results (driver-labeled)

| Driver | Command | Result |
|---|---|---|
| **Real NVIDIA** (GeForce RTX 2080, driver 580.82.07) — first attempt, contaminated | `nice -n19 ctest --test-dir build/linux-native --output-on-failure` | 33/34 passed, 1 failed: `rx_asset_gltf_gpu_tests`, a `REQUIRE(maxPumpDuration < kCiStallDetector)` self-calibrated wall-clock stall detector in `async_import_test.cpp` (untouched by this diff, its own header comment documents it as load-sensitive under "dual CPU+GPU load" — tripped by MY OWN concurrent review commands, not this diff) |
| **Real NVIDIA**, re-run in isolation (no concurrent work) | same command | **34/34 passed, 0 failed**, 187.64 s, zero unfiltered validation errors |
| **Lavapipe** (Mesa llvmpipe, `lvp_icd.json`, under `xvfb-run`) — confirmed genuinely on lavapipe via the "dedicated transfer queue not present" log signature (vs NVIDIA's "ACQUIRED") | `VK_ICD_FILENAMES=lvp_icd.json xvfb-run -a nice -n19 ctest --test-dir build/linux-native --output-on-failure` | **34/34 passed, 0 failed**, 115.01 s, zero unfiltered validation errors |
| **Wine-tier** (`windows-cross-zig`, CI-filtered subset) | `ctest --test-dir build/windows-cross-zig -E 'rx_rhi_vk\|rx_graph_gpu\|rx_material_gpu\|rx_material_brdf_gpu\|rx_debug_ui_gpu\|rx_frame_loop_gpu\|rx_ibl_gpu\|sample' --output-on-failure` | **14/14 passed, 0 failed**, 147.08 s |

The one observed failure was reproduced to be a pre-existing, diff-unrelated
timing flake caused by my own review-session CPU contention (I had been
running `git clone`/`sha256sum`/ImageMagick commands in parallel with the
backgrounded ctest run); a clean isolated re-run confirmed 34/34. Flagging
this transparently rather than silently re-running until green.

## Adjudications

**Tiled-EXR rejection (implementer's independent scope call): SOUND, not a
finding.** tinyexr's convenience `LoadEXRFromMemory()` does have a working
tiled-read branch, but the pinned build's `SaveEXR*` has no public
tiled-write API, so the implementer could not generate a fixture to
rigorously value-verify that path — and chose to reject tiled outright
rather than ship an unverified-but-plausibly-correct code path. This is
consistent with (a) the brief's own explicit baseline framing ("baseline
scanline EXR ... is the bar"), (b) the fact that HDRI/DCC environment
exports are essentially always scanline in practice, and (c) — most
importantly — this project's own standing "never silently-wrong pixels"
ethos, which this same task applies rigorously everywhere else (DWAA,
UINT-channel). Shipping an EXR-envelope member specifically *because* it
can't be tested would be the one place this task would have contradicted
its own methodology. Non-blocking; worth a one-line pointer for a future
ticket that a real-OpenEXR-based tool (`exrmaketiled` or the Python
`OpenEXR` bindings, both of which support tiled write, unlike this
tinyexr build) could produce a verifiable tiled fixture if tiled support
is ever needed.

**No extension-based secondary detection implemented: SOUND, not a
finding.** The brief's scope text says "detect by magic number ... with
extension as a secondary hint," but `decodeTextureForUpload()` and its
siblings operate purely on `std::span<const std::byte>` — there is no
filename/path parameter anywhere in this call chain (by design: this
whole layer is the byte-source abstraction the grep-enforced invariant
protects), so there is no extension available to consult even if desired.
The existing `.hdr` (`stbi_is_hdr_from_memory`) and KTX2
(`looksLikeKtx2`) paths this EXR path was told to mirror are themselves
purely magic-byte-gated with zero extension involvement — the
implementation is exactly consistent with established precedent. The
brief's phrasing most plausibly describes user-facing file-naming
convention (sample's `--env foo.exr`), not a mandate to thread path
information into the pure-decode layer, which would have been a
scope-creeping architecture change this task correctly avoided.

## Findings (non-blocking)

- [style, `src/rx_asset/texture_decode.cpp`] `decodeStbHdrForUpload()` and
  `decodeExrForUpload()` carry near-identical ~8-line
  decode-failure-to-`TextureDecodeResult` boilerplate; could be factored
  into one shared helper parameterized on the decode function and error
  prefix. Cosmetic only.
- [style, `src/rx_asset/texture_decode.cpp`, `decodeExrImage()`] The
  header-parse-failure branch unconditionally appends the "(known
  excluded codecs: DWAA, DWAB ...; ZFP ...)" parenthetical to *every*
  `ParseEXRHeaderFromMemory` failure, not only ones actually caused by
  those codecs — for a genuinely corrupt/unrelated header failure the
  message is still truthful and actionable (it leads with tinyexr's own
  real error text) but the trailing context is slightly over-broad.
  Cosmetic only; test coverage already asserts on the substrings that
  matter.

## Not independently verifiable / out of scope for this review

- Whether tinyexr's tiled-read branch is itself correct was not
  independently tested (no tool at hand to author a real tiled EXR to
  compare against) — moot given the rejection call above, noted for
  completeness only.
- No formal published benchmark numbers were produced for EXR decode
  specifically. Judged not applicable: decode is a one-time asset-load
  cost off the per-frame render path, the bake/render chain downstream is
  byte-for-byte unchanged, and this ticket is an owner-inserted point
  addition inside Phase 5 Stage 1, not a phase exit — the repo's
  benchmark-gate mandate targets phase-exit stress samples, which this
  is not.

## Hygiene

Single commit (`02346b9` on base `9d65db3`), author `Yousef Wadi
<ywadi85@gmail.com>` matching local git config, zero AI-attribution hits
across commit message/diff/touched docs, branch not pushed
(`git ls-remote origin` has no `exr-support` ref), `main`/`origin/main`
unchanged at `9d65db3`. `assets/test/ASSET-NOTES.md` carries the required
provenance one-liner; all four new binary fixtures are procedurally
generated (generator source committed at `tools/gen_exr_env_fixtures`,
matching the repo's `gen_gltf_compression_fixtures` precedent) — no
third-party imagery committed. Worktree left clean after review
(sabotage edit reverted via `git checkout`, confirmed byte-identical;
only pre-existing untracked build-artifact directories remain).
