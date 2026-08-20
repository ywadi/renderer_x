#include <rx_asset/texture_decode.h>
#include <rx_core/log.h>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace rx::asset {

const char* textureRoleName(TextureRole role) {
    switch (role) {
        case TextureRole::BaseColor:
            return "baseColor";
        case TextureRole::Emissive:
            return "emissive";
        case TextureRole::Normal:
            return "normal";
        case TextureRole::MetallicRoughness:
            return "metallicRoughness";
        case TextureRole::Occlusion:
            return "occlusion";
        case TextureRole::GenericData:
            return "genericData";
    }
    return "unknown";
}

bool roleExpectsSrgb(TextureRole role) {
    // [glTF 2.0 spec, quoted in gate matrix-issue03] baseColorTexture/
    // emissiveTexture "MUST contain 8-bit values encoded with the sRGB
    // opto-electronic transfer function"; every other role is linear --
    // metallicRoughnessTexture/normalTexture are explicit MUST-linear
    // spec text, occlusionTexture is convention-linear (the spec defines
    // only its scalar semantics, never a transfer function -- gate matrix
    // Conflict C3, cited precisely rather than lumped in with the
    // MUST-linear pair).
    return role == TextureRole::BaseColor || role == TextureRole::Emissive;
}

// D10 role -> transcode-target table [matrix-issue03 "Role->format matrix
// completeness" row]: TOTAL over TextureRole via an exhaustive switch with
// NO default case -- adding an enumerator without a case here produces a
// compiler WARNING (-Wswitch; this project's build carries no -Werror
// anywhere, verified directly, so this is real but non-fatal build-log
// signal, not a hard compile error), not a silently-wrong runtime
// default. BC4_R
// for single-channel occlusion is D10's "recorded option" -- NOT
// implemented here (BC7_UNORM stands for occlusion, matching D10's own
// table verbatim); a future task may add it as a genuinely distinct
// entry once something calls for it.
const RoleFormatEntry& roleFormatTable(TextureRole role) {
    static constexpr RoleFormatEntry kBaseColorEmissive{KTX_TTF_BC7_RGBA, VK_FORMAT_BC7_SRGB_BLOCK,
                                                          VK_FORMAT_R8G8B8A8_SRGB, /*isSrgb=*/true};
    static constexpr RoleFormatEntry kNormal{KTX_TTF_BC5_RG, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM,
                                              /*isSrgb=*/false};
    static constexpr RoleFormatEntry kUnormData{KTX_TTF_BC7_RGBA, VK_FORMAT_BC7_UNORM_BLOCK,
                                                 VK_FORMAT_R8G8B8A8_UNORM, /*isSrgb=*/false};
    switch (role) {
        case TextureRole::BaseColor:
        case TextureRole::Emissive:
            return kBaseColorEmissive;
        case TextureRole::Normal:
            return kNormal;
        case TextureRole::MetallicRoughness:
        case TextureRole::Occlusion:
        case TextureRole::GenericData:
            return kUnormData;
    }
    // Unreachable given the exhaustive switch above (every TextureRole
    // enumerator is covered) -- kept as a defensive terminator rather
    // than UB on an out-of-range enum value, matching this codebase's own
    // memoryCategoryName()-style convention for exhaustive-but-defensive
    // lookups.
    return kUnormData;
}

TranscodePlan planTranscodeFormat(TextureRole role, const FormatSupportQuery& isFormatSupported) {
    const RoleFormatEntry& entry = roleFormatTable(role);
    TranscodePlan plan;
    plan.transcodeTarget = entry.transcodeTarget;
    if (isFormatSupported && isFormatSupported(entry.exactFormat)) {
        plan.vkFormat = entry.exactFormat;
        plan.usedRgba32Fallback = false;
    } else {
        plan.transcodeTarget = KTX_TTF_RGBA32;
        plan.vkFormat = entry.rgba32Fallback;
        plan.usedRgba32Fallback = true;
    }
    return plan;
}

