# Task report — OpenEXR (.exr) input support (issue #75, owner insertion into Phase 5 Stage 1 between T10 and T11)

Branch: `task/exr-support`, worktree `/media/ywadi/second/renderer_x-worktrees/exr-support`, base `9d65db3`.
Commit: `02346b9510a12c12168600ccaf8afc8d900200bd` — "feat(rx_asset): OpenEXR (.exr) environment input support (issue #75)".

## Fix round 1 (independent review: spec PASS / quality Approved, 2 non-blocking style findings, closed in-round)

Commit: `38cbf184237ed416c2ea0467c2b3a454e4c6499f` — "fix(rx_asset): EXR review round 1 -- factor shared error wrapping, scope codec message".

1. **Shared error-wrapping boilerplate**: `decodeStbHdrForUpload()`/`decodeExrForUpload()` each built an identical fresh `TextureDecodeResult{role, Outcome::Failed, failureReason}` by hand on their own decode-failure branch (~8 duplicated lines). Factored into a new private helper, `makeFailedResult(TextureRole role, std::string reason)`, placed in the same anonymous namespace as `finalizeHdrFloatUpload()` (naming/idiom matches the file's existing lowerCamelCase free-function convention, e.g. `exceedsDimensionLimit`, `checkColorspaceAgreement`). Both call sites now reduce to a single `return makeFailedResult(role, ...)` line; no behavior change (verified via the unchanged existing test assertions on both paths).
2. **Unconditional codec parenthetical**: `decodeExrImage()`'s header-parse-failure branch appended `" (known excluded codecs: DWAA, DWAB ...; ZFP ...)"` to EVERY header-parse failure, including ones structurally unrelated to compression support (e.g. a truncated/corrupt file). Verified directly against the existing 16-byte-truncated fixture: tinyexr's own message there is `"Failed to read attribute."` — no codec involved, so the parenthetical was actively misleading for that case. Fix: the parenthetical is now appended only when tinyexr's own message contains the substring `"compression"` (covers all three of its real compression-support failure strings: `"Unknown compression type."`, `"PIZ compression is not supported."`, `"ZFP compression is not supported."` — verified by direct source read, `third_party` tinyexr pin unchanged). Strengthened both affected TEST_CASEs with explicit assertions: the corrupt/truncated-fixture test now additionally asserts `failureReason.find("known excluded codecs") == npos` and `find("DWAA") == npos`; the DWAA-rejection test now additionally asserts `failureReason.find("known excluded codecs") != npos` (the positive mirror).

**Test evidence (message-text assertions changed, so re-ran the exact affected targets, per the coordinator's instruction — lavapipe only for this cosmetic round, both drivers already proved the underlying logic in the original round)**:
- `tools/check_byte_source_invariant.sh` — green (no new file I/O in `texture_decode.cpp`).
- `rx_asset_gltf_tests` (device-free, hosts the changed/added TEST_CASEs), run directly: **69/69 test cases, 9044/9044 assertions, 0 failed** (was 9041 before this round — +3 from the two new/strengthened assertions).
- Lavapipe (Mesa 25.1.5, `lvp_icd.json`, under `xvfb-run`, NICE'd): `ctest -R 'rx_asset_tests|rx_asset_gltf_tests|rx_asset_gltf_gpu_tests|sample_08_gltf_viewer'` — **6/6 tests passed**, 0 failed, 33.69 s (`rx_asset_tests` 3.25 s, `rx_asset_gltf_tests` 0.02 s, `rx_asset_gltf_gpu_tests` 25.38 s, `sample_08_gltf_viewer_headless` 1.81 s, `sample_08_gltf_viewer_quit_during_load` 1.47 s, `sample_08_gltf_viewer_exr_env_headless` 1.77 s).

No commit pushed. No AI attribution in either commit.

## Scope delivered

1. Library-first EXR decode: tinyexr vendored the same way this repo vendors every other no-CMakeLists third-party dependency (FetchContent_Populate, source-only, PARENT_SCOPE'd `_SOURCE_DIR`, implementation compiled straight into the consumer).
2. `rx_asset`'s float-image dispatch (`texture_decode.cpp`) gains an EXR sibling to the existing Radiance `.hdr` path: magic-number detection (`looksLikeExr`, `0x76 0x2F 0x31 0x01`), decode via tinyexr, producing the identical `DecodedStbHdrImage` float-RGBA payload the `.hdr` path produces. Both containers now share one packing tail (`finalizeHdrFloatUpload`) into the unchanged Environment-role → T9 bake chain.
3. Scope bar enforced and probed directly against the pinned release (not assumed from its README).
4. Sample 08's `--env` flag accepts `.exr` with zero new sample logic — routing is entirely magic-number-driven inside `decodeTextureForUpload()`, which the sample already calls unconditionally.

## Library choice — tinyexr vs. alternatives

- **OpenEXR proper** (the reference implementation): rejected as too heavy a dependency for one input path. It is a multi-library CMake project of its own (OpenEXR + Imath, its own zlib dependency, its own thread pool) — exactly the class of weight this repo's own T6 header comment already rejected in favor of reusing `stb_image.h`'s built-in Radiance reader for the sibling `.hdr` path, rather than adding a dedicated decoder.
- **miniexr** (same author as tinyexr): rejected outright — it is a WRITE-only EXR encoder, no decode API at all. No use for an input path.
- **tinyexr**: the de-facto lightweight single-purpose EXR decoder this ecosystem reaches for exactly this need (used by Blender's own glTF importer, Filament, and many other engines to read EXR without linking full OpenEXR).

**Correction vs. the task brief's own text**: the brief describes tinyexr as "zlib-licensed". Verified directly against the pinned tag's own `LICENSE` file: tinyexr is **BSD-3-Clause**, not zlib. Permissive either way, no attribution requirement beyond this repository's own — recorded accurately here rather than repeating the brief's assumption.

**Correction vs. the "single-file" framing**: both the brief and this project's own established convention (stb, mikktspace) suggested a literal single header. Verified directly by compiling against the pinned tag: `tinyexr.h` itself unconditionally `#include`s two small companion headers from the same repo — `exr_reader.hh` (191 lines) and `streamreader.hh` — a v3.x upstream refactor split part of the reader out of the monolithic header. Still a lightweight, decode-focused vendoring (nowhere near full OpenEXR's weight): the FetchContent_Populate fetches the whole tinyexr source tree (matching how every other no-CMakeLists dependency here is vendored), and only `tinyexr.h`/`exr_reader.hh`/`streamreader.hh` (via the include path) plus `deps/miniz/miniz.c` (explicitly compiled) are ever referenced — the rest of the tree (examples/, attic/, `deps/astcenc` for an unrelated ASTC-texture feature, etc.) is unused dead weight on disk, never compiled.

## Pin and vendoring

- `RX_TINYEXR_TAG "v3.2.0"` (`third_party/CMakeLists.txt`), the latest tagged release (published 2026-07-08), fetched via `FetchContent_Declare` + `FetchContent_Populate` (source-only, no `add_subdirectory` — tinyexr's own `CMakeLists.txt` builds its test/benchmark suite, irrelevant here), `tinyexr_SOURCE_DIR` propagated `PARENT_SCOPE` to the root scope for `src/rx_asset` (a sibling directory) to consume — the identical pattern this file already uses for stb/volk/VMA/mikktspace.
- Implementation TU: `src/rx_asset/tinyexr_impl.cpp` (`#define TINYEXR_IMPLEMENTATION` + `#include <tinyexr.h>`), the sole such TU in the codebase, mirroring `stb_image_resize_impl.cpp`/`rx_rhi_vk/src/stb_impl.cpp`/`vma_impl.cpp`'s own single-TU-implementation discipline.
- DEFLATE implementation: `TINYEXR_USE_MINIZ=1` (the header's own default) — tinyexr's vendored `deps/miniz/miniz.c` is compiled straight into `rx_asset` (mirrors `mikktspace.c`'s own "no build system → compile into the consumer" precedent), avoiding both a system zlib dependency and `TINYEXR_USE_STB_ZLIB` (which would pull `stb_image_write.h`'s encode-only `stbi_zlib_compress()` symbol into every consumer for a decode-only need).
- `target_include_directories(rx_asset PRIVATE "${mikktspace_SOURCE_DIR}" "${tinyexr_SOURCE_DIR}" "${tinyexr_SOURCE_DIR}/deps/miniz")`.

## Supported-envelope evidence (probed directly, not assumed)

Built a standalone probe (`tinyexr.h` + `deps/miniz` at the pinned tag, compiled with `g++`/`gcc`, `/tmp/.../scratchpad/exr_probe`) and ran it against synthetic fixtures and one real-world asset:

- **Compression round-trip, all 7 non-exotic codecs**: NONE, RLE, ZIPS, PXR24, B44, B44A, PIZ all wrote via `SaveEXRImageToMemory` and read back correctly via `LoadEXRFromMemory` on an 8×4 HALF-pixel fixture (texel0 authored as `(0.1, 0.2, 0.5, 1.0)`): NONE/RLE/ZIPS/PIZ decoded to `(0.09998, 0.19995, 0.50000, 1.00000)` (pure half-quantization); B44/B44A (genuinely lossy block compression) decoded to `(0.10016, 0.19971, 0.50000, 1.00000)` — larger error, expected for a lossy block codec, not a bug.
- **DWAA/DWAB**: confirmed genuinely unimplemented, not merely undocumented. Source evidence: `tinyexr.h`'s own compression-type check (`ParseEXRHeaderFromMemory`'s attribute parser) has explicit `ok=true` branches for NONE/RLE/ZIPS/ZIP (value < PIZ), PIZ, PXR24/B44/B44A — DWAA(8)/DWAB(9) match none of them, so `ok` stays false and the parse returns `TINYEXR_ERROR_UNSUPPORTED_FORMAT` with `"Unknown compression type."`. Empirically confirmed: a byte-patched fixture (valid ZIP-compressed EXR with the compression attribute's single value byte flipped from `3` to `8`) fails `ParseEXRHeaderFromMemory` with exactly that message (`ret=-8`).
- **ZFP**: `TINYEXR_USE_ZFP` left at its header default (`0`) — would need a separate `libzfp` dependency; tinyexr's own message for this case is already specific (`"ZFP compression is not supported."`).
- **UINT pixel-type footgun (the reason this decode path gates on pixel type before calling tinyexr's convenience loader)**: built a 2-texel UINT-channel EXR (`R=1000, G=2000, B=3000, A=1` as `uint32`), round-tripped through `LoadEXRFromMemory`. Result: `R=0.000000 G=0.000000 B=0.000000 A=0.000000` — `LoadEXRImageFromMemory`'s own decode requires `requested_pixel_types[c] == TINYEXR_PIXELTYPE_UINT` whenever a channel's native type is UINT (no float conversion path), and the RGBA-packing loop then does `reinterpret_cast<float**>(images)[idxR][i]` unconditionally — reinterpreting the raw `uint32` bits as an IEEE-754 float, not converting the value. `1000u` reinterpreted as `float` bits is a tiny denormal that prints as `0.0`. This is exactly the "silently wrong pixels" bug class the ticket cites (T6's LDR-collapse regression) — `decodeExrImage()` therefore rejects any UINT-typed channel explicitly, before ever calling `LoadEXRFromMemory`.
- **Deep/multipart/tiled**: `EXRVersion`'s own flags byte (byte offset 5 of the file) carries `tiled` (bit `0x02`), `non_image`/deep (bit `0x08`), `multipart` (bit `0x10`) — confirmed by direct bit-patching a valid file and re-parsing with `ParseEXRVersionFromMemory` (an 8-byte parse, always available regardless of the rest of the file's validity). `decodeExrImage()` checks all three from this cheap version-level parse, before ever attempting the full header parse (which behaves unpredictably/confusingly for these shapes — e.g. a deep-flagged file with an otherwise-ordinary flat-image body fails `ParseEXRHeaderFromMemory` with `"name" attribute not found in the header. "type" attribute not found in the header.`, a much less legible diagnostic than this decode path's own named rejection).
- **Real production 4K HDRI** (coordinator-supplied, local-only, NOT committed): `/home/ywadi/Downloads/DayEnvironmentHDRI020_4K/DayEnvironmentHDRI020_4K_HDR.exr` (35,990,734 bytes, ambientCG-style asset). Header: `4096×2048`, `compression_type=4` (**PIZ**), 4 channels `A/B/G/R` all `pixel_type=1` (**HALF**) — squarely inside the supported envelope. `LoadEXRFromMemory` decoded it in **488.4 ms** (single-threaded probe binary), center texel `(0.760742, 0.889648, 0.445068, 1.000000)` — plausible daylight-HDRI values. Run through the REAL production path (`sample_08_gltf_viewer --validate --env <path>`, real NVIDIA driver, offscreen): decoded, baked (`total_ms=226.557`, same bake-resolution cost as the tiny fixture — bake params are fixed, independent of source size), bound, and rendered with **zero unfiltered validation errors**, process exit code **0**. This file is licensed separately from this repository (owner's downloaded asset) and was used for LOCAL verification only — not committed, not referenced except by this absolute path.

**Resulting rejection boundary** (`texture_decode.h`'s own EXR block comment states this verbatim): single-part **scanline** EXR (never tiled/deep/multipart), **HALF or FLOAT** pixel-type channels, any of **NONE/RLE/ZIPS/ZIP/PIZ/PXR24/B44/B44A** compression. Note: `tinyexr`'s convenience `LoadEXRFromMemory()` can technically decode a single-part TILED file too (verified by reading its own RGBA-packing code, which has a `tiled` branch) — this decode path rejects tiled anyway, deliberately, because (a) the ticket's own scope note frames "baseline scanline EXR" as the bar, (b) HDRI/DCC environment exports are essentially always scanline, and (c) tinyexr's own SaveEXR has no public tiled-write API, so a tiled fixture cannot be generated and value-verified the same rigorous way the scanline envelope was — accepting an untested code path would itself risk the "silently-wrong-pixels" class this ticket is about. Flagged here as the one place this task made a stricter-than-strictly-necessary call; recommended and taken rather than blocking, per the brief's own ambiguity-resolution instruction.

## Design notes

- `decodeExrImage()` (`texture_decode.cpp`) does its own two-stage validation BEFORE calling tinyexr's convenience `LoadEXRFromMemory()`: (1) `ParseEXRVersionFromMemory` → reject tiled/deep/multipart by name; (2) `ParseEXRHeaderFromMemory` → reject unsupported compression (tinyexr's own allow-list, wrapped with a message naming the known-excluded codecs) and UINT-typed channels (this repo's own gate, since tinyexr has none). Only after both stages pass does it call `LoadEXRFromMemory` for the actual decode+RGBA-pack — no hand-rolled EXR pixel decode written anywhere in this task; every actual decode byte comes from tinyexr's own library code, honoring "library-first."
- Refactored the `.hdr` upload tail into a shared `finalizeHdrFloatUpload()` (dimension gate, `R16G16B16A16_SFLOAT` format-support gate, `glm::packHalf4x16` packing) so `.hdr` and `.exr` share one implementation for the packing step downstream of decode — a behavior change to either gate applies identically to both containers, never independently re-derived per format. Existing `.hdr` tests continue to pass unchanged (same code path, just refactored).
- Byte-source invariant: `decodeExrImage`/`looksLikeExr` touch only the `std::span<const std::byte>` handed in — zero `fopen`/`ifstream`/`ofstream`/`std::filesystem` member other than `path`. `tools/check_byte_source_invariant.sh` verified green (see below).

## Committed fixtures and provenance ("same generator, second container")

`samples/08_gltf_viewer/environments/gate_test_env.hdr` (Phase 5 Task 10's own committed procedural fixture) was itself written by a one-off Python script that was **never committed** (task-10-report.md's own text: "written directly via a one-off Python script... procedurally-generated-fixture precedent"). Rather than guess-reconstruct that undocumented formula — risking silent value drift that could ripple into the already-passing, committed D17 visual reference PNGs baked from the exact existing `.hdr` bytes, when this ticket is explicitly routing-only ("no new sample logic") — this task's generator (`tools/gen_exr_env_fixtures`, a host-only C++ tool matching this repo's own `gen_gltf_compression_fixtures` convention) instead:

1. Decodes the existing, **untouched**, committed `gate_test_env.hdr` via `rx::asset::decodeStbImageHdr()` — the exact SAME production decode function the real Environment-role `.hdr` path already uses.
2. Re-encodes those exact floats as HALF-pixel-type, ZIP-compressed OpenEXR via tinyexr's own `SaveEXRImageToMemory()`.

This guarantees true content identity **by construction** — one canonical set of pixel values, two container encoders — rather than an approximation of a lost formula. `gate_test_env.hdr` itself is never modified.

Outputs (all committed, 1358 bytes each, 64×32):
- `assets/test/textures/gate_test_env.exr` — device-free unit-test fixture (rx_asset's own established fixture directory).
- `samples/08_gltf_viewer/environments/gate_test_env.exr` — byte-identical copy, the sample's own `--env` full-chain fixture.
- `assets/test/textures/exr_deep_rejected.exr` / `exr_tiled_rejected.exr` / `exr_dwaa_rejected.exr` — the same generator, bit-patching the version-flags byte (deep/tiled) or the header's `compression` attribute value byte (DWAA) of an otherwise-valid encode.

Provenance recorded in `assets/test/ASSET-NOTES.md` per the test-assets policy.

Format choice justification: HALF pixel type exercises the half leg of the ticket's "half and float" bar; its ~10-bit mantissa is finer than the SOURCE `.hdr`'s own already-existing RGBE quantization (8-bit shared-exponent mantissa) over this fixture's value range, so the EXR round-trip adds at most a small further step on top of quantization the `.hdr` already carries. ZIP is a real (non-NONE) compression inside the supported envelope, giving real compressed-scanline coverage rather than an uncompressed-only round-trip.

## Acceptance proofs, with numbers

**Container-equivalence** (`src/rx_asset/tests/texture_decode_test.cpp`, TEST_CASE "decodeExrImage/container-equivalence"): decodes `gate_test_env.hdr` via `decodeStbImageHdr()` and `gate_test_env.exr` via `decodeExrImage()`, compares all 8192 floats (`64×32×4`) with `doctest::Approx(...).epsilon(0.01)` (1%, ~20× the ~0.05% theoretical half-quantization noise floor for headroom). **Observed max absolute per-channel diff: `0.0` exactly** — the source `.hdr`'s own RGBE-quantized values happened to land exactly on the finer half-precision grid for every texel in this fixture. Far better than the epsilon bound required.

**Full-chain (decode → T9 bake → render)**: new ctest `sample_08_gltf_viewer_exr_env_headless` (`samples/08_gltf_viewer/CMakeLists.txt`) runs the SAME headless binary/SAME `gateOneFrame()`/`loaded_scene.png` D17 comparison `sample_08_gltf_viewer_headless` already runs, pointed at `gate_test_env.exr` via the pre-existing `--env <path>` flag (zero new sample logic — container format is detected by magic number inside `decodeTextureForUpload()`, never by this flag's own file extension). Passed on both drivers (see per-driver counts below) — the EXR input path reproduces the identical rendered result the `.hdr` default already gates on, through the real bake chain, not a separate/looser check.

**Rejection tests** (3 TEST_CASEs, `exr_deep_rejected.exr`/`exr_tiled_rejected.exr`/`exr_dwaa_rejected.exr`): each asserts `decodeExrImage()` returns `std::nullopt` AND the failure-reason string contains both the named variant (`"deep"`/`"tiled"`/`"DWAA"`) and the literal substring `"supported envelope"`. A fourth TEST_CASE confirms the SAME DWAA fixture routed through the full `decodeTextureForUpload()` dispatch produces `Outcome::Failed` (never `Checkerboard`, never a silent success) with the actionable reason intact.

**Revert-discrimination** (performed live during verification, not a committed test — the brief's own "then restore green" instruction): temporarily added a one-line R/B channel swap immediately after `decodeExrImage()`'s `rgba32.assign(...)` call (`texture_decode.cpp`), rebuilt, reran the container-equivalence TEST_CASE:
- **Sabotaged**: 1 test case failed; **4097 of 8201 assertions failed**; `maxAbsDiff` jumped from `0.0` to **`1.5625`** (vs. the `0.01` epsilon — two orders of magnitude over, correctly discriminated as a real corruption, not quantization noise).
- **Reverted**: rebuilt, reran the full device-free suite: **69/69 test cases, 9041/9041 assertions, green.**

**Byte-source invariant**: `tools/check_byte_source_invariant.sh` — green, both before and after this task's changes (`decodeExrImage`/`looksLikeExr` add zero direct filesystem I/O to `texture_decode.cpp`; `tools/gen_exr_env_fixtures/main.cpp`, the fixture generator, is a separate host-only tool, outside the two grep-protected files, and legitimately uses `std::ifstream`/`std::ofstream` for its own one-off regeneration job).

## Test results, driver-labeled

Full serial `ctest` (no `-j`), NICE'd (`nice -n19`), from a fully rebuilt `build/linux-native`:

| Driver | Command | Result | Wall time |
|---|---|---|---|
| **Real NVIDIA** (GeForce RTX 2080, driver 580.82.07) | `nice -n19 ctest --test-dir build/linux-native --output-on-failure` | **34/34 tests passed**, 0 failed | 182.77 s |
| **Lavapipe** (Mesa 25.1.5, `/usr/share/vulkan/icd.d/lvp_icd.json`, under `xvfb-run`) | `VK_ICD_FILENAMES=<lvp_icd.json> xvfb-run -a nice -n19 ctest --test-dir build/linux-native --output-on-failure` | **34/34 tests passed**, 0 failed | 98.02 s |

Both runs include `sample_08_gltf_viewer_exr_env_headless` (new) passing alongside the pre-existing `sample_08_gltf_viewer_headless`/`sample_08_gltf_viewer_quit_during_load`, and the 9 new EXR-specific device-free TEST_CASEs inside `rx_asset_gltf_tests` (`looksLikeExr`, container-equivalence, 3× rejection, corrupt/truncated, empty span, `decodeTextureForUpload` EXR-Ready, `decodeTextureForUpload` EXR-Failed/DWAA). Zero unfiltered validation errors on either driver (every sample/GPU-test binary's own `CHECK_FALSE(context.hasValidationErrors())` — the four pre-existing, documented known-false-positive guards in `context.cpp` are unchanged by this task and remain the only suppressed messages).

**Wine-tier** (`windows-cross-zig` preset, cross-compiled via zig, run under Wine): touched code (`rx_asset`) is IN scope for CI's own filtered subset (not matched by CI's own exclusion regex `rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_material_brdf_gpu|rx_debug_ui_gpu|rx_frame_loop_gpu|rx_ibl_gpu|sample`). Ran the identical CI-filtered command:

```
ctest --test-dir build/windows-cross-zig -E 'rx_rhi_vk|rx_graph_gpu|rx_material_gpu|rx_material_brdf_gpu|rx_debug_ui_gpu|rx_frame_loop_gpu|rx_ibl_gpu|sample' --output-on-failure
```

**14/14 tests passed**, 0 failed, 131.11 s — including `rx_asset_tests` (14.63 s) and `rx_asset_gltf_tests` (0.08 s, the binary hosting this task's new device-free EXR TEST_CASEs).

## Ambiguities encountered and resolutions taken

1. **Tiled EXR**: brief text said "tiled if unsupported" may be rejected; tinyexr's convenience loader turns out to partially support it. Resolved by rejecting anyway (see "Design notes" above) — the ticket's stated baseline ("scanline is the bar") plus the inability to generate a rigorously value-verified tiled fixture (no public tiled-write API in this tinyexr build) made rejection the safer, still-honest call. Not load-bearing enough to block on.
2. **Brief's license claim** ("zlib-licensed"): incorrect per direct verification (BSD-3-Clause). Corrected in this report and in `third_party/CMakeLists.txt`'s own vendoring comment; does not change the library choice (both are permissive, no-attribution-required licenses).
3. **Brief's "single-file" framing**: no longer accurate for the pinned release (a small 3-header + 1 vendored-C-file surface, not a literal single header). Documented honestly rather than silently treating it as single-file; does not change the vendoring pattern used (still "no CMakeLists.txt, fetch source, compile exactly what's needed into the consumer").
4. **Full-chain test shape**: the brief's acceptance text ("decode → T9 bake → render... must match the .hdr-driven render within the same epsilon reasoning") was read as calling for a genuine pixel-rendered comparison, not merely a bake-chain numeric check — resolved by adding a new ctest entry that reuses sample 08's existing `--env`/D17-gate machinery unmodified (zero new C++ logic, one new `add_test()` line), rather than inventing a separate GPU unit test with its own looser tolerance.

## Files touched

- `third_party/CMakeLists.txt` — tinyexr vendoring block.
- `src/rx_asset/CMakeLists.txt` — `tinyexr_impl.cpp` + `deps/miniz/miniz.c` sources, include dirs.
- `src/rx_asset/tinyexr_impl.cpp` — new, the sole `TINYEXR_IMPLEMENTATION` TU.
- `src/rx_asset/include/rx_asset/texture_decode.h` — `looksLikeExr`, `decodeExrImage` declarations + EXR envelope documentation; updated dispatch-order comment.
- `src/rx_asset/texture_decode.cpp` — `looksLikeExr`, `decodeExrImage`, `finalizeHdrFloatUpload` (refactored shared tail), `decodeExrForUpload`; updated `decodeTextureForUpload()` dispatch.
- `src/rx_asset/tests/texture_decode_test.cpp` — `readRepoFile()` helper + 9 new EXR TEST_CASEs.
- `tools/gen_exr_env_fixtures/{main.cpp,CMakeLists.txt}` — new host-only fixture generator.
- `CMakeLists.txt` — `add_subdirectory(tools/gen_exr_env_fixtures)`.
- `samples/08_gltf_viewer/CMakeLists.txt` — new `sample_08_gltf_viewer_exr_env_headless` ctest entry.
- `samples/08_gltf_viewer/main.cpp` — comment-only clarification that `--env` accepts `.exr` (no behavior change).
- `assets/test/ASSET-NOTES.md` — provenance note for the new fixtures.
- New committed binary fixtures: `assets/test/textures/gate_test_env.exr`, `assets/test/textures/exr_{deep,tiled,dwaa}_rejected.exr`, `samples/08_gltf_viewer/environments/gate_test_env.exr`.
