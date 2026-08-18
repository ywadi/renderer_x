# Completeness matrix — Issue #2: glTF 2.0 scene import

## 1. Header

- **Ticket:** #2 "glTF 2.0 scene import" — plan **Task 13**
  (`docs/superpowers/plans/2026-08-11-phase4-scene-assets.md`; the issue
  body's first line still reads "Plan Task 10" from before the 2026-08-18
  renumbering — the amendment section carries the correction).
- **Binding decisions:** D5 (threading contract), D6 (registry/handles),
  D7 (import pipeline), D8 (48-byte pooled vertex format), D11 (fallback
  assets), D12 (flatten-at-import), D16 (test content), D24 (eviction
  invariant), D25 (UploadTicket consumption); gap closures G2 (tangents),
  G8 (multi-primitive submeshes), G11 (AABBs); FG2 import half
  (lights/cameras parse-and-preserve); the 2026-08-12 IO-source
  abstraction invariant; the plan Task 9 depth rule (decode-to-open /
  preserve-later / log-don't-drop).
- **Sources consulted (all fetched/verified 2026-08-18):**
  - fastgltf **v0.9.0** (published 2025-07-08, current latest tag; no
    v0.10 exists; `main` HEAD `a31be25` still declares VERSION 0.9.0) —
    https://github.com/spnda/fastgltf (raw source at the tag:
    `include/fastgltf/core.hpp`, `include/fastgltf/types.hpp`,
    `src/fastgltf.cpp`, `src/io.cpp`, `CMakeLists.txt`,
    `docs/{overview,tools,guides,options}.rst`); docs site
    https://fastgltf.readthedocs.io/ (the `docs.fastgltf.dev` domain named
    in the research brief does not resolve — dead link).
  - meshoptimizer **v1.2** (published 2026-06-30 per GitHub API) —
    https://github.com/zeux/meshoptimizer (README.md, `src/meshoptimizer.h`,
    LICENSE.md, release notes; 20 `.cpp` files + 1 header). gltfpack
    README (`gltf/README.md`, same repo) for gltfpack's default extension
    output.
  - MikkTSpace (github.com/mmikk/MikkTSpace — **no tags or releases
    exist**; last commit 2020-03-25; `mikktspace.h`/`mikktspace.c` read
    directly).
  - Draco (github.com/google/draco — `src/draco/compression/decode.h`,
    `cmake/draco_options.cmake`, top-level `CMakeLists.txt`, LICENSE).
  - glTF 2.0 specification — JSON schemas fetched via GitHub API from
    `KhronosGroup/glTF/specification/2.0/schema/` (animation.sampler,
    accessor.sparse, glTFProperty) and `Specification.adoc` (raw); the
    rendered spec HTML at registry.khronos.org returned HTTP 403 this
    session.
  - Khronos glTF 2.0 extension registry — directory listing + per-extension
    READMEs via GitHub API: **26 Khronos/ + 31 Vendor/ + 3 Archived/ = 60
    extensions** (61 entries minus a `.gitkeep`).
  - Delivered code at HEAD `bf5b853`: `third_party/CMakeLists.txt` (full
    dependency inventory), `src/rx_core/include/rx_core/handle.h`,
    `src/rx_rhi_vk/{include/rx_rhi_vk/upload.h,src/upload.cpp}`,
    `src/rx_task/include/rx_task/scheduler.h`, `docs/threading.md`.

**Disposition legend** (per the research brief): `consume-now` (Phase 4
implements), `preserve-later` (import/store now, consume later),
`log-don't-drop` (detected + logged, never silently ignored),
`N/A-Phase-4` (genuinely out of scope, justified).

---

## 2A. Matrix — glTF 2.0 core features

