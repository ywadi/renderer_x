#pragma once

// gltf_pipeline.h -- PRIVATE (not installed). The device-free,
// fastgltf-free per-primitive CPU pipeline [D7, matrix-issue02 rows 5-8]:
// mandatory-attribute defaults -> tangent generation (file-provided
// passthrough or MikkTSpace) -> the canonical meshoptimizer sequence
// (remap -> cache -> overdraw -> fetch) -> AABB from final positions.
// Operates on plain typed spans so it is unit-testable without fastgltf
// and without a GPU -- import_gltf.cpp is the only caller, feeding it
// already-decoded (accessor-tool-read, sparse/normalized-resolved,
// index-widened) attribute arrays; this function never sees a raw glTF
// byte or JSON value. Thread-affinity: NONE intrinsically (no shared
// mutable state) -- called from a parallelFor() worker chunk by
// import_gltf.cpp [D5/matrix-issue02 row 15].
#include <rx_asset/geometry_pool.h>
#include <rx_asset/mesh_asset.h>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <vector>

namespace rx::asset {

// meshoptimizer's own README example threshold [gate ruling C6, matrix
// row 6]: "typical 1.01" from an in-repo research note was superseded by
// the library's own current documented example (1.05), which is what
// this task adopts.
inline constexpr float kOverdrawThreshold = 1.05F;

struct PrimitiveInput {
    std::span<const glm::vec3> positions;  // mandatory; caller has already skipped POSITION-less primitives
    std::span<const glm::vec3> normals;    // empty -> flat-generate
    std::span<const glm::vec2> uvs;        // empty -> zero-fill, tangent forced to +X/w=1 (MikkTSpace skipped)
    std::span<const glm::vec4> tangents;   // empty (with uvs non-empty) -> MikkTSpace generates
    std::span<const uint32_t> indices;     // empty -> treated as an implicit sequential 0..positions.size()-1 index buffer (glTF non-indexed primitive)
};

struct PrimitiveOutput {
    bool ok = false;
    bool rejectedNonFinite = false;  // NaN/Inf position found -- ok stays false, vertices/indices stay empty
    bool generatedNormals = false;
    bool generatedTangents = false;
    bool zeroFilledUVs = false;

    std::vector<PoolVertex> vertices;  // post-meshoptimizer, D8 layout
    std::vector<uint32_t> indices;     // post-meshoptimizer, u32
    AABB bounds;                        // from FINAL positions [G11]
};

[[nodiscard]] PrimitiveOutput processPrimitive(const PrimitiveInput& input);

}  // namespace rx::asset
