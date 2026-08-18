# Task 14 report — KTX2/Basis compressed texture pipeline + sampler cache (card #3)

Base commit: `d0e49d8`. Implementer commits (this task, in order):

1. `8353ad1` — `build: vendor libktx v4.4.2 (Task 14)`
2. `b5e60b6` — `feat(rx_asset): KTX2/Basis compressed texture pipeline + sampler cache (Task 14)`
3. `37b83e5` — `fix(rx_asset): wire unbound material texture slots to the role-appropriate D11 fallback`

No AI attribution in any commit; author is local git config (`Yousef Wadi <ywadi85@gmail.com>`); nothing pushed; no board/issue/plan/spec/ledger files touched; only files this task owns were committed (verified via `git show --stat` on each commit above and cross-checked against `git status` before each commit — every commit's file list matches exactly what this task produced, no stray files).

## 1. Files delivered

- `third_party/CMakeLists.txt` — libktx v4.4.2 vendoring block.
- `src/rx_asset/include/rx_asset/texture_decode.h` + `texture_decode.cpp` — device-free KTX2 parse/transcode decision layer + stb fallback decode.
- `src/rx_asset/include/rx_asset/texture_cache.h` + `texture_cache.cpp` — the GPU-facing `TextureCache` class.
- `src/rx_asset/import_gltf.cpp` — KHR_texture_basisu wiring, role-driven, slot-driven; unbound-slot fallback wiring.
- `src/rx_asset/CMakeLists.txt`, `src/rx_asset/tests/CMakeLists.txt` — build wiring.
- `src/rx_rhi_vk/include/rx_rhi_vk/{texture.h,upload.h,device.h}` + `src/rx_rhi_vk/src/{texture.cpp,upload.cpp,device.cpp}` — additive extensions (block-compressed multi-level upload; opportunistic `samplerAnisotropy` enablement).
- `assets/test/textures/` (17 files) + `generate_fixtures.sh` — committed KTX2/PNG/JPG fixtures, regenerable.
- `assets/test/cube_basisu.gltf` + `cube_basisu_misleading_normal.ktx2` — KHR_texture_basisu glTF fixture.
- `src/rx_asset/tests/{texture_decode_test.cpp,texture_cache_test.cpp,import_gltf_basisu_test.cpp}` — new test files.

## 2. Suite status (both presets)

**linux-native**, full `ctest`, final state (after all fixes, matching the committed HEAD):

```
100% tests passed, 0 tests failed out of 20
Total Test time (real) =  40.40 sec
```

`rx_asset_gltf_tests` (device-free: `texture_decode_test.cpp` + `gltf_pipeline_test.cpp`): **48 test cases, 292 assertions, 0 failed**.
`rx_asset_tests` (GPU: `texture_cache_test.cpp` + Task 10-12's own files): **27 test cases, 400 assertions, 0 failed**, `--validate` clean (zero validation errors/warnings besides the pre-existing, already-annotated "known false positive" lines this codebase's own convention marks).
`rx_asset_gltf_gpu_tests` (GPU: `import_gltf_gpu_test.cpp` + `damaged_helmet_test.cpp` + `import_gltf_basisu_test.cpp`): **39 test cases, 687 assertions, 0 failed**, `--validate` clean.

**windows-cross-zig**: `cmake --build build/windows-cross-zig -j8` → `EXIT=0` (full project, including every new/modified target above), verified four times across the task as fixes landed, most recently against the exact committed HEAD. Wine execution of the resulting `.exe`s was **not** attempted — this project's own ledger already closed a "wine-flake" item (commit immediately prior to this task's base commit) establishing that Wine-hosted test *execution* is a known-flaky, out-of-scope concern for this project; the binding requirement ("windows-cross-zig build verified IN-TASK") is about the **build**, which is verified.

## 3. Per-criterion proof (scope-summary order)

