// gen_gltf_compression_fixtures -- see this directory's CMakeLists.txt
// for the full rationale. Regenerates:
//   assets/test/cube_meshopt.{gltf,bin}          (EXT_meshopt_compression)
//   assets/test/cube_draco.{gltf,bin}            (KHR_draco_mesh_compression, materialed)
//   assets/test/cube_draco_reference.{gltf,bin}  (round-11/issue-31: uncompressed twin of
//                                                  cube_draco.gltf -- identical geometry +
//                                                  material as plain accessors, the render-
//                                                  equivalence gate's ground truth)
//   assets/test/cube_draco_corrupt.{gltf,bin}    (round-11/issue-31: same extension shape,
//                                                  deliberately-truncated compressed payload --
//                                                  the decode-failure fixture)
// Run with one argument: the repository root (tools/fetch_assets.sh
// invokes it as `gen_gltf_compression_fixtures <repo-root>`).
#include <meshoptimizer.h>
#include <draco/compression/encode.h>
#include <draco/mesh/triangle_soup_mesh_builder.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Vec3 {
    float x, y, z;
};
struct Vec2 {
    float x, y;
};

// A small quad (4 verts, 2 triangles) -- same shape as several of this
// task's other hand-authored fixtures. Kept deliberately simple: what
// matters here is exercising the REAL codec round-trip, not geometric
// complexity.
const std::vector<Vec3> kPositions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
const std::vector<Vec3> kNormals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
const std::vector<Vec2> kUVs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
const std::vector<uint32_t> kIndices = {0, 1, 2, 0, 2, 3};

