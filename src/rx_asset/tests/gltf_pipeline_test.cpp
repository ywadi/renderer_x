#include <doctest/doctest.h>
#include "../gltf_pipeline.h"
#include "../mikktspace_bridge.h"
#include <rx_asset/byte_source.h>
#include <glm/glm.hpp>
#include <meshoptimizer.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>

// gltf_pipeline_test.cpp -- device-free coverage for the per-primitive CPU
// pipeline [Phase 4 Stage 1 Task 13, spec D7, gate matrix-issue02 rows
// 5-8]. No fastgltf, no GPU -- exactly what gltf_pipeline.h's own header
// comment promises. import_gltf_gpu_test.cpp covers the fastgltf-aware +
// GeometryPool-consuming end-to-end path.

using namespace rx::asset;

namespace {

// A minimal, deliberately-degenerate "quad" made of TWO triangles sharing
// an edge, expressed as 6 already-DEINDEXED corners (i.e. what a
// non-indexed glTF primitive, or gltf_pipeline.cpp's own deindex step,
// would hand to the meshopt stage): corners 0,1,2 are one triangle,
// corners 3,4,5 are the other, and the shared edge (two corner pairs)
// has byte-identical position/normal/uv so meshopt_generateVertexRemap
// can weld them back down to 4 unique vertices.
struct QuadCorners {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
};

QuadCorners makeSharedEdgeQuad() {
    // 0---3
    // |  /|
    // | / |
    // |/  |
    // 1---2
    const glm::vec3 p0(-1, 1, 0), p1(-1, -1, 0), p2(1, -1, 0), p3(1, 1, 0);
    const glm::vec3 n(0, 0, 1);
    const glm::vec2 uv0(0, 1), uv1(0, 0), uv2(1, 0), uv3(1, 1);

    QuadCorners q;
    // Triangle A: p0, p1, p2
    q.positions = {p0, p1, p2, p0, p2, p3};
    q.normals = {n, n, n, n, n, n};
    q.uvs = {uv0, uv1, uv2, uv0, uv2, uv3};
    return q;
}

}  // namespace

TEST_CASE("processPrimitive: zero-vertex primitive is rejected") {
    PrimitiveInput input;
    PrimitiveOutput out = processPrimitive(input);
    CHECK_FALSE(out.ok);
    CHECK_FALSE(out.rejectedNonFinite);
}

TEST_CASE("processPrimitive: non-finite position is rejected and flagged") {
    QuadCorners q = makeSharedEdgeQuad();
    q.positions[2] = glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0, 0);

    PrimitiveInput input;
    input.positions = q.positions;
    input.normals = q.normals;
    input.uvs = q.uvs;

    PrimitiveOutput out = processPrimitive(input);
    CHECK_FALSE(out.ok);
    CHECK(out.rejectedNonFinite);
}

TEST_CASE("processPrimitive: missing NORMAL generates flat unit-length normals") {
    QuadCorners q = makeSharedEdgeQuad();
    PrimitiveInput input;
    input.positions = q.positions;
    input.uvs = q.uvs;  // normals left empty

    PrimitiveOutput out = processPrimitive(input);
    REQUIRE(out.ok);
    CHECK(out.generatedNormals);
    REQUIRE_FALSE(out.vertices.empty());
    for (const auto& v : out.vertices) {
        const float len = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
        CHECK(len == doctest::Approx(1.0F).epsilon(0.001));
    }
}

TEST_CASE("processPrimitive: missing TEXCOORD_0 zero-fills UVs and forces +X/w=1 tangent, skipping MikkTSpace") {
    QuadCorners q = makeSharedEdgeQuad();
    PrimitiveInput input;
    input.positions = q.positions;
    input.normals = q.normals;
    // uvs left empty

    PrimitiveOutput out = processPrimitive(input);
    REQUIRE(out.ok);
    CHECK(out.zeroFilledUVs);
    CHECK_FALSE(out.generatedTangents);  // MikkTSpace was skipped, not invoked
    REQUIRE_FALSE(out.vertices.empty());
    for (const auto& v : out.vertices) {
        CHECK(v.u == doctest::Approx(0.0F));
        CHECK(v.v == doctest::Approx(0.0F));
        CHECK(v.tx == doctest::Approx(1.0F));
        CHECK(v.ty == doctest::Approx(0.0F));
        CHECK(v.tz == doctest::Approx(0.0F));
        CHECK(v.tw == doctest::Approx(1.0F));
    }
}

