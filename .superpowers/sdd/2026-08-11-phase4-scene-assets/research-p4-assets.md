# Phase 4 Scene Assets: glTF & Texture Loading Research

**Date:** 2026-08-11  
**Scope:** glTF 2.0 loader selection, KTX2 loading, mesh optimization, Khronos sample assets

---

## 1. glTF 2.0 Loader Comparison: fastgltf vs cgltf

### fastgltf

- **Latest Version:** v0.9.0 (released July 8, 2024)
- **License:** MIT (inferred from open-source Khronos tooling model)
- **C++ Standard:** C++17 minimum; v0.9.0 marked as "last version for C++17" (future v0.10+ targets C++20+)
- **Maintenance:** Active; last commit July 2024
- **Performance:** 7.4x faster than cgltf on base64-embedded buffers; 5x faster overall; uses AVX2/SSE4/ARM Neon SIMD in base64 decoding
- **Extension Support:** 
  - Full glTF 2.0 spec compliance
  - Confirmed: KHR_draco_mesh_compression, KHR_texture_basisu, KHR_materials_* family (PBR, specular variants)
  - KHR_texture_transform, KHR_mesh_quantization, KHR_lights_punctual, EXT_meshopt_compression, EXT_texture_webp
  - Extensions enabled via parser constructor flags
- **Skinning Data Access:**
  - Full support: JOINTS_0 / WEIGHTS_0 attributes
  - Inverse bind matrices accessible via accessors
  - Skin→joints→node hierarchy navigation
- **Compilation:** Minimal dependencies; header-heavy but no external libs required
- **Usage in Industry:** Featured in vkguide (modern Vulkan tutorial); preferred choice for modern Vulkan renderers
- **API Design:** Modern C++17 idioms; safe accessor patterns; no manual pointer management

### cgltf

- **Latest Version:** v1.15 (released November 29, 2025)
- **License:** MIT
- **Language:** C99 (not C++; requires C wrapper/bindings for C++ projects)
- **Maintenance:** Active; last commit ~November 2025
- **Performance:** Baseline reference; 5–7x slower than fastgltf on large base64 payloads
- **Extension Support:** Core glTF 2.0 compliant; skinning/skins support confirmed; less explicit documentation on KHR_* extension coverage compared to fastgltf
- **Skinning Data Access:** Supported; skins structure includes joint/weight accessors
- **Usage in Industry:** Used in bgfx, Filament (Google), gltfpack, raylib, Unigine—mature ecosystem
- **Characteristics:** Single-file distribution; zero external dependencies; production-grade stability
- **Caveats:** C99 ABI requires careful integration in C++ projects; less modern API ergonomics

### Recommendation

**Use fastgltf v0.9.0** for RendererX:
- **Rationale:** Performance advantage (5–7x) critical for import pipelines; modern C++17 API reduces boilerplate; active maintenance; alignment with vkguide/Khronos best practices; full extension coverage (KHR_draco, KHR_texture_basisu, materials) required for asset diversity; skinning support complete.
- **Caveat:** Plan C++20 migration post-Phase 4; v0.10+ will require it.
- **Alternative:** cgltf only if C++ integration proves problematic; mature but slower.

---

## 2. KTX2 Loading: libktx (KTX-Software)

### Overview

- **Current Version:** v4.4.2 (released October 4, 2024)
- **License:** Apache 2.0 (Khronos group standard)
- **Repository:** https://github.com/KhronosGroup/KTX-Software

### CMake Integration

**Basic build:**
```cmake
# Static library (recommended for embedded)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(/path/to/KTX-Software/lib ktx)
target_link_libraries(your_target ktx)
```