void writeFile(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

std::string gltfHeader(const char* extUsed, const char* extRequired, const char* generatorNote) {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"asset\": {\"version\": \"2.0\", \"generator\": \"" << generatorNote << "\"},\n";
    if (extUsed != nullptr) {
        ss << "  \"extensionsUsed\": [\"" << extUsed << "\"],\n";
    }
    if (extRequired != nullptr) {
        ss << "  \"extensionsRequired\": [\"" << extRequired << "\"],\n";
    }
    return ss.str();
}

// ---------------------------------------------------------------------
// EXT_meshopt_compression fixture: POSITION compressed as an Attributes
// stream (mode=ATTRIBUTES, filter=NONE), indices compressed as a
// Triangles stream (mode=TRIANGLES). NORMAL/TEXCOORD_0 stay plain
// (uncompressed) accessors -- this fixture's job is to prove the
// mode->decoder dispatch and the whole byte-source/bufferView plumbing,
// not to compress every attribute a real content pipeline would.
// ---------------------------------------------------------------------
void genMeshoptFixture(const std::filesystem::path& outDir) {
    std::vector<unsigned char> posEncoded(meshopt_encodeVertexBufferBound(kPositions.size(), sizeof(Vec3)));
    size_t posEncodedSize = meshopt_encodeVertexBuffer(posEncoded.data(), posEncoded.size(), kPositions.data(),
                                                         kPositions.size(), sizeof(Vec3));
    posEncoded.resize(posEncodedSize);

    std::vector<unsigned char> idxEncoded(meshopt_encodeIndexBufferBound(kIndices.size(), kPositions.size()));
    size_t idxEncodedSize = meshopt_encodeIndexBuffer(idxEncoded.data(), idxEncoded.size(), kIndices.data(), kIndices.size());
    idxEncoded.resize(idxEncodedSize);

    std::vector<uint8_t> buf;
    const auto pad4 = [&]() {
        while (buf.size() % 4 != 0) buf.push_back(0);
    };
    const size_t posOff = buf.size();
    buf.insert(buf.end(), posEncoded.begin(), posEncoded.end());
    pad4();
    const size_t idxOff = buf.size();
    buf.insert(buf.end(), idxEncoded.begin(), idxEncoded.end());
    pad4();
    const size_t normOff = buf.size();
    for (const Vec3& n : kNormals) {
        const float f[3] = {n.x, n.y, n.z};
        buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(f), reinterpret_cast<const uint8_t*>(f) + sizeof(f));
    }
    const size_t uvOff = buf.size();
    for (const Vec2& uv : kUVs) {
        const float f[2] = {uv.x, uv.y};
        buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(f), reinterpret_cast<const uint8_t*>(f) + sizeof(f));
    }
    pad4();

    writeFile(outDir / "cube_meshopt.bin", buf.data(), buf.size());

    std::ostringstream ss;
    ss << gltfHeader("EXT_meshopt_compression", "EXT_meshopt_compression",
                      "rx_asset gen_gltf_compression_fixtures (Task 13, meshoptimizer v1.2 real encode)");
    ss << "  \"scene\": 0,\n"
       << "  \"scenes\": [{\"nodes\": [0]}],\n"
       << "  \"nodes\": [{\"mesh\": 0, \"name\": \"meshopt_quad\"}],\n"
       << "  \"meshes\": [{\"name\": \"quad\", \"primitives\": [{\"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, "
          "\"TEXCOORD_0\": 2}, \"indices\": 3, \"mode\": 4}]}],\n"
       << "  \"accessors\": [\n"
       << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\", \"min\": [0,0,0], "
          "\"max\": [1,1,0]},\n"
       << "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},\n"
       << "    {\"bufferView\": 3, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC2\"},\n"
       << "    {\"bufferView\": 1, \"componentType\": 5125, \"count\": 6, \"type\": \"SCALAR\"}\n"
       << "  ],\n"
       << "  \"bufferViews\": [\n"
       << "    {\"buffer\": 0, \"byteOffset\": " << posOff << ", \"byteLength\": " << posEncodedSize
       << ", \"extensions\": {\"EXT_meshopt_compression\": {\"buffer\": 0, \"byteOffset\": " << posOff
       << ", \"byteLength\": " << posEncodedSize << ", \"byteStride\": 12, \"count\": 4, \"mode\": \"ATTRIBUTES\"}}},\n"
       << "    {\"buffer\": 0, \"byteOffset\": " << idxOff << ", \"byteLength\": " << idxEncodedSize
       << ", \"extensions\": {\"EXT_meshopt_compression\": {\"buffer\": 0, \"byteOffset\": " << idxOff
       << ", \"byteLength\": " << idxEncodedSize << ", \"byteStride\": 4, \"count\": 6, \"mode\": \"TRIANGLES\"}}},\n"
       << "    {\"buffer\": 0, \"byteOffset\": " << normOff << ", \"byteLength\": " << (kNormals.size() * 12) << "},\n"
       << "    {\"buffer\": 0, \"byteOffset\": " << uvOff << ", \"byteLength\": " << (kUVs.size() * 8) << "}\n"
       << "  ],\n"
       << "  \"buffers\": [{\"uri\": \"cube_meshopt.bin\", \"byteLength\": " << buf.size() << "}]\n"
       << "}\n";

    std::ofstream jf(outDir / "cube_meshopt.gltf");
    jf << ss.str();
    std::printf("wrote cube_meshopt.gltf (%zu bytes JSON, %zu bytes bin; position %zu->%zu bytes, indices %zu->%zu "
                "bytes)\n",
                ss.str().size(), buf.size(), kPositions.size() * sizeof(Vec3), posEncodedSize,
                kIndices.size() * sizeof(uint32_t), idxEncodedSize);
}

// Shared material block both the Draco-compressed fixture and its
// uncompressed reference twin (below) carry -- IDENTICAL values in both,
// so a render-equivalence gate comparing the two rendered images has no
// material-side variable to account for, only the decode path itself.
// Distinct from cube_textured.gltf's own baseColorFactor (warm red)
// specifically so the two fixture families are never visually confusable
// if ever rendered side by side.
constexpr const char* kSharedMaterialJson =
    "  \"materials\": [\n"
    "    {\"name\": \"draco_quad_material\", \"pbrMetallicRoughness\": {\"baseColorFactor\": [0.2, 0.6, 0.9, "
    "1.0], \"metallicFactor\": 0.1, \"roughnessFactor\": 0.6}, \"doubleSided\": false, \"alphaMode\": "
    "\"OPAQUE\"}\n"
    "  ],\n";

