// gen_exr_env_fixtures -- see this directory's CMakeLists.txt for the full
// rationale. [Phase 5, issue #75, owner insertion into Stage 1 between T10
// and T11] Regenerates:
//   assets/test/textures/gate_test_env.exr                 (container-equivalence + rejection-test sibling)
//   samples/08_gltf_viewer/environments/gate_test_env.exr  (full-chain --env routing fixture)
//   assets/test/textures/exr_deep_rejected.exr              (deep/non-image variant -- rejection fixture)
//   assets/test/textures/exr_tiled_rejected.exr             (tiled variant -- rejection fixture)
//   assets/test/textures/exr_dwaa_rejected.exr              (DWAA compression -- rejection fixture)
//
// PROVENANCE / "same generator, second container": samples/08_gltf_viewer/
// environments/gate_test_env.hdr (the existing committed Radiance fixture,
// task-10-report.md) was itself written by a one-off Python script that
// was NEVER committed (task-10-report.md's own line: "written directly
// via a one-off Python script... this is what the D17 gate's own default
// ... headless run now bakes and binds" -- no generator source alongside
// it). Rather than guess-reconstruct that undocumented formula (risking
// silent value drift that could ripple into the ALREADY-PASSING,
// committed D17 visual reference PNGs baked from the EXACT existing .hdr
// bytes -- this ticket is explicitly routing-only, "no new sample logic"),
// this tool instead decodes the existing, frozen, untouched gate_test_env
// .hdr through rx::asset::decodeStbImageHdr() -- the SAME production
// decode function the real Environment-role .hdr path already uses -- and
// re-encodes those EXACT floats as OpenEXR via tinyexr's own
// SaveEXRImageToMemory(). This guarantees true content identity BY
// CONSTRUCTION (one canonical set of pixel values, two container
// encoders), never an approximation of a lost formula, and never touches
// gate_test_env.hdr's own bytes.
//
// FORMAT CHOICE for the written .exr: HALF pixel type, ZIP compression.
// HALF exercises the half-precision leg of the ticket's own "half and
// float" scope bar; its ~10-bit mantissa is comparable to (and in most of
// this fixture's value range, finer than) the 8-bit shared-exponent
// mantissa the SOURCE .hdr's own Radiance RGBE encoding already
// quantized these floats to, so re-quantizing to half introduces at most
// a small ADDITIONAL step on top of quantization this fixture's floats
// already carry -- exactly the "half-float quantization" epsilon
// task-exr-report.md justifies for the container-equivalence proof. ZIP
// is a real (non-NONE) compression codec inside the supported envelope
// (texture_decode.h's own EXR block comment), giving the container-
// equivalence/full-chain tests real compressed-scanline coverage, not
// just an uncompressed round-trip.
//
// Run with one argument: the repository root (matches
// gen_gltf_compression_fixtures's own `<repo-root>` CLI convention).
#include <rx_asset/texture_decode.h>

#include <tinyexr.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.good()) {
        std::fprintf(stderr, "gen_exr_env_fixtures: could not open %s\n", path.string().c_str());
        std::exit(1);
    }
    auto size = file.tellg();
    file.seekg(0);
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

void writeFile(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::printf("gen_exr_env_fixtures: wrote %zu bytes -> %s\n", bytes.size(), path.string().c_str());
}

// Encodes `rgba32` (tightly packed, 4 floats/texel, row-major top-to-
// bottom -- exactly rx::asset::DecodedStbHdrImage's own layout) as a
// scanline EXR: HALF pixel type, `compression`, four channels (A,B,G,R --
// tinyexr's own alphabetical channel-order convention, verified directly
// against its own example code and this task's probe). Aborts the whole
// tool (fixture generation is a one-off host step, not a runtime path --
// a hard failure here should stop the build, not be silently swallowed)
// on any tinyexr write failure.
std::vector<unsigned char> encodeExr(const std::vector<float>& rgba32, int width, int height, int compression) {
    EXRHeader header;
    InitEXRHeader(&header);
    EXRImage image;
    InitEXRImage(&image);

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<float> rCh(pixelCount);
    std::vector<float> gCh(pixelCount);
    std::vector<float> bCh(pixelCount);
    std::vector<float> aCh(pixelCount);
    for (size_t i = 0; i < pixelCount; ++i) {
        rCh[i] = rgba32[i * 4 + 0];
        gCh[i] = rgba32[i * 4 + 1];
        bCh[i] = rgba32[i * 4 + 2];
        aCh[i] = rgba32[i * 4 + 3];
    }
    float* imagePtrs[4] = {aCh.data(), bCh.data(), gCh.data(), rCh.data()};
    image.images = reinterpret_cast<unsigned char**>(imagePtrs);
    image.width = width;
    image.height = height;
    image.num_channels = 4;

    header.num_channels = 4;
    header.channels = static_cast<EXRChannelInfo*>(malloc(sizeof(EXRChannelInfo) * 4));
    const char* channelNames[4] = {"A", "B", "G", "R"};
    for (int i = 0; i < 4; ++i) {
        std::memset(header.channels[i].name, 0, sizeof(header.channels[i].name));
        std::strncpy(header.channels[i].name, channelNames[i], sizeof(header.channels[i].name) - 1);
    }
    header.pixel_types = static_cast<int*>(malloc(sizeof(int) * 4));
    header.requested_pixel_types = static_cast<int*>(malloc(sizeof(int) * 4));
    for (int i = 0; i < 4; ++i) {
        header.pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;         // source samples handed to tinyexr are float32
        header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_HALF;  // stored on disk as half
    }
    header.compression_type = compression;

    unsigned char* mem = nullptr;
    const char* err = nullptr;
    size_t sz = SaveEXRImageToMemory(&image, &header, &mem, &err);
    free(header.channels);
    free(header.pixel_types);
    free(header.requested_pixel_types);
    if (sz == 0) {
        std::fprintf(stderr, "gen_exr_env_fixtures: SaveEXRImageToMemory failed: %s\n", err != nullptr ? err : "(no message)");
        if (err != nullptr) {
            FreeEXRErrorMessage(err);
        }
        std::exit(1);
    }
    std::vector<unsigned char> result(mem, mem + sz);
    free(mem);
    return result;
}