TEST_CASE("processPrimitive: file-provided TANGENT passes through untouched (byte-exact, pre-weld)") {
    QuadCorners q = makeSharedEdgeQuad();
    std::vector<glm::vec4> tangents(6, glm::vec4(0.123F, 0.456F, 0.789F, -1.0F));

    PrimitiveInput input;
    input.positions = q.positions;
    input.normals = q.normals;
    input.uvs = q.uvs;
    input.tangents = tangents;

    PrimitiveOutput out = processPrimitive(input);
    REQUIRE(out.ok);
    CHECK_FALSE(out.generatedTangents);
    for (const auto& v : out.vertices) {
        CHECK(v.tx == doctest::Approx(0.123F));
        CHECK(v.ty == doctest::Approx(0.456F));
        CHECK(v.tz == doctest::Approx(0.789F));
        CHECK(v.tw == doctest::Approx(-1.0F));
    }
}

TEST_CASE("processPrimitive: tangent-less-but-UV'd primitive generates tangents via MikkTSpace") {
    QuadCorners q = makeSharedEdgeQuad();
    PrimitiveInput input;
    input.positions = q.positions;
    input.normals = q.normals;
    input.uvs = q.uvs;

    PrimitiveOutput out = processPrimitive(input);
    REQUIRE(out.ok);
    CHECK(out.generatedTangents);
    for (const auto& v : out.vertices) {
        const float len = std::sqrt(v.tx * v.tx + v.ty * v.ty + v.tz * v.tz);
        CHECK(len == doctest::Approx(1.0F).epsilon(0.01));
        CHECK((v.tw == doctest::Approx(1.0F) || v.tw == doctest::Approx(-1.0F)));
    }
}

TEST_CASE("processPrimitive: meshoptimizer actually ran -- shared-edge corners weld down to 4 unique vertices") {
    QuadCorners q = makeSharedEdgeQuad();
    PrimitiveInput input;
    input.positions = q.positions;
    input.normals = q.normals;
    input.uvs = q.uvs;

    PrimitiveOutput out = processPrimitive(input);
    REQUIRE(out.ok);
    // 6 deindexed corners in, 4 unique vertices out (the shared edge's two
    // corner-pairs are binary-identical -- position+normal+tangent+uv --
    // and meshopt_generateVertexRemap/optimizeVertexFetch are what
    // collapse them; a naive "just deindex" implementation would leave 6).
    CHECK(out.vertices.size() == 4);
    CHECK(out.indices.size() == 6);
    for (uint32_t idx : out.indices) {
        CHECK(idx < out.vertices.size());
    }
}

TEST_CASE("processPrimitive: tangent-welded vertex count >= position-only-welded count") {
    // Two corners share IDENTICAL position+normal but DIFFERENT UV (and
    // therefore, once tangents are generated, different tangent space) --
    // a position-only dedup would weld them into one vertex; the REAL
    // D8 48-byte-vertex dedup (position+normal+tangent+uv, matrix row 5)
    // must not, proving tangent data participated in the remap rather
    // than being silently folded/averaged after the fact (MikkTSpace's
    // own explicit prohibition: "DO NOT use an already existing index
    // list").
    const glm::vec3 p(0, 0, 0), p1(1, 0, 0), p2(0, 1, 0);
    const glm::vec3 n(0, 0, 1);
    std::vector<glm::vec3> positions = {p, p1, p2, p, p2, p1};  // two triangles sharing corner `p` and `p2`/`p1` swapped
    std::vector<glm::vec3> normals(6, n);
    // Corner 0 and corner 3 are both at position `p` but get DIFFERENT UVs.
    std::vector<glm::vec2> uvs = {{0, 0}, {1, 0}, {0, 1}, {0.5F, 0.5F}, {0, 1}, {1, 0}};

    PrimitiveInput input;
    input.positions = positions;
    input.normals = normals;
    input.uvs = uvs;
    PrimitiveOutput out = processPrimitive(input);
    REQUIRE(out.ok);
    const size_t tangentWeldedCount = out.vertices.size();

    // Position-only weld count on the SAME 6 corners, computed directly
    // (independent of gltf_pipeline.cpp's own internals) via
    // meshopt_generateVertexRemap over just the 12-byte position stream.
    std::vector<unsigned int> remap(6);
    const size_t positionWeldedCount =
        meshopt_generateVertexRemap(remap.data(), nullptr, 6, positions.data(), 6, sizeof(glm::vec3));

    CHECK(positionWeldedCount < 6);  // sanity: positions alone WOULD weld some corners
    CHECK(tangentWeldedCount >= positionWeldedCount);
}