ColorspaceCheck checkColorspaceAgreement(TextureRole role, khr_df_transfer_e containerTransfer) {
    ColorspaceCheck result;
    result.containerTransfer = containerTransfer;
    result.expectedTransfer = roleExpectsSrgb(role) ? KHR_DF_TRANSFER_SRGB : KHR_DF_TRANSFER_LINEAR;
    // KHR_DF_TRANSFER_UNSPECIFIED (a container that never set a transfer
    // field at all) is not treated as a disagreement -- there is nothing
    // concrete to contradict the role with, and warning on every such
    // (very common, e.g. hand-authored raw KTX2) file would violate the
    // "one log per asset, no spam on the ordinary case" discipline D11
    // establishes for the rest of this pipeline.
    result.disagrees = containerTransfer != KHR_DF_TRANSFER_UNSPECIFIED && containerTransfer != result.expectedTransfer;
    return result;
}

const char* ktx2ParseErrorName(Ktx2ParseError error) {
    switch (error) {
        case Ktx2ParseError::None:
            return "None";
        case Ktx2ParseError::NotKtx2:
            return "NotKtx2";
        case Ktx2ParseError::ZeroDimension:
            return "ZeroDimension";
        case Ktx2ParseError::UnsupportedLayout:
            return "UnsupportedLayout";
    }
    return "Unknown";
}

bool looksLikeKtx2(std::span<const std::byte> bytes) {
    // KTX2 spec 3.1 "File Identifier" -- fixed 12-byte magic, the same
    // constant libktx's own reader validates against (verified directly,
    // lib/texture2.c's ktxCheckHeader2_ / checkheader.c).
    static constexpr std::array<uint8_t, 12> kMagic{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
                                                     0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < kMagic.size()) {
        return false;
    }
    return std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) == 0;
}

bool exceedsDimensionLimit(uint32_t width, uint32_t height, uint32_t maxDimension2D) {
    return width > maxDimension2D || height > maxDimension2D;
}

namespace {

bool isUnsupportedLayoutFor(const ktxTexture2* tex) {
    return tex->isArray || tex->isCubemap || tex->numFaces > 1 || tex->numLayers > 1 || tex->numDimensions != 2;
}

}  // namespace

DecodedKtx2Texture::DecodedKtx2Texture(ktxTexture2* texture, bool wasBasisEncoded, bool unsupportedLayout,
                                        VkFormat currentFormat, khr_df_transfer_e originalTransfer)
    : texture_(texture), wasBasisEncoded_(wasBasisEncoded), unsupportedLayout_(unsupportedLayout),
      currentFormat_(currentFormat), originalTransfer_(originalTransfer) {}

DecodedKtx2Texture::DecodedKtx2Texture(DecodedKtx2Texture&& other) noexcept
    : texture_(std::exchange(other.texture_, nullptr)), wasBasisEncoded_(other.wasBasisEncoded_),
      unsupportedLayout_(other.unsupportedLayout_), currentFormat_(other.currentFormat_),
      originalTransfer_(other.originalTransfer_) {}

DecodedKtx2Texture& DecodedKtx2Texture::operator=(DecodedKtx2Texture&& other) noexcept {
    if (this != &other) {
        if (texture_ != nullptr) {
            ktxTexture2_Destroy(texture_);
        }
        texture_ = std::exchange(other.texture_, nullptr);
        wasBasisEncoded_ = other.wasBasisEncoded_;
        unsupportedLayout_ = other.unsupportedLayout_;
        currentFormat_ = other.currentFormat_;
        originalTransfer_ = other.originalTransfer_;
    }
    return *this;
}

DecodedKtx2Texture::~DecodedKtx2Texture() {
    if (texture_ != nullptr) {
        ktxTexture2_Destroy(texture_);
    }
}