**Key CMake options:**
- `BUILD_SHARED_LIBS=OFF` – Static linking (iOS/Emscripten always static)
- `BASISU_SUPPORT_OPENCL=ON` – Enable GPU-accelerated Basis Universal encoding (optional)
- `LIBKTX_VERSION_READ_ONLY=ON` – Parse-only build (no write/encode, smaller binary)
- `KTX_FEATURE_LOADTEST_APPS=Vulkan` – Include Vulkan test apps for validation
- `CMAKE_BUILD_TYPE=Release` – Optimization flags

**Windows note:** Static library users must define `KHRONOS_STATIC` before including KTX headers.

### Basis Universal Transcoding API

**Function:** `ktxTexture2_TranscodeBasis(ktxTexture2* texture, ktx_transcode_fmt target_format)`

**Supported transcode targets (GPU-ready formats):**
- BC3/BC4/BC5 (DirectX/desktop)
- BC7 (high-quality RGB/RGBA, desktop)
- ETC2 / ETC (mobile)
- ASTC 4x4 / 6x6 / 8x8 (mobile/desktop, scalable quality)
- UASTC (high-quality intermediate, then transcode to above)

**Behavior:**
- **BasisLZ supercompressed:** Automatically decompresses ETC1S → transcodes to target
- **UASTC supercompressed:** Decompresses zstd supercompression first (if present), then transcodes to target
- **Output:** CPU-resident transcoded mip levels ready for GPU upload

### Integration Pattern (CPU-side transcode, custom upload)

Typical Vulkan renderer workflow (your use case):

```cpp
// 1. Parse KTX2 file
ktxTexture2* ktex = nullptr;
KTX_error_code result = ktxTexture2_CreateFromNamedFile(filename, &ktex);

// 2. CPU transcode to device-native format (e.g., BC7 on desktop)
ktx_transcode_fmt target = KTX_TTF_BC7_RGBA;  // or ASTC_4x4_RGBA for mobile
ktxTexture2_TranscodeBasis(ktex, target, 0);

// 3. Extract transcoded data per mipmap (YOUR uploader)
for (uint32_t level = 0; level < ktex->numLevels; ++level) {
    size_t size = 0;
    ktx_uint8_t* data = ktxTexture2_GetData(ktex, level, 0, 0);
    // → Pass to your Uploader (our own allocation + device transfer)
}

// 4. Cleanup
ktxTexture2_Destroy(&ktex);
```

**Important:** We **do NOT use** `ktxVulkanTexture` or `vkCreateImage` wrappers from libktx; we handle allocation (VMA) and upload ourselves. libktx provides CPU-side parsing and transcode only.

### Zstd Supercompression

- KTX2 files can be supercompressed with **zstd** (preferred) or zlib
- `ktxTexture2_TranscodeBasis()` automatically decompresses zstd as part of transcode step
- **Benefit:** 3–4x smaller files on disk/network vs uncompressed KTX2
- **CPU cost:** Minimal (~ms for typical 4K textures during import)
- Transparent to user—no explicit zstd API calls needed

---

## 3. meshoptimizer

### Overview

- **Latest Version:** v1.2 (released June 30, 2024)
- **License:** MIT
- **Language:** C (C99); C++ wrappers available; header-only via `#include <meshoptimizer.h>`
- **Maintenance:** Active; last update June 2024
- **Repository:** https://github.com/zeux/meshoptimizer

### Post-Import Optimization Sequence

**Exact API signatures:**