| Feature | First-tier precedent (cited) | Phase-4 disposition | Library support (verified) | Proposed acceptance criterion |
|---|---|---|---|---|
| Accessor component types 5120 BYTE / 5121 UBYTE / 5122 SHORT / 5123 USHORT / 5125 UINT / 5126 FLOAT; types SCALAR/VEC2/VEC3/VEC4/MAT2/MAT3/MAT4 | glTF 2.0 spec (accessor schema; values confirmed from `Specification.adoc`) | consume-now | fastgltf v0.9.0 full support; `fastgltf/tools.hpp` `copyFromAccessor`/`iterateAccessor`/`getAccessorElement` convert component types and honor `normalized` (docs/tools.rst) | Importer reads every attribute through fastgltf accessor tools (never raw byte pokes); u16 and u32 index fixtures both import; u8 indices (legal per spec) widen to u32; a MAT4 `inverseBindMatrices` accessor round-trips in preserved skin data |
| Normalized integer accessors (`normalized: true`) | glTF 2.0 spec §3.6.2.2; required by KHR_mesh_quantization content | consume-now | fastgltf tools apply the normalization scale during conversion ("properly respects normalization … while copying and converting the data" — docs/tools.rst) | A fixture with normalized u16 TEXCOORD_0 imports; UV float values equal the reference float-authored file within 1/65535 per component |
| Sparse accessors | glTF 2.0 spec "Sparse Accessors" (`accessor.sparse.schema.json`: count/indices/values overriding base data; "Indices MUST strictly increase") | consume-now | fastgltf tools auto-apply sparse substitution ("All of these tools also directly support sparse accessors" — docs/tools.rst; binary-search substitution in `tools.hpp`) | A fixture whose POSITION accessor carries a sparse override imports with the substituted values (vertex-level assert against expected positions); importer contains no raw accessor reads that would bypass substitution |
| Primitive mode 4 TRIANGLES | glTF 2.0 spec `mesh.primitive.mode`; the only mode the D8 pooled pipeline consumes | consume-now | fastgltf parses `Primitive::type` (`PrimitiveType` enum, types.hpp) | Cube + DamagedHelmet import through the full D7 pipeline; submesh index counts divisible by 3 asserted |
| Primitive modes 0 POINTS / 1 LINES / 2 LINE_LOOP / 3 LINE_STRIP / 5 TRIANGLE_STRIP / 6 TRIANGLE_FAN | glTF 2.0 spec mode enum 0–6 (confirmed from `Specification.adoc`) | log-don't-drop (ruled) | fastgltf parses all modes into `PrimitiveType` | A fixture containing a LINES primitive imports: the primitive is skipped with one WARN naming mesh index, primitive index, and mode string; remaining TRIANGLES primitives in the same mesh import normally; no crash, no silent absence. STRIP/FAN note: triangulation is a recorded non-goal for Phase 4 (log includes "convert offline" guidance) |
| POSITION attribute (mandatory) | glTF 2.0 spec: primitives without POSITION are valid JSON but undrawable | consume-now | fastgltf exposes attributes via `Primitive::findAttribute` | Primitive lacking POSITION → primitive skipped + WARN with identifiers (not a file-level failure); file-level result still succeeds if other primitives are valid |
| Missing NORMAL | Plan Task 13 rule (flat-generate + warn); spec allows NORMAL absence (flat shading implied) | consume-now | Generation is importer-side (per-face flat normals); no library dependency | Fixture without normals imports; generated normals unit-length; exactly one WARN per primitive naming it |
| Missing TEXCOORD_0 | D8 requires uv0; MikkTSpace requires UVs to define tangent space | consume-now | Importer-side | Fixture without UVs imports: UVs zero-filled, MikkTSpace skipped for that primitive, tangent set to +X/w=1, one WARN naming the primitive; D8 layout invariant preserved |
| TANGENT from file (VEC4, w = handedness ±1) else MikkTSpace | D7/G2; glTF 2.0 spec `TANGENT` semantics (w is handedness) | consume-now | fastgltf reads TANGENT as VEC4; MikkTSpace generates per-corner (see §2D MikkTSpace row) | File-provided tangents pass through untouched (byte-compare on a fixture); tangent-less fixture gets MikkTSpace tangents; w component preserved/emitted as ±1 only |
| COLOR_0, TEXCOORD_1..n, JOINTS_n/WEIGHTS_n for n≥1 | D7 explicit deferral (COLOR_0/TEXCOORD_1 "recorded, not silently dropped") | log-don't-drop (ruled) | fastgltf parses all attribute sets | Fixture with COLOR_0 and TEXCOORD_1 imports; each unconsumed attribute produces one WARN naming attribute + primitive; JOINTS_0/WEIGHTS_0 (set 0) are NOT logged-skipped — they are preserved (skin row below) |
| Multi-primitive meshes → submeshes (G8) | D7 submesh model; glTF spec: per-primitive material | consume-now | fastgltf `Mesh::primitives` vector | A 2-primitive fixture yields one `MeshAsset` with 2 `Submesh` entries, distinct `MaterialHandle`s, distinct pool ranges, per-submesh AABBs; mesh-level AABB = union |
| Morph targets (POSITION/NORMAL/TANGENT deltas; mesh + node weights) | Ruled preserve-later (plan Task 9); spec "Morph Targets" section (weights applied additively) | preserve-later | fastgltf `Primitive::targets` (vector of per-target attribute sets), `Mesh::weights`, `Node::weights` (types.hpp) | A morph fixture round-trips: target count, per-target attribute presence, and default weights preserved in `MeshAsset`; unconsumed (no rendering); unit test deep-compares stored targets against the file |
| Animations: channels (target node+path), samplers (input/output accessors), interpolation LINEAR/STEP/CUBICSPLINE | Ruled preserve-later; spec `animation.sampler.schema.json`: CUBICSPLINE output count "MUST equal three times the number of input elements … an in-tangent, a spline vertex, and an out-tangent", ≥2 keyframes | preserve-later | fastgltf `Animation{channels,samplers}`, `AnimationInterpolation{Linear,Step,CubicSpline}`, `AnimationPath{Translation,Rotation,Scale,Weights}` (types.hpp) | An animation fixture (one channel per path type, one sampler per interpolation mode) round-trips: keyframe times, output values (including full 3× CUBICSPLINE triplets), interpolation enums, and channel targets preserved bit-exact in `ImportedScene`; unconsumed |
| Skins (joints, inverseBindMatrices, skeleton) | Seed 14 ruling (preserved); spec skins section | preserve-later (already ruled) | fastgltf `Skin{inverseBindMatrices, skeleton, joints}` (types.hpp) | DamagedHelmet has no skin — a skinned fixture (e.g. a two-bone strip, committed) round-trips: joint indices, IBM matrices, JOINTS_0/WEIGHTS_0 preserved in `MeshAsset::skin`; already required by plan Task 13 steps |
| Cameras (perspective: yfov/aspectRatio/znear/optional zfar; orthographic: xmag/ymag/znear/zfar) | FG2 ruling (parse-and-preserve); spec camera section | preserve-later (ruled) | fastgltf `Camera` holds `variant<Perspective, Orthographic>` (types.hpp) | Issue #2 amendment's lights+camera fixture round-trips both camera types with all parameters, including the infinite-perspective case (absent zfar) |
| Scene/node graph: default `scene` absent; multiple scenes; nodes outside any scene | glTF spec: `scene` is optional; a file may have zero scenes | consume-now (D12 flattening) | fastgltf `Asset::{scenes, defaultScene}` | `scene` present → that scene imports; absent but `scenes` non-empty → scene 0 + INFO log; zero scenes → empty `ImportedScene` + WARN (no crash); nodes not reachable from the imported scene produce no instances (count asserted) |
| Node transforms: `matrix` XOR TRS decomposition | glTF spec: node has either matrix or translation/rotation/scale | consume-now | fastgltf `Node::transform` is a `variant<TRS, matrix>` | Fixture with one matrix node and one TRS node; flattened world transforms equal precomputed references (within 1e-5); nested 3-level hierarchy composes correctly |
| Negative / non-uniform scale in flattened transforms | Flatten-at-import (D12) makes this an import-time correctness issue: negative determinant flips winding | consume-now | Importer-side (determinant check on the flattened world matrix) | A fixture with scale (-1,1,1) imports; the instance is flagged (or its winding corrected) and one WARN names the node; documented behavior — never silent inside-out rendering. AABB row below covers bounds under these transforms |
| Samplers: wrapS/wrapT 10497 REPEAT / 33071 CLAMP_TO_EDGE / 33648 MIRRORED_REPEAT; mag/min filters incl. mipmap variants | Spec sampler section (values confirmed); consumed by Task 14's sampler cache (G6) | consume-now (via Task 14) | fastgltf parses `Sampler{magFilter,minFilter,wrapS,wrapT}` | Importer carries sampler state per texture reference into material parameter sets; Task 14 test asserts glTF sampler → `VkSampler` mapping incl. all three wrap modes; absent sampler → spec defaults (REPEAT + auto filtering) |
| Material pbrMetallicRoughness core (baseColorFactor/Texture, metallicFactor, roughnessFactor, metallicRoughnessTexture, normalTexture(+scale), occlusionTexture(+strength), emissiveFactor/Texture) | D22 StandardPBR consumes exactly this set | consume-now | fastgltf `Material`/`PBRData` typed fields (types.hpp) | All listed factors/textures parse into the material parameter set; DamagedHelmet material values assert against known reference values; textures resolve in Task 14 (fallback handles D11 until then); normalTexture `scale` and occlusionTexture `strength` are carried (not dropped) |
| alphaMode OPAQUE/MASK(alphaCutoff)/BLEND; doubleSided | D22 specialization axes | consume-now | fastgltf `Material::{alphaMode, alphaCutoff, doubleSided}` | Parameter sets carry all three modes + cutoff + doubleSided; a MASK-material fixture preserves its cutoff value; consumed by Task 16 pipeline variants |
| Image MIME types image/png, image/jpeg (core) | Spec image section; Task 14 stb fallback path | consume-now (via Task 14) | fastgltf exposes `MimeType::{PNG,JPEG}` per source; stb_image already vendored (`third_party/CMakeLists.txt:189-217`, pinned commit `2c980bb`) | PNG and JPEG image sources decode through the stb path with the D10 "convert to KTX2" WARN; unsupported MIME (e.g. WebP without decoder) → checkerboard fallback + WARN naming image + MIME |
| Image/buffer source variants: external relative URI, base64 data URI, GLB bufferView | All three are core-spec (image section); DamagedHelmet ships as .gltf+URI and .glb variants | consume-now (all three) — **see Conflicts C3**: plan text lists "image-source variants" under log-don't-drop, but the phase's own exit sample requires consuming them | fastgltf `DataSource` variant: `sources::URI{fileByteOffset,uri,mimeType}`, `sources::Array` (base64 pre-decoded by fastgltf), `sources::BufferView`; verified types.hpp:1857-1903 | Three fixtures (same cube, three packagings: .gltf+external .bin/.png, .gltf fully data-URI-embedded, .glb) import with identical results (deep compare); each source variant has a dedicated test |
| GLB container edge cases | Spec GLB container format; fastgltf `Error::InvalidGLB` | consume-now (error path) | fastgltf validates GLB structure (`loadGltfBinary`) | Truncated .glb, wrong-magic .glb, and JSON-chunk-only .glb each yield a named error result (no crash, no partial registry mutation); asserted via deliberately corrupted fixtures |
| `extras` application JSON (legal on every glTF object per `glTFProperty.schema.json`) | Ruled log-don't-drop; game engines round-trip this (it is the ecosystem's app-data channel) | log-don't-drop (ruled) | fastgltf does **not** store extras: "you are expected to store any data from extras yourself" (docs/guides.rst); detection requires `Parser::setExtrasParseCallback` (+`setUserPointer`), callback receives `simdjson::dom::object*` + object `Category` + index | Importer registers the extras callback; import of a fixture with extras on a node, a mesh, and a material logs one summary line (count per object category); zero extras → zero log noise; see New gaps N3 for the preservation-API follow-up |
| Unicode / percent-encoded relative URIs | glTF spec: `uri` is percent-encoded UTF-8; real DCC exports contain spaces/non-ASCII | consume-now | fastgltf's `fastgltf::URI` percent-decodes (`URI decodedUri(uri.path())` — src/io.cpp:407); decoded UTF-8 path is then handed to OUR byte source (fastgltf's own filesystem resolution is bypassed, §2D row 2) | A fixture referencing `tex%20ürë.bin` resolves through the injected byte source on linux-native AND windows-cross-zig (UTF-8 handled on the host side of the byte-source seam); a URI escaping the asset root (`../`) is passed to the byte source verbatim with the contract that the HOST decides policy — the renderer logs the request at debug level |
| Absolute / http(s) URIs | Spec allows them; no first-tier engine fetches network URIs at import | log-don't-drop | fastgltf hands back the URI untouched when external loading is disabled | Absolute or non-file-scheme URI → fallback asset + WARN naming the URI; never a network fetch, never a crash |

