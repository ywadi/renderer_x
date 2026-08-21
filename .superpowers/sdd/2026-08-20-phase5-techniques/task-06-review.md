# Task 6 review — Cubemap/array KTX2 loading + HDR image input (issue #42)

Independent reviewer round. Commits under review: `0d26462`, `db99857`,
`aa7e3e1`, `2badf74`, `44ae638`, `163672d` (base `c2f9eae`). Order of
authority: `rulings-2026-08-20.md` ("T6 (#42)" + RC7e) > plan > gate matrix
(`matrix-p5t06-ktx2-cubemap-hdr.md`) > ticket (#42).

## Verdict 1 — Spec compliance: **PASS**

The implementation matches the T6 ruling exactly, as amended by the gate
matrix's per-row disposition:

- **Cubemap-only.** `isUnsupportedLayoutFor()` (`src/rx_asset/texture_decode.cpp`)
  reads `tex->numDimensions != 2 || (tex->isArray && !tex->isCubemap) ||
  (tex->isCubemap && tex->numLayers > 1)` — character-for-character the
  formula the ruling specifies for row 3. Flat 2D-arrays and cube-arrays
  stay rejected via two independent clauses, each with its own regression
  fixture (`array2d_rejected.ktx2`, `cubearray_rejected.ktx2`) and both a
  device-free (`texture_decode_test.cpp`) and TextureCache-level
  (`texture_cache_test.cpp`) test — both re-run and pass.
- **Environment upload format R16G16B16A16_SFLOAT.** Confirmed by direct
  code read (`decodeStbHdrForUpload()`) and by a real GPU readback
  (`glm::unpackHalf4x16` off a `vkCmdCopyImageToBuffer`) on real NVIDIA
  hardware, isolated: `4.0/0.5/0.5` (TL), `2.0/2.0/2.0` (BR), both exactly
  as authored.
- **Environment D11 fallback = uniform mid-gray.** Isolated GPU readback
  on real NVIDIA confirms `(0.5, 0.5, 0.5, 1.0)`, not black, not an
  8-bit-cast shortcut — uploaded through the same float-packing path real
  HDR content uses.
- **Byte-source grep becomes a real CI check (RC7e).** `tools/
  check_byte_source_invariant.sh` exists, is wired into both CI jobs
  (`linux-native`, `windows-cross-zig`) immediately after checkout, and
  its failure mode was independently reproduced (see Findings/Verification
  below) — the "grep-enforced" wording is now literally true.

Two-old-tests-replaced-by-flip discrimination proof (matrix row 2) is
real: `grep` for the two Phase-4 test names
("cubemap KTX2 -> checkerboard fallback", "cubemap fixture is classified
UnsupportedLayout") returns zero hits anywhere in the tree — they were
deleted, not left alongside new passing coverage.

Row 10 (HDR stays flat 2D, no cube-plumbing collision) verified by code
read: `decodeStbHdrForUpload()` never touches `isCube`/`faceIndex`/
`createCubeForPresuppliedMips()`. Row 9's silent-tonemap fix is
provably closed process-wide, not just on a new path: the only
production call site of the 8-bit `decodeStbImage()` is inside
`decodeStbForUpload()`, itself reachable only through
`decodeTextureForUpload()`'s dispatch, which now checks
`stbi_is_hdr_from_memory()` before ever falling through to that branch —
and `TextureCache::decodeForUpload()` is the sole production entry into
that dispatch (`texture_cache.cpp:471`).

## Verdict 2 — Code quality: **Approved**, one minor finding

### Findings

1. **[MINOR]** The two new row-3 regression tests
   (`array2d_rejected.ktx2`/`cubearray_rejected.ktx2`, both in
   `texture_decode_test.cpp` and the combined `texture_cache_test.cpp`
   case) assert only the boolean/enum outcome
   (`isUnsupportedLayout()`/`Ktx2ParseError::UnsupportedLayout`/
   checkerboard handle), never the actual WARN/`failureReason` text, via
   `LogCapture`. This is inconsistent with this same file's own
   established convention for D11 rejection paths — e.g. the
   sRGB-mislabeled-normal WARN test and the log-once-dedup test both use
   `LogCapture` to lock the exact message text in place
   (`texture_cache_test.cpp:1408`, `:1447`). Manually verified the
   message itself IS actionable ("is an array, cube-array, or non-2D
   KTX2 container -- unsupported (cubemap-only per the Phase 5 Task 6
   ruling; zero charter consumer needs a general texture array or
   cube-array); falling back to checkerboard",
   `texture_decode.cpp:612-617`), and it's a single shared string across
   all three rejected shapes (array/cube-array/1D-3D) rather than
   discriminating which one triggered — reasonable given precedent, but
   not regression-locked. Non-blocking; recommend a follow-up
   `LogCapture`-based assertion if this file's own WARN-text discipline
   is meant to be uniform.

No other quality issues found: comments are thorough and load-bearing
(not filler), naming is consistent with the codebase's conventions, the
new `createCubeForPresuppliedMips()` correctly reuses
`StorageImage::create()`'s existing cube-flag/view-type precedent rather
than reinventing it (verified: `storage_image.cpp`'s `viewTypeFor()`/
`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` pattern predates this task), no
dead/duplicate code, no stray debug output, `ImageMipLevel::
baseArrayLayer`/`Texture2D::isCube()`/`arrayLayers()` all default
byte-identically for every pre-existing caller.

## TRANSFER_SRC_BIT-on-all-textures adjudication: **justified as scoped (blanket)**

`TextureCache::registerRealTexture()` now requests
`VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT` for every
real (non-fallback and fallback) texture it builds, not just cubes/HDR.
Ruling: **the blanket scope is justified, not overbroad**, for these
reasons:

- **Format-feature cost is genuinely zero, verified against the Vulkan
  1.3 core spec's Required Format Support guarantee**, not merely
  asserted: any format that supports `VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT`
  is required to also support `VK_FORMAT_FEATURE_TRANSFER_SRC_BIT` and
  `VK_FORMAT_FEATURE_TRANSFER_DST_BIT`. This applies uniformly to the
  uncompressed formats this task touches (`R8G8B8A8_*`,
  `R16G16B16A16_SFLOAT`) AND to the BC/UASTC-transcoded formats the
  pre-existing BaseColor/Normal/etc. roles already use — the widening
  costs nothing on any conformant implementation for any role, not just
  the two this task's own tests exercise.
- **The theoretical counter-risk the attention lens asks about (usage
  flags gating a driver's internal compression heuristic) is a
  render-target/storage-image concern on certain tile-based mobile GPUs
  (framebuffer/AFBC-style compression tied to attachment or storage
  usage), not a SAMPLED-only-image concern.** `registerRealTexture()`
  never requests `COLOR_ATTACHMENT_BIT` or `STORAGE_BIT` here — only
  `SAMPLED_BIT | TRANSFER_SRC_BIT | TRANSFER_DST_BIT` (the last already
  mandatory pre-task). Steam Deck (AMD RDNA2, this project's own
  documented hardware floor) does not implement this usage-gated
  compression pattern for sampled textures. The risk this scope decision
  needed to weigh does not actually apply to the images being widened.
- **A cube/HDR-only scoping would need a per-role or per-`isCube`
  conditional threaded through `registerRealTexture()`'s single
  `kRealTextureUsage` constant**, adding real branching complexity to
  save nothing (same spec-guaranteed zero cost either way), and would
  leave a future consumer needing debug/diagnostic readback of a
  BaseColor/Normal texture unable to do so without another RHI change —
  the blanket form is the simpler, more forward-compatible design for a
  cost that doesn't exist.
- Self-flagged explicitly by the implementer (Concern #2) as reaching
  past the ticket's literal "loading" framing into a production
  usage-flag change — correct due diligence; reviewed and found
  justified rather than requiring narrowing.

**Forward-looking note (not a finding against this task):** this
reasoning is specific to sampled-only images. It should not be assumed
to extend automatically to a future `COLOR_ATTACHMENT_BIT`/
`STORAGE_BIT` usage-flag widening, where the compression-heuristic
caveat above becomes live on some hardware classes — a future task
touching render-target or storage-image usage flags should re-derive
this, not cite this ruling as precedent.

## Verification performed (empirical minimum)

All GPU runs driver-labeled per RC7a. NICEd, foreground, solo GPU;
`cd -P` used throughout; no destructive git operations.

- **Full serial ctest, lavapipe** (`VK_ICD_FILENAMES=lvp_icd.json`):
  `100% tests passed, 0 tests failed out of 31` (150.74s) — reproduced
  independently, matches the report's own 31/31 claim.
- **rx_asset_tests, real NVIDIA** (RTX 2080, driver 580.82.07,
  `nvidia_icd.json`, confirmed via `vulkaninfo --summary`): 44/44 cases,
  25314/25314 assertions, 0 failed — matches report exactly.
- **rx_rhi_vk_tests, real NVIDIA** (same driver): 97/97 cases,
  2398/2398 assertions, 0 failed — matches report exactly.
- **rx_asset_tests / rx_rhi_vk_tests, lavapipe** (cross-check): 44/44
  (25314 assertions) and 97/97 (2387 assertions) respectively — the
  11-assertion driver delta matches the report's documented
  `samplerAnisotropy`-conditional-coverage explanation, not a new
  discrepancy.
- **Sabotage re-proof (matrix row 11).** Swapped the expected RGBA
  colors of cube faces 0 and 1 in the GPU per-face readback test
  (`texture_cache_test.cpp`), rebuilt, ran on real NVIDIA: **6150/24627
  assertions failed**, every failure an exact, face-attributed color
  mismatch at both mip 0 and mip 5 for exactly the two swapped faces,
  zero failures on the four untouched faces — genuine value-discriminating
  coverage, not "it loaded." Reverted; `git diff --stat` on the file is
  empty (byte-identical restore); rebuilt clean.
- **RC7e grep injection re-proof (matrix row 14).** Injected a real
  `fopen(` call into `texture_decode.cpp`: script failed loudly, naming
  the exact violating line. Reverted; `git diff --stat` empty
  (byte-identical restore); script passes clean again.
- **CI wiring, both jobs.** Confirmed on disk: identical `tools/
  check_byte_source_invariant.sh` step present in both `linux-native`
  (line 53-54) and `windows-cross-zig` (line 442-443) jobs, immediately
  after checkout. GitHub Actions' default `run:` shell (`bash -eo
  pipefail`) plus the script's own `set -euo pipefail`/`exit 1` means a
  failure here fails the step and the job — exit-code propagation
  confirmed by construction and by the injection test above.
- **Fixture provenance, ktxinfo (v4.4.2, the pinned release binary).**
  `cubemap.ktx2`: 4x4, faceCount=6, layerCount=0, levelCount=1,
  `VK_FORMAT_R8G8B8A8_SRGB`. `cubemap_mips.ktx2`: 32x32, faceCount=6,
  levelCount=6. `array2d_rejected.ktx2`: 4x4, layerCount=3, faceCount=1.
  `cubearray_rejected.ktx2`: 4x4, layerCount=2, faceCount=6. All four
  match their documented shapes exactly; all four also pass `ktx2check`
  clean (no warnings/errors).
- **Hand-authored RGBE fixture math, independently re-derived.**
  Decoded the actual committed bytes of `equirect_test.hdr` (not the
  script comment) via a standalone RGBE decode: TL=(4.0,0.5,0.5),
  TR=(0.5,4.0,0.5), BL=(0.5,0.5,4.0), BR=(2.0,2.0,2.0) — exact match to
  both the generation-script comment and the test assertions.
  `corrupt.hdr`'s bytes confirmed to be a valid-signature/garbled-
  resolution-line file as documented (decode-failure path already
  covered by the passing test suite).
- **Mid-gray fallback and HDR >1.0 tests, isolated on real NVIDIA.**
  Both isolated (`--test-case` filtered) runs pass clean: 18/18 and
  25/25 assertions respectively.
- **Bundled `.cache` fix (44ae638).** The three stray
  `rx_compute_pipeline_test*.cache` files present in the initial session
  snapshot are gone from the repo root; re-running
  `ComputePipelineCache`'s tests on real NVIDIA shows load/save against
  `/tmp/rx_compute_pipeline_test3.cache` (persistence still exercised —
  32 bytes loaded from a prior run, 6423 bytes saved back) and zero
  stray files reappear in the repo root afterward.
- **Wine `rx_core_tests` timeout concern.** `git diff --stat -- src/
  rx_core/` is empty — zero diff overlap confirmed independently.
  Re-ran `rx_core_tests.exe` once under Wine (90s timeout): hung/timed
  out, reproducing the report's claim (now a 3rd independent occurrence
  against this same binary). Re-ran `rx_asset_tests.exe` (the actual
  binary carrying every new cube/HDR test) once under Wine with Xvfb,
  matching CI's own harness pattern: 44/44 cases, 25314/25314
  assertions, 0 failed, 0 skipped — confirms the task's real surface is
  clean under Wine, not merely build-clean. Classification: same
  Wine-teardown-flake watch-item category as T5's ledgered
  `rx_platform_tests` entry (different binary, same "hangs near/after
  teardown, unrelated diff" shape) — **not a blocker for this task.**
- **Build.** `cmake --build build/linux-native` and `cmake --build
  build/windows-cross-zig` both rebuilt clean (incremental, following
  the sabotage/injection/revert cycles above) with zero errors/warnings
  in the ninja output.
- **Commit hygiene.** 6 commits, each cleanly pathspec-scoped to its
  stated purpose (verified via `git show --stat` per commit): rx_rhi_vk
  primitives, fixtures, rx_asset decode/cache layer, CI grep script, the
  bundled cache-leak fix, the SDD report. Author/committer identity is
  the user's own configured git identity on all 6. `git log` grep for
  claude/anthropic/co-authored/generated-by/ai-assistant: zero hits on
  any of the 6 commit messages. `git status`: branch ahead of origin by
  6 commits, nothing pushed. `.superpowers/sdd/2026-08-20-phase5-
  techniques/progress.md`'s pre-existing modification was left untouched
  throughout this review.

## Not independently re-verified (honest gaps)

- **windows-cross-zig "74/74 targets, zero warnings" full-clean-build
  claim**: re-verified via an *incremental* rebuild only (which was
  clean), not a from-scratch `rm -rf build/windows-cross-zig` rebuild —
  a full clean rebuild was judged not worth the wall-clock cost for a
  claim already corroborated by a clean incremental build plus a
  passing Wine run of the actual new-test-carrying binary.
- **Full Wine ctest suite (13/14 with the CI's exact `-E` exclusion
  filter)**: spot-checked via the two most relevant individual binaries
  (`rx_core_tests` reproducing the timeout, `rx_asset_tests` passing
  clean) rather than reproducing the full 14-test filtered run — judged
  sufficient given the specific concern (diff overlap + task-surface
  health), not the general Wine-job health.
- **Steam Deck hardware.** Not run; consistent with RC8's own
  honest-manual-until-Deck-enters-the-loop caveat and not a requirement
  for this Stage-0 infrastructure/loading task.