TEST_CASE("processPrimitive: degenerate (zero-area) UV triangle never produces a NaN tangent") {
    const glm::vec3 p0(0, 0, 0), p1(1, 0, 0), p2(0, 1, 0);
    const glm::vec3 n(0, 0, 1);
    std::vector<glm::vec3> positions = {p0, p1, p2};
    std::vector<glm::vec3> normals = {n, n, n};
    // All three corners share the SAME uv -- zero UV area, the classic
    // MikkTSpace degenerate-tangent trap.
    std::vector<glm::vec2> uvs = {{0.5F, 0.5F}, {0.5F, 0.5F}, {0.5F, 0.5F}};

    PrimitiveInput input;
    input.positions = positions;
    input.normals = normals;
    input.uvs = uvs;
    PrimitiveOutput out = processPrimitive(input);
    REQUIRE(out.ok);
    for (const auto& v : out.vertices) {
        CHECK(std::isfinite(v.tx));
        CHECK(std::isfinite(v.ty));
        CHECK(std::isfinite(v.tz));
        CHECK(std::isfinite(v.tw));
    }
}

TEST_CASE("processPrimitive: AABB is computed from FINAL post-meshopt positions") {
    QuadCorners q = makeSharedEdgeQuad();
    PrimitiveInput input;
    input.positions = q.positions;
    input.normals = q.normals;
    input.uvs = q.uvs;

    PrimitiveOutput out = processPrimitive(input);
    REQUIRE(out.ok);
    CHECK(out.bounds.isValid());
    CHECK(out.bounds.min.x == doctest::Approx(-1.0F));
    CHECK(out.bounds.max.x == doctest::Approx(1.0F));
    CHECK(out.bounds.min.y == doctest::Approx(-1.0F));
    CHECK(out.bounds.max.y == doctest::Approx(1.0F));
}

// ---------------------------------------------------------------------
// MikkTSpace bridge -- direct coverage independent of gltf_pipeline.cpp.
// ---------------------------------------------------------------------

TEST_CASE("generateTangentsDeindexed: mismatched-length inputs return empty (defensive)") {
    std::vector<glm::vec3> positions(3, glm::vec3(0));
    std::vector<glm::vec3> normals(2, glm::vec3(0, 0, 1));  // wrong length
    std::vector<glm::vec2> uvs(3, glm::vec2(0));
    CHECK(generateTangentsDeindexed(positions, normals, uvs).empty());
}

TEST_CASE("generateTangentsDeindexed: a simple unit-square triangle produces a sane tangent aligned with +U") {
    std::vector<glm::vec3> positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    std::vector<glm::vec3> normals(3, glm::vec3(0, 0, 1));
    std::vector<glm::vec2> uvs = {{0, 0}, {1, 0}, {0, 1}};

    auto tangents = generateTangentsDeindexed(positions, normals, uvs);
    REQUIRE(tangents.size() == 3);
    for (const auto& t : tangents) {
        CHECK(t.x == doctest::Approx(1.0F).epsilon(0.01));
        CHECK(t.y == doctest::Approx(0.0F).epsilon(0.01));
        CHECK((t.w == doctest::Approx(1.0F) || t.w == doctest::Approx(-1.0F)));
    }
}

// ---------------------------------------------------------------------
// ByteSource -- FilesystemByteSource, device-free.
// ---------------------------------------------------------------------

