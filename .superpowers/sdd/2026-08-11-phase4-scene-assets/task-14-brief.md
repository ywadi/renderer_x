# Task 14 brief — KTX2/Basis compressed texture pipeline + sampler cache (card #3)

You are the implementer for Phase 4 Stage 1 Task 14 of RendererX
(Vulkan 1.3 renderer middleware, C++20, repo `/media/ywadi/second/renderer_x`,
main checkout — base commit `d0e49d8`, tree clean except SDD workspace
files which are not yours).

## Requirements — read IN THIS ORDER; they are your spec

1. Plan task body: `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md`
   — section `### Task 14:` INCLUDING "Added" and "Gate hardening"
   blocks (BINDING).
2. Spec decisions D10, D11 (+ D5, D24, D25):
   `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`.
3. Completeness matrix (acceptance criteria row by row):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue03-ktx2-textures.md`.
4. Coordinator rulings (+ Errata; they win on conflict):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/rulings-2026-08-18.md`
   — section `**#3 KTX2 (Task 14).**`.
5. Ticket body: `gh issue view 3` (amendment + GATE HARDENED blocks).

Order of authority: rulings (+errata) > spec > matrix > ticket.

## Landed context you build on (don't fight it)

- Task 13: `rx_asset` has `Registry`, the byte-source abstraction
  (`ByteSource`/`FilesystemByteSource` in byte_source.{h,cpp}),
  material parameter sets with `TextureRef` (sampler state + role
  context carried), KHR_texture_basisu payload routing, D11 fallback
  scaffolding in fallbacks.cpp, fixture/fetch conventions
  (`tools/fetch_assets.sh`, documented generator invocations).
  `Registry::importGltf(source, GeometryPool&, TextureCache*)` — the
  TextureCache parameter exists, currently nullptr-tolerant; you make
  it real and wire material texture resolution through it.
- Task 11: `UploadTicket` (timeline semaphore) — texture uploads
  consume tickets; sync load may wait once per batch at a documented
  point, never per-mip.
- Task 10: `MemoryCategory` accounting — texture bytes attributed to
  the textures category via the existing choke points; TextureCache
  exposes resident-stats (bytes by role + count) that balance to zero
  at teardown (accounting test pattern from Task 10).
- Known upstream fastgltf v0.9.0 bugs (documented in import_gltf.cpp):
  use the functor iterateAccessor overload, never copyFromAccessor
  with normalized data or the range overload.

## Scope summary (details in the matrix — this is a map)

- VENDOR libktx (KTX-Software) **v4.4.2** exactly (rulings; v5 is RC —
  do not bump): vendoring commit records Apache-2.0 PLUS the bundled
  third-party licenses actually compiled (at minimum the Basis
  transcoder; Ericsson etcdec if ETC decode compiles); evaluate
  parse/transcode-only build options to shrink the binary (encoder
  OFF); windows-cross-zig build verified IN-TASK.
- `src/rx_asset/texture_cache.{h,cpp}`: `TextureCache::load(source,
  role)` → TextureHandle (bindless index inside; D5 main-thread
  affinity for GPU mutation; decode/transcode is worker-eligible CPU
  work but this task may keep it synchronous — Task 15 parallelizes).
- KTX2 WITHOUT filesystem: `ktxTexture2_CreateFromMemory` (or
  ktxStream custom vtable) fed from the byte source; ZERO
  std::filesystem/fopen in texture_cache.cpp (grep-enforced);
  in-memory load test; path convenience overload wraps the byte
  source.
