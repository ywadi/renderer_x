#include <rx_asset/texture_decode.h>
#include <rx_core/log.h>
#include <stb_image.h>
#include <algorithm>
#include <array>
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
// NO default case -- adding an enumerator without a case here is a
// compile ERROR (-Wswitch), not a silently-wrong runtime default. BC4_R
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

}  // namespace rx::asset