std::optional<DecodedKtx2Texture> DecodedKtx2Texture::parseAndTranscode(std::span<const std::byte> bytes,
                                                                          const TranscodePlan& plan,
                                                                          Ktx2ParseError& outError) {
    outError = Ktx2ParseError::None;

    ktxTexture2* texture = nullptr;
    // ktxTexture2_CreateFromMemory ONLY -- zero std::filesystem/fopen in
    // this whole translation unit (the IO-source abstraction invariant;
    // grep-enforced review criterion, matrix-issue03). LOAD_IMAGE_DATA_BIT
    // is required -- without it pData stays null (header-only parse) and
    // this class would have nothing to transcode or upload.
    KTX_error_code rc = ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(bytes.data()), bytes.size(),
                                                       KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture);
    if (rc != KTX_SUCCESS || texture == nullptr) {
        outError = Ktx2ParseError::NotKtx2;
        return std::nullopt;
    }

    if (texture->baseWidth == 0 || texture->baseHeight == 0) {
        outError = Ktx2ParseError::ZeroDimension;
        ktxTexture2_Destroy(texture);
        return std::nullopt;
    }

    // Captured HERE, immediately after parse, BEFORE any transcode call --
    // see containerTransferFunction()'s own header comment (texture_decode.h)
    // for why a live re-query after TranscodeBasis() is wrong: that call
    // REWRITES the object's own DFD to describe the transcode OUTPUT
    // format (e.g. a BC5 target has no sRGB variant at all, so the DFD
    // reports LINEAR post-transcode regardless of the container's
    // original claim) -- reproduced directly, this task's own testing
    // (the sRGB-mislabeled-normal fixture's disagreement silently
    // vanished before this fix). Captured for EVERY branch below,
    // including the unsupported-layout and non-Basis ones, so the
    // accessor's contract is uniform regardless of which path a given
    // container took.
    khr_df_transfer_e originalTransfer = ktxTexture2_GetTransferFunction_e(texture);

    if (isUnsupportedLayoutFor(texture)) {
        // Still a structurally valid, parseable container -- returned as
        // a real (if inert) instance so a caller can inspect width()/
        // height()/isUnsupportedLayout() for its own WARN, per this
        // class's own header comment. outError is ALSO set (matrix's
        // "Cubemap/array/3D" row's caller uses whichever signal is more
        // convenient at its own call site).
        outError = Ktx2ParseError::UnsupportedLayout;
        return DecodedKtx2Texture(texture, /*wasBasisEncoded=*/false, /*unsupportedLayout=*/true,
                                   static_cast<VkFormat>(texture->vkFormat), originalTransfer);
    }

    // The non-discovery-via-KTX_INVALID_OPERATION mandate [matrix-issue03,
    // BINDING]: NeedsTranscoding() (DFD-color-model-driven internally,
    // verified directly against basis_transcode.cpp/texture2.c at v4.4.2)
    // is the ONLY thing consulted to decide whether TranscodeBasis() is
    // even called -- a plain, already-uploadable (non-Basis) KTX2 never
    // reaches a transcode call at all, so it can never hit
    // KTX_INVALID_OPERATION as a "detection" mechanism.
    if (!ktxTexture2_NeedsTranscoding(texture)) {
        return DecodedKtx2Texture(texture, /*wasBasisEncoded=*/false, /*unsupportedLayout=*/false,
                                   static_cast<VkFormat>(texture->vkFormat), originalTransfer);
    }

    rc = ktxTexture2_TranscodeBasis(texture, plan.transcodeTarget, /*transcodeFlags=*/0);
    if (rc != KTX_SUCCESS) {
        RX_LOG_ERROR("rx_asset: DecodedKtx2Texture::parseAndTranscode: ktxTexture2_TranscodeBasis failed: {}",
                     ktxErrorString(rc));
        outError = Ktx2ParseError::NotKtx2;
        ktxTexture2_Destroy(texture);
        return std::nullopt;
    }

    // ROLE-AUTHORITATIVE RELABEL [D10/gate ruling #3, BINDING]: libktx's
    // own TranscodeBasis() picks the sRGB-vs-UNORM `vkFormat` VARIANT from
    // the CONTAINER's own DFD transfer field (verified directly,
    // basis_transcode.cpp: `srgb = KHR_DFDVAL(BDB, TRANSFER) ==
    // KHR_DF_TRANSFER_SRGB`) -- exactly the double-sRGB-decode bug class
    // this project's own D10 table exists to prevent (Godot #99589). The
    // resulting BLOCK BYTES are identical regardless of which VkFormat
    // variant gets chosen (same block-compression target either way,
    // confirmed directly against the transcoder's own switch -- the DFD
    // transfer bit only steers which enum LABEL comes out, not what gets
    // encoded), so overriding `plan.vkFormat` (the ROLE's own choice) here
    // is free -- `texture->vkFormat`/its (POST-transcode) DFD are never
    // read again past this point. currentFormat_ is therefore
    // plan.vkFormat, NOT texture->vkFormat, on this branch.
    return DecodedKtx2Texture(texture, /*wasBasisEncoded=*/true, /*unsupportedLayout=*/false, plan.vkFormat,
                               originalTransfer);
}

