# Matrix — Issue #42: Cubemap/array KTX2 loading + HDR image input (FG1 rider)

**Plan task:** Task 6 (`docs/superpowers/plans/2026-08-20-phase5-techniques.md:286-308`), Stage 0.
**Spec/registry decisions binding this ticket:** FG1 (environment lighting —
this is the TextureCache-side prerequisite the skybox/IBL consumer at
Stage 1 Task 10 needs; registry text: `docs/superpowers/specs/2026-08-09-
toolchain-platform-rhi-design.md:194-196`), D10 (role→format table, Phase 4,
KTX2 role-authoritative format selection — this ticket's cube/HDR work
extends the SAME table's discipline, never bypasses it), D11
(fallback/checkerboard philosophy — the non-cube regression path must stay
byte-identical to D11's existing contract), the master registry's "Cubemap/
array KTX2 loading" line (`...design.md:320-322`, "Phase 4 ships the log
path only") and "libktx v5 / UASTC-HDR" watch-item line (`:323-324`,
confirms the v4.4.2 pin stands and v5/UASTC-HDR stays out of scope for this
ticket).

**Sources consulted (2026-08-20):**
- `src/rx_asset/texture_decode.cpp` (661 lines) + `include/rx_asset/
  texture_decode.h` (410 lines) — complete read.
- `src/rx_asset/include/rx_asset/texture_cache.h` (483 lines) + `src/
  rx_asset/texture_cache.cpp` (660 lines) — complete read.
- `src/rx_asset/tests/texture_decode_test.cpp:317-327`, `src/rx_asset/
  tests/texture_cache_test.cpp:759-768` — the two existing cubemap-fixture
  test cases, read in full.
- `src/rx_asset/tests/texture_cache_test.cpp:1220-1280` and `src/rx_asset/
  tests/import_gltf_basisu_test.cpp:100-186` — the
  `cube_basisu_misleading_normal.ktx2` role-mismatch tests, read to
  disambiguate fixture inventory (see matrix row 5).
- `assets/test/textures/generate_fixtures.sh:81-99,178-200` — the cubemap-
  fixture and non-Basis-raw-KTX2 generation blocks, read in full including
  comments.
- `src/rx_rhi_vk/src/texture.cpp` (372 lines, complete) and `include/
  rx_rhi_vk/upload.h:260-356` + `src/upload.cpp:190-520` (targeted,
  `uploadToImage`/`uploadImageMips`/`reserveRingSpace` bodies) — the
  image-creation/upload surface this ticket must extend.
- `src/rx_rhi_vk/include/rx_rhi_vk/bindless.h:226`, `src/bindless.cpp:250` —
  `BindlessTable::registerSampledImage(VkImageView, VkImageLayout)` takes a
  bare view handle, no view-type discrimination — verified NOT a gap (see
  row 6).
- `third_party/CMakeLists.txt:737-860` (libktx vendoring block, full read)
  — pin confirmed `set(RX_KTX_TAG "v4.4.2")` (`:856`).
- Vendored libktx header at the pinned tag,
  `.deps-cache/ktx-b1de9e977325133a/include/ktx.h:260-270,759-765` —
  `ktxTexture::isArray`/`isCubemap`/`numDimensions`/`numLayers`/`numFaces`
  fields confirmed present exactly as `texture_decode.cpp:141-143`'s
  `isUnsupportedLayoutFor()` already consumes them; `:541`
  `ktxTexture_GetImageOffset(This, level, layer, faceSlice, pOffset)`
  macro signature confirmed (the `faceSlice` parameter is the per-face
  selector `DecodedKtx2Texture::levels()` — `texture_decode.cpp:272-300`
  — does not yet pass, hardcoding `layer=0, faceSlice=0`).
- **`gh api repos/KhronosGroup/KTX-Software/releases` and `/tags`, fetched
  2026-08-20** (live GitHub API, not the Phase-4 gate's cached account):
  confirms `v4.4.2` published **2025-10-04T09:03:07Z**, `v5.0.0-rc1`
  published **2026-05-01T11:09:37Z**, `v5.0.0-rc2` published
  **2026-08-17T08:17:09Z** — both v5 tags remain prereleases as of this
  research date, no stable v5.0.0 tag exists.
- **`gh api repos/KhronosGroup/KTX-Software/releases/tags/v5.0.0-rc2`,
  fetched 2026-08-20** (release body, read in full): confirms UASTC-HDR
  is a v5.0-introduced feature — direct quotes: "Support for the Binomial
  UASTC HDR formats has been added to all supported API bindings and
  relevant tools" and "New UASTC HDR transcode targets: `KTX_TTF_RGB_HALF`
  and `KTX_TTF_RGB9E5`" (new in rc2 vs rc1) plus rc1's own "New Features in
  v5.0" section: `ktxBasisParams::codec` gains `KTX_BASIS_CODEC_UASTC_
  HDR_4x4`/`KTX_BASIS_CODEC_UASTC_HDR_6x6_INTERMEDIATE`,
  `ktxTexture2_TranscodeBasis` gains UASTC-HDR→ASTC-HDR/BC6H-Unsigned
  transcode. Cross-checked against `v4.4.2`'s and `v4.4.0`'s own release
  bodies (`gh api .../releases/tags/v4.4.2` and `/v4.4.0`, fetched
  2026-08-20): **zero** occurrences of "HDR" in either body — UASTC-HDR is
  genuinely absent from the vendored pin, not merely undocumented.
- Vendored stb_image.h, read directly at
  `build/linux-native/_deps/stb-src/stb_image.h` (the actual FetchContent
  checkout this repo's own CMake build uses, not a paraphrase):
  `stbi_loadf_from_memory`/`stbi_is_hdr_from_memory` declared `:458,479`,
  implemented `:1478,1517`; grep confirms **no** `STBI_NO_HDR` (or any
  other `STBI_ONLY_*`/`STBI_NO_*` HDR-disabling macro) anywhere in
  `src/rx_rhi_vk/src/stb_impl.cpp` (the sole `STB_IMAGE_IMPLEMENTATION`
  TU, read in full — 21 lines, defines nothing but that one macro) or
  `third_party/CMakeLists.txt` — HDR decode is already fully compiled in,
  simply uncalled.
- Grep sweep, `grep -rn "stbi_loadf\|stbi_is_hdr\|\.hdr\b" src/rx_asset/`
  and repo-wide outside vendored/build trees: zero hits — confirms
  `decodeStbImage()` (`texture_decode.cpp:306-343`) calls only the 8-bit
  `stbi_load_from_memory` entry point today; no HDR call site exists
  anywhere in this codebase pre-task.
- Grep sweep, `grep -rn "TextureRole::" src/` (non-test): confined to
  `texture_decode.{h,cpp}`, `texture_cache.{h,cpp}`, `import_gltf.cpp`,
  `import_pipeline.h` — no `rx_material` or other consumer references
  `TextureRole` at all; the enumerator's blast radius is fully contained
  inside `rx_asset` (see row 8).
- `samples/{05_multipass,07_stress,08_gltf_viewer,09_scene}/main.cpp`,
  grepped for `kHdrFormat`: all four define
  `constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;` —
  verified first-hand as the process-wide existing HDR-format convention
  (cross-referenced by, not copied from, the sibling `matrix-p5t03-hdr-
  scene-color.md`).
- **Independent second pass (2026-08-20, this addendum):** vendored
  `stb_image.h` (`build/linux-native/_deps/stb-src/stb_image.h`) read
  directly at `:1137-1177,1884` — `stbi__load_main()`'s own HDR-branch,
  not merely the public `stbi_loadf*`/`stbi_is_hdr*` declarations already
  cited above (see row 9 addendum). `.github/workflows/ci.yml` and every
  script under `tools/` grepped for `filesystem`/`fopen`
  invariant-checking logic — zero hits (see row 14 addendum). Phase 5
  plan file (`…phase5-techniques.md`) grepped for `stb_image_resize`:
  zero hits — `stb_image_resize2` is a Phase-4-era dependency
  (`src/rx_asset/stb_image_resize_impl.cpp`,
  `third_party/CMakeLists.txt:208-210`'s `RX_STB_TAG`), unrelated to
  this ticket's own scope and not named anywhere in Phase 5's own
  planning text — noted only because an earlier verification pass
  assumed otherwise. `gh api repos/KhronosGroup/KTX-Software/releases/tags/v5.0.0-rc2`'s
  body re-read for its "New Features in v5.0 / Tools" section: quotes
  "**The legacy tools have been removed**" with an explicit
  `toktx` → `ktx create` replacement-table entry (see New gaps, fifth
  bullet).

---

## The matrix

| # | Criterion | Verification method & evidence expectation | Current code state (verified, cited) | Disposition | Proposed binding acceptance criterion |
|---|-----------|----------------------------------------------|------------------------------------------|-------------|------------------------------------------|
| 1 | Cube/array KTX2 containers are currently REJECTED via a defined fallback, never silently mis-decoded as a 2D slice | Code read, not test-only | `isUnsupportedLayoutFor()` (`texture_decode.cpp:141-143`) returns true for `isArray \|\| isCubemap \|\| numFaces>1 \|\| numLayers>1 \|\| numDimensions!=2`; `parseAndTranscode()` (`:215-225`) still returns a real (inert) `DecodedKtx2Texture` with `unsupportedLayout_=true`/`outError=UnsupportedLayout`, but `levels()` (`:272-300`) returns empty for it; `decodeKtx2ForUpload()` (`:511-520`) maps this to `Outcome::Checkerboard` with a named warning; `TextureCache::applyDecodeResult()` (`texture_cache.cpp:396-398`) returns `checkerboard_` directly for `Checkerboard` outcome — never a throw, never a crash. Proven empirically by `texture_cache_test.cpp:765-767` (`CHECK(handle == fixture->cache->checkerboardHandle())`). | N/A — baseline fact, grounds row 2's discrimination requirement | (context row only) |
| 2 | Discrimination proof: the two existing "cube → checkerboard" tests must FLIP once real cube support lands | Byte-identical-regression discipline, matching the plan's "reference-vs-ground-truth… gates that bake a bug certify the bug" rule | `texture_decode_test.cpp:317-327` ("cubemap fixture is classified UnsupportedLayout, never silently treated as a 2D slice") and `texture_cache_test.cpp:759-768` ("cubemap KTX2 → checkerboard fallback") both currently assert REJECTION as correct — exactly the tests a no-op implementation could leave passing while shipping nothing. | consume-now | Both tests are DELETED (not left alongside a new positive test asserting the same fixture two contradictory ways) once cube support lands; a NEW test explicitly proves the flip by loading the SAME `cubemap.ktx2`/its mip'd successor (row 4) and asserting `handle != checkerboardHandle()` plus real per-face values (row 6) — the flip itself, not just new coverage, is the discrimination proof this row exists to require. |
| 3 | Scope of "supported": cubemap (isCubemap, numFaces==6, numLayers==1) vs. general 2D array (isArray, numLayers>1, numFaces==1) vs. cube-array (isCubemap && numLayers>1) vs. 1D/3D (numDimensions!=2) | Code review + registry/plan text search | Ticket Scope text says "cubemap/array" (both, mirroring the registry line's own title); the ticket's Key Acceptance Criteria and Files sections name ONLY cubemap (6-face + mip test); no texture-array or cube-array consumer exists anywhere in the Stage 0-4 plan text (`grep` of the plan for "texture array" outside this ticket returns nothing); FG1's real consumer (Stage 1 Task 9/10, equirect→cubemap/prefiltered specular/skybox) needs exactly ONE cubemap per environment, never an array of environments. `numDimensions!=2` (1D/3D volumes) has zero charter consumer. | needs-coordinator-decision | Land CUBEMAP support only; narrow `isUnsupportedLayoutFor()`'s predicate to `numDimensions != 2 \|\| (isArray && !isCubemap) \|\| (isCubemap && numLayers > 1)` — flat 2D-array and cube-array stay explicitly rejected (each gets its own still-rejected regression test, row 5), 1D/3D stays rejected unchanged. Recommendation in Open Questions 1: narrow to cubemap-only; do not build unused array plumbing against zero charter demand. |
| 4 | Non-cube regression: existing 2D KTX2/PNG/JPG/all 6 roles behave BYTE-IDENTICALLY after this task | Byte-identical regression run across the full existing suite | Every pre-existing `texture_decode_test.cpp`/`texture_cache_test.cpp` case not touching cube/HDR fixtures is an implicit regression gate; the ticket names this explicitly as a bar. | consume-now | Full existing `rx_asset` test suite (both files) re-run unmodified — except rows 2's two deliberately-flipped cases — and passes byte-identical; `TextureRole::Environment`'s insertion point (row 8) is verified to be APPENDED at the end of the enum, never inserted before an existing value, so `static_cast<size_t>(role)` is unchanged for all 6 pre-existing roles (a real footgun if violated: every existing `byRole[idx]`/`roleFallback_[idx]` index would silently shift). |
| 5 | Fixture-inventory disambiguation: does `assets/test/cube_basisu_misleading_normal.ktx2` (repo root, referenced by `texture_cache_test.cpp:1233+` and `import_gltf_basisu_test.cpp:137`) ALSO exercise cube-layout code, or is it unrelated lineage? | Direct test-assertion read (not filename inference — the file is named "cube_..." but that is exactly the trap this row exists to catch) | Read in full: `import_gltf_basisu_test.cpp:134-166` imports it via `cube_basisu.gltf`'s `baseColorTexture` and asserts `record.width == 4`/`record.height == 4`/`record.role == TextureRole::BaseColor`/`CHECK_FALSE(record.isFallback)` — a **successful, non-fallback, 4×4 2D load**. `texture_cache_test.cpp:1264-1273` (the combined glTF→pixel test) asserts the identical `4×4` REAL (non-`Checkerboard`) resolution. Neither test would pass if the file were cube-shaped (`isUnsupportedLayoutFor()` would force `Checkerboard`/registry fallback, contradicting `CHECK_FALSE(record.isFallback)`) — this is conclusive: **the file is a plain 2D KTX2, "cube" in its name refers only to the unrelated `cube_basisu.gltf` scene/mesh it ships with**, not to any cube-layout container. Confirmed unrelated to this ticket (its lineage is matrix-issue03/Task-14's KHR_texture_basisu role-mismatch scenario, Phase 4). | N/A — genuinely N/A to this ticket | No action needed; recorded here so the implementing task does not misread this file as a second cube fixture or accidentally touch its two existing, unrelated tests. |
| 6 | New fixture required: existing `assets/test/textures/cubemap.ktx2` is INSUFFICIENT for "6 faces + a lower mip" | Fixture provenance audit (read the generation script, not just the binary) | `generate_fixtures.sh:178-182`: `toktx --t2 --cubemap ... face_px.png ... face_nz.png`, **no `--genmipmap`**, 4×4-pixel-per-face sources, raw (non-Basis) storage, comment states it was "built... since this container is never transcoded, only classified-and-rejected" — deliberately minimal, single-level, built only to prove rejection (rows 1-2). Its six distinct flat per-face colors (`#FF0000`/`#00FFFF`/`#00FF00`/`#FF00FF`/`#0000FF`/`#FFFF00` for +X/-X/+Y/-Y/+Z/-Z) ARE the right shape for a per-face-VALUE test — just not at a size/mip-depth that also proves "a lower mip." | consume-now | `generate_fixtures.sh` grows a second cubemap fixture (e.g. `cubemap_mips.ktx2`) at a larger base size (32×32 or 64×64/face, ≥3 real mip levels) reusing the identical per-face-color convention, via a documented `toktx --t2 --cubemap --genmipmap face_px.png face_nx.png face_py.png face_ny.png face_pz.png face_nz.png` command; the ORIGINAL `cubemap.ktx2` is kept (its history is part of the gate record) and repurposed as the row-2 discrimination fixture (still a valid single-level real cube once support lands, just not the mip-depth test). |
| 7 | `Texture2D`/`Uploader::uploadImageMips` have NO array-layer concept at all — genuinely new plumbing, not "call the existing chunking 6×" | Code read, not assumption | Confirmed against `texture.cpp` (372 lines, complete): both `Texture2D::create()` (`:84-186`) and `createForPresuppliedMips()` (`:188-255`) hardcode `imageInfo.arrayLayers = 1` (`:132,212`), `viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D` (`:161,236`), no `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` anywhere in the file. `Uploader::ImageMipLevel` (`upload.h:276-281`) has fields `{data, size, mipLevel, extent}` only — no layer/face field; every `VkBufferImageCopy` region built in `uploadImageMips()` (`upload.cpp:444,501`) hardcodes `region.imageSubresource.baseArrayLayer = 0`. A 6-face upload cannot be expressed through the current API by any caller regardless of TextureCache-side changes. **Not a gap for the CHUNKING mechanism itself**: `reserveRingSpace()`/the row-group-splitting logic (`upload.cpp:39-57` region + `:365-508`) is pure byte-range copying keyed only on `bufferOffset`/`imageOffset.y`/`imageExtent`, entirely orthogonal to which array layer a region targets — verified directly, no interaction hazard. **Also not a gap for bindless registration**: `BindlessTable::registerSampledImage(VkImageView, VkImageLayout)` (`bindless.h:226`, `bindless.cpp:250`) takes a bare, opaque `VkImageView` — a `VK_IMAGE_VIEW_TYPE_CUBE` view registers exactly like a 2D one, no code change needed there. | consume-now | `Texture2D` grows a cube-aware creation path (new factory `createCubeForPresuppliedMips()`, or an `arrayLayers`/`isCube` parameter on the existing one) setting `arrayLayers=6`, `flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`, `imageType=VK_IMAGE_TYPE_2D` (cubes are still 2D-typed in Vulkan), and a `VK_IMAGE_VIEW_TYPE_CUBE` view; `ImageMipLevel` (or a new parallel struct) grows a `baseArrayLayer`/`faceIndex` field threaded into `uploadImageMips()`'s per-region `imageSubresource.baseArrayLayer` (one region per face×mip, widening the existing "one region per entry" design by one dimension — the chunking/reclaim/ticket machinery underneath is reused completely unmodified, confirmed above). `DecodedKtx2Texture::levels()` (`texture_decode.cpp:272-300`) also needs a face-aware form: it currently hardcodes `ktxTexture_GetImageOffset(tex, level, /*layer=*/0, /*faceSlice=*/0, &offset)` — the vendored `ktx.h:541` macro's third parameter (`faceSlice`) is exactly the per-face selector, unused today. The ticket's Files list ("rx_rhi_vk uploader only if a cube/array gap surfaces — additive") should be corrected: the gap is certain, not conditional, so `texture.h`+`upload.h` are REQUIRED files for the implementing task, not optional. |
| 8 | `TextureRole::Environment`'s blast radius across the 6-role-total design | Grep sweep across `rx_asset` and all consumers | `TextureRole` (`texture_decode.h:41-48`) has exactly 6 enumerators; two exhaustive switches with no `default:` consume it (`textureRoleName()` `texture_decode.cpp:13-29`, `roleFormatTable()` `:61-71` — a missed case is a `-Wswitch` warning, non-fatal, this project carries no `-Werror`, verified directly); `TextureCacheStats::byRole` is `std::array<RoleStats, 6>` (`texture_cache.h:107-109`) and `roleFallback_` is `std::array<TextureHandle, 6>` (`:435`) — both HARDCODED 6. Grep of `TextureRole::` across all of `src/` (non-test) confines every reference to `texture_decode.{h,cpp}`, `texture_cache.{h,cpp}`, `import_gltf.cpp` (5 glTF-material-slot call sites, `:1030-1042`), `import_pipeline.h` — **no `rx_material` or other cross-module consumer exists**; the blast radius is fully contained inside `rx_asset`. `import_gltf.cpp`'s 5 call sites map glTF MATERIAL slots (baseColor/MR/normal/occlusion/emissive) — glTF has no material-level "environment" slot, so none of those 5 call sites need touching; Environment is a Scene-level concept Stage 1 Task 10 wires up, not glTF-import-level. Tests: grep of `byRole\[` across `texture_cache_test.cpp` shows index-driven access (`byRole[roleIdx]`), never a literal `6` — low regression risk. | consume-now | Add `TextureRole::Environment` as a 7th enumerator, appended at the END (row 4's index-stability requirement); bump `byRole`/`roleFallback_` to `std::array<_, 7>`; add the two exhaustive-switch cases. `roleFormatTable()` does NOT try to fit Environment into the existing Basis-transcode-shaped `RoleFormatEntry` (an environment texture is never Basis-encoded) — the HDR decode path (row 9) bypasses `planTranscodeFormat()`/`roleFormatTable()` entirely for this role. |
| 9 | Equirect HDR (Radiance `.hdr`) input — library availability and current call-site state | Direct header/TU verification, not assumed from memory | Vendored `stb_image.h` (the actual FetchContent checkout under `build/*/​_deps/stb-src/`, read directly) already declares+implements `stbi_loadf_from_memory`/`stbi_is_hdr_from_memory` — genuinely zero new dependency, zero new vendoring commit needed. `src/rx_rhi_vk/src/stb_impl.cpp` (21 lines, the sole `STB_IMAGE_IMPLEMENTATION` TU) defines nothing but that macro — no `STBI_NO_HDR`/`STBI_ONLY_*` anywhere in the repo disables it. `decodeStbImage()` (`texture_decode.cpp:306-343`) calls ONLY `stbi_load_from_memory` (8-bit) today — confirmed via full-file read and a repo-wide grep for `stbi_loadf`/`stbi_is_hdr`/`.hdr\b` outside vendored/build trees (zero hits). This is "wire up an existing capable dependency," not new decode-branch invention — matching this repo's own "no reinvented wheels" rule almost for free. **Addendum, worse than a plain absence:** `stb_image.h`'s own `stbi__load_main()` (`:1137-1177`, read directly) auto-detects an HDR-format input internally and, on that branch, calls `stbi__hdr_load()` then **`stbi__hdr_to_ldr()`** (`:1176-1177`, tonemap defined `:1884`) before returning to its 8-bit caller — meaning a Radiance `.hdr` file hits TODAY'S `decodeStbImage()`/`TextureCache::load()` not with an error, but with a silent, successful decode that tonemaps/clamps every texel to 8-bit LDR and discards all superunity data with zero warning. This is a live, currently-reachable correctness gap for anyone pointing a `.hdr` file at the cache today, not merely a missing feature. | consume-now | A new decode function (parallel to `decodeStbImage()`, e.g. `decodeStbImageHdr()`) calls `stbi_is_hdr_from_memory()` to detect Radiance content BEFORE `decodeTextureForUpload()`'s existing `looksLikeKtx2(...)  ? ... : decodeStbForUpload(...)` dispatch (`texture_decode.cpp:648-658`) ever reaches the 8-bit branch — closing the silent-tonemap gap as a side effect of implementing the feature, not merely adding a new capability alongside an unfixed old bug. On the HDR branch, `stbi_loadf_from_memory()` decodes float RGB(A); routes through `TextureRole::Environment` (row 8) at `VK_FORMAT_R16G16B16A16_SFLOAT` (Open Question 2) with `isFormatSupported()` still gating device support; byte-source-only (no filesystem call in the new function, same grep-enforced invariant as every other path in this file — see row 14 addendum on what "grep-enforced" actually means today). |
| 10 | Equirect HDR is a flat 2D lat-long texture, NOT a cubemap — must not collide with row 7's cube plumbing | Code-review / API-shape clarity | The ticket separates these as two distinct deliverables ("KTX2 cubemap/array... **plus** equirectangular HDR input"); no structural reason for them to share code beyond both extending `TextureRole`/the format table. | consume-now | Equirect HDR loads through the EXISTING single-layer 2D `Texture2D::createForPresuppliedMips()` path, unmodified — no row-7 cube plumbing touches this half at all. Equirect→cubemap CONVERSION (a compute pass) is explicitly Stage 1 Task 9's job, not this ticket's; this ticket only gets the raw float bytes onto the GPU as one sampled 2D image. |
| 11 | GPU test asserts EXACT per-face cube VALUES, not mere execution | GPU test w/ driver labels (lavapipe + real driver, per the plan's standing real-GPU-verification constraint), decoded-value discipline | No existing GPU-level cube-sampling test exists anywhere in the repo (grep of `tests/` for cube-sampling/`textureCube`/`samplerCube` returns nothing outside the two rejection-only tests, rows 1-2). An established GPU-readback pattern already exists for the analogous 2D case (`buildQuadPipeline()`/`renderAndReadbackQuadrants()`, `texture_cache_test.cpp:1220+`) — the natural template to extend with a cube-sampling variant. | consume-now | A new GPU test uploads `cubemap_mips.ktx2` (row 6), samples all 6 faces at mip 0 via a `samplerCube`/direct face-indexed readback (mirroring the existing quadrant-readback pattern), and asserts each face's sampled color matches its authored flat color exactly (near-exact tolerance, not eyeballed); ALSO samples a lower mip and asserts it remains face-distinguishable (box-filtered but not aliased across faces), proving mip data landed in the correct face's subresource range. Both lavapipe and the real (NVIDIA) driver run this test, each report labeled by driver per the plan's standing constraint. |
| 12 | Chunked-staging path (#32-verified) interacts correctly with per-face regions | Code read + Vulkan-copy-semantics reasoning | `upload.cpp`'s chunked row-group splitting (`upload.h:316-343`'s doc comment, `upload.cpp:452-509`) operates PER MIP LEVEL, splitting an oversized level's rows across ring-buffer trips — orthogonal to `imageSubresource.baseArrayLayer` (independent field from the row-chunking math, which only touches `bufferOffset`/`imageOffset.y`/`imageExtent.height`). No interaction hazard found by inspection: chunking one face's one mip level works identically whether `baseArrayLayer` is the current hardcoded 0 or a per-face 0-5 value once row 7 lands. | consume-now | Regression test: a cube fixture with a base-mip level large enough to force chunking (a small test-only ring-buffer capacity, matching the existing chunking tests' own technique — grep `upload_test.cpp` for how today's non-cube chunking tests force a small ring, then mirror it) proves per-face chunked uploads land in the CORRECT face's subresource, not just the correct rows within one face. |
| 13 | Equirect HDR value-readback proof (the ">1.0 texel survives" bar) | GPU test, decoded-value discipline | No existing test exercises any float-format texture UPLOAD/readback anywhere in `rx_asset`/`rx_rhi_vk` tests (grep for `SFLOAT` in `src/rx_asset/tests`/`src/rx_rhi_vk/tests` returns only unrelated render-target-format hits, not texture-upload tests). | consume-now | A tiny committed `.hdr` fixture (a handful of texels, at least one channel value >1.0 — the ticket's own explicit bar) loads via `TextureCache`, uploads, and is read back on GPU (compute `imageLoad` or fragment readback, matching this repo's established readback pattern) asserting the >1.0 texel survives exactly (or within float-precision tolerance for the chosen format) — proving no accidental UNORM clamp/8-bit truncation anywhere in the path. Both drivers, per the plan's standing constraint. |
| 14 | IO-source/byte-source abstraction invariant preserved for every new code path | Grep-enforced invariant (Phase 4 pattern, explicitly re-asserted by this ticket's own acceptance text: "loads respect the byte-source abstraction (no filesystem in the load path — grep-enforced, Phase 4 invariant)") | `parseAndTranscode()`'s own comment (`texture_decode.cpp:183-184`) states "ktxTexture2_CreateFromMemory ONLY — zero std::filesystem/fopen in this whole translation unit," verified true for the whole file by full read; `decodeStbImage()` likewise calls only `stbi_load_from_memory`, never a path-based stb entry point. **Addendum: the word "grep-enforced" is not literally true anywhere in this repo today**, verified directly — `.github/workflows/ci.yml` and every script under `tools/` were grepped for `filesystem`/`fopen` invariant-checking logic: zero hits. No CI workflow, no `tools/*.sh` script, and no committed test performs this grep anywhere in the repo. The phrase (here, in the ticket body itself, and in `matrix-issue03-ktx2-textures.md`'s own row 2) describes a manual-code-review criterion, not an automated one, despite its wording. | consume-now (behavior) / needs-coordinator-ruling (enforcement mechanism) | New HDR/cube code (rows 6-13) calls `stbi_loadf_from_memory`/`ktxTexture2_CreateFromMemory` exclusively — same review criterion (`grep -n "std::filesystem\|fopen\|ifstream" src/rx_asset/texture_decode.cpp src/rx_asset/texture_cache.cpp` returns nothing outside the one documented `load(const std::filesystem::path&)` convenience overload, `texture_cache.cpp:485-493`, which wraps `FilesystemByteSource` in a DIFFERENT translation unit, `byte_source.cpp`) applies unchanged to the new code. Recommend this ticket's hardened acceptance text either drop the word "grep-enforced" in favor of "code-review-enforced" (matching reality), OR — cheaper, and closes a real process gap rather than repeating an inaccurate claim a fourth time — add the actual grep as a small `tools/` script + CI step in the same task, since the new HDR/cube code is exactly the kind of surface a real future regression could slip a filesystem call into unnoticed. See Open Questions 5. |

---

## Conflicts

None found against the plan/ticket/registry text. Row 3 (cubemap-only vs.
full array/cube-array scope) is not a stated conflict but an
under-specified boundary in the ticket's own "cubemap/array" phrasing,
inherited verbatim from the registry line's title — flagged as Open
Question 1, not silently resolved.

## New gaps

- **Row 7 (Texture2D/Uploader array-layer plumbing) is the largest
  surfaced gap.** The ticket's own Files list treats `rx_rhi_vk` as
  "additive… only if a gap surfaces" — this matrix proves the gap is
  certain, not conditional (hardcoded `arrayLayers=1`/`VIEW_TYPE_2D`/
  `baseArrayLayer=0` at every relevant call site, verified by direct
  read). The implementing task's Files list should promote
  `texture.h`/`upload.h` from conditional to required.
- **`TextureRole::Environment`'s D11 fallback texture** has no precedent
  in the existing fallback set (white/flat-normal/neutral-MR) — a
  genuinely new design decision, not a mechanical array-resize (Open
  Question 3).
- **`DecodedKtx2Texture::levels()`'s face-blindness** (hardcoded
  `layer=0, faceSlice=0` in its `ktxTexture_GetImageOffset` call) is a
  second, smaller plumbing gap inside `rx_asset` itself, distinct from
  row 7's `rx_rhi_vk`-side gap — both must land together for a real
  6-face upload to be possible end to end.
- **Radiance `.hdr` input is not merely absent today, it is actively
  mis-handled** (row 9 addendum): a `.hdr` file handed to today's
  `TextureCache::load()` does not error — it silently decodes through
  `stbi_load_from_memory`'s internal HDR-autodetect-then-tonemap path
  (`stb_image.h:1137,1176-1177,1884`, vendored header, read directly)
  and returns clamped 8-bit LDR bytes with no warning at all. Real and
  currently reachable, not hypothetical — previously undocumented
  anywhere in the planning corpus (checked `matrix-issue03`'s own gaps
  list; its N2 is the unrelated STB mip-generation gap).
- **libktx v5's tool-suite removal is a new, previously unregistered
  risk to this project's own fixture-generation tooling**, distinct
  from the already-registered "libktx v5 / UASTC-HDR" watch item (that
  item is about the TRANSCODE feature surface; this is about the CLI
  TOOL surface). Live-fetched from `gh api
  repos/KhronosGroup/KTX-Software/releases/tags/v5.0.0-rc2` (2026-08-20):
  the release notes state "**The legacy tools have been removed**" with
  an explicit replacement table whose first row reads `toktx` →
  `ktx create`. `assets/test/textures/generate_fixtures.sh` (this
  project's own committed, D17-regeneration-discipline script) invokes
  the `toktx` binary by name throughout (`:14-19,31-35`, including its
  own header comment naming the exact v4.4.2 release-binary download
  URL) — when a future task eventually migrates off the v4.4.2 pin,
  `generate_fixtures.sh` itself will need rewriting to target
  `ktx create`'s different flag surface, not just a version-string bump.
  Proposed fit: append this as a second line to the existing libktx v5
  registry watch item at the eventual pin-refresh task (Open Questions 5
  is a narrower, this-ticket-scoped cousin about the byte-source
  grep-enforcement gap, not this finding).
- **The "grep-enforced" byte-source invariant has no automated backing
  anywhere in this repo** (row 14 addendum) — a process gap that
  predates this ticket (already miscited in the Phase-4 gate matrix and
  in three source comments) but cheap to close here: a five-line
  `tools/` grep script + one CI step, rather than a fourth uncorrected
  repetition of the claim.

## Verification health

**Verified first-hand (primary source read directly, in full or by
targeted grep/gh-api against the real artifact):** `texture_decode.{h,cpp}`
(both complete), `texture_cache.{h,cpp}` (both complete), `texture.cpp`
(complete), `upload.h`/`upload.cpp` (targeted, all cited regions), the four
existing cube/misleading-fixture test cases (all read in full),
`generate_fixtures.sh`'s relevant blocks (complete), `bindless.h`/
`bindless.cpp`'s `registerSampledImage` (complete), `third_party/
CMakeLists.txt`'s libktx vendoring block (complete, 737-860), the vendored
`ktx.h` struct fields and `GetImageOffset` macro at the pinned tag, the
vendored `stb_image.h`'s HDR declarations/implementation AND its internal
`stbi__load_main()`/`stbi__hdr_to_ldr()` auto-tonemap path (both at the
actual FetchContent checkout path, not a cached/paraphrased copy), the
`stb_impl.cpp` TU (complete, 21 lines), `.github/workflows/ci.yml` +
`tools/*.sh` (grepped for the byte-source-invariant enforcement mechanism
— none found), the Phase 5 plan file (grepped for `stb_image_resize` —
zero hits, correcting an earlier assumption), and the live GitHub API for
KTX-Software's tag/release dates and the v5.0.0-rc2 release body (all
`gh api`, fetched 2026-08-20 — a fresh, independent re-verification, not
reused from the Phase-4 gate's cached account of the same facts).

**Cross-referenced, not independently re-derived:** the `kHdrFormat =
VK_FORMAT_R16G16B16A16_SFLOAT` convention (grepped first-hand across all
four samples that define it; the sibling `matrix-p5t03-hdr-scene-color.md`
independently documents the same fact for its own ticket — cited as
corroboration, not as this matrix's source).

**Not independently re-verified (relied on Phase-4 gate text as
authoritative, low risk):** the exact numeric values of
`ktx_transcode_fmt_e` enumerators (already established by
`.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue03-
ktx2-textures.md`, not re-derived here); libktx's exact internal
non-Basis-float-KTX2 code path inside `ktxTexture2_NeedsTranscoding()` —
inferred from its documented DFD-color-model gating (consistent with
`texture_decode.cpp:228-233`'s own comment) and corroborated by the
existing `raw_rgba8.ktx2` fixture already proving the same
"NeedsTranscoding()==false, upload verbatim" branch for a different
non-Basis format, but not traced line-by-line inside libktx's own C
source for this specific case.

**Gaps flagged for the implementing task, not resolved here:** the exact
chunked-staging force-small-ring test technique (row 12) is cited by
description, not by exact file/line, since no cube-upload test exists yet
to point at — the implementing task should locate the existing non-cube
chunking test's ring-size-forcing mechanism and mirror it exactly.

**Dead ends:** one WebFetch attempt at a raw `CHANGELOG.md` path 404'd
this session and was immediately replaced with the GitHub releases API
(which succeeded on the first try) rather than left unresolved; otherwise
none — every other source needed for this ticket was reachable (local
file reads, `gh api`, and the vendored dependency cache all resolved
cleanly).

## Open Questions

1. **Cubemap-only vs. full array/cube-array support (row 3).**
   Recommendation: **cubemap-only**
   (`isCubemap && numFaces==6 && numLayers==1`). Rationale: zero charter
   consumer needs 2D-array or cube-array textures anywhere in the Stage
   0-4 plan text; FG1's actual consumer needs exactly one cubemap per
   environment. Building unused array/cube-array plumbing against zero
   demand contradicts this project's own "no speculative future-proofing"
   discipline. `isUnsupportedLayoutFor()` is narrowed, not deleted — true
   arrays and cube-arrays stay explicitly rejected with their own
   regression tests.
2. **Environment upload format: `R16G16B16A16_SFLOAT` vs
   `R32G32B32A32_SFLOAT` (row 9).** Recommendation:
   **`R16G16B16A16_SFLOAT`**. Rationale: matches the process-wide HDR
   working-format convention already used by every sample (`kHdrFormat`
   in samples 05/07/08/09, verified first-hand via grep, all
   `VK_FORMAT_R16G16B16A16_SFLOAT`), halves storage/bandwidth vs. 32-bit,
   and half-float dynamic range is the production-standard choice for
   authored HDR environment content (Filament and comparable engines
   store prefiltered/equirect environments at half-float) — no charter
   text anywhere calls for full 32-bit float environment storage. Note
   this is technically owned by Task 1's own D-series ("HDR working
   format" is explicitly named there, `…phase5-techniques.md:144`) — this
   ticket's acceptance text should defer to that ruling and fall back to
   this recommendation only if Task 1 has not decided by the time Task 6
   dispatches.
3. **Environment role's D11 fallback-texture content (new gap above).**
   Recommendation: a small uniform mid-gray (not black) 1×1 or 4×4
   texture, uploaded through the SAME new float path (proving the float
   path works before Stage 1 lands a real HDR asset) rather than reusing
   an 8-bit fallback cast to float — mid-gray reads as "neutral ambient"
   for an unbound environment, consistent with Phase 4's own D22
   interim-flat-ambient philosophy, rather than "no light at all" (black).
4. **New fixture naming/location (row 6).** Recommendation:
   `assets/test/textures/cubemap_mips.ktx2`, generated by an addition to
   the EXISTING `generate_fixtures.sh` script (not a new script) — keeps
   the "one script, one regeneration command" discipline this fixture set
   already established for `cubemap.ktx2` itself.
5. **Does the "grep-enforced" byte-source invariant get an actual grep
   this task, or does the ticket's own wording just get corrected (row
   14/New gaps)?** Recommendation: **add the real mechanism** — a small
   `tools/check_byte_source_invariant.sh` (or equivalent CI step)
   grepping `src/rx_asset/texture_decode.cpp`/`texture_cache.cpp` for
   `std::filesystem`/`fopen`/`ifstream` outside the one documented
   `FilesystemByteSource`-wrapping overload, wired into `ci.yml`.
   Rationale: this ticket is introducing genuinely new decode call sites
   (HDR/cube) into exactly the files this invariant protects — the
   cheapest moment to build real enforcement is the same moment new
   surface area is added, not a future cleanup task; the fix is a
   five-line script, cheap relative to the cost of a fourth undetected
   repetition of an inaccurate "grep-enforced" claim.