// ---------------------------------------------------------------------
// Uncompressed twin of genDracoFixture()'s own quad -- IDENTICAL
// positions/normals/uvs/indices and the IDENTICAL material (above),
// stored as plain (non-Draco) accessors. Round-11/issue-31 addition: the
// render-equivalence gate (samples/09_scene conventions -- exact
// counters + a pixel-tolerance gate) needs a ground truth to compare the
// Draco-decoded render against; comparing against THIS twin (rendered
// through the identical sample binary/pipeline) is a stronger, more
// precise gate than a committed reference PNG would be here, and adds no
// second reference-regeneration mechanism (gate ruling #15) since
// nothing about this comparison is a committed golden file -- both
// images are rendered fresh at gate time.
// ---------------------------------------------------------------------
void genDracoReferenceFixture(const std::filesystem::path& outDir) {
    std::vector<uint8_t> buf;
    const size_t posOff = buf.size();
    for (const Vec3& p : kPositions) {
        const float f[3] = {p.x, p.y, p.z};
        buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(f), reinterpret_cast<const uint8_t*>(f) + sizeof(f));
    }
    const size_t normOff = buf.size();
    for (const Vec3& n : kNormals) {
        const float f[3] = {n.x, n.y, n.z};
        buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(f), reinterpret_cast<const uint8_t*>(f) + sizeof(f));
    }
    const size_t uvOff = buf.size();
    for (const Vec2& uv : kUVs) {
        const float f[2] = {uv.x, uv.y};
        buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(f), reinterpret_cast<const uint8_t*>(f) + sizeof(f));
    }
    const size_t idxOff = buf.size();
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(kIndices.data()),
               reinterpret_cast<const uint8_t*>(kIndices.data()) + kIndices.size() * sizeof(uint32_t));

    writeFile(outDir / "cube_draco_reference.bin", buf.data(), buf.size());

    std::ostringstream ss;
    ss << gltfHeader(nullptr, nullptr,
                      "rx_asset gen_gltf_compression_fixtures (round-11/issue-31, uncompressed Draco-fixture twin)");
    ss << "  \"scene\": 0,\n"
       << "  \"scenes\": [{\"nodes\": [0]}],\n"
       << "  \"nodes\": [{\"mesh\": 0, \"name\": \"draco_reference_quad\"}],\n"
       << kSharedMaterialJson
       << "  \"meshes\": [{\"name\": \"quad\", \"primitives\": [{\"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, "
          "\"TEXCOORD_0\": 2}, \"indices\": 3, \"material\": 0, \"mode\": 4}]}],\n"
       << "  \"accessors\": [\n"
       << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\", \"min\": [0,0,0], "
          "\"max\": [1,1,0]},\n"
       << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},\n"
       << "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC2\"},\n"
       << "    {\"bufferView\": 3, \"componentType\": 5125, \"count\": 6, \"type\": \"SCALAR\"}\n"
       << "  ],\n"
       << "  \"bufferViews\": [\n"
       << "    {\"buffer\": 0, \"byteOffset\": " << posOff << ", \"byteLength\": " << (kPositions.size() * 12)
       << ", \"target\": 34962},\n"
       << "    {\"buffer\": 0, \"byteOffset\": " << normOff << ", \"byteLength\": " << (kNormals.size() * 12)
       << ", \"target\": 34962},\n"
       << "    {\"buffer\": 0, \"byteOffset\": " << uvOff << ", \"byteLength\": " << (kUVs.size() * 8)
       << ", \"target\": 34962},\n"
       << "    {\"buffer\": 0, \"byteOffset\": " << idxOff << ", \"byteLength\": " << (kIndices.size() * 4)
       << ", \"target\": 34963}\n"
       << "  ],\n"
       << "  \"buffers\": [{\"uri\": \"cube_draco_reference.bin\", \"byteLength\": " << buf.size() << "}]\n"
       << "}\n";

    std::ofstream jf(outDir / "cube_draco_reference.gltf");
    jf << ss.str();
    std::printf("wrote cube_draco_reference.gltf (%zu bytes JSON, %zu bytes bin -- uncompressed twin)\n", ss.str().size(),
                buf.size());
}