uint32_t DecodedKtx2Texture::width() const { return texture_ != nullptr ? texture_->baseWidth : 0; }
uint32_t DecodedKtx2Texture::height() const { return texture_ != nullptr ? texture_->baseHeight : 0; }
uint32_t DecodedKtx2Texture::numLevels() const { return texture_ != nullptr ? texture_->numLevels : 0; }
VkFormat DecodedKtx2Texture::currentVkFormat() const { return currentFormat_; }
bool DecodedKtx2Texture::isUnsupportedLayout() const { return unsupportedLayout_; }

std::vector<MipLevelData> DecodedKtx2Texture::levels() const {
    std::vector<MipLevelData> result;
    if (texture_ == nullptr || unsupportedLayout_) {
        return result;
    }
    result.reserve(texture_->numLevels);
    const ktx_uint8_t* base = ktxTexture_GetData(ktxTexture(texture_));
    for (uint32_t level = 0; level < texture_->numLevels; ++level) {
        ktx_size_t offset = 0;
        if (ktxTexture_GetImageOffset(ktxTexture(texture_), level, 0, 0, &offset) != KTX_SUCCESS) {
            continue;
        }
        ktx_size_t size = ktxTexture_GetImageSize(ktxTexture(texture_), level);
        // True (sub-block-tolerant) mip-level extent -- NEVER rounded up
        // to block granularity. See this struct's own header comment for
        // why `bytes.size()` can legitimately be one whole compressed
        // block even when width/height report 2 or 1 (the sub-block mip
        // tail, gate matrix-issue03's own "classic off-by-one" row).
        uint32_t levelWidth = std::max<uint32_t>(1U, texture_->baseWidth >> level);
        uint32_t levelHeight = std::max<uint32_t>(1U, texture_->baseHeight >> level);
        MipLevelData data;
        data.level = level;
        data.width = levelWidth;
        data.height = levelHeight;
        data.bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(base + offset), size);
        result.push_back(data);
    }
    return result;
}

// ---------------------------------------------------------------------
// stb PNG/JPG fallback path
// ---------------------------------------------------------------------