// Locates the "compression" attribute's single data byte inside an
// already-encoded EXR header (scanning the raw attribute stream: each
// attribute is `name\0 type\0 int32-size data[size]`) and overwrites it
// with `newCompression` -- used ONLY to build the DWAA-rejection fixture
// below (a syntactically valid header that DECLARES an unsupported
// compression code tinyexr's own ParseEXRHeaderFromMemory() rejects at
// header-parse time, before any pixel data is ever touched -- exactly
// what decodeExrImage()'s own rejection path exercises). Aborts if the
// attribute cannot be found (a hard, loud failure for a fixture-
// generation bug, never a silently-wrong fixture).
std::vector<unsigned char> patchCompressionByte(std::vector<unsigned char> bytes, unsigned char newCompression) {
    const std::string needle = "compression";
    for (size_t i = 0; i + needle.size() < bytes.size(); ++i) {
        if (std::memcmp(&bytes[i], needle.data(), needle.size()) != 0 || bytes[i + needle.size()] != '\0') {
            continue;
        }
        size_t p = i + needle.size() + 1;
        size_t typeStart = p;
        while (p < bytes.size() && bytes[p] != '\0') {
            ++p;
        }
        if (p >= bytes.size()) {
            continue;
        }
        std::string type(reinterpret_cast<char*>(&bytes[typeStart]), p - typeStart);
        ++p;  // skip the type string's own null terminator
        if (type != "compression" || p + 4 > bytes.size()) {
            continue;
        }
        int32_t size = 0;
        std::memcpy(&size, &bytes[p], 4);
        p += 4;
        if (size != 1 || p >= bytes.size()) {
            continue;
        }
        bytes[p] = newCompression;
        return bytes;
    }
    std::fprintf(stderr, "gen_exr_env_fixtures: could not locate the 'compression' attribute to patch\n");
    std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gen_exr_env_fixtures <repo-root>\n");
        return 1;
    }
    std::filesystem::path repoRoot(argv[1]);

    std::filesystem::path hdrPath = repoRoot / "samples/08_gltf_viewer/environments/gate_test_env.hdr";
    std::vector<std::byte> hdrBytes = readFile(hdrPath);

    std::string failureReason;
    auto decoded = rx::asset::decodeStbImageHdr(std::span<const std::byte>(hdrBytes), &failureReason);
    if (!decoded.has_value()) {
        std::fprintf(stderr, "gen_exr_env_fixtures: decodeStbImageHdr(%s) failed: %s\n", hdrPath.string().c_str(),
                      failureReason.c_str());
        return 1;
    }
    std::printf("gen_exr_env_fixtures: decoded %s -> %ux%u float RGBA texels\n", hdrPath.string().c_str(),
                 decoded->width, decoded->height);

    std::vector<unsigned char> goodExr =
        encodeExr(decoded->rgba32, static_cast<int>(decoded->width), static_cast<int>(decoded->height),
                   TINYEXR_COMPRESSIONTYPE_ZIP);

    writeFile(repoRoot / "assets/test/textures/gate_test_env.exr", goodExr);
    writeFile(repoRoot / "samples/08_gltf_viewer/environments/gate_test_env.exr", goodExr);

    // Deep (non-image) rejection fixture: patch the EXRVersion flags
    // byte's non_image bit (0x08, the 11th bit per the OpenEXR 2.0 spec
    // and tinyexr's own ParseEXRVersionFromMemory()) directly onto the
    // ALREADY-VALID gate_test_env.exr bytes -- version.non_image is all
    // decodeExrImage()'s own rejection gate inspects (an 8-byte parse),
    // so the remainder of the file being an ordinary flat image is
    // irrelevant to what this fixture exercises.
    {
        std::vector<unsigned char> deepExr = goodExr;
        deepExr[5] |= 0x08;
        writeFile(repoRoot / "assets/test/textures/exr_deep_rejected.exr", deepExr);
    }

    // Tiled rejection fixture: same technique, the tiled bit (0x02, 9th
    // bit).
    {
        std::vector<unsigned char> tiledExr = goodExr;
        tiledExr[5] |= 0x02;
        writeFile(repoRoot / "assets/test/textures/exr_tiled_rejected.exr", tiledExr);
    }

    // DWAA-compression rejection fixture: patch the header's own
    // "compression" attribute value byte from ZIP(3) to DWAA(8) --
    // tinyexr's ParseEXRHeaderFromMemory() has no allow-list entry for
    // DWAA (its own header marks it "Not yet supported"; verified
    // directly, task-exr-report.md), so this is genuinely rejected by the
    // vendored library itself, not merely by this repository's own
    // wrapper.
    {
        std::vector<unsigned char> dwaaExr = patchCompressionByte(goodExr, TINYEXR_COMPRESSIONTYPE_DWAA);
        writeFile(repoRoot / "assets/test/textures/exr_dwaa_rejected.exr", dwaaExr);
    }

    return 0;
}
