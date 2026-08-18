# Task 13 brief — Import core: fastgltf + MikkTSpace + meshoptimizer + Draco (card #2)

You are the implementer for Phase 4 Stage 1 Task 13 of RendererX
(Vulkan 1.3 renderer middleware, C++20, repo `/media/ywadi/second/renderer_x`,
main checkout — base commit `6a825f7`, tree clean). This is Stage 1's
LARGEST task; take the scope seriously and completely — no partial work.

## Requirements — read IN THIS ORDER; they are your spec

1. Plan task body: `docs/superpowers/plans/2026-08-11-phase4-scene-assets.md`
   — section `### Task 13:` INCLUDING its "Added acceptance criteria"
   and "Gate hardening" blocks (BINDING).
2. Spec decisions D7, D11, D12, D16 (+ D5, D24, D25, D8):
   `docs/superpowers/specs/2026-08-11-phase4-scene-assets-design.md`.
3. Completeness matrix — your acceptance criteria row by row, ALL FOUR
   sections (core features 2A, Khronos extensions 2B, vendor extensions
   2C, pipeline/IO/invariants 2D):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/matrix-issue02-gltf-import.md`.
4. Coordinator rulings (amend the matrix, win on conflict):
   `.superpowers/sdd/2026-08-11-phase4-scene-assets/gate/rulings-2026-08-18.md`
   — section `**#2 glTF import (Task 13).**` AND the Errata section
   (E1: never copy the alignment=48 wording; GeometryPool suballocates
   in element units).
5. Ticket body: `gh issue view 2` (amendment + GATE HARDENED blocks).

Order of authority: rulings (+errata) > spec > matrix > ticket.

## Landed context you build on (don't fight it)

- Task 12 (110f36a/20a9b3f): `rx_asset` library EXISTS with
  `GeometryPool` (element-unit suballocation; `upload(span<PoolVertex>,
  span<uint32_t>) -> MeshRange`; main-thread-guarded; feeds the
  geometry-pool accounting category). Your importer targets this API.
- Task 11: `Uploader::flush()` returns `UploadTicket`;
  `isComplete/wait` exist. Sync import may wait ONCE at a documented
  point per import — never per-primitive/per-upload (matrix 2D row 12).
- Task 10: `MemoryCategory` accounting + `RxMemoryReport` exist.

## Scope summary (full detail lives in the matrix — this is a map)

- VENDORING (third_party, pinned per repo convention, licenses in the
  vendoring commit, windows-cross-zig verified in-task): fastgltf
  v0.9.0 (MIT + embedded simdjson Apache-2.0, FASTGLTF_COMPILE_AS_CPP20),
  meshoptimizer v1.2 (MIT), MikkTSpace (NO tags — pin commit hash,
  quote header license text), Draco (Apache-2.0,
  DRACO_GLTF_BITSTREAM=ON, tests/executables/transcoder OFF, dead-strip
  note per matrix 2D row 9).
- `src/rx_asset` grows: `import_gltf.{h,cpp}`, `registry.{h,cpp}`,
  `fallbacks.cpp`; handles per plan interface (MeshHandle etc. via
  `rx::core::Handle<Tag>`); `Registry::importGltf(source, GeometryPool&,
  TextureCache* /*=nullptr until Task 14*/)`.
- BYTE-SOURCE INVARIANT (matrix 2D row 2, re-worded by rulings C2):
  never set `Options::LoadExternal*`; document bytes via
  `GltfDataGetter`/`FromBytes`; the importer resolves every
  `sources::URI` ITSELF through the injected byte source; spy-source
  test proves zero direct filesystem reads; path-taking overload is a
  thin filesystem-backed-source wrapper; in-memory .glb import test.
- DECODE-TO-OPEN (all consume-now): EXT_meshopt_compression (decode via
  meshoptimizer mode→function map, all modes + filters, gltfpack
  fixtures), KHR_draco_mesh_compression (google/draco decode),
  KHR_mesh_quantization (fastgltf tools convert/denormalize), sparse
  accessors, normalized accessors, u8/u16→u32 index widening with value
  round-trip test, KHR_texture_basisu routing (payload preserved for
  Task 14), image/buffer source variants ALL THREE consume-now
  (external URI via byte source, data URI, GLB bufferView) — three
  same-cube packagings deep-equal test.
- CONSUME-NOW per rulings: KHR_texture_transform offset/scale parsed +
  carried in material parameter sets (rotation logged);
  KHR_materials_unlit → maps to the Unlit disposition in the parameter
  set (consumed by Task 16); EXT_mesh_gpu_instancing → TRS arrays
  expand into InstanceRecords during D12 flattening.
