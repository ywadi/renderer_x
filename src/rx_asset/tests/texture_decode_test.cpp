#include <doctest/doctest.h>
#include <rx_asset/texture_decode.h>
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <algorithm>
#include <array>
#include <cstddef>
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

std::vector<std::byte> readFixture(const std::string& name) {
    std::string path = std::string(RX_ASSET_ROOT_DIR) + "/assets/test/textures/" + name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE_MESSAGE(file.good(), "fixture missing: " << path);
    auto size = file.tellg();
    file.seekg(0);
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

// A permissive support predicate a test can point planTranscodeFormat()
// at -- always reports every format supported, i.e. the exact-format
// branch always wins.
bool alwaysSupported(VkFormat) { return true; }
// The "format-support-forced-off" seam [matrix-issue03's own lavapipe row]
// -- always reports NOTHING supported, forcing the RGBA32-fallback
// branch deterministically regardless of what any real driver advertises.
bool neverSupported(VkFormat) { return false; }

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

TEST_CASE("DecodedKtx2Texture: cubemap fixture is classified UnsupportedLayout, never silently treated as a "
          "2D slice [matrix 'Cubemap/array/3D' row]") {
    auto bytes = readFixture("cubemap.ktx2");
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

TEST_CASE("stbRgba8Format: role-correct SRGB vs UNORM, mirroring roleFormatTable()'s own colorspace column") {
    CHECK(stbRgba8Format(TextureRole::BaseColor) == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(stbRgba8Format(TextureRole::Emissive) == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(stbRgba8Format(TextureRole::Normal) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(stbRgba8Format(TextureRole::MetallicRoughness) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(stbRgba8Format(TextureRole::Occlusion) == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(stbRgba8Format(TextureRole::GenericData) == VK_FORMAT_R8G8B8A8_UNORM);
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