## 2B. Matrix — Khronos extension registry (26 entries, all dispositioned)

Registry fetched 2026-08-18 from `KhronosGroup/glTF/extensions/2.0/Khronos/`.
"fastgltf" column = verified against the v0.9.0 `Extensions` enum
(core.hpp:140-235) and typed structs (types.hpp).

| Extension | Phase-4 disposition | fastgltf v0.9.0 support (verified) | Proposed acceptance criterion |
|---|---|---|---|
| KHR_draco_mesh_compression | **consume-now** (decode-to-open, ruled) | Parses metadata only into `Primitive::dracoCompression` (`DracoCompressedPrimitive{bufferView, attributes}`, types.hpp:2291); **does NOT decode** — zero `draco::` calls in fastgltf.cpp, no CMake dependency; fastgltf's own comparison table: "Built-in Draco decompression: ❌" (docs/overview.rst) | Importer vendors google/draco and decodes via `draco::Decoder::DecodeMeshFromBuffer` (decode.h verified); a Draco-compressed fixture imports with counts/positions equal to its uncompressed twin (tolerance = quantization step); Draco attribute-ID → glTF attribute mapping from `DracoCompressedPrimitive::attributes` honored; corrupted Draco stream → error result + fallback, no crash. Vendoring: see §2D row 9 |
| KHR_mesh_quantization | **consume-now** (decode-to-open, ruled) | Extension flag relaxes accessor validation; **no auto-dequantization** — fastgltf returns quantized component types; fastgltf accessor tools convert+denormalize on read | Enable `Extensions::KHR_mesh_quantization` in the Parser; a gltfpack-quantized (default flags) fixture imports; positions/UVs/normals converted to the D8 float format via fastgltf tools; vertex data equals the unquantized twin within quantization tolerance (positions: dequantization grid step; normals/UVs: 1/(2^bits−1)) |
| KHR_meshopt_compression (Khronos RC successor to EXT_) | log-don't-drop + **New gap N1** | **NOT supported** by fastgltf v0.9.0 (no enum bit; verified absent from core.hpp) | File listing it in `extensionsRequired` → fastgltf `Error::UnknownRequiredExtension` → named error + fallback assets (D11), log names the extension; in `extensionsUsed` only → generic extension logging row (below) covers it |
| KHR_texture_basisu | **consume-now** (via Task 14) | Parsed: `Texture::basisuImageIndex` points at the KTX2 image; image bytes returned undecoded (fastgltf never decodes images) | Texture with basisu source routes its KTX2 payload to Task 14's TextureCache; a .gltf using KHR_texture_basisu + GLB-embedded KTX2 renders in sample 08 (pixel gate) |
| KHR_lights_punctual | **preserve-later** (FG2, ruled) | Parsed: `fastgltf::Light{type: Directional/Spot/Point, color, intensity, range, inner/outerConeAngle, name}`; `Node::lightIndex` (types.hpp:2772) | Issue #2 amendment's fixture round-trips all three light types with full parameter sets into `ImportedScene`; spot cone angles preserved; unconsumed until techniques phase |
| KHR_texture_transform | log-don't-drop (ruled) — **see Conflicts C4**: gltfpack emits it BY DEFAULT with quantized UVs, so log-only breaks UV correctness on the exact content class the compression ruling targets | Parsed: `TextureTransform{rotation, uvOffset, uvScale, texCoordIndex}` on `TextureInfo::transform` (types.hpp:2377) | Per the standing ruling: WARN naming material + texture + the transform values when present. If the coordinator re-rules per C4: consume offset/scale (rotation may stay logged) as a per-texture parameter-set field; acceptance then = gltfpack-default cube renders with correct UVs vs the unquantized twin (pixel gate) |
| KHR_materials_unlit | log-don't-drop (ruled: "every KHR_materials_* beyond core") — **see Conflicts C5** (D22 ships an Unlit material; mapping is near-free) | Parsed: `Material::unlit` bool | Per ruling: WARN naming the material. If re-ruled: material maps to the D22 Unlit shader; fixture renders unlit (pixel gate) |
| KHR_materials_emissive_strength | log-don't-drop (ruled) | Parsed for free: `Material::emissiveStrength` (plain field, default 1.0) | WARN naming material + strength value when ≠ 1.0; row notes the value is already in the parsed material if the techniques phase wants it preserved-in-parameter-set at zero cost |
| KHR_materials_specular | log-don't-drop (ruled) | Parsed: `MaterialSpecular` → `Material::specular` | One WARN per material naming the extension (shared criterion for all log-ruled KHR_materials_* rows: the WARN fires once per material+extension pair, includes material name/index, and a fixture carrying the extension proves it in tests) |
| KHR_materials_ior | log-don't-drop (ruled) | Parsed: `Material::ior` (default 1.5) | Shared KHR_materials_* log criterion |
| KHR_materials_transmission | log-don't-drop (ruled) | Parsed: `MaterialTransmission` | Shared criterion + the WARN states the material will render opaque (transmission ignored) — the visually-loudest of these gaps deserves the explicit message |
| KHR_materials_volume | log-don't-drop (ruled) | Parsed: `MaterialVolume` | Shared criterion |
| KHR_materials_sheen | log-don't-drop (ruled) | Parsed: `MaterialSheen` | Shared criterion |
| KHR_materials_clearcoat | log-don't-drop (ruled) | Parsed: `MaterialClearcoat` | Shared criterion |
| KHR_materials_iridescence | log-don't-drop (ruled) | Parsed: `MaterialIridescence` | Shared criterion |
| KHR_materials_anisotropy | log-don't-drop (ruled) | Parsed: `MaterialAnisotropy` | Shared criterion |
| KHR_materials_dispersion | log-don't-drop (ruled) | Parsed: `Material::dispersion` | Shared criterion |
| KHR_materials_diffuse_transmission | log-don't-drop (ruled) | Parsed: `MaterialDiffuseTransmission` | Shared criterion |
| KHR_materials_variants | log-don't-drop (ruled) | Parsed: `Primitive::mappings` (variant→material indices) | WARN names the variant count; default material (the primitive's own `material`) is used |
| KHR_animation_pointer | log-don't-drop | NOT supported by fastgltf v0.9.0 (no enum bit) | Covered by the generic extensionsUsed/Required logging row (§2C last row); noted for the animation phase (New gap N4) |
| KHR_gaussian_splatting | log-don't-drop | NOT supported (no enum bit) | Generic logging row; an entirely different rendering paradigm — no Phase-4 retrofit risk |
| KHR_interactivity | log-don't-drop | NOT supported | Generic logging row |
| KHR_node_visibility / KHR_node_hoverability / KHR_node_selectability | log-don't-drop | NOT supported (no enum bits) | Generic logging row; visibility is scene-API territory (Stage 2 layers cover the renderer-side equivalent) |
| KHR_xmp_json_ld | log-don't-drop | NOT supported | Generic logging row (metadata-only; no rendering impact) |
| **Archived** (KHR_materials_pbrSpecularGlossiness, KHR_techniques_webgl, KHR_xmp) | log-don't-drop | pbrSpecularGlossiness parse exists but is gated behind `FASTGLTF_ENABLE_DEPRECATED_EXT` (default OFF); other two unsupported | Build fastgltf with the deprecated gate OFF; archived extensions hit the generic logging row; a specGloss-only asset renders with fallback material + WARN recommending re-export (Khronos archived the extension) |

