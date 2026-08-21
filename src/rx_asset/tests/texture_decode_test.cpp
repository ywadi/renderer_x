#include <doctest/doctest.h>
#include <rx_asset/texture_decode.h>
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <stb_image.h>
#include <glm/gtc/packing.hpp>
#include <glm/vec4.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// texture_decode_test.cpp -- device-free coverage for rx::asset's KTX2/
// Basis parse+transcode decision layer and the stb PNG/JPG fallback
// decode [Phase 4 Stage 1 Task 14, spec D10, gate matrix-issue03 as
// amended by gate/rulings-2026-08-18.md #3]. No GPU, no VkDevice --
// texture_decode.h's own header comment: pure CPU parse/transcode logic.
// texture_cache_test.cpp covers the GPU-facing layer (upload/bindless/
// sampler cache/D24 eviction) built on top of this one.

using namespace rx::asset;

namespace {

std::vector<std::byte> readRepoFile(const std::string& repoRelativePath) {
    std::string path = std::string(RX_ASSET_ROOT_DIR) + "/" + repoRelativePath;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE_MESSAGE(file.good(), "file missing: " << path);
    auto size = file.tellg();
    file.seekg(0);
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

std::vector<std::byte> readFixture(const std::string& name) {
    return readRepoFile("assets/test/textures/" + name);
}

// A permissive support predicate a test can point planTranscodeFormat()
// at -- always reports every format supported, i.e. the exact-format
// branch always wins.
bool alwaysSupported(VkFormat) { return true; }
// The "format-support-forced-off" seam [matrix-issue03's own lavapipe row]
// -- always reports NOTHING supported, forcing the RGBA32-fallback
// branch deterministically regardless of what any real driver advertises.
bool neverSupported(VkFormat) { return false; }

// ===== generateStbMipChain() test-only reference math [texture-path
// round, D10 Option A] -- the IEC 61966-2-1 sRGB transfer-function
// formulas, used ONLY to compute this file's OWN expected values; the
// production code under test (texture_decode.cpp's generateStbMipChain())
// never calls these -- it hands the sRGB<->linear conversion to
// stb_image_resize2's own STBIR_TYPE_UINT8_SRGB path instead (this task's
// "prefer a ready-made library" rule). Kept deliberately independent so
// this test is a real cross-check, not a tautology against the same code
// path it is meant to verify.
float srgbByteToLinearRef(uint8_t byteValue) {
    float c = static_cast<float>(byteValue) / 255.0F;
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}
uint8_t linearToSrgbByteRef(float linear) {
    float c = linear <= 0.0031308F ? linear * 12.92F : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
    c = std::clamp(c, 0.0F, 1.0F);
    return static_cast<uint8_t>(std::lround(c * 255.0F));
}

// Swaps spdlog's default logger for an ostream-capturing one for the
// scope of one TEST_CASE, mirroring src/rx_core/tests/log_test.cpp's own
// established "RX_LOG_INFO writes... through spdlog's default logger"
// pattern (the lightweight rx_core-only capture mechanism -- this binary
// does not link rx_material, so the public rxSetLogCallback ABI is not an
// option here).
struct LogCapture {
    std::ostringstream stream;
    std::shared_ptr<spdlog::logger> previousDefault;

    LogCapture() {
        rx::core::log::init();
        previousDefault = spdlog::default_logger();
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream);
        auto testLogger = std::make_shared<spdlog::logger>("texture_decode_test", sink);
        testLogger->set_pattern("%v");
        spdlog::set_default_logger(testLogger);
    }
    ~LogCapture() { spdlog::set_default_logger(previousDefault); }

    std::string str() const { return stream.str(); }
    // Counts non-overlapping occurrences of `needle` -- used for the D11
    // "ONE log per asset" dedup assertions.
    int count(const std::string& needle) const {
        const std::string s = str();
        int n = 0;
        size_t pos = 0;
        while ((pos = s.find(needle, pos)) != std::string::npos) {
            ++n;
            pos += needle.size();
        }
        return n;
    }
};

}  // namespace

// ===== Role -> format table [D10, TOTAL over TextureRole] ==================

