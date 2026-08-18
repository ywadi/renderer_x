#include "gltf_pipeline.h"
#include "mikktspace_bridge.h"
#include <meshoptimizer.h>
#include <cmath>
#include <numeric>

namespace rx::asset {

namespace {

bool isFinite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

std::vector<uint32_t> effectiveIndices(const PrimitiveInput& input) {
    if (!input.indices.empty()) {
        return {input.indices.begin(), input.indices.end()};
    }
    // Non-indexed primitive [glTF spec: `indices` is optional] -- implicit
    // sequential 0..N-1.
    std::vector<uint32_t> sequential(input.positions.size());
    std::iota(sequential.begin(), sequential.end(), 0U);
    return sequential;
}

glm::vec3 flatFaceNormal(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 cross = glm::cross(b - a, c - a);
    const float len = glm::length(cross);
    if (!(len > 1e-12F) || !std::isfinite(len)) {
        return glm::vec3(0.0F, 1.0F, 0.0F);  // degenerate/zero-area triangle -- arbitrary finite fallback, never NaN
    }
    return cross / len;
}

}  // namespace

PrimitiveOutput processPrimitive(const PrimitiveInput& input) {
    PrimitiveOutput out;

    if (input.positions.empty()) {
        return out;  // ok stays false -- caller WARNs + skips (zero-vertex primitive)
    }

    std::vector<uint32_t> idx = effectiveIndices(input);
    const size_t cornerCount = idx.size();
    if (cornerCount == 0 || cornerCount % 3 != 0) {
        return out;  // not triangulated -- caller is responsible for only calling this on TRIANGLES primitives
    }

    // ---- Deindex positions; reject on any non-finite value [G11/matrix row 10] ----
    std::vector<glm::vec3> cornerPositions(cornerCount);
    for (size_t i = 0; i < cornerCount; ++i) {
        const glm::vec3& p = input.positions[idx[i]];
        if (!isFinite(p)) {
            out.rejectedNonFinite = true;
            return out;
        }
        cornerPositions[i] = p;
    }

    // ---- Deindex or generate normals ----
    std::vector<glm::vec3> cornerNormals(cornerCount);
    if (input.normals.empty()) {
        out.generatedNormals = true;
        for (size_t face = 0; face < cornerCount / 3; ++face) {
            const glm::vec3 n = flatFaceNormal(cornerPositions[face * 3 + 0], cornerPositions[face * 3 + 1], cornerPositions[face * 3 + 2]);
            cornerNormals[face * 3 + 0] = n;
            cornerNormals[face * 3 + 1] = n;
            cornerNormals[face * 3 + 2] = n;
        }
    } else {
        for (size_t i = 0; i < cornerCount; ++i) {
            cornerNormals[i] = input.normals[idx[i]];
        }
    }

    // ---- Deindex or zero-fill UVs ----
    std::vector<glm::vec2> cornerUVs(cornerCount);
    if (input.uvs.empty()) {
        out.zeroFilledUVs = true;
        std::fill(cornerUVs.begin(), cornerUVs.end(), glm::vec2(0.0F, 0.0F));
    } else {
        for (size_t i = 0; i < cornerCount; ++i) {
            cornerUVs[i] = input.uvs[idx[i]];
        }
    }

    // ---- Tangents: file passthrough, MikkTSpace, or the UV-less +X/w=1 default ----
    std::vector<glm::vec4> cornerTangents(cornerCount);
    if (input.uvs.empty()) {
        // [matrix row "Missing TEXCOORD_0"]: MikkTSpace requires UVs to
        // define tangent space at all -- skipped entirely, per-corner
        // default.
        std::fill(cornerTangents.begin(), cornerTangents.end(), glm::vec4(1.0F, 0.0F, 0.0F, 1.0F));
    } else if (!input.tangents.empty()) {
        for (size_t i = 0; i < cornerCount; ++i) {
            cornerTangents[i] = input.tangents[idx[i]];
        }
    } else {
        out.generatedTangents = true;
        cornerTangents = generateTangentsDeindexed(cornerPositions, cornerNormals, cornerUVs);
        if (cornerTangents.size() != cornerCount) {
            // Defensive: generateTangentsDeindexed() only returns a
            // mismatched size on malformed input, which cannot happen
            // given the arrays above are already length-matched --
            // still guarded so a future bug there degrades safely
            // instead of an out-of-bounds read below.
            cornerTangents.assign(cornerCount, glm::vec4(1.0F, 0.0F, 0.0F, 1.0F));
        }
    }

    // ---- Build the deindexed D8 48-byte vertex stream ----
    std::vector<PoolVertex> deindexedVertices(cornerCount);
    for (size_t i = 0; i < cornerCount; ++i) {
        PoolVertex& v = deindexedVertices[i];
        v.px = cornerPositions[i].x;
        v.py = cornerPositions[i].y;
        v.pz = cornerPositions[i].z;
        v.nx = cornerNormals[i].x;
        v.ny = cornerNormals[i].y;
        v.nz = cornerNormals[i].z;
        v.tx = cornerTangents[i].x;
        v.ty = cornerTangents[i].y;
        v.tz = cornerTangents[i].z;
        v.tw = cornerTangents[i].w;
        v.u = cornerUVs[i].x;
        v.v = cornerUVs[i].y;
    }

    // ---- meshoptimizer canonical sequence [D7, matrix row 6]:
    // generateVertexRemap (over the FULL 48-byte vertex, tangent
    // included -- matrix row 5's welded-count test depends on this) ->
    // remap -> optimizeVertexCache -> optimizeOverdraw(1.05) ->
    // optimizeVertexFetch. ----
    std::vector<unsigned int> remapTable(cornerCount);
    const size_t uniqueCount = meshopt_generateVertexRemap(remapTable.data(), nullptr, cornerCount, deindexedVertices.data(),
                                                             cornerCount, sizeof(PoolVertex));

    std::vector<PoolVertex> remappedVertices(uniqueCount);
    meshopt_remapVertexBuffer(remappedVertices.data(), deindexedVertices.data(), cornerCount, sizeof(PoolVertex), remapTable.data());

    std::vector<unsigned int> remappedIndices(cornerCount);
    meshopt_remapIndexBuffer(remappedIndices.data(), static_cast<const unsigned int*>(nullptr), cornerCount, remapTable.data());

    std::vector<unsigned int> cacheOptIndices(cornerCount);
    meshopt_optimizeVertexCache(cacheOptIndices.data(), remappedIndices.data(), cornerCount, uniqueCount);

    std::vector<unsigned int> overdrawOptIndices(cornerCount);
    meshopt_optimizeOverdraw(overdrawOptIndices.data(), cacheOptIndices.data(), cornerCount, &remappedVertices[0].px, uniqueCount,
                              sizeof(PoolVertex), kOverdrawThreshold);

    std::vector<PoolVertex> finalVertices(uniqueCount);
    const size_t finalUniqueCount =
        meshopt_optimizeVertexFetch(finalVertices.data(), overdrawOptIndices.data(), cornerCount, remappedVertices.data(), uniqueCount,
                                     sizeof(PoolVertex));
    finalVertices.resize(finalUniqueCount);

    out.vertices = std::move(finalVertices);
    out.indices.assign(overdrawOptIndices.begin(), overdrawOptIndices.end());

    // ---- AABB from FINAL post-meshoptimizer positions [G11] ----
    AABB bounds;
    for (const PoolVertex& v : out.vertices) {
        bounds.expand(glm::vec3(v.px, v.py, v.pz));
    }
    out.bounds = bounds;

    out.ok = true;
    return out;
}

}  // namespace rx::asset