## 2C. Matrix — Vendor / multi-vendor extensions (31 registry entries)

| Extension | Phase-4 disposition | fastgltf v0.9.0 support (verified) | Proposed acceptance criterion |
|---|---|---|---|
| EXT_meshopt_compression | **consume-now** (decode-to-open, ruled) | Parses metadata only: `BufferView::meshoptCompression` → `CompressedBufferView{buffer, byteOffset, byteLength, byteStride, count, mode, filter}`; `MeshoptCompressionMode{Attributes,Triangles,Indices}`, `MeshoptCompressionFilter{None,Octahedral,Quaternion,Exponential}` (types.hpp:2737); **does NOT decode** (zero `meshopt_decode*` calls in fastgltf.cpp; no dependency) | Importer decodes via meshoptimizer: mode→function `Attributes`→`meshopt_decodeVertexBuffer`, `Triangles`→`meshopt_decodeIndexBuffer`, `Indices`→`meshopt_decodeIndexSequence`; filters via `meshopt_decodeFilterOct/Quat/Exp` (all verified present in `src/meshoptimizer.h` lines 290-424). A `gltfpack -cc` cube imports identical (deep compare) to its uncompressed twin; fallback-buffer semantics honored (the extension's fallback buffer is NOT read when decoding — fastgltf marks it `sources::Fallback`); corrupted compressed stream → decode failure → error result + fallback, no crash |
| EXT_mesh_gpu_instancing | log-don't-drop + **New gap N2** (cheap consume-at-import candidate: D12 flattening could expand instance TRS attributes into InstanceRecords) | Supported: parsed by fastgltf (enum bit `EXT_mesh_gpu_instancing`) | Per current ruling: WARN naming node + instance count — the WARN must state the file will render ONE instance instead of N (the silently-wrong-looking case); fixture proves it. N2 records the consume proposal for coordinator ruling |
| EXT_texture_webp | log-don't-drop | Parsed: `Texture::webpImageIndex`; image bytes undecoded; stb_image (vendored) has no WebP decoder | WebP-only texture → checkerboard fallback + WARN naming image and MIME; if the texture also carries a core PNG/JPEG fallback source (per that extension's design), the fallback source is used and logged at INFO |
| EXT_texture_astc | log-don't-drop | NOT supported (no enum bit) | Generic logging row |
| MSFT_texture_dds | log-don't-drop | Parsed (`ddsImageIndex`) but no DDS decode path in Phase 4 | Same criterion as EXT_texture_webp (fallback source honored if present, else checkerboard + WARN) |
| MSFT_packing_normalRoughnessMetallic / MSFT_packing_occlusionRoughnessMetallic | log-don't-drop | Enum bits exist (parse-level) | Generic logging row |
| EXT_lights_image_based | log-don't-drop | NOT supported | Generic logging row; FG1 (IBL) is the registered techniques-phase consumer |
| EXT_lights_ies | log-don't-drop | NOT supported | Generic logging row |
| EXT_mesh_manifold, EXT_mesh_primitive_edge_visibility, EXT_mesh_primitive_restart | log-don't-drop | NOT supported | Generic logging row (manifold is a marker; restart/edge-visibility affect non-triangle rendering out of Phase-4 scope) |
| MPEG_* (9: accessor_timed, animation_timing, audio_spatial, buffer_circular, media, mesh_linking, scene_dynamic, texture_video, viewport_recommended) | log-don't-drop | NOT supported | Generic logging row (streamed-media scene profile; out of product scope by design — renderer middleware, no media stack) |
| ADOBE_materials_* (3), AGI_articulations, AGI_stk_metadata, CESIUM_primitive_outline, FB_geometry_metadata, GRIFFEL_bim_data, NV_materials_mdl | log-don't-drop | NOT supported | Generic logging row |
| GODOT_single_root | log-don't-drop | Supported (enum bit) — layout hint only | Generic logging row; D12 flattening is unaffected by root-count layout hints |
| **Generic extension surfacing (mechanism row)** | consume-now (the logging machinery itself) | fastgltf exposes `Asset::extensionsUsed`/`extensionsRequired` string lists; unknown extension in `extensionsRequired` → `Error::UnknownRequiredExtension` ("extension required by glTF is not supported by fastgltf at all" — core.hpp:75-95) | Importer logs, once per import at INFO, the full `extensionsUsed` list with each entry's disposition tag (consumed / preserved / logged / unknown); an unknown entry in `extensionsRequired` yields a named error + D11 fallbacks (tested with a synthetic fixture); an unknown entry in `extensionsUsed` only never fails the import (tested). This one mechanism is what makes "log-don't-drop" true for ALL current and future registry entries, including everything above marked "generic logging row" |