- Transcode: `ktxTexture2_TranscodeBasis` with the D10 role→target
  matrix, TOTAL over TextureRole via exhaustive switch (compile-time
  completeness): baseColor/emissive→BC7(SRGB image), normal→BC5
  (UNORM), metallicRoughness/occlusion→BC7 UNORM (BC4_R recorded
  option), genericData→BC7 UNORM; RGBA32 fallback path when the
  device/driver lacks a target format — fallback path CI-tested via a
  forced-off seam, exact-format path asserted where supported;
  IN-TASK: log a vkGetPhysicalDeviceFormatProperties2 dump for
  BC7_SRGB/BC7_UNORM/BC5_UNORM/BC1_SRGB on the CI driver into your
  report (the matrix's lavapipe evidence says BC works — verify).
- Non-Basis KTX2 DETECTED before transcode (needs-transcoding check
  via DFD color model — never discover via KTX_INVALID_OPERATION):
  uploadable-as-stored when format supported, else fallback + WARN.
- ETC1S and UASTC and UASTC+zstd fixtures (documented toktx recipes);
  supercompression auto-inflation (no explicit zstd calls in cache).
- Colorspace: glTF ROLE is authoritative for the created VkFormat
  (BC7 SRGB vs UNORM relabel is free); container-DFD disagreement →
  ONE WARN naming file/role/both transfer functions (Godot #99589
  class made loud); sRGB-mislabeled-normal fixture test; cite the
  spec precisely per rulings C3 (MUST-linear for MR/normal;
  convention-linear for occlusion).
- Mip chains from the container: per-level copies correct for
  block-compressed INCLUDING sub-block tail mips (2x2, 1x1 — extent
  true size, data one 4x4 block; the classic off-by-one); deep-mip
  readback probe; mips-absent → mip 0 + ONE WARN recommending
  --genmipmap. **Uploader block-compressed support is an EXPLICIT
  acceptance item (matrix N4)**: extend
  `rx_rhi_vk::Uploader::uploadToImage` (or add a sibling entry point)
  for block-compressed layouts — block-row pitch, per-level regions —
  you MAY touch src/rx_rhi_vk for this; keep the extension additive
  (existing uploadToImage behavior byte-identical; existing tests
  unmodified).
- Cubemap/array/3D KTX2: WARN naming layout + fallback, never a
  silently-wrong 2D slice; WARN names FG1 as the scheduled consumer.
- stb PNG/JPG fallback path: role-correct RGBA8 SRGB/UNORM, KTX2-
  recommendation WARN, 16-bit PNG downconvert documented, decode
  failure → checkerboard; mip-0-only recorded limitation (rulings N2).
- Sampler cache (G6): glTF→Vk mapping TOTAL (3 wrap modes; min-filter
  mipmap variants → (VkFilter, VkSamplerMipmapMode) per the canonical
  table documented in the header); absent sampler → glTF defaults;
  cache key (wrapS,wrapT,mag,min,mipmapMode,maxAniso); dedup test
  (identical → one VkSampler) + negative test; aniso 8x default
  clamped to device max, cleanly off when unsupported (verify CI
  driver in-task alongside the BC dump).
- D11 fallbacks: role-appropriate UTILITY textures for unbound slots
  (flat-normal for normal slots — never checkerboard there);
  checkerboard for missing/failed; every failure mode mapped + tested;
  ONE log per asset (dedup criterion — no per-frame spam).
- D24: residency-tolerant TextureHandle resolve (evicted → fallback
  bindless index, never stale slot; eviction releases through
  DeletionQueue); evict→fallback→reload→real test at texture level.
- Dimension/format limits: oversized → WARN + checkerboard (not a
  validation error); non-multiple-of-4 base dims OK (block rounding);
  1x1 case; zero-dim/corrupt header → named parse error.
- KHR_texture_basisu wiring: role inferred from the MATERIAL SLOT
  that references the texture (slot-driven, never filename-driven —
  misleading-filename test); glTF-referenced KTX2 renders through the
  cache (quadrant pixel GPU test per the plan).
- Committed fixtures generated by DOCUMENTED toktx invocations
  (regenerable script/README next to fixtures, D17 discipline):
  ETC1S, UASTC, UASTC+zstd, mips-absent, cubemap (log path),
  non-multiple-of-4, sRGB-mislabeled-normal.
- Discrimination standard: scratch-worktree revert evidence for the
  load-bearing tests most at risk of vacuousness (suggested: role-
  authoritative colorspace WARN; sub-block mip-tail regions; sampler
  dedup; needs-transcoding detection).

## Global constraints (binding)

- **NO AI attribution of any kind** in commits; author stays local
  git config; conventional factual messages; commit locally; do NOT
  push; do NOT touch board/issues/plan/spec/ledger; commit only your
  own files.
- Production grade; TDD; suite green BOTH presets; zero unfiltered
  validation errors; per-directory style; D5 one-liners on new public
  headers; new dep builds windows-cross-zig IN THIS TASK.

## Report contract

Full report →
`.superpowers/sdd/2026-08-11-phase4-scene-assets/task-14-report.md`
(per-criterion proof, command output tails, CI-driver format dump,
revert evidence, deviations, self-review). FINAL MESSAGE: ONLY
status, commit SHAs, one-line test summary, concerns.