TEST_CASE("roleFormatTable is TOTAL over TextureRole with the exact D10 role->format assignments") {
    CHECK(roleFormatTable(TextureRole::BaseColor).transcodeTarget == KTX_TTF_BC7_RGBA);
    CHECK(roleFormatTable(TextureRole::BaseColor).exactFormat == VK_FORMAT_BC7_SRGB_BLOCK);
    CHECK(roleFormatTable(TextureRole::BaseColor).isSrgb);

    CHECK(roleFormatTable(TextureRole::Emissive).exactFormat == VK_FORMAT_BC7_SRGB_BLOCK);
    CHECK(roleFormatTable(TextureRole::Emissive).isSrgb);

    CHECK(roleFormatTable(TextureRole::Normal).transcodeTarget == KTX_TTF_BC5_RG);
    CHECK(roleFormatTable(TextureRole::Normal).exactFormat == VK_FORMAT_BC5_UNORM_BLOCK);
    CHECK_FALSE(roleFormatTable(TextureRole::Normal).isSrgb);

    CHECK(roleFormatTable(TextureRole::MetallicRoughness).transcodeTarget == KTX_TTF_BC7_RGBA);
    CHECK(roleFormatTable(TextureRole::MetallicRoughness).exactFormat == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK_FALSE(roleFormatTable(TextureRole::MetallicRoughness).isSrgb);

    CHECK(roleFormatTable(TextureRole::Occlusion).exactFormat == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK_FALSE(roleFormatTable(TextureRole::Occlusion).isSrgb);

    CHECK(roleFormatTable(TextureRole::GenericData).exactFormat == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK_FALSE(roleFormatTable(TextureRole::GenericData).isSrgb);
}

TEST_CASE("roleExpectsSrgb: baseColor/emissive MUST-sRGB per spec; normal/metallicRoughness MUST-linear; "
          "occlusion convention-linear [gate matrix Conflict C3]") {
    CHECK(roleExpectsSrgb(TextureRole::BaseColor));
    CHECK(roleExpectsSrgb(TextureRole::Emissive));
    CHECK_FALSE(roleExpectsSrgb(TextureRole::Normal));
    CHECK_FALSE(roleExpectsSrgb(TextureRole::MetallicRoughness));
    CHECK_FALSE(roleExpectsSrgb(TextureRole::Occlusion));
    CHECK_FALSE(roleExpectsSrgb(TextureRole::GenericData));
}

// ===== planTranscodeFormat: the format-support-forced-off seam ============

TEST_CASE("planTranscodeFormat picks the exact block-compressed format when the device supports it") {
    TranscodePlan plan = planTranscodeFormat(TextureRole::BaseColor, alwaysSupported);
    CHECK(plan.transcodeTarget == KTX_TTF_BC7_RGBA);
    CHECK(plan.vkFormat == VK_FORMAT_BC7_SRGB_BLOCK);
    CHECK_FALSE(plan.usedRgba32Fallback);
}

TEST_CASE("planTranscodeFormat falls back to role-correct RGBA32 when the device does NOT support the exact "
          "format [matrix-issue03 format-support-forced-off seam]") {
    TranscodePlan baseColor = planTranscodeFormat(TextureRole::BaseColor, neverSupported);
    CHECK(baseColor.transcodeTarget == KTX_TTF_RGBA32);
    CHECK(baseColor.vkFormat == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(baseColor.usedRgba32Fallback);

    TranscodePlan normal = planTranscodeFormat(TextureRole::Normal, neverSupported);
    CHECK(normal.vkFormat == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(normal.usedRgba32Fallback);
}

// ===== checkColorspaceAgreement [D10/gate ruling #3, Godot #99589] ========

TEST_CASE("checkColorspaceAgreement: role and container transfer function agreeing never disagrees") {
    CHECK_FALSE(checkColorspaceAgreement(TextureRole::BaseColor, KHR_DF_TRANSFER_SRGB).disagrees);
    CHECK_FALSE(checkColorspaceAgreement(TextureRole::Normal, KHR_DF_TRANSFER_LINEAR).disagrees);
}

TEST_CASE("checkColorspaceAgreement: a container claiming sRGB for a MUST-linear role (normal) disagrees "
          "[the sRGB-mislabeled-normal acceptance criterion's own decision function]") {
    ColorspaceCheck check = checkColorspaceAgreement(TextureRole::Normal, KHR_DF_TRANSFER_SRGB);
    CHECK(check.disagrees);
    CHECK(check.containerTransfer == KHR_DF_TRANSFER_SRGB);
    CHECK(check.expectedTransfer == KHR_DF_TRANSFER_LINEAR);
}

TEST_CASE("checkColorspaceAgreement: a container claiming linear for a MUST-sRGB role (baseColor) disagrees") {
    CHECK(checkColorspaceAgreement(TextureRole::BaseColor, KHR_DF_TRANSFER_LINEAR).disagrees);
}

TEST_CASE("checkColorspaceAgreement: KHR_DF_TRANSFER_UNSPECIFIED never disagrees (nothing concrete to "
          "contradict the role with -- avoids warning on every ordinary hand-authored KTX2)") {
    CHECK_FALSE(checkColorspaceAgreement(TextureRole::BaseColor, KHR_DF_TRANSFER_UNSPECIFIED).disagrees);
    CHECK_FALSE(checkColorspaceAgreement(TextureRole::Normal, KHR_DF_TRANSFER_UNSPECIFIED).disagrees);
}

// ===== looksLikeKtx2 =========================================================

TEST_CASE("looksLikeKtx2: true for a real KTX2 fixture's own magic bytes, false for a PNG/short/empty span") {
    auto ktx2Bytes = readFixture("basecolor_uastc.ktx2");
    CHECK(looksLikeKtx2(std::span<const std::byte>(ktx2Bytes)));

    auto pngBytes = readFixture("quadrant.png");
    CHECK_FALSE(looksLikeKtx2(std::span<const std::byte>(pngBytes)));

    std::array<std::byte, 4> tooShort{};
    CHECK_FALSE(looksLikeKtx2(std::span<const std::byte>(tooShort)));
    CHECK_FALSE(looksLikeKtx2(std::span<const std::byte>()));
}

// ===== exceedsDimensionLimit ================================================

TEST_CASE("exceedsDimensionLimit: pure comparison against a caller-supplied device limit") {
    CHECK_FALSE(exceedsDimensionLimit(1024, 1024, 4096));
    CHECK(exceedsDimensionLimit(8192, 1024, 4096));
    CHECK(exceedsDimensionLimit(1024, 8192, 4096));
    CHECK_FALSE(exceedsDimensionLimit(4096, 4096, 4096));  // exactly at the limit is NOT "exceeds"
}

// ===== DecodedKtx2Texture::parseAndTranscode ================================

TEST_CASE("DecodedKtx2Texture: UASTC baseColor fixture parses, needs transcoding, and transcodes to the "
          "role's exact BC7_SRGB format") {
    auto bytes = readFixture("basecolor_uastc.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::BaseColor, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());
    CHECK(error == Ktx2ParseError::None);
    CHECK(decoded->wasBasisEncoded());
    CHECK(decoded->width() == 4);
    CHECK(decoded->height() == 4);
    CHECK(decoded->numLevels() == 1);
    CHECK(decoded->currentVkFormat() == VK_FORMAT_BC7_SRGB_BLOCK);
    CHECK_FALSE(decoded->isUnsupportedLayout());

    auto levels = decoded->levels();
    REQUIRE(levels.size() == 1);
    CHECK(levels[0].level == 0);
    CHECK(levels[0].width == 4);
    CHECK(levels[0].height == 4);
    // One 4x4 BC7 block == 16 bytes, exactly.
    CHECK(levels[0].bytes.size() == 16);
}

TEST_CASE("DecodedKtx2Texture: ETC1S fixture parses and transcodes identically to the UASTC fixture's own "
          "target selection (both route through the SAME role->format decision)") {
    auto bytes = readFixture("basecolor_etc1s.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::BaseColor, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->wasBasisEncoded());
    CHECK(decoded->currentVkFormat() == VK_FORMAT_BC7_SRGB_BLOCK);
}

TEST_CASE("DecodedKtx2Texture: UASTC+zstd supercompressed fixture auto-inflates with no explicit zstd call "
          "anywhere in this codebase [D10 supercompression row]") {
    auto bytes = readFixture("basecolor_uastc_zstd.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::BaseColor, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->currentVkFormat() == VK_FORMAT_BC7_SRGB_BLOCK);
    auto levels = decoded->levels();
    REQUIRE(levels.size() == 1);
    CHECK(levels[0].bytes.size() == 16);
}

TEST_CASE("DecodedKtx2Texture: full mip chain (16x16 -> 1x1) reports TRUE per-level extents including the "
          "sub-block 2x2/1x1 tail, each level's byte size the exact block-rounded count [the classic "
          "off-by-one this row exists to pin down]") {
    auto bytes = readFixture("basecolor_withmips_uastc.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::BaseColor, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->numLevels() == 5);

    auto levels = decoded->levels();
    REQUIRE(levels.size() == 5);
    // True extents: 16,8,4,2,1 -- NEVER rounded up to block granularity.
    const std::array<uint32_t, 5> expectedExtent{16, 8, 4, 2, 1};
    // Block-rounded byte counts: ceil(w/4)*ceil(h/4)*16 bytes/block.
    // Levels 0-2 (16x16, 8x8, 4x4) round EXACTLY to whole blocks already;
    // levels 3-4 (2x2, 1x1) are the sub-block TAIL -- their true extent
    // shrinks below one block while the byte count does NOT (still
    // exactly one whole 16-byte BC7 block), which is precisely the
    // off-by-one a naive "size shrinks below one block" assumption would
    // get wrong.
    const std::array<size_t, 5> expectedBytes{256, 64, 16, 16, 16};
    for (size_t i = 0; i < levels.size(); ++i) {
        CAPTURE(i);
        CHECK(levels[i].level == i);
        CHECK(levels[i].width == expectedExtent[i]);
        CHECK(levels[i].height == expectedExtent[i]);
        CHECK(levels[i].bytes.size() == expectedBytes[i]);
    }
}

TEST_CASE("DecodedKtx2Texture: mips-absent fixture loads mip 0 only (1 level) -- the runtime-mip-generation-"
          "never-happens-for-compressed-formats acceptance row") {
    auto bytes = readFixture("mips_absent.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::BaseColor, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->numLevels() == 1);
}

TEST_CASE("DecodedKtx2Texture: non-multiple-of-4 base dimensions (6x5) parse with their TRUE extent -- Vulkan "
          "permits BC images with non-block-aligned extents, copies round up in blocks") {
    auto bytes = readFixture("nonmult4.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::BaseColor, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->width() == 6);
    CHECK(decoded->height() == 5);
    auto levels = decoded->levels();
    REQUIRE(levels.size() == 1);
    CHECK(levels[0].width == 6);
    CHECK(levels[0].height == 5);
    // ceil(6/4)*ceil(5/4) = 2*2 = 4 blocks * 16 bytes = 64.
    CHECK(levels[0].bytes.size() == 64);
}

// [Phase 5 Task 6, ticket #42, gate matrix-p5t06-ktx2-cubemap-hdr row 2 --
// the DISCRIMINATION PROOF] The Phase 4 test this replaces asserted
// cubemap.ktx2 was REJECTED (isUnsupportedLayout()==true) -- exactly the
// assertion a no-op cube implementation could leave passing. This test
// loads the SAME fixture (now regenerated with a real, uploadable format
// -- see generate_fixtures.sh's own comment) and asserts the FLIP:
// isUnsupportedLayout() is now false, isCube() is true, and every one of
// the 6 faces' own per-texel bytes matches its authored flat color
// EXACTLY (decoded-value discipline, not just "it parsed") -- the flip
// itself, not merely new coverage, is what this row requires.
TEST_CASE("DecodedKtx2Texture: cubemap fixture now parses as a SUPPORTED cube -- isUnsupportedLayout() flips to "
          "false, every face's real per-texel color is decoded correctly [matrix row 2, discrimination proof]") {
    auto bytes = readFixture("cubemap.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::GenericData, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());
    CHECK_FALSE(decoded->isUnsupportedLayout());
    CHECK(error == Ktx2ParseError::None);
    CHECK(decoded->isCube());
    CHECK(decoded->numFaces() == 6);
    CHECK(decoded->width() == 4);
    CHECK(decoded->height() == 4);

    // Standard KTX2 cube face order (+X,-X,+Y,-Y,+Z,-Z), matching
    // generate_fixtures.sh's own toktx invocation order exactly.
    struct FaceColor {
        uint32_t face;
        std::array<uint8_t, 4> rgba;
    };
    constexpr std::array<FaceColor, 6> kExpected{{
        {0, {0xFF, 0x00, 0x00, 0xFF}},  // +X red
        {1, {0x00, 0xFF, 0xFF, 0xFF}},  // -X cyan
        {2, {0x00, 0xFF, 0x00, 0xFF}},  // +Y green
        {3, {0xFF, 0x00, 0xFF, 0xFF}},  // -Y magenta
        {4, {0x00, 0x00, 0xFF, 0xFF}},  // +Z blue
        {5, {0xFF, 0xFF, 0x00, 0xFF}},  // -Z yellow
    }};

    for (const FaceColor& expected : kExpected) {
        auto levels = decoded->levels(expected.face);
        REQUIRE(levels.size() == 1);  // cubemap.ktx2 is deliberately single-level (row 6's own discrimination fixture)
        CHECK(levels[0].width == 4);
        CHECK(levels[0].height == 4);
        REQUIRE(levels[0].bytes.size() == 4 * 4 * 4);  // 4x4 texels, RGBA8
        for (size_t texel = 0; texel < 16; ++texel) {
            const auto* px = reinterpret_cast<const uint8_t*>(levels[0].bytes.data()) + texel * 4;
            CHECK(px[0] == expected.rgba[0]);
            CHECK(px[1] == expected.rgba[1]);
            CHECK(px[2] == expected.rgba[2]);
            CHECK(px[3] == expected.rgba[3]);
        }
    }
}

// [Phase 5 Task 6, gate matrix row 3] Flat 2D ARRAY (isArray && !isCubemap)
// stays explicitly REJECTED after the narrowed predicate -- its own
// dedicated regression fixture/test, distinct from the cube-array test
// below (the two rejection clauses are independent, gate matrix Open
// Question 1).
TEST_CASE("DecodedKtx2Texture: flat 2D-array KTX2 (isArray, numLayers>1, not a cubemap) stays REJECTED -- "
          "cubemap-only support, zero charter consumer for a general texture array [matrix row 3]") {
    auto bytes = readFixture("array2d_rejected.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::GenericData, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());  // structurally valid, parseable -- just an unsupported LAYOUT
    CHECK(decoded->isUnsupportedLayout());
    CHECK_FALSE(decoded->isCube());
    CHECK(error == Ktx2ParseError::UnsupportedLayout);
    CHECK(decoded->levels().empty());
}

// [Phase 5 Task 6, gate matrix row 3] CUBE-ARRAY (isCubemap && numLayers>1)
// ALSO stays explicitly REJECTED -- the narrowed predicate's OTHER
// rejection clause, distinct from the flat-array test above. This is the
// one shape that could plausibly be mistaken for "just a bigger cubemap"
// by an implementation that only checked isCubemap without also checking
// numLayers -- this test exists specifically to catch that mistake.
TEST_CASE("DecodedKtx2Texture: cube-array KTX2 (isCubemap AND numLayers>1) stays REJECTED, distinct from the "
          "now-supported single-layer cubemap case [matrix row 3]") {
    auto bytes = readFixture("cubearray_rejected.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::GenericData, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());  // structurally valid, parseable -- just an unsupported LAYOUT
    CHECK(decoded->isUnsupportedLayout());
    CHECK(error == Ktx2ParseError::UnsupportedLayout);
    CHECK(decoded->levels().empty());
}

TEST_CASE("DecodedKtx2Texture: non-Basis (raw RGBA8) fixture is detected via NeedsTranscoding() -- never "
          "discovered by a blind transcode call hitting KTX_INVALID_OPERATION [matrix's explicitly forbidden "
          "discovery mechanism]") {
    auto bytes = readFixture("raw_rgba8.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::BaseColor, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());
    CHECK(error == Ktx2ParseError::None);
    CHECK_FALSE(decoded->wasBasisEncoded());
    CHECK(decoded->currentVkFormat() == VK_FORMAT_R8G8B8A8_SRGB);
    auto levels = decoded->levels();
    REQUIRE(levels.size() == 1);
    CHECK(levels[0].bytes.size() == 4 * 4 * 4);  // uncompressed RGBA8, tightly packed
}

TEST_CASE("DecodedKtx2Texture: sRGB-mislabeled-normal fixture parses/transcodes successfully (role wins for "
          "the FORMAT) and its own containerTransferFunction() reports the container's real (disagreeing) "
          "claim for the caller's WARN") {
    auto bytes = readFixture("srgb_mislabeled_normal.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::Normal, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());
    CHECK(decoded->wasBasisEncoded());
    // ROLE wins: BC5_UNORM_BLOCK, never a hypothetical "BC5 sRGB" (which
    // does not even exist as a Vulkan format -- BC5 has no sRGB variant
    // at all, making this the one role where the bug class is structurally
    // impossible on the CREATED side; the WARN still fires because the
    // CONTAINER claimed sRGB).
    CHECK(decoded->currentVkFormat() == VK_FORMAT_BC5_UNORM_BLOCK);

    khr_df_transfer_e containerTransfer = decoded->containerTransferFunction();
    CHECK(containerTransfer == KHR_DF_TRANSFER_SRGB);
    ColorspaceCheck check = checkColorspaceAgreement(TextureRole::Normal, containerTransfer);
    CHECK(check.disagrees);
}

TEST_CASE("DecodedKtx2Texture: a corrupted KTX2 container (valid magic, garbage payload) fails parse with a "
          "named error, never a crash") {
    auto bytes = readFixture("corrupt.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::GenericData, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    CHECK_FALSE(decoded.has_value());
    CHECK(error == Ktx2ParseError::NotKtx2);
}

TEST_CASE("DecodedKtx2Texture: zero-length/garbage-magic bytes fail parse with a named error") {
    std::vector<std::byte> garbage(64, std::byte{0xCD});
    TranscodePlan plan = planTranscodeFormat(TextureRole::GenericData, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(garbage), plan, error);
    CHECK_FALSE(decoded.has_value());
    CHECK(error == Ktx2ParseError::NotKtx2);

    std::vector<std::byte> empty;
    error = Ktx2ParseError::None;
    auto decodedEmpty = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(empty), plan, error);
    CHECK_FALSE(decodedEmpty.has_value());
    CHECK(error == Ktx2ParseError::NotKtx2);
}

TEST_CASE("DecodedKtx2Texture is move-only and move-constructible/assignable without double-freeing the "
          "underlying ktxTexture2*") {
    auto bytes = readFixture("basecolor_uastc.ktx2");
    TranscodePlan plan = planTranscodeFormat(TextureRole::BaseColor, alwaysSupported);
    Ktx2ParseError error = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded.has_value());

    DecodedKtx2Texture moved = std::move(*decoded);
    CHECK(moved.width() == 4);
    CHECK(moved.numLevels() == 1);

    auto decoded2 = DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte>(bytes), plan, error);
    REQUIRE(decoded2.has_value());
    moved = std::move(*decoded2);  // move-assign over an already-live instance -- must destroy the first cleanly
    CHECK(moved.width() == 4);
}

// ===== stb PNG/JPG fallback decode ==========================================

TEST_CASE("decodeStbImage: a real PNG fixture decodes to the right dimensions and RGBA8 byte count") {
    auto bytes = readFixture("quadrant.png");
    std::string failureReason;
    auto decoded = decodeStbImage(std::span<const std::byte>(bytes), &failureReason);
    REQUIRE(decoded.has_value());
    CHECK(decoded->width == 8);
    CHECK(decoded->height == 8);
    CHECK(decoded->rgba8.size() == 8 * 8 * 4);
    CHECK_FALSE(decoded->was16Bit);
}

TEST_CASE("decodeStbImage: a real JPEG fixture decodes successfully") {
    auto bytes = readFixture("quadrant.jpg");
    std::string failureReason;
    auto decoded = decodeStbImage(std::span<const std::byte>(bytes), &failureReason);
    REQUIRE(decoded.has_value());
    CHECK(decoded->width == 8);
    CHECK(decoded->height == 8);
}

TEST_CASE("decodeStbImage: a 16-bit-per-channel PNG downconverts to 8-bit and reports was16Bit [D10 '16-bit "
          "PNG downconvert documented']") {
    auto bytes = readFixture("sixteen_bit.png");
    std::string failureReason;
    auto decoded = decodeStbImage(std::span<const std::byte>(bytes), &failureReason);
    REQUIRE(decoded.has_value());
    CHECK(decoded->was16Bit);
    CHECK(decoded->rgba8.size() == 4 * 4 * 4);
}

TEST_CASE("decodeStbImage: a corrupt/non-image byte stream fails cleanly with a non-empty failure reason, "
          "never a crash") {
    auto bytes = readFixture("corrupt.png");
    std::string failureReason;
    auto decoded = decodeStbImage(std::span<const std::byte>(bytes), &failureReason);
    CHECK_FALSE(decoded.has_value());
    CHECK_FALSE(failureReason.empty());
}

TEST_CASE("decodeStbImage: an empty byte span fails cleanly") {
    std::vector<std::byte> empty;
    std::string failureReason;
    auto decoded = decodeStbImage(std::span<const std::byte>(empty), &failureReason);
    CHECK_FALSE(decoded.has_value());
}

// ===== decodeStbImageHdr() [Phase 5 Task 6, ticket #42, gate matrix-
// p5t06-ktx2-cubemap-hdr row 9/13] ==========================================

TEST_CASE("decodeStbImageHdr: a real Radiance .hdr fixture decodes to the exact authored float values, "
          "including super-unity (>1.0) texels -- decoded-value discipline, the ticket's own explicit bar") {
    auto bytes = readFixture("equirect_test.hdr");
    CHECK(stbi_is_hdr_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size())) !=
          0);

    std::string failureReason;
    auto decoded = decodeStbImageHdr(std::span<const std::byte>(bytes), &failureReason);
    REQUIRE(decoded.has_value());
    CHECK(decoded->width == 2);
    CHECK(decoded->height == 2);
    REQUIRE(decoded->rgba32.size() == 2 * 2 * 4);

    // Exact per-texel values, matching generate_fixtures.sh's own RGBE
    // encoding comment precisely (mantissa_byte * ldexp(1, exponent-136)):
    // TL=(4,0.5,0.5) TR=(0.5,4,0.5) BL=(0.5,0.5,4) BR=(2,2,2). Row-major,
    // top-to-bottom (stb's own HDR row order matches the Radiance "-Y"
    // top-to-bottom convention this fixture was authored with).
    auto texel = [&](size_t index) {
        return glm::vec4(decoded->rgba32[index * 4 + 0], decoded->rgba32[index * 4 + 1],
                          decoded->rgba32[index * 4 + 2], decoded->rgba32[index * 4 + 3]);
    };
    constexpr float kEps = 0.0001F;
    glm::vec4 tl = texel(0);
    glm::vec4 tr = texel(1);
    glm::vec4 bl = texel(2);
    glm::vec4 br = texel(3);
    CHECK(tl.r == doctest::Approx(4.0F).epsilon(kEps));
    CHECK(tl.g == doctest::Approx(0.5F).epsilon(kEps));
    CHECK(tl.b == doctest::Approx(0.5F).epsilon(kEps));
    CHECK(tr.r == doctest::Approx(0.5F).epsilon(kEps));
    CHECK(tr.g == doctest::Approx(4.0F).epsilon(kEps));
    CHECK(tr.b == doctest::Approx(0.5F).epsilon(kEps));
    CHECK(bl.r == doctest::Approx(0.5F).epsilon(kEps));
    CHECK(bl.g == doctest::Approx(0.5F).epsilon(kEps));
    CHECK(bl.b == doctest::Approx(4.0F).epsilon(kEps));
    CHECK(br.r == doctest::Approx(2.0F).epsilon(kEps));
    CHECK(br.g == doctest::Approx(2.0F).epsilon(kEps));
    CHECK(br.b == doctest::Approx(2.0F).epsilon(kEps));
    // The explicit >1.0-survives bar, asserted directly and loudly (not
    // just implied by the exact-value checks above).
    CHECK(tl.r > 1.0F);
}

TEST_CASE("decodeStbImageHdr: a corrupt/truncated .hdr byte stream fails cleanly with a non-empty failure "
          "reason, never a crash [mirrors decodeStbImage()'s identical corrupt.png test]") {
    auto bytes = readFixture("corrupt.hdr");
    std::string failureReason;
    auto decoded = decodeStbImageHdr(std::span<const std::byte>(bytes), &failureReason);
    CHECK_FALSE(decoded.has_value());
    CHECK_FALSE(failureReason.empty());
}

TEST_CASE("decodeTextureForUpload: an .hdr byte stream routes to the HDR float path BEFORE the 8-bit stb branch "
          "ever sees it -- VK_FORMAT_R16G16B16A16_SFLOAT output, closing the silent-tonemap gap row 9's own "
          "addendum documents") {
    auto bytes = readFixture("equirect_test.hdr");
    TextureDecodeResult result =
        decodeTextureForUpload(std::span<const std::byte>(bytes), TextureRole::Environment,
                                /*maxImageDimension2D=*/4096, alwaysSupported);
    REQUIRE(result.outcome == TextureDecodeResult::Outcome::Ready);
    CHECK(result.format == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(result.width == 2);
    CHECK(result.height == 2);
    CHECK_FALSE(result.isCube);
    REQUIRE(result.levels.size() == 1);
    CHECK(result.levels[0].level == 0);
    CHECK(result.levels[0].faceIndex == 0);
    // 2x2 texels * 8 bytes/texel (R16G16B16A16_SFLOAT, packed via
    // glm::packHalf4x16) == 32 bytes.
    CHECK(result.levels[0].bytes.size() == 2 * 2 * 8);

    // Decode the packed bytes back and confirm the >1.0 texel survived
    // the SAME float32->float16 packing this task's real upload path
    // uses (glm::packHalf4x16/glm::unpackHalf4x16 are exact inverses for
    // any value representable in half precision -- 4.0 is exact).
    uint64_t packedTl = 0;
    std::memcpy(&packedTl, result.levels[0].bytes.data(), sizeof(packedTl));
    glm::vec4 decodedTl = glm::unpackHalf4x16(packedTl);
    CHECK(decodedTl.r == doctest::Approx(4.0F).epsilon(0.001));
    CHECK(decodedTl.r > 1.0F);
}

// ===== decodeExrImage()/looksLikeExr() [issue #75, owner insertion into
// Phase 5 Stage 1 between T10 and T11] =====================================
//
// gate_test_env.hdr/gate_test_env.exr provenance: tools/gen_exr_env_fixtures
// (main.cpp's own header comment has the full "same generator, second
// container" rationale) decodes the EXISTING, untouched, committed
// samples/08_gltf_viewer/environments/gate_test_env.hdr via
// decodeStbImageHdr() -- the SAME production function -- and re-encodes
// those exact floats as HALF-pixel-type, ZIP-compressed EXR via tinyexr's
// own SaveEXRImageToMemory(). assets/test/textures/gate_test_env.exr is a
// byte-identical copy of samples/08_gltf_viewer/environments/
// gate_test_env.exr (the sample's own --env fixture, exercised end-to-end
// by sample_08_gltf_viewer_exr_env_headless, this file's own sibling
// ctest) -- kept here too so this device-free suite never reads outside
// rx_asset's own established assets/test/textures/ fixture directory
// (readFixture()'s own convention, top of this file).

TEST_CASE("looksLikeExr: the OpenEXR magic number, and only that magic number, routes true") {
    auto bytes = readFixture("gate_test_env.exr");
    CHECK(looksLikeExr(std::span<const std::byte>(bytes)));

    auto hdrBytes = readFixture("equirect_test.hdr");
    CHECK_FALSE(looksLikeExr(std::span<const std::byte>(hdrBytes)));

    std::vector<std::byte> empty;
    CHECK_FALSE(looksLikeExr(std::span<const std::byte>(empty)));
}

TEST_CASE("decodeExrImage/container-equivalence: gate_test_env.exr decodes to the SAME float values "
          "gate_test_env.hdr decodes to, within half-float quantization epsilon -- the float-fidelity assertion "
          "that structurally rules out the LDR-collapse bug class this ticket cites, for the EXR container") {
    auto hdrBytes = readRepoFile("samples/08_gltf_viewer/environments/gate_test_env.hdr");
    std::string hdrFailure;
    auto hdrDecoded = decodeStbImageHdr(std::span<const std::byte>(hdrBytes), &hdrFailure);
    REQUIRE_MESSAGE(hdrDecoded.has_value(), "decodeStbImageHdr(gate_test_env.hdr) failed: ", hdrFailure);

    auto exrBytes = readFixture("gate_test_env.exr");
    CHECK(looksLikeExr(std::span<const std::byte>(exrBytes)));
    std::string exrFailure;
    auto exrDecoded = decodeExrImage(std::span<const std::byte>(exrBytes), &exrFailure);
    REQUIRE_MESSAGE(exrDecoded.has_value(), "decodeExrImage(gate_test_env.exr) failed: ", exrFailure);

    REQUIRE(exrDecoded->width == hdrDecoded->width);
    REQUIRE(exrDecoded->height == hdrDecoded->height);
    REQUIRE(exrDecoded->rgba32.size() == hdrDecoded->rgba32.size());

    // Epsilon justification: gate_test_env.exr stores HALF (10-bit
    // mantissa, ~2^-11 relative precision) samples re-encoded from
    // gate_test_env.hdr's own ALREADY RGBE-quantized floats (8-bit
    // shared-exponent mantissa, ~2^-8 relative precision) -- half is
    // finer than the source's own existing quantization step over this
    // fixture's value range, so the EXR round-trip adds only a small
    // further step on top of noise the .hdr already carries. 1% (0.01)
    // relative epsilon is ~20x the ~0.05% half-quantization noise floor
    // (comfortably covers accumulated per-channel rounding) while staying
    // far tighter than the deliberate channel-order sabotage this file's
    // own discrimination test below proves this epsilon rejects (that
    // corruption swaps entire channels -- order-of-magnitude, not
    // percent-scale, differences for any non-gray texel).
    double maxAbsDiff = 0.0;
    for (size_t i = 0; i < hdrDecoded->rgba32.size(); ++i) {
        double hdrVal = hdrDecoded->rgba32[i];
        double exrVal = exrDecoded->rgba32[i];
        maxAbsDiff = std::max(maxAbsDiff, std::abs(hdrVal - exrVal));
        CHECK(exrVal == doctest::Approx(hdrVal).epsilon(0.01));
    }
    INFO("max abs per-channel diff across ", hdrDecoded->rgba32.size(), " floats: ", maxAbsDiff);
    CHECK(maxAbsDiff < 0.01);
}

TEST_CASE("decodeExrImage: rejects a deep (non-image) EXR loudly, naming the variant and the supported "
          "envelope -- never silently-wrong pixels") {
    auto bytes = readFixture("exr_deep_rejected.exr");
    std::string failureReason;
    auto decoded = decodeExrImage(std::span<const std::byte>(bytes), &failureReason);
    CHECK_FALSE(decoded.has_value());
    CHECK(failureReason.find("deep") != std::string::npos);
    CHECK(failureReason.find("supported envelope") != std::string::npos);
}

TEST_CASE("decodeExrImage: rejects a tiled EXR loudly, naming the variant and the supported envelope") {
    auto bytes = readFixture("exr_tiled_rejected.exr");
    std::string failureReason;
    auto decoded = decodeExrImage(std::span<const std::byte>(bytes), &failureReason);
    CHECK_FALSE(decoded.has_value());
    CHECK(failureReason.find("tiled") != std::string::npos);
    CHECK(failureReason.find("supported envelope") != std::string::npos);
}

TEST_CASE("decodeExrImage: rejects DWAA compression loudly (genuinely unimplemented by the pinned tinyexr, "
          "confirmed directly -- not merely assumed from its README), naming the known-excluded codecs and the "
          "supported envelope") {
    auto bytes = readFixture("exr_dwaa_rejected.exr");
    std::string failureReason;
    auto decoded = decodeExrImage(std::span<const std::byte>(bytes), &failureReason);
    CHECK_FALSE(decoded.has_value());
    CHECK(failureReason.find("DWAA") != std::string::npos);
    // [issue #75 fix round 1] This IS a genuine compression-support
    // failure (tinyexr's own "Unknown compression type." message) -- the
    // known-excluded-codec parenthetical must be present, the mirror
    // image of the corrupt/truncated test's own negative assertion below.
    CHECK(failureReason.find("known excluded codecs") != std::string::npos);
    CHECK(failureReason.find("supported envelope") != std::string::npos);
}

TEST_CASE("decodeExrImage: a corrupt/truncated .exr byte stream fails cleanly with a non-empty failure reason, "
          "never a crash [mirrors decodeStbImageHdr()'s identical corrupt.hdr test] -- and, since this failure is "
          "unrelated to compression support (tinyexr's own message here is \"Failed to read attribute.\", "
          "confirmed directly), the known-excluded-codec parenthetical must NOT be appended [issue #75 fix round "
          "1: scoping proof, mirrors the DWAA rejection test's own positive case above]") {
    auto bytes = readFixture("gate_test_env.exr");
    std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + 16);
    std::string failureReason;
    auto decoded = decodeExrImage(std::span<const std::byte>(truncated), &failureReason);
    CHECK_FALSE(decoded.has_value());
    CHECK_FALSE(failureReason.empty());
    CHECK(failureReason.find("known excluded codecs") == std::string::npos);
    CHECK(failureReason.find("DWAA") == std::string::npos);
}

TEST_CASE("decodeExrImage: an empty byte span fails cleanly") {
    std::vector<std::byte> empty;
    std::string failureReason;
    auto decoded = decodeExrImage(std::span<const std::byte>(empty), &failureReason);
    CHECK_FALSE(decoded.has_value());
}

TEST_CASE("decodeTextureForUpload: an .exr byte stream routes to the EXR float path -- "
          "VK_FORMAT_R16G16B16A16_SFLOAT output, same downstream shape as the .hdr path (finalizeHdrFloatUpload() "
          "is the SAME shared tail both containers feed)") {
    auto bytes = readFixture("gate_test_env.exr");
    TextureDecodeResult result =
        decodeTextureForUpload(std::span<const std::byte>(bytes), TextureRole::Environment,
                                /*maxImageDimension2D=*/4096, alwaysSupported);
    REQUIRE(result.outcome == TextureDecodeResult::Outcome::Ready);
    CHECK(result.format == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(result.width == 64);
    CHECK(result.height == 32);
    CHECK_FALSE(result.isCube);
    REQUIRE(result.levels.size() == 1);
    CHECK(result.levels[0].level == 0);
    CHECK(result.levels[0].faceIndex == 0);
    CHECK(result.levels[0].bytes.size() == 64 * 32 * 8);
}

TEST_CASE("decodeTextureForUpload: an unsupported-variant .exr routes to Failed with the actionable reason, "
          "never a checkerboard/silent-success outcome") {
    auto bytes = readFixture("exr_dwaa_rejected.exr");
    TextureDecodeResult result =
        decodeTextureForUpload(std::span<const std::byte>(bytes), TextureRole::Environment,
                                /*maxImageDimension2D=*/4096, alwaysSupported);
    CHECK(result.outcome == TextureDecodeResult::Outcome::Failed);
    CHECK(result.failureReason.find("DWAA") != std::string::npos);
}

TEST_CASE("stbRgba8Format: role-correct SRGB vs UNORM, mirroring roleFormatTable()'s own colorspace column") {
    CHECK(stbRgba8Format(TextureRole::BaseColor) == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(stbRgba8Format(TextureRole::Emissive) == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(stbRgba8Format(TextureRole::Normal) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(stbRgba8Format(TextureRole::MetallicRoughness) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(stbRgba8Format(TextureRole::Occlusion) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(stbRgba8Format(TextureRole::GenericData) == VK_FORMAT_R8G8B8A8_UNORM);
}

// ===== generateStbMipChain() [texture-path round, D10 Option A] ===========
//
// Every fixture below is a hand-built, tiny (2x2/5x3) in-memory RGBA8
// buffer -- NOT a PNG file round-trip -- so the expected numbers are
// exact arithmetic on known input bytes, not "whatever a real photo
// happens to contain". A 2x2 -> 1x1 step is an EXACT arithmetic box
// average by construction (STBIR_FILTER_BOX's own documented "same
// result as box for integer scale ratios" -- an exact 2x downsample has
// no fractional-coverage ambiguity at all), so these are not
// approximations of what the library does -- they pin its behavior.

TEST_CASE("generateStbMipChain: BaseColor (sRGB role) box-averages in LINEAR space, re-encodes sRGB -- a "
          "naive byte average would FAIL this assertion by ~60 byte values [texture-path round item 1, "
          "revert-discrimination: reverting to `(255+0+255+0)/4` naive averaging fails this CHECK]") {
    // 2x2, two texels white (255,255,255,255), two texels black (0,0,0,255)
    // -- alpha constant 255 throughout (glTF baseColor alpha is linear
    // coverage, never sRGB -- see this fixture's own role comment).
    const std::array<uint8_t, 16> src{
        255, 255, 255, 255,  // texel 0: white
        0,   0,   0,   255,  // texel 1: black
        255, 255, 255, 255,  // texel 2: white
        0,   0,   0,   255,  // texel 3: black
    };
    auto mips = generateStbMipChain(std::span<const uint8_t>(src), 2, 2, TextureRole::BaseColor);
    REQUIRE(mips.size() == 1);  // 2x2 -> 1x1, exactly one step
    CHECK(mips[0].level == 1);
    CHECK(mips[0].width == 1);
    CHECK(mips[0].height == 1);
    REQUIRE(mips[0].bytes.size() == 4);

    auto channel = [&](size_t i) { return static_cast<uint8_t>(mips[0].bytes[i]); };

    // CORRECT (linear-space) reference: sRGB-decode 255 and 0 (-> linear
    // 1.0 and 0.0), average (-> linear 0.5), re-encode sRGB.
    float linearAvg = (srgbByteToLinearRef(255) + srgbByteToLinearRef(0)) / 2.0F;
    uint8_t expectedCorrect = linearToSrgbByteRef(linearAvg);
    CAPTURE(linearAvg);
    CAPTURE(static_cast<int>(expectedCorrect));
    CAPTURE(static_cast<int>(channel(0)));

    // The discriminating band: correct linear-space averaging of
    // (255,0,255,0) lands at ~188 (IEC 61966-2-1 formula: 1.055*0.5^(1/2.4)
    // - 0.055 -> 0.735 -> byte 187-188); a NAIVE byte average of the same
    // four bytes is (255+0+255+0)/4 = 127.5 -> byte 127 or 128, a ~60-value
    // gap -- generously tolerant (+-12) on the correct side while staying
    // FAR outside the naive value's own neighborhood proves this is a real
    // discriminating assertion, not a coincidence of a wide tolerance.
    CHECK(channel(0) > static_cast<uint8_t>(expectedCorrect - 12));
    CHECK(channel(0) < static_cast<uint8_t>(expectedCorrect + 12));
    CHECK(channel(1) == channel(0));  // R/G/B identical -- the source is achromatic
    CHECK(channel(2) == channel(0));
    // The naive-average value (127 or 128) must NOT be in range -- this is
    // the actual revert-discrimination check: a hand-reverted
    // `(a+b+c+d)/4` byte-average implementation produces 127 or 128 here,
    // clearly outside [expectedCorrect-12, expectedCorrect+12] for any
    // expectedCorrect near 188.
    CHECK(channel(0) > 150);

    // Alpha is constant 255 input -- plain average either way, sRGB or
    // not (glTF alpha is never sRGB-transformed).
    CHECK(channel(3) == 255);
}

TEST_CASE("generateStbMipChain: role == Normal renormalizes the averaged tangent-space vector to unit length "
          "-- a plain (unrenormalized) average would FAIL the unit-length assertion [texture-path round item 2, "
          "revert-discrimination: skipping the renormalize step leaves a ~0.80-length vector, failing the "
          "[0.99,1.01] length band below]") {
    // Two symmetric unit tangent-space normals, N1=(0.6,0.8,0) and
    // N2=(-0.6,0.8,0) -- their PLAIN average is (0,0.8,0), length 0.8 (a
    // real, non-degenerate "flattening" case, not a contrived zero-vector
    // edge case). Byte-encode: (v*0.5+0.5)*255, rounded.
    auto encode = [](float v) -> uint8_t {
        return static_cast<uint8_t>(std::lround(std::clamp((v * 0.5F + 0.5F) * 255.0F, 0.0F, 255.0F)));
    };
    const uint8_t n1r = encode(0.6F), n1g = encode(0.8F), n1b = encode(0.0F);
    const uint8_t n2r = encode(-0.6F), n2g = encode(0.8F), n2b = encode(0.0F);

    const std::array<uint8_t, 16> src{
        n1r, n1g, n1b, 255, n2r, n2g, n2b, 255, n1r, n1g, n1b, 255, n2r, n2g, n2b, 255,
    };
    auto mips = generateStbMipChain(std::span<const uint8_t>(src), 2, 2, TextureRole::Normal);
    REQUIRE(mips.size() == 1);
    REQUIRE(mips[0].bytes.size() == 4);

    auto decode = [](std::byte b) { return (static_cast<float>(b) / 255.0F) * 2.0F - 1.0F; };
    float x = decode(mips[0].bytes[0]);
    float y = decode(mips[0].bytes[1]);
    float z = decode(mips[0].bytes[2]);
    float length = std::sqrt(x * x + y * y + z * z);
    CAPTURE(x);
    CAPTURE(y);
    CAPTURE(z);
    CAPTURE(length);

    // THE discriminating assertion: unit length. The pre-renormalize
    // average has length ~0.80 (the two input vectors' own Y=0.8, X/Z
    // canceling), which fails this tight [0.99,1.01] band outright -- a
    // reverted implementation that skips renormalizeNormalTexelsInPlace()
    // fails HERE, not on some unrelated symptom.
    CHECK(length > 0.99F);
    CHECK(length < 1.01F);
    // The averaged direction itself is still preserved (Y dominant,
    // pointing "up") -- renormalization changes MAGNITUDE, not direction.
    CHECK(y > 0.9F);
}

TEST_CASE("generateStbMipChain: MetallicRoughness/Occlusion/GenericData use a plain LINEAR box average -- no "
          "sRGB transform, no renormalization [texture-path round item 3]") {
    // Same achromatic (255,0,255,0) checkerboard as the sRGB test above,
    // but role=MetallicRoughness this time: the CORRECT answer here is
    // the PLAIN byte average (127 or 128) -- the exact value the sRGB
    // test's own revert-discrimination check rejects for BaseColor.
    const std::array<uint8_t, 16> src{
        255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255,
    };
    auto mips = generateStbMipChain(std::span<const uint8_t>(src), 2, 2, TextureRole::MetallicRoughness);
    REQUIRE(mips.size() == 1);
    auto channel = [&](size_t i) { return static_cast<uint8_t>(mips[0].bytes[i]); };
    CAPTURE(static_cast<int>(channel(0)));
    // Plain arithmetic mean of (255,0,255,0) = 127.5 -> 127 or 128
    // depending on the resize implementation's own rounding -- NEVER the
    // sRGB-correct ~188 the BaseColor test above expects for the
    // identical input bytes (proving role, not content, selects the
    // colorspace path).
    CHECK(channel(0) >= 127);
    CHECK(channel(0) <= 128);
}

TEST_CASE("generateStbMipChain: chain length/dimensions follow the standard floor-halving formula for a "
          "non-power-of-two source (5x3), matching Vulkan's own floor(log2(max(w,h)))+1 total-level rule "
          "[texture-path round item 5]") {
    // Content doesn't matter for this test -- a flat mid-gray source is
    // enough (chain SHAPE, not per-texel color, is under test).
    std::vector<uint8_t> src(static_cast<size_t>(5) * 3 * 4, 128);
    auto mips = generateStbMipChain(std::span<const uint8_t>(src), 5, 3, TextureRole::GenericData);
    // 5x3 -(floor-half)-> 2x1 -(floor-half)-> 1x1: 2 generated levels (+
    // level 0, which this function never returns) = 3 total, matching
    // floor(log2(5))+1 = 2+1 = 3.
    REQUIRE(mips.size() == 2);
    CHECK(mips[0].level == 1);
    CHECK(mips[0].width == 2);
    CHECK(mips[0].height == 1);
    CHECK(mips[1].level == 2);
    CHECK(mips[1].width == 1);
    CHECK(mips[1].height == 1);
}

TEST_CASE("generateStbMipChain: a power-of-two square source (8x8) produces the exact 4-level chain "
          "(8,4,2,1) [texture-path round item 5/6c]") {
    std::vector<uint8_t> src(static_cast<size_t>(8) * 8 * 4, 200);
    auto mips = generateStbMipChain(std::span<const uint8_t>(src), 8, 8, TextureRole::BaseColor);
    REQUIRE(mips.size() == 3);  // levels 1,2,3 (level 0 excluded) -> 4x4, 2x2, 1x1
    const std::array<uint32_t, 3> expectedExtent{4, 2, 1};
    for (size_t i = 0; i < mips.size(); ++i) {
        CAPTURE(i);
        CHECK(mips[i].level == i + 1);
        CHECK(mips[i].width == expectedExtent[i]);
        CHECK(mips[i].height == expectedExtent[i]);
    }
}

TEST_CASE("generateStbMipChain: degenerate input (width/height 0, or fewer bytes than width*height*4) returns "
          "an empty chain rather than fabricating levels from too little data") {
    std::vector<uint8_t> tooShort(4, 0);  // claims to be 2x2 (needs 16 bytes) but only has 4
    CHECK(generateStbMipChain(std::span<const uint8_t>(tooShort), 2, 2, TextureRole::BaseColor).empty());
    CHECK(generateStbMipChain(std::span<const uint8_t>(tooShort), 0, 2, TextureRole::BaseColor).empty());
    CHECK(generateStbMipChain(std::span<const uint8_t>(tooShort), 2, 0, TextureRole::BaseColor).empty());
}

// ===== decodeTextureForUpload() end-to-end: stb path now generates its =====
// ===== own full runtime mip chain [texture-path round, D10 Option A]  =====

TEST_CASE("decodeTextureForUpload: a real 8x8 PNG fixture (stb path) now produces a FULL 4-level mip chain "
          "(8,4,2,1), level 0 byte-identical to the un-mipped decode, format/role unaffected") {
    auto bytes = readFixture("quadrant.png");
    TextureDecodeResult result =
        decodeTextureForUpload(std::span<const std::byte>(bytes), TextureRole::BaseColor, 4096, alwaysSupported);
    REQUIRE(result.outcome == TextureDecodeResult::Outcome::Ready);
    CHECK(result.width == 8);
    CHECK(result.height == 8);
    CHECK(result.format == VK_FORMAT_R8G8B8A8_SRGB);
    REQUIRE(result.levels.size() == 4);
    const std::array<uint32_t, 4> expectedExtent{8, 4, 2, 1};
    for (size_t i = 0; i < result.levels.size(); ++i) {
        CAPTURE(i);
        CHECK(result.levels[i].level == i);
        CHECK(result.levels[i].width == expectedExtent[i]);
        CHECK(result.levels[i].height == expectedExtent[i]);
    }
    // Level 0 itself is UNCHANGED by this task -- exactly stb's own
    // decoded bytes, byte-for-byte (only levels 1+ are new).
    CHECK(result.levels[0].bytes.size() == 8 * 8 * 4);
}

// ===== Suppress unused-function warnings for the LogCapture utility ========
// (reserved for texture_cache_test.cpp-style WARN-content assertions if a
// future device-free case needs one; kept here since this is the shared
// device-free binary's own translation unit and the utility is otherwise
// header-local-only noise to duplicate per file).
TEST_CASE("LogCapture utility captures spdlog output for the scope of one TEST_CASE") {
    LogCapture capture;
    RX_LOG_WARN("marker-{}", 7);
    CHECK(capture.count("marker-7") == 1);
    CHECK(capture.count("marker-7-nonexistent") == 0);
}