## 2D. Matrix — pipeline, IO abstraction, invariants, error handling

| Feature | Precedent / ruling | Disposition | Library support (verified) | Proposed acceptance criterion |
|---|---|---|---|---|
| 1. fastgltf vendoring | Plan global constraint: pinned tags, license + tag in vendoring commit, windows-cross-zig verified in-task | consume-now | **v0.9.0** (2025-07-08) is the pin candidate; MIT (LICENSE.md: "Copyright (c) 2022 - 2025 Sean Apeler"); embeds simdjson (Apache-2.0 — record BOTH licenses); C++17 default with `FASTGLTF_COMPILE_AS_CPP20=ON` for this repo (options.rst); release notes state v0.9.0 is "likely the last version … for C++17" | Vendored at tag v0.9.0 with `FASTGLTF_COMPILE_AS_CPP20=ON`; MIT + embedded simdjson Apache-2.0 recorded in the vendoring commit; both presets (linux-native, windows-cross-zig) build in the adopting task |
| 2. Byte-source abstraction (IO-source invariant, 2026-08-12 + D25-adjacent amendment) | Issue #2: "host-injectable byte source … fastgltf already supports callback-based buffer/URI loading; use it" — **see Conflicts C2: only half true** | consume-now | fastgltf v0.9.0: `GltfDataGetter` is an abstract interface (core.hpp:569 — `read/reset/bytesRead/totalSize`) the host can subclass; `GltfDataBuffer::FromBytes/FromSpan` give pure in-memory loads. **BUT external URI resolution is hardcoded** to `std::filesystem`+`std::ifstream` in the private `Parser::loadFileFromUri` (src/io.cpp:407) with NO override hook — the only callbacks are `setBufferAllocationCallback` (where decoded bytes land, not how they are read) and `setBase64DecodeCallback` | The importer (a) feeds the document through a `GltfDataGetter` implementation backed by the renderer's byte-source interface, (b) NEVER sets `Options::LoadExternalBuffers`/`Options::LoadExternalImages`, and (c) resolves every `sources::URI` itself through the same byte source. Acceptance tests: import of the external-URI cube with a spy byte source asserts every byte request went through it (zero direct filesystem calls — enforced by a test-double that fails on miss); the path-taking convenience overload is a thin wrapper over a filesystem-backed byte source |
| 3. In-memory-source import (amendment requirement) | Issue #2 amendment: "in-memory-source import test required" | consume-now | `GltfDataBuffer::FromBytes(const std::byte*, size_t)` verified at v0.9.0 | A .glb held entirely in memory (no file on disk anywhere) imports successfully with results deep-equal to the file-based import of the same bytes |
| 4. Error taxonomy for malformed files | fastgltf `Error` enum, 15 members verified (core.hpp:75-95): None, InvalidPath, MissingExtensions, UnknownRequiredExtension, InvalidJson, InvalidGltf, InvalidOrMissingAssetField, InvalidGLB, MissingField, MissingExternalBuffer, UnsupportedVersion, InvalidURI, InvalidFileData, FailedWritingFiles, FileBufferAllocationFailed; `getErrorName`/`getErrorMessage` exist | consume-now | As left | `ImportResult` carries a named error mapping for every fastgltf::Error member (exhaustive switch — compiler-enforced); malformed-file test battery: not-JSON garbage, valid-JSON-invalid-glTF, truncated GLB, unsupported `asset.version`, invalid URI — each yields error result + log via the public sink + zero registry mutation + no crash; `getErrorName` included in the log line |
| 5. MikkTSpace integration (G2) | D7 commits MikkTSpace ("industry-standard reference implementation") | consume-now | Repo has NO tags/releases/LICENSE file; license = zlib-style text ONLY in source headers ("This software is provided 'as-is' … 1. The origin … must not be misrepresented…"); GitHub's license API reports null — vendoring commit must quote the header text. Interface is per-face-corner: `m_getPosition/Normal/TexCoord(face, vert)`; output doc-comment: "the results are returned unindexed. … averaging/overwriting tangent spaces by using an already existing index list WILL produce INCORRECT results. DO NOT! use an already existing index list." Thread-safe per its own header ("these are both thread safe!"); direct malloc/free (~15 sites, no allocator hooks); degenerate triangles inherit neighbor tangents (MARK_DEGENERATE path in mikktspace.c) | Pipeline order is deindex → MikkTSpace per-corner tangents → `meshopt_generateVertexRemap` over the FULL 48-byte vertex (tangent included) → remap → cache → overdraw → fetch; a test asserts tangent-welded vertex count ≥ the position-welded count (proves tangents participated in dedup, i.e. the forbidden fold-back never happened); degenerate-UV fixture imports without NaN tangents; vendoring commit records the header license text + pinned commit hash (no tag exists) |
| 6. meshoptimizer optimization sequence | meshoptimizer v1.2 README "Core pipeline": "to maximize rendering efficiency you should typically feed it through a set of optimizations (**the order is important!**): 1. Indexing 2. Vertex cache … 3. (optional) Overdraw … 4. Vertex fetch …"; `optimizeOverdraw` requires cache-optimized input; `optimizeVertexFetch` "has to be performed on the final index buffer" | consume-now | All six functions verified in v1.2 README with signatures; README example uses overdraw threshold **1.05** (the in-repo research file says 1.01 — see Conflicts C6) | remap→cache→overdraw(1.05)→fetch runs per primitive in exactly that order; the existing plan test ("meshopt actually ran — index order differs from source") plus a vertex-count assert (dedup happened); threshold documented as the README's 1.05 unless benchmarked otherwise |
| 7. meshoptimizer vendoring | Plan Task 9 text: "meshoptimizer's decode is already vendored" — **FALSE, see Conflicts C1** | consume-now | v1.2 (2026-06-30) is the pin candidate; MIT; NOT header-only — `src/meshoptimizer.h` + 20 `.cpp` files; v1.2 release notes: core-library work "sponsored by Valve" | Vendored at v1.2 in the same task; license + tag recorded; both presets build; the decode + optimization + (optional) tangent functions all come from this single dependency |
| 8. EXT_meshopt decode function set | Extension spec modes/filters (KhronosGroup/glTF Vendor/EXT_meshopt_compression) map 1:1 to meshoptimizer decoders | consume-now | `meshopt_decodeVertexBuffer` (meshoptimizer.h:396), `decodeIndexBuffer` (:290), `decodeIndexSequence` (:318), `decodeFilterOct/Quat/Exp` (:421-423) all verified present; mode↔function correspondence is an inference from matching spec bitstream text to function docs (the spec names no C functions) — flagged, high confidence | Covered by §2C EXT_meshopt_compression row; additionally: a fixture per mode (Attributes/Triangles/Indices) and per filter (Oct/Quat/Exp) decodes correctly — gltfpack can generate all of these from one source asset with documented flags |
| 9. Draco vendoring | "Prefer ready-made libraries" (CLAUDE.md); needed only for KHR_draco decode | consume-now | Apache-2.0 (LICENSE verified); C++17 (`CMakeLists.txt:17`); full CMake support; **NO decode-only build option exists** — full option list from `cmake/draco_options.cmake` verified: closest levers are `DRACO_GLTF_BITSTREAM` (restricts feature set to the glTF Draco profile, still builds enc+dec) and domain toggles `DRACO_MESH_COMPRESSION`/`DRACO_POINT_CLOUD_COMPRESSION`; internal `*_dec`/`*_enc` object libraries exist but are not separately consumable; transcoder deps (eigen/tinygltf) gated behind `DRACO_TRANSCODER_SUPPORTED` (default OFF) and NOT needed for `Decoder::DecodeMeshFromBuffer` | Vendor with `DRACO_GLTF_BITSTREAM=ON`, tests/executables/transcoder OFF; accept encoder objects in the static lib and rely on `-ffunction-sections`+`--gc-sections` dead-stripping (measured binary-size delta recorded in the vendoring commit); windows-cross-zig build verified in-task (C++17 under zig cross — same bar every dependency passed) |
| 10. AABB computation (G11) | D7: AABB from FINAL (post-meshopt) positions; glTF accessor min/max exists but files lie (min/max is not revalidated by loaders) | consume-now | Importer-side; no library | AABBs computed from post-optimization vertex data, never trusted from accessor min/max; unit tests: cube AABB exact; single-point primitive (min==max) valid; zero-vertex primitive skipped + WARN; non-finite positions (NaN/Inf) → primitive rejected + WARN naming it (fed by a corrupt fixture); instance-level world AABB transforms all 8 corners (correct under rotation and negative scale — test at 90° + scale(-1,1,1)) |
| 11. D24 eviction invariant | D24 (spec) + issue #2 amendment; `rx_core` generational `Handle`/`HandlePool` already delivered (handle.h:8-72: index+generation, isLive checks generation) | consume-now | HandlePool::get returns nullptr on dead handle (handle.h:50-55) — the residency-tolerant pattern extends this to "live but nonresident → fallback" | Registry resolve (`mesh()/material()/texture()`) yields the D11 fallback while an asset is nonresident (never null-deref, never assert); all public returns are handles or short-lived const refs whose lifetime is documented as "until the next registry mutation" — no stored raw pointers/indices in any consumer-facing type (review-blocking criterion); one manual deferred-eviction test: evict-by-handle → resolve yields fallback → re-import → resolve yields real asset, all draws in between render fallback without validation errors |
| 12. D25 UploadTicket consumption | D25 (spec) + issue #28: `flush()` today ends in unconditional `vkWaitForFences(..., UINT64_MAX)` (upload.cpp:278, verified) | consume-now | Task 11 delivers the ticket API before this task (sequencing in plan) | Every geometry/texture upload in the importer consumes `UploadTicket` (poll or explicit wait at a documented point — sync import may wait ONCE at the end, not per-upload); test asserts import of an N-primitive file produces ≤1 blocking wait on the sync path; async path never calls `wait()` (issue #22 matrix) |
| 13. Fallback assets on failure (D11) | D11: "a bad asset must be visible and named, never a crash and never silently absent" | consume-now | Registry-owned defaults (D11) | Missing file → fallback + ERROR log naming path; garbage file → error result + no partial registry state; missing texture reference → checkerboard + WARN; per-failure-mode tests enumerated in row 4 |
| 14. Test content (D16) | Committed tiny cube (<20 KB) + fetch_assets.sh (DamagedHelmet mandatory, --sponza local-only), checksums, CI-cached | consume-now | glTF-Sample-Assets: DamagedHelmet CC BY 4.0, Sponza CC BY 4.0 (licenses recorded in fetch script per D16) | Cube committed and hand-authored (readable JSON); fetch script verifies checksums; CI caches like slang-prebuilt; CC BY attribution text embedded in the script; the extension fixtures added by this matrix (sparse, morph, animation, skin, lights/camera, quantized, meshopt-compressed, draco-compressed, per-source-variant cubes) are either hand-authored-committed or generated by documented gltfpack invocations in the fetch script |
| 15. Threading placement of import stages | D5 / docs/threading.md "Worker-allowed": "Parse/decode/transcode/optimize (fastgltf parsing, libktx Basis transcode, meshoptimizer passes, MikkTSpace tangent generation — Stage 1)" | consume-now | rx_task delivered: `parallelFor`, `postToMain`/`pumpMain` (guarded), IO pinned-task thread (scheduler.cpp) | Sync `importGltf` parallelizes per-primitive CPU work internally (plan Task 15 note: "parallelism is the default, not an async-only property"); GPU-object mutation stays on the calling main thread; MikkTSpace's documented thread-safety (row 5) permits per-primitive parallel generation |