- PRESERVE-LATER (bit-exact round-trip tests): animations (channels/
  samplers/interpolations incl. CUBICSPLINE 3x triplets), morph
  targets (+weights), skins (IBM/joints/JOINTS_0/WEIGHTS_0), cameras
  (both types incl. infinite-perspective), KHR_lights_punctual (all 3
  light types, full params).
- LOG-DON'T-DROP: the generic extensionsUsed/Required surfacing
  mechanism (one INFO summary with per-entry disposition tags;
  UnknownRequiredExtension → named error + D11 fallbacks; unknown
  extensionsUsed never fails — both tested); non-triangle primitive
  modes (WARN + skip, TRIANGLES siblings still import); COLOR_0/
  TEXCOORD_1/JOINTS_n≥1 (WARN each); every unimplemented
  KHR_materials_* (one WARN per material+extension, transmission's
  message states renders-opaque); absolute/network URIs → fallback+WARN.
- PIPELINE per primitive (matrix 2D rows 5-8): fastgltf accessor tools
  (never raw byte pokes) → missing-normal flat-gen + WARN → missing-UV
  zero-fill + tangent +X/w=1 + WARN → TANGENT passthrough else
  MikkTSpace (deindex → per-corner tangents → meshopt_generateVertexRemap
  over the FULL 48B vertex → remap→cache→overdraw(1.05)→fetch;
  tangent-welded-count >= position-welded-count test; degenerate-UV
  no-NaN test) → AABB from FINAL post-meshopt positions (never accessor
  min/max; NaN/Inf reject; 8-corner world transform under rotation +
  negative scale) → GeometryPool upload (u32 indices).
- Node graph: D12 flattening (matrix/TRS variants, 3-level nesting vs
  precomputed references, default-scene rules incl. zero-scene WARN,
  unreachable nodes produce no instances); negative-determinant scale
  flagged + WARN.
- Materials parsed to parameter sets (full pbrMetallicRoughness core
  incl. normal scale / occlusion strength carried; alphaMode + cutoff +
  doubleSided; sampler state carried per texture ref); textures resolve
  in Task 14 — D11 fallback handles until then.
- D11 fallbacks + full error taxonomy: exhaustive compiler-enforced
  switch over all 15 fastgltf::Error members; malformed-file battery
  (not-JSON, valid-JSON-invalid-glTF, truncated GLB, wrong-magic,
  unsupported version, invalid URI) → named error + log via public
  sink + ZERO partial registry mutation + no crash.
- D24 eviction invariant (matrix 2D row 11): residency-tolerant
  resolve (fallback while nonresident, never null/assert), documented
  reference-lifetime rules, no raw pointer/index escapes in public
  surface, manual evict→fallback→reimport→real test.
- Threading (matrix 2D row 15): sync import parallelizes per-primitive
  CPU work internally via rx_task parallelFor (MikkTSpace is
  thread-safe per its header); GPU-object mutation stays on the calling
  main thread; no parallel on/off flag in any API signature.
- Test content (D16 + matrix 2D row 14): committed hand-authored cube
  (<20KB, readable JSON); `tools/fetch_assets.sh` (DamagedHelmet
  mandatory + --sponza optional, checksums, CC BY texts, CI-cached like
  slang-prebuilt); extension fixtures (sparse/morph/anim/skin/lights-
  camera/quantized/meshopt/draco/source-variant cubes) hand-authored or
  generated by DOCUMENTED gltfpack/tool invocations in the fetch
  script; DamagedHelmet integration test (counts/submeshes/skin
  preservation); CI wiring for the fetch cache.
- Discrimination standard (established Tasks 10-12): scratch-worktree
  revert evidence in the report for the load-bearing tests most at risk
  of vacuousness (suggested: byte-source spy, meshopt-actually-ran,
  widening round-trip, zero-partial-mutation on error).

## Global constraints (binding)

- **NO AI attribution of any kind** in commits; author stays local git
  config; conventional factual messages; multiple commits fine (e.g.
  vendoring separate from importer); commit locally; do NOT push; do
  NOT touch board/issues/plan/spec/ledger; commit only your own files.
- Production grade; TDD; suite green BOTH presets; zero unfiltered
  validation errors (GPU tests); per-directory style; new public
  headers carry D5 one-liners; new deps must build windows-cross-zig
  IN THIS TASK.

## Report contract

Full report →
`.superpowers/sdd/2026-08-11-phase4-scene-assets/task-13-report.md`
(per-criterion proof vs ALL matrix sections, command output tails,
revert evidence, deviations, self-review). FINAL MESSAGE: ONLY status,
commit SHAs, one-line test summary, concerns.