std::optional<DecodedStbImage> decodeStbImage(std::span<const std::byte> bytes, std::string* outFailureReason) {
    if (bytes.empty()) {
        if (outFailureReason != nullptr) {
            *outFailureReason = "empty byte span";
        }
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    int channelsInFile = 0;
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int len = static_cast<int>(bytes.size());

    // stb transparently downconverts a 16-bit-per-channel PNG to 8-bit
    // when read through stbi_load_from_memory (never stbi_load_16_*) --
    // D10's own "16-bit PNG downconvert documented" acceptance line.
    // Surfaced to the caller purely for its own log line; the decode
    // itself needs no different code path either way.
    bool was16Bit = stbi_is_16_bit_from_memory(data, len) != 0;

    stbi_uc* pixels = stbi_load_from_memory(data, len, &width, &height, &channelsInFile, /*desired_channels=*/4);
    if (pixels == nullptr) {
        if (outFailureReason != nullptr) {
            const char* reason = stbi_failure_reason();
            *outFailureReason = reason != nullptr ? reason : "unknown stb_image failure";
        }
        return std::nullopt;
    }

    DecodedStbImage result;
    result.width = static_cast<uint32_t>(width);
    result.height = static_cast<uint32_t>(height);
    result.was16Bit = was16Bit;
    result.rgba8.assign(pixels, pixels + (static_cast<size_t>(width) * height * 4));
    stbi_image_free(pixels);
    return result;
}

VkFormat stbRgba8Format(TextureRole role) {
    return roleExpectsSrgb(role) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

// ---------------------------------------------------------------------
// [texture-path round, D10 Option A] Runtime mip-chain generation for the
// stb decode path -- see this function's own declaration (texture_decode.h)
// for the full per-role correctness contract this implements.
// ---------------------------------------------------------------------

namespace {

// Standard floor-halving step: max(1, extent >> 1) -- the same formula
// Vulkan's own mip-dimension rule uses, and DecodedKtx2Texture::levels()'s
// own `levelWidth`/`levelHeight` computation (texture_decode.cpp, above)
// already relies on for the KTX2 side of this codebase.
uint32_t nextMipExtent(uint32_t extent) { return std::max<uint32_t>(1U, extent >> 1); }

// Decodes one UNORM byte channel to a signed unit-range float
// (tangent-space normal-map convention: byte 0 -> -1.0, byte 255 -> +1.0).
float decodeSignedUnorm(uint8_t byteValue) { return (static_cast<float>(byteValue) / 255.0F) * 2.0F - 1.0F; }

// Inverse of decodeSignedUnorm(), rounding to nearest and clamping (a
// renormalized vector's components can transiently exceed [-1,1] by a
// hair from float rounding).
uint8_t encodeSignedUnorm(float value) {
    float scaled = (value * 0.5F + 0.5F) * 255.0F;
    scaled = std::clamp(scaled, 0.0F, 255.0F);
    return static_cast<uint8_t>(std::lround(scaled));
}

// [Normal-map mip correctness, texture-path round item 2] Renormalizes
// every RGB-encoded tangent-space texel in `rgba8` (tightly packed, 4
// bytes/texel) to unit length in place -- undoes the "flattening" a plain
// per-channel box average produces (see generateStbMipChain()'s own
// header comment for why this is needed at all). Alpha (byte index 3 of
// every texel) is left exactly as the box-average resize already wrote it
// -- normal maps carry no meaningful alpha channel for this path to
// reinterpret.
void renormalizeNormalTexelsInPlace(std::vector<uint8_t>& rgba8) {
    for (size_t i = 0; i + 3 < rgba8.size(); i += 4) {
        float x = decodeSignedUnorm(rgba8[i + 0]);
        float y = decodeSignedUnorm(rgba8[i + 1]);
        float z = decodeSignedUnorm(rgba8[i + 2]);
        float lengthSquared = x * x + y * y + z * z;
        if (lengthSquared > 1e-12F) {
            float invLength = 1.0F / std::sqrt(lengthSquared);
            x *= invLength;
            y *= invLength;
            z *= invLength;
        } else {
            // Degenerate: the averaged vector canceled to (near-)zero
            // length (e.g. two exactly opposite input normals) -- fall
            // back to tangent-space "straight up" rather than propagate a
            // zero/NaN-length normal downstream; the same (0,0,1)
            // convention D11's own flat-normal fallback texture already
            // uses (texture_cache.cpp's buildFallbackTextures()).
            x = 0.0F;
            y = 0.0F;
            z = 1.0F;
        }
        rgba8[i + 0] = encodeSignedUnorm(x);
        rgba8[i + 1] = encodeSignedUnorm(y);
        rgba8[i + 2] = encodeSignedUnorm(z);
    }
}

}  // namespace

std::vector<DecodedTextureLevel> generateStbMipChain(std::span<const uint8_t> mip0Rgba8, uint32_t width,
                                                       uint32_t height, TextureRole role) {
    std::vector<DecodedTextureLevel> result;
    if (width == 0 || height == 0 ||
        mip0Rgba8.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4U) {
        return result;  // malformed/degenerate input -- never fabricate levels from too little data
    }

    const bool isSrgb = roleExpectsSrgb(role);
    const bool isNormal = role == TextureRole::Normal;

    std::vector<uint8_t> prevLevel(mip0Rgba8.begin(), mip0Rgba8.end());
    uint32_t prevWidth = width;
    uint32_t prevHeight = height;

    while (prevWidth > 1 || prevHeight > 1) {
        uint32_t nextWidth = nextMipExtent(prevWidth);
        uint32_t nextHeight = nextMipExtent(prevHeight);
        std::vector<uint8_t> nextLevel(static_cast<size_t>(nextWidth) * static_cast<size_t>(nextHeight) * 4U);

        // Library-first [this task's own "no reinvented wheels" rule]:
        // stb_image_resize2 (already vendored -- same `stb` repo FetchContent
        // as stb_image.h itself, see stb_image_resize_impl.cpp's own
        // comment) expresses exactly the two box-average kernels this
        // needs directly:
        //   - STBIR_TYPE_UINT8_SRGB: decode sRGB->linear, filter, re-encode
        //     sRGB -- the library's OWN documented sRGB-aware resize mode,
        //     never a hand-rolled gamma-curve loop. STBIR_RGBA (not the
        //     "_PM"/"NO_AW" layout) applies the library's alpha-weighted
        //     RGB blend, matching glTF's own baseColor-alpha-is-linear-
        //     coverage convention (this function's own header comment).
        //   - STBIR_TYPE_UINT8 + STBIR_4CHANNEL: plain per-channel linear
        //     box average, deliberately with NO alpha weighting at all
        //     (STBIR_4CHANNEL, not STBIR_RGBA) -- correct for both the
        //     Normal role (channel 3 is not real alpha) and the plain
        //     linear-data roles (MetallicRoughness/Occlusion/GenericData).
        // STBIR_FILTER_BOX explicitly (not STBIR_FILTER_DEFAULT): "same
        // result as box for integer scale ratios" per the library's own
        // enum comment -- the exact, unambiguous arithmetic-mean semantics
        // this task's own correctness tests assert against for every
        // power-of-two halving step.
        void* resizeResult =
            isSrgb ? stbir_resize(prevLevel.data(), static_cast<int>(prevWidth), static_cast<int>(prevHeight), 0,
                                   nextLevel.data(), static_cast<int>(nextWidth), static_cast<int>(nextHeight), 0,
                                   STBIR_RGBA, STBIR_TYPE_UINT8_SRGB, STBIR_EDGE_CLAMP, STBIR_FILTER_BOX)
                    : stbir_resize(prevLevel.data(), static_cast<int>(prevWidth), static_cast<int>(prevHeight), 0,
                                   nextLevel.data(), static_cast<int>(nextWidth), static_cast<int>(nextHeight), 0,
                                   STBIR_4CHANNEL, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_BOX);
        if (resizeResult == nullptr) {
            RX_LOG_ERROR(
                "rx_asset: generateStbMipChain: stbir_resize failed building level {} ({}x{} -> {}x{}, role={}) -- "
                "chain truncated",
                result.size() + 1, prevWidth, prevHeight, nextWidth, nextHeight, textureRoleName(role));
            break;
        }

        if (isNormal) {
            renormalizeNormalTexelsInPlace(nextLevel);
        }

        DecodedTextureLevel level;
        level.level = static_cast<uint32_t>(result.size() + 1);
        level.width = nextWidth;
        level.height = nextHeight;
        level.bytes.resize(nextLevel.size());
        std::memcpy(level.bytes.data(), nextLevel.data(), nextLevel.size());
        result.push_back(std::move(level));

        prevLevel = std::move(nextLevel);
        prevWidth = nextWidth;
        prevHeight = nextHeight;
    }

    return result;
}

// ---------------------------------------------------------------------
// [Phase 4 Stage 1 Task 15] decodeTextureForUpload()
// ---------------------------------------------------------------------

namespace {

TextureDecodeResult decodeKtx2ForUpload(std::span<const std::byte> bytes, TextureRole role,
                                          uint32_t maxImageDimension2D, const FormatSupportQuery& isFormatSupported) {
    TextureDecodeResult result;
    result.role = role;

    TranscodePlan plan = planTranscodeFormat(role, isFormatSupported);

    Ktx2ParseError parseError = Ktx2ParseError::None;
    auto decoded = DecodedKtx2Texture::parseAndTranscode(bytes, plan, parseError);
    if (!decoded.has_value()) {
        result.outcome = TextureDecodeResult::Outcome::Failed;
        result.failureReason = std::string("KTX2 parse/transcode failed: ") + ktx2ParseErrorName(parseError);
        return result;
    }

    if (decoded->isUnsupportedLayout()) {
        result.outcome = TextureDecodeResult::Outcome::Checkerboard;
        result.failureReason = "cubemap/array/non-2D KTX2 container";
        result.warnings.push_back(
            {"unsupported-layout",
             "is a cubemap/array/non-2D KTX2 container -- unsupported in Phase 4 (FG1, the registered "
             "techniques-phase skybox/IBL consumer, is the scheduled loader for this layout); falling back to "
             "checkerboard"});
        return result;
    }

    if (exceedsDimensionLimit(decoded->width(), decoded->height(), maxImageDimension2D)) {
        result.outcome = TextureDecodeResult::Outcome::Checkerboard;
        result.failureReason = "exceeds maxImageDimension2D";
        result.warnings.push_back(
            {"oversized", "is " + std::to_string(decoded->width()) + "x" + std::to_string(decoded->height()) +
                              ", exceeding this device's maxImageDimension2D=" + std::to_string(maxImageDimension2D) +
                              " -- falling back to checkerboard"});
        return result;
    }

    ColorspaceCheck colorCheck = checkColorspaceAgreement(role, decoded->containerTransferFunction());
    if (colorCheck.disagrees) {
        result.warnings.push_back(
            {"colorspace",
             std::string("role=") + textureRoleName(role) +
                 " container DFD transfer function=" + std::to_string(static_cast<int>(colorCheck.containerTransfer)) +
                 " disagrees with the role's own expectation=" +
                 std::to_string(static_cast<int>(colorCheck.expectedTransfer)) +
                 " -- glTF role is authoritative for the created format (D10); the container's own self-reported "
                 "transfer function is ignored (Godot #99589 double-sRGB-decode class made loud, not silently "
                 "wrong)"});
    }

    VkFormat finalFormat = decoded->currentVkFormat();
    if (!decoded->wasBasisEncoded()) {
        if (!isFormatSupported || !isFormatSupported(finalFormat)) {
            result.outcome = TextureDecodeResult::Outcome::Checkerboard;
            result.failureReason = "non-Basis KTX2 stored format unsupported for sampling";
            result.warnings.push_back(
                {"unsupported-stored-format",
                 "is a non-Basis KTX2 stored as format " + std::to_string(static_cast<int>(finalFormat)) +
                     ", unsupported for sampling on this device -- falling back to checkerboard"});
            return result;
        }
    }

    std::vector<MipLevelData> levels = decoded->levels();
    if (levels.empty()) {
        result.outcome = TextureDecodeResult::Outcome::Failed;
        result.failureReason = "KTX2 container reported zero usable mip levels";
        return result;
    }
    if (levels.size() == 1) {
        result.warnings.push_back(
            {"no-mips", "has no mip chain (1 level) -- recommend regenerating with `toktx --genmipmap` (D10; no "
                        "runtime mip generation for block-compressed formats)"});
    }

    result.outcome = TextureDecodeResult::Outcome::Ready;
    result.format = finalFormat;
    result.width = decoded->width();
    result.height = decoded->height();
    result.levels.reserve(levels.size());
    for (const MipLevelData& lvl : levels) {
        DecodedTextureLevel out;
        out.level = lvl.level;
        out.width = lvl.width;
        out.height = lvl.height;
        out.bytes.assign(lvl.bytes.begin(), lvl.bytes.end());
        result.levels.push_back(std::move(out));
    }
    return result;
}

TextureDecodeResult decodeStbForUpload(std::span<const std::byte> bytes, TextureRole role,
                                         uint32_t maxImageDimension2D) {
    TextureDecodeResult result;
    result.role = role;

    std::string failureReason;
    auto decoded = decodeStbImage(bytes, &failureReason);
    if (!decoded.has_value()) {
        result.outcome = TextureDecodeResult::Outcome::Failed;
        result.failureReason = std::string("stb decode failed: ") + failureReason;
        return result;
    }

    result.warnings.push_back(
        {"stb-recommend-ktx2",
         "loaded via stb (PNG/JPG) -- recommend converting to KTX2 for GPU block-compressed upload (D10); this "
         "path now generates its own runtime box-filtered mip chain (texture-path round, D10 Option A) but still "
         "uploads uncompressed RGBA8 bytes, unlike a pre-authored KTX2's block-compressed levels"});
    if (decoded->was16Bit) {
        result.warnings.push_back(
            {"stb-16bit", "is a 16-bit-per-channel source image -- stb_image downconverts to 8-bit on decode"});
    }

    if (exceedsDimensionLimit(decoded->width, decoded->height, maxImageDimension2D)) {
        result.outcome = TextureDecodeResult::Outcome::Checkerboard;
        result.failureReason = "exceeds maxImageDimension2D";
        result.warnings.push_back(
            {"oversized", "is " + std::to_string(decoded->width) + "x" + std::to_string(decoded->height) +
                              ", exceeding this device's maxImageDimension2D=" + std::to_string(maxImageDimension2D) +
                              " -- falling back to checkerboard"});
        return result;
    }

    result.outcome = TextureDecodeResult::Outcome::Ready;
    result.format = stbRgba8Format(role);
    result.width = decoded->width;
    result.height = decoded->height;
    DecodedTextureLevel level0;
    level0.level = 0;
    level0.width = decoded->width;
    level0.height = decoded->height;
    level0.bytes.resize(decoded->rgba8.size());
    std::memcpy(level0.bytes.data(), decoded->rgba8.data(), decoded->rgba8.size());
    result.levels.push_back(std::move(level0));

    // [texture-path round, D10 Option A] The rest of the chain (levels
    // 1..N) -- see generateStbMipChain()'s own header comment for the
    // per-role sRGB/renormalization correctness this applies. Appended
    // onto the SAME result.levels applyDecodeResult() (texture_cache.cpp)
    // already uploads generically for the KTX2 path -- no new upload/
    // registration code needed for this to reach the GPU (sponza-visual-
    // investigation.md §2.7's own finding).
    std::vector<DecodedTextureLevel> mips =
        generateStbMipChain(std::span<const uint8_t>(decoded->rgba8), decoded->width, decoded->height, role);
    for (DecodedTextureLevel& mip : mips) {
        result.levels.push_back(std::move(mip));
    }
    return result;
}

}  // namespace

TextureDecodeResult decodeTextureForUpload(std::span<const std::byte> bytes, TextureRole role,
                                            uint32_t maxImageDimension2D, const FormatSupportQuery& isFormatSupported) {
    if (bytes.empty()) {
        TextureDecodeResult result;
        result.role = role;
        result.outcome = TextureDecodeResult::Outcome::Failed;
        result.failureReason = "empty byte span";
        return result;
    }
    return looksLikeKtx2(bytes) ? decodeKtx2ForUpload(bytes, role, maxImageDimension2D, isFormatSupported)
                                 : decodeStbForUpload(bytes, role, maxImageDimension2D);
}

}  // namespace rx::asset