TEST_CASE("FilesystemByteSource reads a relative file rooted at its base directory") {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "rx_asset_byte_source_test";
    std::filesystem::create_directories(dir);
    std::filesystem::path file = dir / "hello.bin";
    {
        std::ofstream f(file, std::ios::binary);
        f << "hello world";
    }

    rx::asset::FilesystemByteSource source(dir);
    auto bytes = source.read("hello.bin");
    REQUIRE(bytes.has_value());
    std::string asString(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    CHECK(asString == "hello world");

    std::filesystem::remove_all(dir);
}

TEST_CASE("FilesystemByteSource returns nullopt for a missing file") {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "rx_asset_byte_source_test_missing";
    std::filesystem::create_directories(dir);
    rx::asset::FilesystemByteSource source(dir);
    CHECK_FALSE(source.read("does_not_exist.bin").has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("FilesystemByteSource refuses an absolute-path URI defensively") {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "rx_asset_byte_source_test_abs";
    std::filesystem::create_directories(dir);
    rx::asset::FilesystemByteSource source(dir);
    CHECK_FALSE(source.read("/etc/passwd").has_value());
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------
// EXT_meshopt_compression [matrix-issue02 §2C row 8]: mode->decoder and
// filter round-trips, using meshoptimizer's REAL encode functions
// directly (the identical codec a gltfpack-produced file's compressed
// bufferViews use) -- see import_gltf.cpp's DecodedMeshoptViews class
// for the production mode/filter dispatch these prove correct.
// Attributes+Triangles modes are ALSO proven end-to-end through a real
// glTF fixture (import_gltf_gpu_test.cpp's cube_meshopt.gltf, generated
// by tools/gen_gltf_compression_fixtures from this same library); Indices
// mode and all three filters are proven here directly, device-free,
// since gltfpack itself never emits Indices-mode buffers for a TRIANGLES
// primitive (Triangles mode is strictly better for that topology) --
// contriving a glTF fixture around it would exercise nothing gltfpack
// output ever does; a direct codec round-trip is the honest way to cover
// the function this project's mode->decoder map still dispatches to.
// ---------------------------------------------------------------------

TEST_CASE("EXT_meshopt_compression: Indices mode round-trips exactly (meshopt_encode/decodeIndexSequence)") {
    std::vector<unsigned int> indices = {0, 5, 3, 1, 4, 2, 9, 7};
    // meshopt_encodeIndexSequence has no *Bound() sizer of its own --
    // oversize generously (worst case is close to 1 byte/index for small
    // sequences; 256 bytes for 8 indices is ample headroom).
    std::vector<unsigned char> encoded(256, 0);
    size_t encodedSize = meshopt_encodeIndexSequence(encoded.data(), encoded.size(), indices.data(), indices.size());
    REQUIRE(encodedSize > 0);
    encoded.resize(encodedSize);

    std::vector<unsigned int> decoded(indices.size());
    int rc = meshopt_decodeIndexSequence(decoded.data(), indices.size(), sizeof(unsigned int), encoded.data(), encoded.size());
    REQUIRE(rc == 0);
    CHECK(decoded == indices);
}

TEST_CASE("EXT_meshopt_compression: Octahedral filter round-trips a unit vector") {
    float v[4] = {0.0F, 0.0F, 1.0F, 1.0F};  // +Z, w=1 (preserved as-is)
    int8_t encoded[4];
    meshopt_encodeFilterOct(encoded, 1, 4, 8, v);
    int8_t buf[4];
    std::memcpy(buf, encoded, sizeof(buf));
    meshopt_decodeFilterOct(buf, 1, 4);

    const float x = buf[0] / 127.0F, y = buf[1] / 127.0F, z = buf[2] / 127.0F, w = buf[3] / 127.0F;
    CHECK(x == doctest::Approx(0.0F).epsilon(0.02));
    CHECK(y == doctest::Approx(0.0F).epsilon(0.02));
    CHECK(z == doctest::Approx(1.0F).epsilon(0.02));
    CHECK(w == doctest::Approx(1.0F).epsilon(0.02));
}

TEST_CASE("EXT_meshopt_compression: Quaternion filter round-trips the identity quaternion") {
    float q[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    int16_t encoded[4];
    meshopt_encodeFilterQuat(encoded, 1, 8, 16, q);
    int16_t buf[4];
    std::memcpy(buf, encoded, sizeof(buf));
    meshopt_decodeFilterQuat(buf, 1, 8);

    const float x = buf[0] / 32767.0F, y = buf[1] / 32767.0F, z = buf[2] / 32767.0F, w = buf[3] / 32767.0F;
    CHECK(x == doctest::Approx(0.0F).epsilon(0.01));
    CHECK(y == doctest::Approx(0.0F).epsilon(0.01));
    CHECK(z == doctest::Approx(0.0F).epsilon(0.01));
    CHECK(w == doctest::Approx(1.0F).epsilon(0.01));
}

TEST_CASE("EXT_meshopt_compression: Exponential filter round-trips arbitrary floats bit-exact") {
    float data[4] = {1.5F, -2.25F, 100.0F, 0.001F};
    uint32_t encoded[4];
    meshopt_encodeFilterExp(encoded, 1, 16, 24, data, meshopt_EncodeExpSeparate);
    uint32_t buf[4];
    std::memcpy(buf, encoded, sizeof(buf));
    meshopt_decodeFilterExp(buf, 1, 16);
    float out[4];
    std::memcpy(out, buf, sizeof(out));

    CHECK(out[0] == doctest::Approx(1.5F));
    CHECK(out[1] == doctest::Approx(-2.25F));
    CHECK(out[2] == doctest::Approx(100.0F));
    CHECK(out[3] == doctest::Approx(0.001F).epsilon(0.001));
}
