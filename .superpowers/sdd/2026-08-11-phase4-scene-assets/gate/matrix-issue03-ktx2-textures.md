# Completeness matrix — Issue #3: KTX2/Basis compressed texture pipeline

## 1. Header

- **Ticket:** #3 "KTX2/Basis compressed texture pipeline" — plan
  **Task 14** (issue body's first line still reads "Plan Task 11" from
  before the 2026-08-18 renumbering; the amendment carries the
  correction).
- **Binding decisions:** D10 (KTX2-first, role-typed formats, sampler
  cache), D11 (checkerboard/utility fallbacks), D5 (BindlessTable/Uploader
  main-thread-only), D24 (residency-tolerant resolve; texture memory in
  #27 accounting categories), D25 (uploads consume #28 UploadTickets);
  gap closures G6 (sampler model), G7 (sRGB-vs-linear by role); the
  IO-source abstraction invariant (issue #3 comment, 2026-08-12-adjacent:
  "TextureCache/KTX2 loading reads through the same host-injectable byte
  source as glTF import"); the FG9 issue comment (resident-memory stats
  mirroring GeometryPool).
- **Sources consulted (fetched/verified 2026-08-18):**
  - KTX-Software (libktx) at tag **v4.4.2** — the current latest STABLE
    tag, published **2025-10-04** (the in-repo research file's "October
    4, 2024" is a year off); `v5.0.0-rc2` (2026-08-17) and `v5.0.0-rc1`
    (2026-05-01) exist as prereleases (v5 is a major rework: UASTC-HDR,
    CMake-only, pending KTX Spec Rev 5). Raw source read at v4.4.2:
    `include/ktx.h`, `lib/basis_transcode.cpp`, `lib/texture2.c`,
    `LICENSE.md`. GitHub API for tag/release dates.
  - Mesa at tip-of-main via the cgit.freedesktop.org mirror (GitLab and
    vulkan.gpuinfo.org were inaccessible to automated fetch — bot-check /
    JS-SPA): `src/gallium/drivers/llvmpipe/lp_screen.c`
    (`llvmpipe_is_format_supported`),
    `src/gallium/frontends/lavapipe/lvp_formats.c`
    (`lvp_physical_device_get_format_properties`).
  - glTF 2.0 spec `Specification.adoc` (raw, KhronosGroup/glTF main) —
    material-texture transfer-function language quoted verbatim below.
  - Basis Universal KTX2 wiki (BinomialLLC) for ecosystem role→format
    convention; Godot issue #99589 (double-sRGB-decode import bug) as the
    named precedent pitfall.
  - Delivered code at HEAD `bf5b853`: `third_party/CMakeLists.txt`
    (stb already vendored, pinned commit `2c980bb`),
    `src/rx_rhi_vk/include/rx_rhi_vk/upload.h` (uploadToImage surface),
    `docs/threading.md` (BindlessTable/Uploader guards).

**Disposition legend:** `consume-now` / `preserve-later` /
`log-don't-drop` / `N/A-Phase-4` (per the research brief).

---

## 2. Matrix

| Feature | First-tier precedent (cited) | Phase-4 disposition | Library support (verified) | Proposed acceptance criterion |
|---|---|---|---|---|
| libktx vendoring | Plan global constraint (pinned tag, license recorded, windows-cross-zig verified in-task) | consume-now | Pin **v4.4.2** (latest stable). License: Apache-2.0 for KTX-Software's own code (LICENSE.md quoted: "Files unique to this repository generally fall under the Apache 2.0 license"), BUT the repo bundles third-party components under their own licenses (`LICENSES/` dir: BSD variants, MIT, Zlib, BSL-1.0; `lib/etcdec.cxx` under an Ericsson license) — GitHub's license API reports `NOASSERTION` for this reason | Vendored at v4.4.2; the vendoring commit records Apache-2.0 PLUS the bundled-component licenses actually compiled into the build (at minimum the Basis transcoder and, if ETC decode is compiled, the Ericsson-licensed etcdec) — not just "Apache-2.0"; both presets build in the adopting task; parse-only/read-only build options evaluated in-task to shrink the binary (encoder not needed) |
| KTX2 container parse WITHOUT filesystem (IO-source invariant) | Issue #3 comment: "reads through the same host-injectable byte source as glTF import, not hardcoded std::filesystem" | consume-now | Verified at v4.4.2: `ktxTexture2_CreateFromMemory(const ktx_uint8_t*, ktx_size_t, flags, ktxTexture2**)` and `ktxTexture2_CreateFromStream(ktxStream*, ...)`; `ktxStream` is a public function-pointer vtable (`read/skip/write/getpos/setpos/getsize/destruct`) with `eStreamTypeCustom` + a `custom_ptr` union member — an explicit bring-your-own-source seam (include/ktx.h) | TextureCache's load path takes bytes (or a stream adapter) from the Task 13 byte source; zero `std::filesystem`/`fopen` in `texture_cache.cpp` (grep-enforceable review criterion); an in-memory KTX2 load test (bytes never touch disk) passes; the path-taking convenience overload wraps the byte-source path (same pattern as import) |
| BasisU transcode target selection | D10 role table; Basis KTX2 wiki convention (color→BC7, normal→BC5 two-channel on desktop) | consume-now | `ktxTexture2_TranscodeBasis(ktxTexture2*, ktx_transcode_fmt_e, ktx_transcode_flags)` verified; full enum verified with values — all D10 targets present: `KTX_TTF_BC7_RGBA`(6), `KTX_TTF_BC5_RG`(5), `KTX_TTF_BC4_R`(4), `KTX_TTF_RGBA32`(13) fallback, plus `BC1_RGB`(2), `BC3_RGBA`(3), `ETC1_RGB`(0), `ETC2_RGBA`(1), `ASTC_4x4_RGBA`(10), `PVRTC1/2_4_RGBA`(9/19) available | Role→target mapping implemented exactly per D10: baseColor/emissive → `KTX_TTF_BC7_RGBA` (sRGB image format), normal → `KTX_TTF_BC5_RG` (Z reconstructed in shader — Task 16 pairing), metallicRoughness/occlusion → `KTX_TTF_BC7_RGBA` (UNORM) with `KTX_TTF_BC4_R` as the recorded single-channel-occlusion option; unit test asserts the chosen `ktx_transcode_fmt_e` per role for a fixture set covering every role |
| ETC1S vs UASTC input; non-Basis KTX2 input | libktx's own validity contract | consume-now | Verified from `lib/basis_transcode.cpp`: transcode requires `colorModel == KHR_DF_MODEL_UASTC \|\| supercompressionScheme == KTX_SS_BASIS_LZ`, else returns `KTX_INVALID_OPERATION` deterministically (doc comment: "not transcodable (not ETC1S/BasisLZ or UASTC)") | ETC1S and UASTC fixtures both load (two `toktx` recipes documented); a plain non-Basis KTX2 (e.g. raw RGBA8) is DETECTED before transcode (via the DFD color model / needs-transcoding query — helper name verified at vendoring, see §5) and either uploaded in its stored `vkFormat` when the device supports it or falls back with a WARN — `KTX_INVALID_OPERATION` from a blind transcode call is never the discovery mechanism; corrupted container → named error + checkerboard, no crash |
| Supercompression (zstd/zlib) | KTX2 spec supercompression schemes | consume-now | Verified: `KTX_SS_NONE=0 / BASIS_LZ=1 / ZSTD=2 / ZLIB=3`; for Basis inputs, `TranscodeBasis` inflates internally ("If the texture contains UASTC images, inflates them, if they have been supercompressed with zstd, then transcodes" — basis_transcode.cpp doc comment); for non-Basis zstd/zlib KTX2, the standard `KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT` path auto-inflates (`ktxTexture2_LoadImageData` → `LOADDATA_INFLATE_ON_LOAD`, texture2.c) | A `toktx --zcmp` UASTC fixture loads and transcodes with no extra caller-side step; the test corpus includes one zstd-supercompressed fixture; no explicit zstd calls exist in TextureCache (the library owns inflation) |
| Mip chain from container | D10: "Mip chains come from the container; if a KTX2 lacks mips the importer warns … no runtime mip generation for compressed formats" | consume-now | Verified: `numLevels`/`baseWidth`/`baseHeight` public fields; `ktxTexture_GetImageOffset(This, level, layer, faceSlice, &offset)` + `ktxTexture_GetData` for per-level upload; the struct's `generateMipmaps` field is only a hint for libktx's own GL/Vk upload helpers (which this pipeline never calls — upload stays ours per D10), so no mip generation happens at parse | Full-chain fixture uploads every level (readback probe samples a deep mip); a mips-absent fixture loads mip 0 with exactly one WARN naming the file and recommending `toktx --genmipmap`; per-level `VkBufferImageCopy` regions are correct for block-compressed formats INCLUDING the sub-block tail mips (2×2, 1×1 — extent stays the true mip size while data size is one 4×4 block; the classic off-by-one this row exists to pin down) |
| Cubemap / array / 3D textures | KTX2 container supports all; Phase-4 TextureCache scope is 2D (D10); FG1 (skybox/IBL, techniques phase) is the registered future cubemap consumer | log-don't-drop | Verified: `isArray`, `isCubemap`, `numFaces` ("6 for cube maps, 1 otherwise"), `numLayers` public fields at v4.4.2 | A cubemap KTX2 and an array KTX2 each produce a WARN naming the file and the unsupported layout + checkerboard fallback, never a crash or a silently-wrong 2D slice; the WARN names FG1 as the scheduled consumer (so the log breadcrumb ages well); see New gaps N1 |
| Colorspace correctness: role decides, container corroborates | glTF 2.0 spec (Specification.adoc, quoted): baseColorTexture "MUST contain 8-bit values encoded with the sRGB opto-electronic transfer function"; emissiveTexture same sRGB MUST; metallicRoughnessTexture "MUST be encoded with linear transfer function"; normalTexture "RGB values stored with linear transfer function". **Precision note:** occlusionTexture's spec text defines scalar semantics (red channel, 0.0–1.0) and never names a transfer function — linear treatment is the universal renderer convention for data textures, not a verbatim spec quote. Named pitfall precedent: Godot 4.3's KTX loader applied an extra sRGB→linear conversion at import, double-darkening glTF models (godotengine/godot#99589) | consume-now | Verified: `ktxTexture2_GetTransferFunction_e()` (current name; `GetOETF*` deprecated aliases) reads the container DFD's transfer field; `TranscodeBasis` picks the sRGB-vs-UNORM `VkFormat` variant from that same DFD field automatically (basis_transcode.cpp: `srgb = KHR_DFDVAL(BDB, TRANSFER) == KHR_DF_TRANSFER_SRGB`) | The **glTF role is authoritative** for the created image's `VkFormat` (BC7 sRGB vs UNORM share identical block data, so relabeling is free): TextureCache creates the image with the role-derived format; when the container's self-reported transfer function disagrees with the role, one WARN names file, role, and both transfer functions (the Godot-bug class becomes loud instead of silent). Test: an sRGB-mislabeled normal-map fixture still creates `VK_FORMAT_BC5_UNORM_BLOCK`+linear semantics with the WARN; the quadrant pixel GPU test (plan Task 14) proves end-to-end sRGB decode on a baseColor fixture |
| Role→format matrix completeness | D10 table; BasisU KTX2 wiki desktop convention (color→BC7, normal→BC5); no formal published Godot/bgfx role table exists (verified absence — their loaders take caller-requested targets) | consume-now | As transcode row above | The matrix is total over `TextureRole`: every role has a defined (transcode target, VkFormat, colorspace) triple — baseColor→BC7/SRGB, emissive→BC7/SRGB, normal→BC5/UNORM, metallicRoughness→BC7/UNORM, occlusion→BC7/UNORM (BC4_R recorded option), genericData→BC7/UNORM; adding a role without a matrix entry fails a static_assert/exhaustive-switch (compile-time completeness) |
| lavapipe (CI driver) BC support | Plan Task 14 note: "lavapipe BC support — verify in-task; if a target format is unsupported on CI's driver, transcode falls back to RGBA8 with warning" | consume-now | Mesa source read (tip-of-main via cgit, 2026-08-18): `llvmpipe_is_format_supported` rejects only ASTC/ATC and non-ETC1 ETC layouts — S3TC(BC1-3)/RGTC(BC4-5)/BPTC(BC6-7) fall through to `return true`; `lvp_physical_device_get_format_properties` then grants compressed formats `SAMPLED_IMAGE`+`TRANSFER`+`LINEAR_FILTER` bits in both tiling modes. Strong evidence BC7/BC5/BC1 (sRGB and UNORM alike) sample on lavapipe. **Caveats:** tip-of-main source, not the CI image's pinned Mesa version; no empirical gpuinfo report obtained (site unfetchable) | Keep the plan's in-task verification as the authoritative gate: the adopting task logs a `vkGetPhysicalDeviceFormatProperties2` dump for BC7_SRGB/BC7_UNORM/BC5_UNORM/BC1_RGB_SRGB on the CI driver into the task report; the RGBA32 fallback path (`KTX_TTF_RGBA32` + WARN) exists regardless and is exercised by a format-support-forced-off test seam, so CI proves the fallback even if the exact-format path runs everywhere |
| stb PNG/JPG fallback path | D10: RGBA8_(SRGB\|UNORM) + "importer warning recommending KTX2 conversion — no runtime block compression in Phase 4 (recorded)" | consume-now | stb_image already vendored (third_party/CMakeLists.txt:189-217, pinned `2c980bb`; impl TU discipline documented there); role→sRGB/UNORM applies identically to the RGBA8 formats | PNG and JPEG fixtures load via stb with the KTX2-recommendation WARN; role-correct `VK_FORMAT_R8G8B8A8_SRGB` vs `_UNORM` chosen; 16-bit PNG handled (stb downconverts — documented) ; stb decode failure → checkerboard + WARN; stb path uploads mip 0 only (see New gaps N2 for the mip-generation gap this leaves) |
| Checkerboard + utility fallbacks (D11) | D11: 4×4 magenta/black checkerboard (missing/failed), 1×1 white / flat-normal / neutral-MR utility textures | consume-now | Registry-owned, created at init (D11) | Every failure mode maps to a named fallback with a test: missing bytes (byte source miss), corrupt container, transcode failure, unsupported layout (cubemap/array row), unsupported MIME; unbound material slots get the role-appropriate UTILITY texture (flat-normal for normal slots — a magenta normal map would shade garbage), not the checkerboard; each failure logs exactly once per asset (no per-frame log spam — dedup criterion) |
| Sampler cache completeness vs glTF sampler space (G6) | glTF sampler space: wrapS/wrapT ∈ {REPEAT 10497, CLAMP_TO_EDGE 33071, MIRRORED_REPEAT 33648} × magFilter {NEAREST, LINEAR} × minFilter {NEAREST, LINEAR, + 4 mipmap variants} (spec sampler section; matrix-issue02 §2A) | consume-now | Sampler creation is rx_rhi_vk-side; glTF enums parsed by fastgltf (Task 13) | The glTF→Vk mapping table is total: all 3 wrap modes → `VkSamplerAddressMode`; minFilter mipmap variants → (`VkFilter`, `VkSamplerMipmapMode`) pairs with the NEAREST_MIPMAP_LINEAR-class combinations mapped per the canonical table (documented in the header); absent sampler → glTF defaults (REPEAT + linear/auto-filter); cache key = (wrapS, wrapT, magFilter, minFilter, mipmapMode, maxAnisotropy) — two glTF samplers with identical state yield ONE `VkSampler` (dedup test from the plan), distinct state yields distinct samplers (negative test); anisotropy 8× default when `samplerAnisotropy` is supported, clamped to `maxSamplerAnisotropy`, cleanly off otherwise (CI-driver-safe — verified in-task alongside the BC dump) |
| Bindless registration + thread affinity (D5) | threading.md: `BindlessTable::registerSampledImage`/`registerSampler` are main-thread-only **[guarded]** | consume-now | Guards delivered (RX_ASSERT_MAIN_THREAD, threading.md) | TextureCache::load performs decode/transcode as pure CPU work (worker-eligible per threading.md "Worker-allowed" list) but all image creation, upload submission, and bindless registration on the main thread; the async path (Task 15) marshals accordingly; TextureHandle embeds the bindless index (plan interface) |
| UploadTicket consumption (D25/#28) | Issue #3 amendment: "uploads consume #28 UploadTickets" | consume-now | Ticket API lands in Task 11 (sequenced before) | Every texture upload goes through `UploadTicket`; the sync load path may wait once per batch at a documented point; no per-mip flush-and-wait (test: N-mip texture load produces ≤1 blocking wait on the sync path, 0 on the async path) |
| Memory accounting + resident stats (D24/#27 + FG9 comment) | Issue #3 amendment: "texture memory attributed in #27's accounting categories"; FG9 comment: "TextureCache exposes resident-memory stats (bytes by role, count) mirroring GeometryPool's stats" | consume-now | #27/Task 10 delivers the category plumbing (sequenced before) | Every texture allocation is attributed to the textures category; `TextureCache::stats()` reports bytes-by-role + count and balances to zero across load/evict/teardown (unit-tested with the accounting test pattern from Task 10); stats feed the HUD/Tracy plots caller-side |
| D24 residency-tolerant resolve | D24; issue #27 invariant text ("a resolve can report not-resident and the caller has a defined path (fallback asset)") | consume-now | Registry handle model (rx_core handle.h generational handles) | Resolving a TextureHandle whose backing image was evicted yields the checkerboard's bindless index (never a stale/dangling bindless slot — the eviction path releases the slot through DeletionQueue and repoints resolution at the fallback); one manual evict → resolve-fallback → reload → resolve-real test at the texture level (mirrors the Task 13 mesh-level test) |
| Dimension/format limits | Vulkan `maxImageDimension2D` (4.4.2-era desktop/Deck drivers and lavapipe all report ≥ 16384 — checked in-task, not assumed); BC block geometry (4×4) | consume-now | Container fields give dims pre-upload (`baseWidth/baseHeight`) | Oversized texture (> device limit) → rejected with WARN + checkerboard (not a validation error); non-multiple-of-4 base dimensions accepted (Vulkan permits BC images with NPOT/non-block-aligned extents; copies round up in blocks — the mip-tail criterion above already exercises sub-block extents); 1×1 smallest case tested; zero-dimension/corrupt-header container → named parse error |
| KHR_texture_basisu wiring from import | Matrix-issue02 §2B (consume-now); D10 KTX2-first | consume-now | fastgltf `Texture::basisuImageIndex` (verified, matrix-issue02) | A glTF referencing KTX2 via KHR_texture_basisu routes through TextureCache with the correct role inferred from the material slot that references it (baseColor slot → baseColor role, etc.); role inference is slot-driven, never filename-driven (test with deliberately misleading filenames) |
| Test fixtures via documented toktx commands | Plan Task 14: "tiny committed .ktx2 fixtures generated by documented toktx commands" (D16 pattern) | consume-now | toktx ships in KTX-Software (the vendored version's own tool builds, or a pinned release binary — decided at vendoring) | Committed fixtures cover: ETC1S, UASTC, UASTC+zstd, mips-absent, cubemap (for the log path), non-multiple-of-4, sRGB-mislabeled-normal; each fixture's exact `toktx` invocation recorded in a script/README next to the fixtures so they are regenerable (D17's regeneration-script discipline applied to fixtures) |

---

## 3. Conflicts (coordinator adjudicates; both sides quoted)

- **C1 — stale dates in the designated fact source.**
  `research-p4-assets.md:56`: "Current Version: v4.4.2 (released October
  4, 2024)". GitHub API reality: v4.4.2 published **2025-10-04**; the
  file's fastgltf/meshoptimizer dates are similarly off by a year
  (documented in matrix-issue02 Conflicts C7). The version PIN (v4.4.2)
  remains correct — the surrounding dates and "next steps" claims should
  not be copied into ticket text.
- **C2 — v5.0.0 release-candidate line.** The research brief and D10 say
  "current release"; the actual current tag is `v5.0.0-rc2` (2026-08-17,
  prerelease, major rework). Pinning the stable v4.4.2 is this matrix's
  recommendation (prerelease pins contradict the vendoring discipline),
  but the choice — and a registry watch item for the v5/KTX-Spec-Rev-5
  transition with its UASTC-HDR support (New gaps N3) — should be an
  explicit ruling, not an accident of wording.
- **C3 — occlusion "linear" attribution.** D10/G7 and the research brief
  treat occlusion as linear alongside metallicRoughness. The glTF spec
  states "linear transfer function" verbatim for normalTexture and
  metallicRoughnessTexture but NOT for occlusionTexture (its text defines
  only red-channel scalar semantics). The pipeline behavior D10 mandates
  is correct and unchanged; the hardened ticket should cite the spec
  precisely (MUST-linear for MR/normal; convention-linear for occlusion)
  rather than attributing all three to spec text.

## 4. New gaps (absent from the entire planning universe; checked against the master registry deferred list, FG1-FG12, and the phase spec)

- **N1 — Cubemap/array KTX2 loading is implied by FG1 but planned
  nowhere.** FG1 (registry) schedules skybox+IBL for the techniques phase
  and even notes "libktx … loads cubemap KTX2 with mip chains — the
  ingredient is in-tree with no consumer" — but no artifact schedules the
  TextureCache-side work (cubemap image creation, 6-face upload loop,
  `numLayers` arrays). Proposed fit: techniques phase, one line riding
  FG1 so the loader work is costed there; Phase 4 ships the log path
  (matrix row) only.
- **N2 — Runtime mip generation for the uncompressed (stb) fallback
  path.** D10 records "no runtime mip generation **for compressed
  formats**" — deliberate and correct. Nothing anywhere addresses mips
  for the stb RGBA8 path: Phase 4 uploads mip 0 only, so PNG/JPG content
  minifies with aliasing until converted to KTX2. Cheap fix exists
  (vkCmdBlitImage chain at upload). Proposed fit: coordinator choice —
  small Phase-4 add to Task 14, or recorded limitation + registry line
  (SDK/streaming phase); the WARN already steers content to KTX2 either
  way.
- **N3 — KTX v5 / UASTC-HDR watch item.** libktx v5.0.0 (RC, 2026-08-17)
  adds UASTC-HDR — HDR texture INPUT, distinct from FG8's HDR display
  OUTPUT, and unregistered anywhere. Proposed fit: registry watch line
  tied to the libktx pin refresh (streaming or techniques phase),
  alongside the environment/IBL work that is HDR input's first real
  consumer.
- **N4 — Uploader block-compressed upload capability is assumed, not
  stated.** `Uploader::uploadToImage` was built for the stb RGBA8 path
  (Phase 2/Task 4 era — upload.h documents "pixels + format + extent in →
  bindless handle out" with a libktx backend anticipated). No artifact
  states it handles block-compressed layouts (row pitch in blocks,
  sub-block mip extents, per-level copies). Likely a small extension, but
  it is Task 14 scope that Task 11's ticket rework should not
  accidentally freeze out. Proposed fit: named as an explicit Task 14
  acceptance item (the mip-chain matrix row carries the criterion);
  flagged here because it is currently zero-mentioned in the planning
  corpus.

## 5. Verification health

**Verified first-hand (2026-08-18):** libktx v4.4.2 tag/date (GitHub
API), license structure (LICENSE.md + LICENSES/ dir), `ktxStream` custom
vtable and all three CreateFrom* entry points, `TranscodeBasis`
signature + full `ktx_transcode_fmt_e` enum with values, the
ETC1S/UASTC-only transcode validity check and its `KTX_INVALID_OPERATION`
failure mode, DFD-driven sRGB format selection, zstd auto-inflation
paths, mip/cubemap/array struct fields, `GetTransferFunction_e` (all
from raw `ktx.h`/`basis_transcode.cpp`/`texture2.c` at the tag); Mesa
llvmpipe/lavapipe BC acceptance logic (raw `lp_screen.c`/`lvp_formats.c`
at tip-of-main via cgit); glTF transfer-function language (raw
`Specification.adoc`, quoted); stb vendoring state (in-repo).

**Inferred / to verify at vendoring (flagged):** the exact name of the
needs-transcoding query used in the non-Basis detection row —
`ktxTexture2_NeedsTranscoding` is the documented libktx helper, but its
presence at v4.4.2 was not independently confirmed this session (the DFD
color-model check `TranscodeBasis` itself uses is the verified fallback
detection mechanism either way); lavapipe's `samplerAnisotropy` feature
status (the sampler row's criterion already requires in-task
verification); the CI image's pinned Mesa version vs. the tip-of-main
source read (the matrix keeps the plan's in-task
format-properties dump as the authoritative gate for exactly this
reason).

**Dead ends:** vulkan.gpuinfo.org (JS-only SPA, no static data) and
gitlab.freedesktop.org (bot-check challenge on every route including the
REST API) were unfetchable; the cgit mirror substituted for Mesa source.
A Mesa `VERSION` file fetch hit a transient 503, so no numbered Mesa
release is cited for the driver claims — treated as "strong evidence,
in-task confirmation required," matching the plan's existing stance.

**Version ambiguities:** none beyond the v4.4.2-vs-v5-RC pin decision
(Conflicts C2). No formal Godot/bgfx role→format table exists to cite —
their loaders accept caller-requested targets; the role convention is
carried by the BasisU wiki + the D10 table itself, with Godot #99589 as
the cautionary colorspace precedent.