---

## 3. Conflicts (coordinator adjudicates; both sides quoted)

- **C1 — "already vendored" claim is false.** Plan Task 9 (line 159-161):
  "compression is non-negotiable: EXT_meshopt_compression (gltfpack's
  output, the de-facto shipping format; **meshoptimizer's decode is
  already vendored**)". Repo reality at HEAD `bf5b853`:
  `third_party/CMakeLists.txt` enumerates spdlog, doctest, glm,
  Vulkan-Headers, SDL3, volk, VMA, stb, vk-bootstrap, enkiTS, Tracy —
  no meshoptimizer; `find . -iname "meshopt*"` and a git-log search
  produce only docs references. meshoptimizer is a commitment
  (registry: "the committed library"), not a vendored artifact. Task 13
  must budget the actual vendoring (20 .cpp files, v1.2).
- **C2 — issue #2's fastgltf-IO claim is half true.** Issue body:
  "fastgltf already supports callback-based buffer/URI loading; use it."
  Verified at v0.9.0: the DOCUMENT bytes are fully abstractable
  (`GltfDataGetter` interface, `FromBytes`/`FromSpan`), but **external
  URI resolution has no callback** — `Parser::loadFileFromUri`
  (src/io.cpp:407) hardcodes `std::filesystem`/`std::ifstream` when
  `Options::LoadExternalBuffers/LoadExternalImages` is set, and the only
  registered callbacks (`setBufferAllocationCallback`,
  `setBase64DecodeCallback`) do not intercept reads. The invariant is
  still fully implementable — by NOT setting those options and resolving
  `sources::URI` renderer-side (matrix §2D row 2) — but the acceptance
  criteria must mandate that specific pattern, not a nonexistent
  fastgltf URI callback.