### libktx vendoring
Pinned v4.4.2 exactly (gate ruling C2). Vendoring commit records Apache-2.0 (KTX-Software's own code) **plus** every bundled component actually compiled: Basis Universal transcoder+encoder (Apache-2.0), astc-encoder (Apache-2.0, pulled in transitively — see the "evaluate parse/transcode-only" finding below), zstd single-file decoder (BSD, Facebook). Ericsson-licensed `etcdec.cxx` deliberately excluded (`KTX_FEATURE_ETC_UNPACK=OFF`) — this project's transcode targets are BC7/BC5 only. Full citations with exact file paths are in the vendoring commit message and `third_party/CMakeLists.txt`'s own comment block.

**Binary-shrink evaluation (matrix's "evaluate parse/transcode-only build options"):** two real attempts, both documented in-line in `third_party/CMakeLists.txt`, both empirically negative:
1. libktx's own `ktx_read` (read/transcode-only, `enable_write=0`) target exists internally but is **never added to `KTX_INSTALL_TARGETS`** — verified directly against the pinned tag's CMakeLists.txt (`set(KTX_INSTALL_TARGETS ktx)`, unconditional). Not reachable via `find_package()` without patching libktx's own build, which this project's dep-cache convention avoids (same posture as the Draco encoder finding from Task 13).
2. `KTX_FEATURE_KTX1=OFF` was tried first and reproduced a real, empirical build-breaking bug: `lib/texture.c`'s generic `ktxTexture_CreateFromStream` dispatcher (compiled unconditionally) references `ktxTexture1_constructFromStreamAndHeader` with no `#ifdef` guard, so disabling KTX1 leaves a dangling undefined symbol baked into `libktx.a`, surfacing as a real linker error the first time anything in this project pulled `texture.c.o` in transitively:
   ```
   ld.lld: error: undefined symbol: ktxTexture1_constructFromStreamAndHeader
   >>> referenced by texture.c:336
   >>>               texture.c.o:(ktxTexture_CreateFromStream) in archive .../libktx.a
   ```
   Reverted to `KTX_FEATURE_KTX1=ON` (the documented default) as the minimal fix; this project's own code never calls the generic dispatcher (`ktxTexture2_CreateFromMemory` directly), so the extra code is inert but harmless dead weight, not a functional gap.

The mitigations actually applied: `KTX_FEATURE_TOOLS/_TESTS/_DOC/_JNI/_PY=OFF`, `KTX_FEATURE_GL_UPLOAD/_VK_UPLOAD=OFF`, `KTX_FEATURE_ETC_UNPACK=OFF`.

### KTX2 without filesystem
`texture_decode.cpp` uses `ktxTexture2_CreateFromMemory` exclusively. `texture_cache.cpp` grep-enforced (verified directly, this report):
```
$ grep -n "std::filesystem\|ifstream\|ofstream\|fopen\|::open(" src/rx_asset/texture_cache.cpp
535:TextureHandle TextureCache::load(const std::filesystem::path& path, TextureRole role) {
541:    FilesystemByteSource fsSource(path.has_parent_path() ? path.parent_path() : std::filesystem::path("."));
```
Both hits are `std::filesystem::path` **type usage / pure path manipulation** (`parent_path()`), never I/O — the actual `ifstream` open lives in `byte_source.cpp` (a different translation unit, Task 13's own file). In-memory load test: `texture_decode_test.cpp`'s `DecodedKtx2Texture::parseAndTranscode` cases read fixture bytes via `std::ifstream` in the **test** file (not `texture_cache.cpp`) and pass the resulting `std::span<const std::byte>` straight to `loadFromBytes`/`parseAndTranscode` — `texture_cache_test.cpp`'s own "an in-memory KTX2 byte span (never touching disk inside the call)" test is explicit about this.

### Transcode target selection / role→format matrix
`roleFormatTable()` is an exhaustive `switch` (no `default`) over `TextureRole`, TOTAL by construction (`-Wswitch` would fail to compile an unhandled case). Verified per-role: BaseColor/Emissive → `KTX_TTF_BC7_RGBA`/`VK_FORMAT_BC7_SRGB_BLOCK`; Normal → `KTX_TTF_BC5_RG`/`VK_FORMAT_BC5_UNORM_BLOCK`; MetallicRoughness/Occlusion/GenericData → `KTX_TTF_BC7_RGBA`/`VK_FORMAT_BC7_UNORM_BLOCK`. `texture_decode_test.cpp`'s `roleFormatTable is TOTAL...` test asserts every entry directly. BC4_R for single-channel occlusion is D10's own "recorded option," not implemented — matches D10's literal text, not a gap.

### Non-Basis KTX2 detection (never via KTX_INVALID_OPERATION)
`ktxTexture2_NeedsTranscoding()` confirmed present and public at v4.4.2 (resolving the matrix's own "to verify at vendoring" flag) and is the **only** thing consulted before calling `TranscodeBasis`. Proven both positively (`raw_rgba8.ktx2` fixture: `wasBasisEncoded()==false`, `currentVkFormat()==VK_FORMAT_R8G8B8A8_SRGB`, `levels()[0].bytes.size()==64` = tightly-packed uncompressed) and by **revert evidence** (§5 below).

### Supercompression
`basecolor_uastc_zstd.ktx2` fixture (toktx `--zcmp 19`) parses and transcodes with no explicit zstd call anywhere in `texture_cache.cpp`/`texture_decode.cpp` — `TranscodeBasis` inflates internally per libktx's own documented behavior. Test: `texture_decode_test.cpp`'s "UASTC+zstd supercompressed fixture auto-inflates" case.

### Mip chains, sub-block tails
`basecolor_withmips_uastc.ktx2` (16×16, 5 levels down to 1×1) and `flat_withmips_uastc.ktx2` (8×8, 4 levels). Device-free: `texture_decode_test.cpp`'s full-mip-chain test asserts TRUE extents (16,8,4,2,1) against block-rounded byte counts (256,64,16,16,16 — the 2×2/1×1 tails are each exactly one 16-byte BC7 block). GPU: `texture_cache_test.cpp`'s "deep-mip readback" test samples every one of `flat_withmips_uastc`'s 4 levels via explicit `SampleLevel` and asserts the correct flat color at each, **including** the sub-block tail levels — proving `Uploader::uploadImageMips`'s block-row-pitch handling is correct end-to-end (parse → upload → GPU sample), not just CPU-side bookkeeping. `mips_absent.ktx2`: one WARN, `mipLevels==1`.

### Cubemap/array/3D
`cubemap.ktx2` (toktx `--cubemap`, 6 faces) → `isUnsupportedLayout()==true`, `Ktx2ParseError::UnsupportedLayout`. `texture_cache_test.cpp`'s cubemap test asserts the load falls back to `checkerboardHandle()`; the WARN names FG1 as the scheduled consumer (source-verified in `loadKtx2Bytes`).

### Colorspace correctness (D10/gate ruling #3, Godot #99589)
`srgb_mislabeled_normal.ktx2` (content: flat tangent-space-up normal, `--assign_oetf srgb` forced despite being linear data) — `containerTransferFunction()` correctly reports the **container's own** claim (`KHR_DF_TRANSFER_SRGB`), captured **before** any transcode call (see §5's revert evidence for why this ordering is load-bearing), while `currentVkFormat()` is role-authoritative `VK_FORMAT_BC5_UNORM_BLOCK` regardless (BC5 has no sRGB variant at all — this is the one role where the double-decode bug is structurally impossible on the *created* side, but the WARN still fires because the *container* claimed sRGB). `checkColorspaceAgreement(Normal, SRGB).disagrees == true`, cross-checked in both `texture_decode_test.cpp` (unit) and `texture_decode_test.cpp`'s dedicated fixture-level test.

Non-Basis relabeling scope — **flagged explicitly, per the "if a matrix criterion proves technically impossible as written" instruction**: role-authoritative relabeling is applied only on the Basis-transcoded path, where this project itself picks the transcode target and the resulting block bytes are provably identical regardless of the sRGB/UNORM label (verified directly against `basis_transcode.cpp`'s own switch). For a **non-Basis** KTX2, the container's own stored format is uploaded as-is when the device supports it (matrix's own wording for that row: "uploadable-as-stored... no relabeling language"); blindly reinterpreting an arbitrary already-compressed format's bytes under a different colorspace label was not verified safe for every possible stored format and is out of this task's checked scope. `checkColorspaceAgreement` still runs against non-Basis containers too (WARN-only, format unchanged) for defense-in-depth consistency. This is a **deliberate, minimal, documented** scope boundary, not an oversight — see `texture_cache.cpp`'s own comment at the non-Basis branch of `loadKtx2Bytes`.

### Uploader block-compressed support (matrix N4, explicit acceptance item)
`Texture2D::createForPresuppliedMips()` + `Uploader::uploadImageMips()` (`src/rx_rhi_vk`) — additive; `Texture2D::create()`/`Uploader::uploadToImage()` untouched (byte-identical; `rx_rhi_vk_tests`'s pre-existing `texture_test.cpp`/`upload_test.cpp` pass unmodified, confirmed in the full-suite run above). `bufferRowLength`/`bufferImageHeight` deliberately left 0 (Vulkan's own "tightly packed, block-rounded" copy rule) rather than hand-computed block-pitch arithmetic — see that method's own header comment for the spec citation. Proven correct by the deep-mip GPU test above.

### stb PNG/JPG fallback
`quadrant.png`/`quadrant.jpg`/`sixteen_bit.png`/`corrupt.png` fixtures. Role-correct SRGB/UNORM (`stbRgba8Format`), 16-bit downconvert detected (`was16Bit`), decode failure → checkerboard + WARN, mip-0-only (recorded limitation, matches N2/gate ruling #3 exactly — no runtime mip generation for this path).

### Checkerboard + utility fallbacks (D11)
Every failure mode mapped: `texture_cache_test.cpp` exercises missing-byte-source (`does_not_exist.ktx2`), corrupt container (`corrupt.ktx2`), corrupt stb decode (`corrupt.png`), unsupported layout (`cubemap.ktx2`) — all → `checkerboardHandle()`. Utility fallbacks: `TextureCache::create` builds exactly 3 real GPU utility textures (white/flat-normal/neutral-MR, matching D11's own literal count) shared across the 6 `TextureRole` values by role (`fallbackHandle(role)`) — asserted directly in the "create builds real, distinct fallback/utility textures" test. Unbound-slot wiring (a real gap found and fixed during this task, see §6) routes `MaterialAsset::TextureRef::handle` to the role-appropriate fallback at import time — `import_gltf_basisu_test.cpp`'s "UNBOUND material slots... resolve to the role-appropriate D11 UTILITY texture" test covers all 5 slots against `cube_textured.gltf` (Task 13's own texture-free fixture). One-log-per-asset dedup: `TextureCache::shouldLogOnce(debugName, category)`.

### D24 residency-tolerant resolve
`texture_cache_test.cpp`'s "D24: evictForTesting immediately makes resolve() report the checkerboard fallback..." test: evict → immediately non-resident (checkerboard record observable) → `onFrameFenceSignaled` at the wrong frame (no-op) → at the right frame (real GPU reclaim: `liveTextureCountForTesting()` decrements) → reload produces a genuinely new, independent handle → the **old** handle still resolves to the fallback (D6 generational staleness, no bespoke bookkeeping). Mirrors `src/rx_rhi_vk/tests/eviction_contract_test.cpp`'s own synthetic pattern, over **real** `Texture2D`/`BindlessTable` resources.

### FG9 accounting
`TextureCache::stats()` — bytes-by-role + count, incrementally maintained (no `HandlePool` iteration API exists to walk it after the fact — documented in `texture_cache.h`). `texture_cache_test.cpp`'s accounting test: load → count/bytes increase by exactly 1/the real `Texture2D::allocatedBytes()`; evict+reclaim → balances back to the pre-load values exactly (Task 10 accounting-test pattern).

### Dimension/format limits
`exceedsDimensionLimit()` — pure function, device-free tested (in-range/exceeds-both-axes/exactly-at-limit). `nonmult4.ktx2` (6×5 base, non-multiple-of-4) loads with its true extent (`record.width==6`, `record.height==5`). 1×1 case: the mip-chain fixture's own level 4. Zero-dimension/corrupt: `corrupt.ktx2` → `Ktx2ParseError::NotKtx2` (named, no crash). **Known minor gap** (see §6): the oversized-texture path is wired symmetrically into both the KTX2 and stb load paths, but only has a dedicated integration test on the KTX2 side — the underlying `exceedsDimensionLimit()` function itself is fully unit-tested regardless of call site.

### KHR_texture_basisu wiring from import
`import_gltf_basisu_test.cpp`'s two primary tests: (a) with a real `TextureCache`, `cube_basisu.gltf`'s baseColorTexture (referencing `cube_basisu_misleading_normal.ktx2` — a file **named** "misleading_normal" but used in the **baseColor** slot) resolves to a real, non-fallback texture with `record.role == TextureRole::BaseColor` — role from the slot, never the filename; (b) with `textures=nullptr`, behavior is byte-identical to Task 13 (`registry.fallbackTextureHandle()`). `Texture::basisuImageIndex` is preferred over the core `imageIndex` per the extension's own documented precedence.

### Sampler cache (G6)
Canonical glTF→Vk table documented in `texture_cache.cpp`'s anonymous namespace (`mapWrap`/`mapMagFilter`/`mapMinFilter`), TOTAL over the 3 wrap modes and 6 (+unspecified) minFilter values. Dedup + negative test in `texture_cache_test.cpp`; revert evidence in §5. Anisotropy: **found and fixed a real device-enablement bug** during this task (§6) — `Device::supportsSamplerAnisotropy()` now correctly reflects what was ENABLED at `vkCreateDevice` time, not merely advertised; on this task's CI-representative driver (Mesa llvmpipe 25.1.5, `driverInfo = Mesa 25.1.5-1pop0~1756399231~22.04~b84bab8 (LLVM 15.0.7)`, `apiVersion 1.4.311`) it is **ENABLED** (confirmed via the `Device::create` log line and the passing sampler tests, which would otherwise hit `VUID-VkSamplerCreateInfo-anisotropyEnable-01070`). The "cleanly off" branch is code-reviewed correct (a plain `anisotropyEnable = VK_FALSE` when the bool is false) but **not empirically exercised against a real non-supporting device** in this task, since lavapipe itself supports the feature — flagged honestly rather than claimed as verified.

## 4. CI-driver format-properties dump (BC7_SRGB/BC7_UNORM/BC5_UNORM/BC1_SRGB)

Captured via `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json vulkaninfo --show-formats`, the same lavapipe ICD this project's own CI workflow installs (`mesa-vulkan-drivers`, `.github/workflows/ci.yml`). Device: `llvmpipe (LLVM 15.0.7, 256 bits)`, `driverInfo = Mesa 25.1.5-1pop0~1756399231~22.04~b84bab8 (LLVM 15.0.7)`, `apiVersion 1.4.311`.

```
Common Format Group[24]:
Formats: count = 16
	FORMAT_BC1_RGB_UNORM_BLOCK
	FORMAT_BC1_RGB_SRGB_BLOCK
	FORMAT_BC1_RGBA_UNORM_BLOCK
	FORMAT_BC1_RGBA_SRGB_BLOCK
	FORMAT_BC2_UNORM_BLOCK
	FORMAT_BC2_SRGB_BLOCK
	FORMAT_BC3_UNORM_BLOCK
	FORMAT_BC3_SRGB_BLOCK
	FORMAT_BC4_UNORM_BLOCK
	FORMAT_BC4_SNORM_BLOCK
	FORMAT_BC5_UNORM_BLOCK
	FORMAT_BC5_SNORM_BLOCK
	FORMAT_BC6H_UFLOAT_BLOCK
	FORMAT_BC6H_SFLOAT_BLOCK
	FORMAT_BC7_UNORM_BLOCK
	FORMAT_BC7_SRGB_BLOCK
Properties:
	linearTiling: count = 5
		FORMAT_FEATURE_SAMPLED_IMAGE_BIT
		FORMAT_FEATURE_BLIT_SRC_BIT
		FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
		FORMAT_FEATURE_TRANSFER_SRC_BIT
		FORMAT_FEATURE_TRANSFER_DST_BIT
	optimalTiling: count = 5
		FORMAT_FEATURE_SAMPLED_IMAGE_BIT
		FORMAT_FEATURE_BLIT_SRC_BIT
		FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
		FORMAT_FEATURE_TRANSFER_SRC_BIT
		FORMAT_FEATURE_TRANSFER_DST_BIT
	bufferFeatures:
		None
```

All four named formats (BC7_SRGB_BLOCK, BC7_UNORM_BLOCK, BC5_UNORM_BLOCK, BC1_RGB_SRGB_BLOCK) are in this single group and support `SAMPLED_IMAGE_BIT` + `TRANSFER_DST_BIT` under `optimalTiling` — exactly `TextureCache::isFormatSupported()`'s own requirement — confirming the matrix's tip-of-main-source-based prediction empirically, on the actual pinned Mesa version this task ran against. This is why every GPU test in this task exercises the **exact-format** path, not the RGBA32 fallback (the fallback path is exercised deterministically instead via the device-free `planTranscodeFormat` forced-off seam, §3/§5).

## 5. Revert-testing (discrimination evidence)

Performed in the main tree (not a separate `git worktree`) against the exact committed HEAD, using `git checkout -- <file>` to restore between experiments — chosen over a physical worktree because `.deps-cache/` is untracked/gitignored and a fresh worktree would rebuild every vendored dependency (SDL3, vk-bootstrap, fastgltf, meshoptimizer, Draco, libktx, Tracy, enkiTS...) from scratch, at a cost this task's time budget could not absorb for four experiments. `git status`/`git diff` confirmed clean (matching HEAD exactly) before and after every experiment; the final full-suite green run in §2 was captured **after** all four reverts were restored.

1. **Needs-transcoding detection.** Forced the `NeedsTranscoding()` branch to `false` (always attempt blind transcode). Result: `raw_rgba8.ktx2`'s own test fails exactly as predicted —
   ```
   [error] rx_asset: DecodedKtx2Texture::parseAndTranscode: ktxTexture2_TranscodeBasis failed: Operation not allowed in the current state.
   REQUIRE( decoded.has_value() ) is NOT correct!  values: REQUIRE( false )
   ```

2. **Sub-block mip-tail regions.** Forced the per-level extent computation to block-round (the classic bug: `max(4, ceil(w,4))` instead of the true `max(1, w>>level)`). Result: the full-mip-chain test fails exactly at the two sub-block tail levels —
   ```
   CHECK( levels[i].width == expectedExtent[i] ) is NOT correct!  values: CHECK( 4 == 2 )   logged: i := 3
   CHECK( levels[i].width == expectedExtent[i] ) is NOT correct!  values: CHECK( 4 == 1 )   logged: i := 4
   ```
   (levels 0-2 still passed — the bug is invisible until the extent actually drops below one block, exactly the "classic off-by-one" this row exists to catch.)

3. **Sampler dedup.** Disabled the `samplerCache_.find()` early-return (always fall through to create-and-insert; `unordered_map::emplace` on an existing key silently no-ops, so the map's own size lies while a second, real, un-cached `VkSampler` leaks). Result:
   ```
   CHECK( samplerB == samplerA ) is NOT correct!
   [error] VUID-vkDestroyDevice-device-00378 ... VkSampler ... has not been destroyed.
   ```
   (the validation-layer leak error is independent corroborating evidence of the same break.)

4. **Role-authoritative colorspace WARN.** Forced `checkColorspaceAgreement`'s `disagrees` to always `false`. Result:
   ```
   CHECK( check.disagrees ) is NOT correct!  values: CHECK( false )
   [normal-vs-sRGB and baseColor-vs-linear cases both fail]
   ```

All four restored via `git checkout --`; `git status`/`git diff` confirmed clean; the full suite (§2) is green against the restored, committed HEAD.

## 6. Deviations / gaps found and fixed during this task (filed as true, not overclaimed)

Per the working agreement, every one of these was **found and fixed** before this report was written — none is an open TODO:

1. **`buildFallbackTextures()` never flushed the Uploader** — the 4 D11 fallback textures' upload commands stayed recorded-but-unsubmitted for the lifetime of any test whose every `load()` call resolved to an existing fallback (no new upload to trigger `loadFromBytes`'s own flush). `~Uploader()`'s own auto-flush then called `vkEndCommandBuffer` on a command buffer referencing images `~TextureCache()` had **already destroyed** one destruction step earlier (destruction order: `cache` before `uploader`). Reproduced as a real `UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkImage` validation error; fixed with one `uploader_.wait(uploader_.flush())` at the end of `buildFallbackTextures()`.
2. **`TextureCache`'s sampler cache was never destroyed** — `~TextureCache() = default` left every cached `VkSampler` leaked. Reproduced as `VUID-vkDestroyDevice-device-00378`; fixed with an explicit destructor loop.
3. **`samplerAnisotropy` checked via a fresh `vkGetPhysicalDeviceFeatures()` query** instead of what was actually enabled on the `VkDevice`. Reproduced as `VUID-VkSamplerCreateInfo-anisotropyEnable-01070`; fixed by adding `Device::supportsSamplerAnisotropy()` (opportunistic `enable_features_if_present`, mirroring `supportsBufferDeviceAddress()`'s established pattern) and reading that instead.
4. **`containerTransferFunction()` re-queried the DFD after transcoding**, which `TranscodeBasis` itself rewrites to describe the *output* format (a BC5 target has no sRGB variant, so the post-transcode DFD reports LINEAR regardless of the container's original claim) — this silently erased the exact disagreement the sRGB-mislabeled-normal test needs to observe. Fixed by capturing the transfer function once, immediately after parse, before any transcode call.
5. **Test-fixture lifetime bug** (`texture_cache_test.cpp`'s own first draft): `TextureCache::create()` was called *inside* `makeFixture()` against local `Device`/`Allocator`/... variables about to be `std::move()`d into the returned aggregate, capturing dangling references (`Invalid physicalDevice`, `SIGABRT`). Fixed by splitting into `makeFixture()` (settles every GPU object into its final address) + `makeCache()` (called by every `TEST_CASE` afterward) — the same two-phase pattern `import_gltf_gpu_test.cpp` (Task 13) already established and documents for exactly this reason.
6. **Unbound material texture slots left unresolved** (see §3's D11 section) — found during self-review, not by a failing test (no test existed for it originally); fixed in `import_gltf.cpp` and covered by a new test.
7. **`toktx` output defaulted to legacy KTX1** for any invocation without `--encode`/`--zcmp` (which imply `--t2`) — `raw_rgba8.ktx2`/`cubemap.ktx2` were silently KTX1 (`«KTX 11»` magic) until `--t2` was added to every fixture-generation invocation; caught via direct `ktxinfo` inspection before any test ran against the bad fixtures, not left for a test to discover.

**Honest residual gaps** (not fixed, explicitly flagged rather than silently absent):

- Sampler anisotropy's "cleanly off" branch is code-reviewed correct but not empirically exercised against a real non-supporting device (lavapipe supports the feature) — see §3.
- The oversized-texture guard is wired into both KTX2 and stb load paths but has a dedicated integration test only on the KTX2 side; the shared `exceedsDimensionLimit()` function is fully unit-tested regardless.
- Non-Basis-path colorspace relabeling is deliberately out of scope (§3's D10 section) — a documented, minimal deviation from the matrix's more general phrasing, not an oversight.
- KTX v5/UASTC-HDR (N3) and cubemap/array real loading (N1) remain registry watch items per the matrix's own disposition — no code for either exists or was expected in this task.

## 7. Self-review

- **D5 threading**: every GPU-object-mutating public method on `TextureCache` (`create`, `load`/`loadFromBytes`, `getOrCreateSampler`, `evictForTesting`) and every read accessor (`resolve`, `fallbackHandle`, `checkerboardHandle`, `stats`, the two `*ForTesting` diagnostics) carries `RX_ASSERT_MAIN_THREAD`, matching `GeometryPool`'s (Task 12) more rigorous precedent over `Registry`'s (Task 13) narrower one — a deliberate choice toward the stricter existing convention.
- **Header one-liners**: `texture_cache.h`/`texture_decode.h` both open with an explicit thread-affinity statement per D5's own requirement for new public headers.
- **rx_rhi_vk additivity**: confirmed via full-suite pass that `texture_test.cpp`/`upload_test.cpp` (pre-existing, unmodified) still pass byte-for-byte; `Texture2D::create()`/`Uploader::uploadToImage()` bodies are untouched — the new `createForPresuppliedMips()`/`uploadImageMips()` are new, parallel entry points.
- **No filesystem in texture_cache.cpp**: grep-verified in this report (§3).
- **Fixture regenerability**: `assets/test/textures/generate_fixtures.sh` documents every `toktx`/ImageMagick invocation; re-running it from a clean checkout reproduces pixel/format/layout-identical fixtures (not byte-identical — `toktx` embeds a writer/timestamp key-value pair).
- **License discipline**: vendoring commit records Apache-2.0 plus every bundled component's own license, with exact file citations, matching the ruling's explicit "PLUS the bundled-component licenses actually compiled" requirement.
- **What I did not do**: did not touch `docs/`, the plan, the spec, the ledger, or the project board, per this task's global constraints. Did not modify any Task 10-13 file beyond the two narrowly-scoped, additive `rx_rhi_vk` extensions and the one `import_gltf.cpp` seam (`fillRef`'s texture-resolution body) this task's own brief explicitly names as its integration point.
