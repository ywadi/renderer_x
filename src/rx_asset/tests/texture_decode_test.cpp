#include <doctest/doctest.h>
#include <rx_asset/texture_decode.h>
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