- **C3 — image-source variants: log-don't-drop vs. the exit sample.**
  Plan Task 9 (line 166-168) lists "image-source variants (external-URI /
  data-URI / .glb-bufferView)" in the log-never-drop set. But sample 08's
  own gate imports DamagedHelmet (external-URI form) and D16/Task 14
  content includes .glb packaging — the phase cannot exit without
  CONSUMING at least external-URI and GLB-bufferView image sources, and
  fastgltf hands data-URI sources back pre-decoded (`sources::Array`) so
  consuming all three is uniform. Proposed re-ruling: consume-now for all
  three source variants (matrix §2A row), reserving log-don't-drop for
  absolute/network URIs only.
- **C4 — KHR_texture_transform log-only vs. gltfpack defaults.** Ruling
  (plan Task 9): KHR_texture_transform is log-don't-drop. Verified from
  gltfpack's README: "KHR_texture_transform (used **by default** when
  textures are present, unless disabled via `-noq` or `-vtf`)" alongside
  "KHR_mesh_quantization (used by default…)". The decode-to-open ruling
  names gltfpack output "the de-facto shipping format" — but a
  gltfpack-default file whose UV transform is logged-not-applied renders
  with wrong UVs (quantized texcoords are scaled/offset via exactly this
  extension). Either (a) consume offset/scale now (rotation may remain
  logged), or (b) require `-vtf`-style full-float UVs in supported
  content and log otherwise — (a) is one multiply-add per UV in the
  importer or material parameter set. Both sides recorded; not resolved
  here.