// ---------------------------------------------------------------------
// KHR_draco_mesh_compression fixture.
// ---------------------------------------------------------------------
void genDracoFixture(const std::filesystem::path& outDir) {
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(2);  // 2 faces

    const int posAtt = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    const int normAtt = builder.AddAttribute(draco::GeometryAttribute::NORMAL, 3, draco::DT_FLOAT32);
    const int uvAtt = builder.AddAttribute(draco::GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32);

    const uint32_t faces[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (int f = 0; f < 2; ++f) {
        const Vec3* p0 = &kPositions[faces[f][0]];
        const Vec3* p1 = &kPositions[faces[f][1]];
        const Vec3* p2 = &kPositions[faces[f][2]];
        builder.SetAttributeValuesForFace(posAtt, draco::FaceIndex(f), p0, p1, p2);

        const Vec3* n0 = &kNormals[faces[f][0]];
        const Vec3* n1 = &kNormals[faces[f][1]];
        const Vec3* n2 = &kNormals[faces[f][2]];
        builder.SetAttributeValuesForFace(normAtt, draco::FaceIndex(f), n0, n1, n2);

        const Vec2* u0 = &kUVs[faces[f][0]];
        const Vec2* u1 = &kUVs[faces[f][1]];
        const Vec2* u2 = &kUVs[faces[f][2]];
        builder.SetAttributeValuesForFace(uvAtt, draco::FaceIndex(f), u0, u1, u2);
    }

    std::unique_ptr<draco::Mesh> mesh = builder.Finalize();
    if (!mesh) {
        std::fprintf(stderr, "draco TriangleSoupMeshBuilder::Finalize() failed\n");
        std::exit(1);
    }

    draco::Encoder encoder;
    // Generous quantization (still lossy by Draco's own design -- there
    // is no lossless float mode for edgebreaker-compressed meshes): the
    // decode-side test tolerates a quantization-step-sized error, not
    // exact equality.
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 16);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, 10);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, 12);

    draco::EncoderBuffer buffer;
    const draco::Status status = encoder.EncodeMeshToBuffer(*mesh, &buffer);
    if (!status.ok()) {
        std::fprintf(stderr, "draco EncodeMeshToBuffer failed: %s\n", status.error_msg());
        std::exit(1);
    }

    writeFile(outDir / "cube_draco.bin", buffer.data(), buffer.size());

    // Draco attribute IDs assigned by the encoder are the SAME as the
    // TriangleSoupMeshBuilder's own att_id return values above (posAtt/
    // normAtt/uvAtt), verified empirically by this tool's own generated
    // fixture round-tripping in import_gltf_gpu_test.cpp.
    std::ostringstream ss;
    ss << gltfHeader("KHR_draco_mesh_compression", "KHR_draco_mesh_compression",
                      "rx_asset gen_gltf_compression_fixtures (Task 13, Draco v1.5.7 real encode; material added "
                      "round-11/issue-31)");
    ss << "  \"scene\": 0,\n"
       << "  \"scenes\": [{\"nodes\": [0]}],\n"
       << "  \"nodes\": [{\"mesh\": 0, \"name\": \"draco_quad\"}],\n"
       << kSharedMaterialJson
       << "  \"meshes\": [{\"name\": \"quad\", \"primitives\": [{\n"
       << "    \"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"TEXCOORD_0\": 2}, \"indices\": 3, \"material\": "
          "0, \"mode\": 4,\n"
       << "    \"extensions\": {\"KHR_draco_mesh_compression\": {\"bufferView\": 0, \"attributes\": {\"POSITION\": "
       << posAtt << ", \"NORMAL\": " << normAtt << ", \"TEXCOORD_0\": " << uvAtt << "}}}\n"
       << "  }]}],\n"
       << "  \"accessors\": [\n"
       << "    {\"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\", \"min\": [0,0,0], \"max\": [1,1,0]},\n"
       << "    {\"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},\n"
       << "    {\"componentType\": 5126, \"count\": 4, \"type\": \"VEC2\"},\n"
       << "    {\"componentType\": 5125, \"count\": 6, \"type\": \"SCALAR\"}\n"
       << "  ],\n"
       << "  \"bufferViews\": [{\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": " << buffer.size() << "}],\n"
       << "  \"buffers\": [{\"uri\": \"cube_draco.bin\", \"byteLength\": " << buffer.size() << "}]\n"
       << "}\n";

    std::ofstream jf(outDir / "cube_draco.gltf");
    jf << ss.str();
    std::printf("wrote cube_draco.gltf (%zu bytes JSON, %zu bytes compressed mesh; attribute ids POSITION=%d NORMAL=%d "
                "TEXCOORD_0=%d)\n",
                ss.str().size(), buffer.size(), posAtt, normAtt, uvAtt);

    // -----------------------------------------------------------------
    // Round-11/issue-31: a decode-FAILURE fixture -- the extension is
    // present (glTF-valid shape, same attribute-id map/bufferView as
    // cube_draco.gltf above) but the compressed payload itself is
    // corrupt (the real encoder's own valid output, truncated to its
    // first 16 bytes -- past Draco's own bitstream header but nowhere
    // near a complete mesh, so draco::Decoder::DecodeMeshFromBuffer()
    // genuinely fails to parse it rather than merely producing an empty
    // mesh). Exercises decodeDracoPrimitive()'s "Draco decode failed: touch
    // {status}" RX_LOG_ERROR path (import_gltf.cpp) -- proves a
    // corrupt-but-extension-tagged primitive fails cleanly (named error,
    // primitive skipped, no crash), never silently or via UB.
    // -----------------------------------------------------------------
    const size_t corruptSize = std::min<size_t>(16, buffer.size());
    writeFile(outDir / "cube_draco_corrupt.bin", buffer.data(), corruptSize);

    std::ostringstream ssCorrupt;
    ssCorrupt << gltfHeader("KHR_draco_mesh_compression", "KHR_draco_mesh_compression",
                             "rx_asset gen_gltf_compression_fixtures (round-11/issue-31, deliberately-corrupt Draco "
                             "payload -- decode-failure fixture)");
    ssCorrupt << "  \"scene\": 0,\n"
              << "  \"scenes\": [{\"nodes\": [0]}],\n"
              << "  \"nodes\": [{\"mesh\": 0, \"name\": \"draco_corrupt_quad\"}],\n"
              << kSharedMaterialJson
              << "  \"meshes\": [{\"name\": \"quad\", \"primitives\": [{\n"
              << "    \"attributes\": {\"POSITION\": 0, \"NORMAL\": 1, \"TEXCOORD_0\": 2}, \"indices\": 3, "
                 "\"material\": 0, \"mode\": 4,\n"
              << "    \"extensions\": {\"KHR_draco_mesh_compression\": {\"bufferView\": 0, \"attributes\": "
                 "{\"POSITION\": "
              << posAtt << ", \"NORMAL\": " << normAtt << ", \"TEXCOORD_0\": " << uvAtt << "}}}\n"
              << "  }]}],\n"
              << "  \"accessors\": [\n"
              << "    {\"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\", \"min\": [0,0,0], \"max\": "
                 "[1,1,0]},\n"
              << "    {\"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\"},\n"
              << "    {\"componentType\": 5126, \"count\": 4, \"type\": \"VEC2\"},\n"
              << "    {\"componentType\": 5125, \"count\": 6, \"type\": \"SCALAR\"}\n"
              << "  ],\n"
              << "  \"bufferViews\": [{\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": " << corruptSize << "}],\n"
              << "  \"buffers\": [{\"uri\": \"cube_draco_corrupt.bin\", \"byteLength\": " << corruptSize << "}]\n"
              << "}\n";

    std::ofstream jfCorrupt(outDir / "cube_draco_corrupt.gltf");
    jfCorrupt << ssCorrupt.str();
    std::printf("wrote cube_draco_corrupt.gltf (%zu bytes JSON, %zu bytes truncated/corrupt compressed payload)\n",
                ssCorrupt.str().size(), corruptSize);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <repo-root>\n", argv[0]);
        return 1;
    }
    std::filesystem::path outDir = std::filesystem::path(argv[1]) / "assets" / "test";
    std::filesystem::create_directories(outDir);

    genMeshoptFixture(outDir);
    genDracoFixture(outDir);
    genDracoReferenceFixture(outDir);
    return 0;
}