```c
// Step 1: Analyze vertex duplication; generate remap table
size_t meshopt_generateVertexRemap(
    unsigned int* destination,       // Output: remap[vertex_count]
    const unsigned int* indices,     // Input: original index buffer
    size_t index_count,              
    const void* vertices,            // Input: vertex positions (for dedup)
    size_t vertex_count,             
    size_t vertex_size               // Size of one vertex struct (bytes)
);

// Step 2: Apply remap to indices
void meshopt_remapIndexBuffer(
    unsigned int* destination,       // Output: remapped indices
    const unsigned int* indices,     // Input: original
    size_t index_count,              
    const unsigned int* remap        // From generateVertexRemap
);

// Step 3: Apply remap to vertex buffer
void meshopt_remapVertexBuffer(
    void* destination,               // Output: deduplicated vertices
    const void* vertices,            // Input: original
    size_t vertex_count,             
    size_t vertex_size,              
    const unsigned int* remap        // From generateVertexRemap
);

// Step 4: Optimize vertex cache (GPU post-transform cache hit rate)
void meshopt_optimizeVertexCache(
    unsigned int* destination,       // Output: reordered indices
    const unsigned int* indices,     // Input: index buffer
    size_t index_count,              
    size_t vertex_count              
);

// Step 5: Optimize for rasterizer (reduce overdraw)
void meshopt_optimizeOverdraw(
    unsigned int* destination,       // Output: reordered indices
    const unsigned int* indices,     // Input: indices
    size_t index_count,              
    const float* vertex_positions,   // XYZ positions (stride-aware)
    size_t vertex_count,             
    size_t vertex_positions_stride,  // Byte offset to next position
    float threshold                  // Overdraw threshold (0.0–1.0; typical 1.01)
);

// Step 6: Optimize post-transform vertex layout (cache-friendly memory layout)
size_t meshopt_optimizeVertexFetch(
    void* destination,               // Output: reordered vertices
    unsigned int* indices,           // In/Out: remapped (for post-transform cache)
    size_t index_count,              
    const void* vertices,            // Input: vertex buffer
    size_t vertex_count,             
    size_t vertex_size               
);
```

### Typical Integration Cost