- **C5 — KHR_materials_unlit vs. D22's Unlit material.** Ruling: every
  KHR_materials_* beyond core is log-don't-drop. D22 simultaneously ships
  an Unlit material as a first-class module. fastgltf parses the
  extension to a plain bool. Mapping `unlit=true` → the D22 Unlit
  material at import is a one-line disposition change with real content
  payoff (unlit assets are common in stylized/mobile-authored content).
  Recorded for adjudication.
- **C6 — overdraw threshold discrepancy.** In-repo research
  (`research-p4-assets.md:189`) says "typical 1.01"; meshoptimizer v1.2's
  own README example uses 1.05. Cosmetic, but the hardened ticket should
  pick one (README's 1.05 suggested) and say why.
- **C7 — stale version facts in the fact-source research file.**
  `research-p4-assets.md` (a designated fact source) claims fastgltf
  v0.9.0 "released July 8, 2024" (actual: 2025-07-08), meshoptimizer
  "v1.2 (released June 30, 2024)" (actual: 2026-06-30; June 2024's tag
  was v0.21), and cites both as "current" as of 2026-08-11 without
  version-scheme sanity checks. The versions themselves happen to still
  be current; the dates and the C++17/C++20 claims ("v0.10+ will require
  C++20" — no v0.10 exists) should not be propagated into ticket text.

## 4. New gaps (absent from the entire planning universe; checked against the master registry deferred list, FG1-FG12, and the phase spec)

- **N1 — KHR_meshopt_compression (Khronos ratification-track successor to
  EXT_meshopt_compression).** Not supported by fastgltf v0.9.0; mutually
  exclusive with the EXT_ form per its own spec. If gltfpack's default
  output migrates, the "de-facto shipping format" ruling silently drifts.
  Proposed fit: registry watch item tied to the fastgltf pin — re-check at
  the streaming phase's dependency refresh; no Phase-4 work beyond the
  clean UnknownRequiredExtension error path (§2B row).
- **N2 — EXT_mesh_gpu_instancing consume-at-import.** Nowhere in the
  planning universe (D26.3's instancing collapse is draw-submission-side,
  a different mechanism). fastgltf already parses it; D12 flattening could
  expand per-node instance TRS arrays into InstanceRecords nearly for
  free, and gltfpack emits the extension for instanced content. Proposed
  fit: Phase 4 Stage 1 cheap-consume if the coordinator accepts the scope
  (est. small: one expansion loop), else geometry phase with the
  log-don't-drop WARN of §2C in the interim.
- **N3 — `extras` preservation/exposure contract for host engines.** The
  log-don't-drop ruling covers detection; no artifact anywhere plans an
  API through which a HOST receives its own application JSON (game teams'
  metadata — FG3-adjacent, since dynamic-content hosts are exactly who
  embeds extras). fastgltf's `setExtrasParseCallback` makes preservation
  cheap when wanted. Proposed fit: SDK-phase ABI projection (same vehicle
  as FG3's contract).
- **N4 — KHR_animation_pointer / KHR_interactivity watch item for the
  animation phase.** The animation-phase registry entry (seed 14:
  consumer-side playback or ozz-animation; renderer contract = joint
  palettes) predates both extensions' registry presence; neither is named
  anywhere. Proposed fit: one registry line so the animation-phase spec
  is obligated to disposition them; no Phase-4 work.

## 5. Verification health

**Verified first-hand this session (2026-08-18):** fastgltf v0.9.0 tag
date, license, C++ standard, full `Extensions` enum, `Error` enum,
`GltfDataGetter`/`GltfDataBuffer` API, `sources::*` variant set,
extras callback contract, and the absence of meshopt/draco decode (raw
source + docs fetches); meshoptimizer v1.2 tag date (GitHub API — a
summarizer-year artifact "June 2024" was caught and corrected against
the raw API), README pipeline order and example threshold, decode
function set with header line numbers; MikkTSpace license text, callback
interface, output-indexing prohibition, thread-safety note,
degenerate handling (raw source); Draco license, C++ standard, decoder
API, full CMake option list (raw source); glTF 2.0 sparse-accessor and
CUBICSPLINE semantics (Khronos JSON schemas via GitHub API);
extension-registry enumeration (60 extensions via GitHub API); gltfpack
default-extension behavior (README); repo state at HEAD `bf5b853`
(dependency inventory, upload.cpp:278, handle.h, threading.md).

**Inferred (flagged, high confidence):** the EXT_meshopt mode↔decoder
function mapping (the extension spec describes bitstreams without naming
C functions; meshoptimizer's README claims the extension wraps its
codec). The claim that an unknown extension in `extensionsUsed` (not
`extensionsRequired`) does not fail fastgltf parsing is inferred from
the `Error::UnknownRequiredExtension` semantics ("extension **required**
by glTF…") — the hardened ticket's test battery (§2C mechanism row)
proves it empirically rather than trusting the inference.

**Dead links / access issues:** `docs.fastgltf.dev` (named in the
research brief) does not resolve — fastgltf.readthedocs.io is the live
docs site. `registry.khronos.org/glTF/specs/2.0/glTF-2.0.html` returned
HTTP 403 to this session's fetches; core-spec facts were taken from the
repo's `Specification.adoc` and JSON schemas instead (equivalent,
authoritative source).

**Version ambiguities:** MikkTSpace has no tags — pin a commit hash
(HEAD `mikktspace.c` last touched 2020-03-25). fastgltf v0.9.0's release
notes signal a C++20-only future major; irrelevant to this repo (C++20
already) but the pin should not be blindly bumped past v0.9.0 without
re-verifying the loading API, which has a history of breaking changes
(the `GltfDataBuffer`→`GltfDataGetter` reshape happened at v0.8).