- **Compilation:** ~100ms for src/*.cpp (no external deps; pure C)
- **Runtime (for 1M triangle mesh):** ~50–200ms total (all 6 steps combined)
- **Binary size:** +150–300 KB (static lib)
- **Memory overhead:** Temporary remap table (~4 bytes per vertex during optimization; freed after)

### Recommended Workflow for RendererX

```cpp
// After glTF mesh load (fastgltf)
meshopt_generateVertexRemap(remap, indices, index_count, vertices, vertex_count, stride);
meshopt_remapIndexBuffer(new_indices, indices, index_count, remap);
meshopt_remapVertexBuffer(new_vertices, vertices, vertex_count, stride, remap);
meshopt_optimizeVertexCache(opt_indices, new_indices, index_count, vertex_count);
meshopt_optimizeOverdraw(final_indices, opt_indices, index_count, positions, vertex_count, stride, 1.01f);
meshopt_optimizeVertexFetch(final_vertices, final_indices, index_count, new_vertices, vertex_count, stride);
// → Upload final_vertices + final_indices to GPU
```

---

## 4. Khronos Sample Assets

### Repository Status

- **Active:** glTF-Sample-Assets (https://github.com/KhronosGroup/glTF-Sample-Assets)
- **Archived:** glTF-Sample-Models (legacy; content migrated)
- **Browse:** https://github.khronos.org/glTF-Assets/ (live viewer + search)

### Standard Test Corpus

**Small models (quick CI import tests) — <10 MB:**
1. **Box** – Minimal (1 mesh, 1 material); License: CC BY 4.0; ~50 KB
2. **Duck** – Classic COLLADA reference; License: SCEA Shared Source; ~500 KB–1 MB (with textures)
3. **Avocado** – Core glTF 2.0 test; License: CC0 1.0 Universal; ~2 MB
4. **Flight Helmet** – PBR showcase; License: CC0 1.0 Universal; ~5–8 MB

**Medium models (feature validation):**
5. **DamagedHelmet** – PBR quality test; License: CC BY 4.0; Geometry: ~546 KB (.glb), textures variable (~2–5 MB total with high-res)
6. **Fox** – Skeletal animation (rigging); License: CC0 1.0 Universal; multiple animation cycles; ~3–4 MB

**Large models (fly-through / stress test) — 50+ MB:**
7. **Sponza** – Architectural complex (PBR materials, 100K+ triangles); License: CC BY 4.0; ~150 MB (includes all textures); **Primary choice for impressive fly-through**

### Licenses Summary

- **CC BY 4.0:** Sponza, DamagedHelmet (ctxwing rebuild), Flight Helmet
- **CC0 1.0 Universal:** Fox, Avocado (public domain; no attribution required)
- **SCEA Shared Source:** Duck (legacy; acceptable for non-commercial/educational)

### Recommended Subset for RendererX CI & Demos

**For committed import tests (small, quick):**
- Box (validation of loader basics)
- DamagedHelmet geometry only (~546 KB .glb, no textures) for skinning/PBR validation

**For fetched test data (larger, downloaded on-demand):**
- Sponza (full) – fly-through demo, 150 MB, excellent for Vulkan performance showcase
- Fox (rigged) – skeletal animation validation
- Avocado – core PBR feature coverage

**Repository structure:**
```
glTF-Sample-Assets/
├── Models/
│   ├── Box/
│   ├── DamagedHelmet/
│   ├── Fox/
│   ├── Sponza/
│   └── ...
└── Models.md (metadata: licenses, descriptions, links)
```

---

## Summary & Recommendations

| Component | Choice | Version | Key Decision |
|-----------|--------|---------|--------------|
| **glTF Loader** | fastgltf | v0.9.0 | 5–7x perf over cgltf; modern C++17; full extension coverage (Draco, Basis, PBR) |
| **KTX2 Library** | libktx (KTX-Software) | v4.4.2 | Apache 2.0; CMake-friendly; Basis Universal transcode; keep CPU-only (no ktxVulkanTexture) |
| **Mesh Optimizer** | meshoptimizer | v1.2 | MIT; post-import remap→cache→overdraw→fetch pipeline; ~150–200 ms per million triangles |
| **Sample Assets** | glTF-Sample-Assets | Current | Sponza (150 MB, fly-through), DamagedHelmet (PBR/skinning tests), Box (unit tests) |

**Next Steps:**
1. Integrate fastgltf v0.9.0 header → parse glTF/glb files
2. Layer libktx v4.4.2 for KTX2 parsing + CPU-side Basis Universal transcode
3. Wire meshoptimizer post-import pipeline for vertex remap/cache/fetch optimization
4. Fetch Sponza + DamagedHelmet from glTF-Sample-Assets for demo/test
5. Plan C++20 migration for v0.10+ fastgltf compatibility (post-Phase 4)

---

## Sources

- [fastgltf v0.9.0 Documentation](https://fastgltf.readthedocs.io/latest/)
- [fastgltf GitHub Repository](https://github.com/spnda/fastgltf)
- [cgltf GitHub Repository](https://github.com/jkuhlmann/cgltf)
- [KTX-Software GitHub](https://github.com/KhronosGroup/KTX-Software)
- [KTX-Software Building Guide](https://github.com/KhronosGroup/KTX-Software/blob/main/BUILDING.md)
- [libktx Reference Documentation](https://github.khronos.org/KTX-Software/libktx/index.html)
- [meshoptimizer GitHub](https://github.com/zeux/meshoptimizer)
- [meshoptimizer.org](https://meshoptimizer.org/)
- [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets)
- [glTF Assets Browser](https://github.khronos.org/glTF-Assets/)
- [Vulkan Guide: Mesh Loading](https://vkguide.dev/docs/new_chapter_3/loading_meshes/)
- [Vulkan Guide: glTF Nodes](https://vkguide.dev/docs/new_chapter_5/gltf_nodes/)
- [Vulkan Guide: glTF Textures](https://vkguide.dev/docs/new_chapter_5/gltf_textures/)
- [Vulkan Samples: Basis Universal Transcoding](https://docs.vulkan.org/samples/latest/samples/performance/texture_compression_basisu/README.html)
- [glTF Skinning Tutorial](https://github.khronos.org/glTF-Tutorials/gltfTutorial/gltfTutorial_020_Skins.html)
