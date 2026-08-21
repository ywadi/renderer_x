#pragma once

// rx_asset/texture_decode.h -- Thread-affinity: NONE. Every function/type
// in this header is pure CPU parse/transcode/decode logic (D5: "decode/
// transcode is worker-eligible CPU work" -- this codebase's `docs/
// threading.md` "Worker-allowed" list) -- nothing here touches a
// VkDevice/VkImage/VkSampler or any other GPU-object-mutation choke
// point, so it carries none of the RX_ASSERT_MAIN_THREAD guards
// texture_cache.h's GPU-facing layer does. Safe to call from any thread,
// including a worker inside a future rx::task::Scheduler::parallelFor
// fan-out (Task 15 wires that up; this task may call it synchronously).
//
// [Phase 4 Stage 1 Task 14, spec D10, gate matrix-issue03 as amended by
// gate/rulings-2026-08-18.md #3] KTX2/Basis container parsing + role-
// typed transcode-target selection + the stb PNG/JPG fallback decode
// path. texture_cache.h (the GPU-facing layer) is this header's only
// intended consumer inside this library, but every type/function here is
// independently, device-freely testable -- see
// src/rx_asset/tests/texture_decode_test.cpp -- mirroring this project's
// established gltf_pipeline.h (device-free) / import_gltf.cpp (GPU)
// split (src/rx_asset/tests/CMakeLists.txt's own comment on that split).

#include <ktx.h>
#include <volk.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rx::asset {

// D10 role table's domain -- the material SLOT a texture is bound to
// (baseColor/emissive/normal/metallicRoughness/occlusion), plus a
// catch-all `GenericData` for anything else (e.g. a texture with no
// material-slot context, or a future glTF extension's own data texture)
// so the role->format table stays TOTAL without inventing slot-specific
// entries no consumer needs yet.
// [Phase 5 Task 6, ticket #42, gate matrix-p5t06-ktx2-cubemap-hdr row 8]
// `Environment` is APPENDED at the end, never inserted before an existing
// value -- static_cast<size_t>(role) must stay unchanged for every
// pre-existing enumerator (matrix row 4's own regression bar: every
// byRole[idx]/roleFallback_[idx] index in texture_cache.h would otherwise
// silently shift). Scene-level (skybox/IBL equirect HDR or cubemap), not
// a glTF material-slot concept -- glTF has no material slot named
// "environment", so import_gltf.cpp's 5 material-slot call sites need no
// change for this role to exist.
enum class TextureRole : uint8_t {
    BaseColor,
    Emissive,
    Normal,
    MetallicRoughness,
    Occlusion,
    GenericData,
    Environment,
};

const char* textureRoleName(TextureRole role);

// One row of the D10 role->format table: `transcodeTarget` is the
// ktx_transcode_fmt_e passed to ktxTexture2_TranscodeBasis() when the
// container IS Basis-encoded; `exactFormat` is the VkFormat the resulting
// bytes are uploaded as when the device supports sampling it;
// `rgba32Fallback` is the role-correct (SRGB vs UNORM) VkFormat used
// instead when it does not (KTX_TTF_RGBA32 always decodes to plain
// R8G8B8A8 bytes regardless of role -- only the FORMAT LABEL those bytes
// get uploaded under is role-dependent, exactly like the exact-format
// case). `isSrgb` mirrors D10/gate ruling #3's "glTF role is
// authoritative for the created format" -- it is NEVER read off the
// container's own DFD transfer field; see checkColorspaceAgreement()
// below for the one place the container's own field is consulted at all
// (purely to decide whether to WARN, never to pick a format).
struct RoleFormatEntry {
    ktx_transcode_fmt_e transcodeTarget;
    VkFormat exactFormat;
    VkFormat rgba32Fallback;
    bool isSrgb;
};

// TOTAL over TextureRole [D10 table; matrix-issue03 "Role->format matrix
// completeness" row: "adding a role without a matrix entry fails a
// static_assert/exhaustive-switch (compile-time completeness)"] --
// texture_decode.cpp's own implementation is a `switch` with NO default
// case, so an added enumerator that is not also given a case here
// produces a compiler WARNING (-Wswitch; verified directly, this project
// has no -Werror anywhere in its build configuration -- a genuine
// "compile ERROR" claim here would be false and was corrected during
// this task's review), not a silently-wrong runtime fallback. The
// warning is still real, visible build-log signal, deliberately kept
// over adding a `default:` case that would silence it.
const RoleFormatEntry& roleFormatTable(TextureRole role);

// Caller-supplied "does this device/driver support SAMPLED_IMAGE +
// TRANSFER_DST under VK_IMAGE_TILING_OPTIMAL for this VkFormat" query.
// TextureCache's real construction path supplies one backed by a real
// vkGetPhysicalDeviceFormatProperties2 call; tests supply a scripted stub
// -- the "format-support-forced-off" seam matrix-issue03's lavapipe row
// calls for, letting the RGBA32-fallback branch be exercised
// deterministically regardless of what the CI driver actually supports.
using FormatSupportQuery = std::function<bool(VkFormat)>;

struct TranscodePlan {
    ktx_transcode_fmt_e transcodeTarget = KTX_TTF_RGBA32;
    VkFormat vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
    bool usedRgba32Fallback = false;
};

// Decides the transcode target + final VkFormat for `role`, consulting
// `isFormatSupported` exactly once (on `roleFormatTable(role).exactFormat`)
// to choose between the exact block-compressed path and the RGBA32
// fallback. Pure function of its two inputs -- never touches a real
// device itself.
TranscodePlan planTranscodeFormat(TextureRole role, const FormatSupportQuery& isFormatSupported);

// [D10/gate ruling #3, Godot #99589 class] Compares the CONTAINER's own
// self-reported DFD transfer function against what `role` expects
// (sRGB for BaseColor/Emissive, linear for everything else -- gate
// matrix Conflict C3: cited precisely, MUST-linear for
// metallicRoughness/normal per the glTF spec text, convention-linear for
// occlusion, which the spec itself never names a transfer function for
// at all). Pure comparison -- the caller (texture_cache.cpp) is
// responsible for emitting the actual WARN (it has the file/debug name
// this function does not) and for the fact the ROLE, not this check,
// picks the created format either way (see RoleFormatEntry's own
// comment) -- this function exists purely to make the disagreement
// LOUD, per D10/#3's "container-DFD disagreement -> ONE WARN" mandate.
struct ColorspaceCheck {
    bool disagrees = false;
    khr_df_transfer_e containerTransfer = KHR_DF_TRANSFER_UNSPECIFIED;
    khr_df_transfer_e expectedTransfer = KHR_DF_TRANSFER_UNSPECIFIED;
};
ColorspaceCheck checkColorspaceAgreement(TextureRole role, khr_df_transfer_e containerTransfer);

// True iff `role` expects sRGB-encoded texel data (BaseColor/Emissive
// per the glTF spec's own MUST-sRGB wording) -- the same predicate
// checkColorspaceAgreement()/roleFormatTable() both key off internally,
// exposed for callers (e.g. the stb fallback path, which has no
// container DFD to compare against at all) that need the role's own
// colorspace expectation directly.
bool roleExpectsSrgb(TextureRole role);

// One already-decoded mip level's upload geometry: `width`/`height` are
// the level's TRUE texel extent (NEVER rounded up to block granularity --
// a 4x4-block format's 2x2 or 1x1 tail level reports its real 2 or 1
// here), while `bytes` is exactly the container's own per-level byte
// range for that level -- one whole compressed block's worth of data for
// a sub-block tail level, per the KTX2/Vulkan block-copy convention
// texture_cache.cpp's upload call relies on (Vulkan's own
// bufferRowLength=0 "tightly packed, rounded up to block granularity"
// copy rule already reconciles the two -- see that call site's own
// comment; this is the classic off-by-one gate matrix-issue03's mip-tail
// row exists to pin down). `bytes` aliases DecodedKtx2Texture's own
// internal buffer -- valid only as long as that object is alive.
struct MipLevelData {
    uint32_t level = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::span<const std::byte> bytes;
};

enum class Ktx2ParseError : uint8_t {
    None,
    NotKtx2,            // CreateFromMemory rejected the container outright, or the Basis transcode call itself failed
    ZeroDimension,       // parsed successfully but baseWidth/baseHeight is 0 -- a corrupt-but-header-valid container
    UnsupportedLayout,   // cubemap / array / non-2D (matrix "Cubemap/array/3D" row) -- caller falls back, never a wrong 2D slice
};

const char* ktx2ParseErrorName(Ktx2ParseError error);

// RAII wrapper around one parsed (and, if Basis-encoded, transcoded)
// ktxTexture2 -- owns the ktxTexture2* for its whole lifetime
// (ktxTexture2_Destroy on destruction); move-only, matching every other
// RAII type in this codebase (rx::rhi::Buffer/Texture2D et al.).
class DecodedKtx2Texture {
public:
    DecodedKtx2Texture(DecodedKtx2Texture&&) noexcept;
    DecodedKtx2Texture& operator=(DecodedKtx2Texture&&) noexcept;
    DecodedKtx2Texture(const DecodedKtx2Texture&) = delete;
    DecodedKtx2Texture& operator=(const DecodedKtx2Texture&) = delete;
    ~DecodedKtx2Texture();

    // Parses `bytes` -- ktxTexture2_CreateFromMemory ONLY, never a
    // filesystem/stream path (the IO-source abstraction invariant;
    // texture_cache.cpp's own grep-enforced "zero std::filesystem" rule
    // starts here) -- and, when `ktxTexture2_NeedsTranscoding()` reports
    // true (the DFD-color-model-driven query gate matrix-issue03 flagged
    // as "to verify at vendoring": CONFIRMED present and public at
    // v4.4.2, see task-14-report.md), transcodes immediately to
    // `plan.transcodeTarget`. A container that does NOT need transcoding
    // (a plain, already-block-compressed-or-uncompressed KTX2) is
    // returned as-is, unmodified -- texture_cache.cpp decides separately
    // whether ITS stored vkFormat is uploadable on the current device.
    // NeedsTranscoding() is the ONLY thing consulted to decide whether to
    // call TranscodeBasis() at all -- a blind transcode-then-check-
    // KTX_INVALID_OPERATION pattern (matrix's explicitly forbidden
    // discovery mechanism) never happens on any path through this
    // function.
    static std::optional<DecodedKtx2Texture> parseAndTranscode(std::span<const std::byte> bytes,
                                                                 const TranscodePlan& plan, Ktx2ParseError& outError);

    uint32_t width() const;
    uint32_t height() const;
    uint32_t numLevels() const;
    // The VkFormat these bytes are ACTUALLY encoded in right now: the
    // container's own stored vkFormat when parseAndTranscode() took the
    // no-transcode-needed branch, or `plan.transcodeTarget`'s
    // corresponding VkFormat after a successful transcode.
    VkFormat currentVkFormat() const;
    bool wasBasisEncoded() const { return wasBasisEncoded_; }
    // The CONTAINER's OWN self-reported transfer function, captured
    // immediately after parse, BEFORE any transcode call -- deliberately
    // NOT a live re-query of the (possibly already-transcoded)
    // ktxTexture2*: `ktxTexture2_TranscodeBasis()` REWRITES the object's
    // own DFD to describe the OUTPUT format (verified directly, this
    // task's own testing -- a BC5 transcode target has no sRGB variant at
    // all, so the post-transcode DFD reports LINEAR regardless of what
    // the container originally claimed), which would silently erase
    // exactly the disagreement this accessor exists to surface. Capturing
    // once, at parse time, is what makes the sRGB-mislabeled-normal
    // acceptance test (and D10/gate ruling #3's WARN) correct at all.
    khr_df_transfer_e containerTransferFunction() const { return originalTransfer_; }

    // [Phase 5 Task 6, gate matrix row 3/Open Question 1] True iff the
    // container declares a layout this TextureCache does not consume --
    // narrowed from Phase 4's blanket "isArray || isCubemap || numFaces>1
    // || numLayers>1 || numDimensions!=2" to `numDimensions != 2 ||
    // (isArray && !isCubemap) || (isCubemap && numLayers > 1)`: a plain,
    // single-layer CUBEMAP (isCubemap && numLayers<=1, which by the KTX2
    // spec always means numFaces==6) is now SUPPORTED -- flat 2D arrays
    // and cube-arrays stay explicitly rejected (zero charter consumer,
    // see the matrix's own Open Question 1), 1D/3D stays rejected
    // unchanged. Callers MUST check this before trusting width()/
    // height()/levels() as "the whole texture" (matrix's "Cubemap/array/
    // 3D KTX2" row: WARN naming the layout + fallback, never a silently-
    // wrong 2D slice). Also true whenever parseAndTranscode() itself
    // already classified the container this way (outError ==
    // UnsupportedLayout) -- exposed as a method too so a caller holding a
    // successfully-constructed instance never needs to separately
    // remember the out-parameter from construction.
    bool isUnsupportedLayout() const;

    // [Phase 5 Task 6] True iff this is a SUPPORTED single-layer cubemap
    // (isUnsupportedLayout() == false and the container's own isCubemap
    // flag is set) -- always false for a rejected/unsupported container
    // or a plain 2D texture. numFaces() is always 6 whenever this is
    // true (the KTX2 spec's own cubemap invariant) and always 1
    // otherwise -- exposed for callers that want the raw count without
    // re-deriving it from isCube().
    bool isCube() const;
    uint32_t numFaces() const;

    // Per-level extent+bytes for face `face` (0 for a plain 2D texture --
    // the only valid value there; 0..numFaces()-1, i.e. 0..5, for a
    // supported cubemap, in the standard +X,-X,+Y,-Y,+Z,-Z KTX2 face
    // order), in ascending level order, reflecting whatever
    // currentVkFormat() the data is actually in right now (see that
    // accessor's own comment). Empty if isUnsupportedLayout(). Defaulted
    // to face 0 so every pre-existing call site (`decoded->levels()`,
    // exclusively 2D textures before this task) keeps compiling and
    // behaving byte-identically -- see matrix row 7's own "DecodedKtx2Texture
    // ::levels()'s face-blindness" gap this parameter closes.
    std::vector<MipLevelData> levels(uint32_t face = 0) const;

private:
    DecodedKtx2Texture(ktxTexture2* texture, bool wasBasisEncoded, bool unsupportedLayout, VkFormat currentFormat,
                        khr_df_transfer_e originalTransfer);

    ktxTexture2* texture_ = nullptr;
    bool wasBasisEncoded_ = false;
    bool unsupportedLayout_ = false;
    VkFormat currentFormat_ = VK_FORMAT_UNDEFINED;
    khr_df_transfer_e originalTransfer_ = KHR_DF_TRANSFER_UNSPECIFIED;
};

// Quick, cheap 12-byte magic-identifier sniff (the KTX2 spec's own fixed
// container signature) -- lets loadFromBytes() (texture_cache.cpp) route
// to libktx vs. stb without paying a full parse-attempt-then-fail
// round-trip on the overwhelmingly common PNG/JPG case. NOT the
// authoritative container-format decision either way:
// ktxTexture2_CreateFromMemory (inside parseAndTranscode() above) is
// always the real, final arbiter for anything this returns true for.
bool looksLikeKtx2(std::span<const std::byte> bytes);

// Oversized-texture guard [matrix "Dimension/format limits" row]: pure
// comparison against a caller-supplied device limit (real callers pass
// VkPhysicalDeviceLimits::maxImageDimension2D; tests pass a synthetic
// small one) so this is deterministically testable device-free without
// assuming what any particular driver/device actually reports.
bool exceedsDimensionLimit(uint32_t width, uint32_t height, uint32_t maxDimension2D);

// ---------------------------------------------------------------------
// stb PNG/JPG fallback path [D10: "no runtime block compression in Phase
// 4"; gate ruling #3 N2 originally recorded "stb path uploads mip 0 only"
// as a limitation -- CLOSED by the texture-path round's D10 Option A
// (generateStbMipChain(), below): decodeStbForUpload() (texture_decode.cpp)
// now appends a full runtime-generated box-filtered mip chain after level
// 0. Still no runtime BLOCK COMPRESSION for this path (that half of N2
// stands) -- mip generation and block compression are independent axes.]
// ---------------------------------------------------------------------

struct DecodedStbImage {
    uint32_t width = 0;
    uint32_t height = 0;
    bool was16Bit = false;              // stb transparently downconverts -- surfaced for the caller's own log line
    std::vector<uint8_t> rgba8;          // tightly packed, 4 bytes/texel, mip 0 ONLY -- see generateStbMipChain()
                                          // below for how the REST of the chain gets built.
};

// Decodes `bytes` via stb_image (already vendored, rx_rhi_vk/src/
// stb_impl.cpp's single STB_IMAGE_IMPLEMENTATION translation unit --
// this function only ever calls the DECLARATION-side API, never redefines
// the implementation macro). Returns std::nullopt (and, if
// `outFailureReason` is non-null, stb's own `stbi_failure_reason()`
// string) on any decode failure -- the caller maps that to the D11
// checkerboard fallback, never a crash.
std::optional<DecodedStbImage> decodeStbImage(std::span<const std::byte> bytes, std::string* outFailureReason);

// Role-correct RGBA8 format for a decodeStbImage() result -- SRGB for
// BaseColor/Emissive, UNORM otherwise, exactly mirroring
// roleFormatTable()'s own colorspace column (roleExpectsSrgb()) but
// returning the UNCOMPRESSED equivalent this path actually uploads.
VkFormat stbRgba8Format(TextureRole role);

// ---------------------------------------------------------------------
// [Phase 5 Task 6, ticket #42, gate matrix-p5t06-ktx2-cubemap-hdr row 9]
// Radiance (.hdr) equirect float input -- library-first: stb_image.h
// (already vendored, the SAME translation unit decodeStbImage() above
// calls into) already declares+implements stbi_loadf_from_memory()/
// stbi_is_hdr_from_memory() -- genuinely zero new dependency. This closes
// a real, previously-silent correctness gap: decodeStbImage() above
// (stbi_load_from_memory, the 8-bit entry point) auto-detects a Radiance
// input internally and decodes it through stb's own
// stbi__hdr_to_ldr() tonemap+clamp path with NO warning at all --
// decodeTextureForUpload() below now checks stbi_is_hdr_from_memory()
// BEFORE ever reaching that 8-bit branch, so a .hdr file is routed here
// instead, never silently tonemapped.
// ---------------------------------------------------------------------

struct DecodedStbHdrImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgba32;  // tightly packed, 4 floats/texel (forced via desired_channels=4)
};

// Decodes `bytes` via stbi_loadf_from_memory -- same failure-reporting
// contract as decodeStbImage() (std::nullopt + stbi_failure_reason() on
// any failure, caller maps to the D11 checkerboard). Caller (this header's
// own decodeTextureForUpload()) is responsible for calling
// stbi_is_hdr_from_memory() FIRST to route here at all -- this function
// itself does not re-check (mirrors decodeStbImage()'s own "caller
// already decided this is the right path" contract).
std::optional<DecodedStbHdrImage> decodeStbImageHdr(std::span<const std::byte> bytes, std::string* outFailureReason);

// ---------------------------------------------------------------------
// [Phase 4 Stage 1 Task 15] Combined KTX2/stb decode -- worker-safe
// (thread-affinity: NONE, same as every other function in this header):
// consolidates exactly the branch logic TextureCache::loadKtx2Bytes()/
// loadStbBytes() (texture_cache.cpp) already ran, MINUS the GPU-facing
// tail (Texture2D creation / Uploader submission / BindlessTable
// registration, all main-thread-only per D5) -- this is the "decode" half
// of the sync path's own loadFromBytes(), reused verbatim by BOTH the sync
// TextureCache::loadFromBytes() (decode here, then register on the SAME
// calling thread, back to back) and the async import pipeline (decode on
// a compute worker via Scheduler::parallelFor(), register later on the
// main thread) -- one shared implementation, per D5/Task 15's "no forked
// logic" rule.
// ---------------------------------------------------------------------

// One already-decoded mip level, OWNED (not aliasing anything else) --
// unlike MipLevelData/DecodedKtx2Texture::levels() above, this type must
// survive a hand-off to a different thread (worker -> main), so it copies
// its bytes out rather than returning a span into a container this
// function's own caller may not keep alive.
struct DecodedTextureLevel {
    uint32_t level = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // [Phase 5 Task 6] Cube face index (0..5, standard KTX2 +X,-X,+Y,-Y,
    // +Z,-Z order) this level's bytes belong to -- 0 for every non-cube
    // producer of this type (every EXISTING call site: decodeStbForUpload()'s
    // LDR/HDR paths, decodeKtx2ForUpload()'s plain-2D branch), so
    // TextureCache::applyDecodeResult()'s ImageMipLevel::baseArrayLayer
    // mapping (upload.h) stays 0 -- byte-identical to every pre-Task-6
    // upload -- for anything that isn't a cube.
    uint32_t faceIndex = 0;
    std::vector<std::byte> bytes;
};

// [texture-path round, D10 Option A -- see .superpowers/sdd/2026-08-11-
// phase4-scene-assets/sponza-visual-investigation.md §2.7] Generates the
// REST of a full, floor-halving box-filtered mip chain for an
// already-decoded RGBA8 mip-0 image -- level 0 itself is NOT included in
// the returned vector (the caller already has it verbatim); this
// function's own output is exactly levels 1..N, where N is the SAME
// total level count Vulkan's own image-creation rule computes
// (floor(log2(max(width,height)))+1 levels overall -- see
// Texture2D::createForPresuppliedMips()'s own VkImageCreateInfo::
// mipLevels, which this task's caller (decodeStbForUpload(),
// texture_decode.cpp) feeds `decoded.levels.size()` into directly, no
// separate cap). `width`/`height` MUST be mip0Rgba8's own true extent
// (mip0Rgba8.size() >= width*height*4, tightly packed) -- returns an
// empty vector on a malformed/degenerate input rather than fabricating
// levels from too little data.
//
// PER-ROLE CORRECTNESS (binding, not a shortcut -- see the implementation's
// own comment for the exact stb_image_resize2 calls this hands off to,
// this task's own "prefer a ready-made library" choice over a hand-rolled
// box-filter loop):
//   - roleExpectsSrgb(role) (BaseColor/Emissive): each level's RGB is
//     decoded sRGB->linear, box-averaged in LINEAR space, and re-encoded
//     back to sRGB -- NEVER a naive byte average, which visibly darkens
//     the image at every level (averaging gamma-encoded bytes as if they
//     were linear energy is a real, measurable error, not a cosmetic
//     one). Alpha is treated as already-linear (glTF's own convention:
//     baseColorTexture alpha is coverage/opacity, never color) and is
//     alpha-weighted into the RGB average (avoids the classic
//     transparent-edge "black fringing" artifact a plain unweighted
//     average produces).
//   - role == Normal: plain LINEAR box average (no sRGB transform -- this
//     is tangent-space vector data, never color), then each output
//     texel's decoded (2*byte/255-1) vector is RENORMALIZED to unit
//     length before re-encoding -- undoing the "flattening" a plain
//     average produces (two divergent unit normals average to a SHORTER
//     vector; sampling that unnormalized visibly softens/darkens
//     normal-mapped detail at a distance).
//   - every other role (MetallicRoughness/Occlusion/GenericData): plain
//     linear box average, no colorspace transform, no renormalization --
//     already-linear scalar/data channels, exactly D10's own table.
//
// Non-power-of-two width/height are handled by the standard floor-halving
// chain (each level's extent is max(1, previous >> 1) independently per
// axis) -- stb_image_resize2's own general resize (fractional-ratio box
// filter) computes the correctly-weighted result for the non-exact-2x
// steps this produces on an odd dimension, not just the power-of-two
// case.
std::vector<DecodedTextureLevel> generateStbMipChain(std::span<const uint8_t> mip0Rgba8, uint32_t width,
                                                       uint32_t height, TextureRole role);

// A single log line this function decided was warranted but could not
// safely emit itself (this function is thread-affinity-NONE and must not
// touch TextureCache's own `loggedFailures_` dedup set, which is
// main-thread-only state) -- `category` is the SAME dedup category
// TextureCache::shouldLogOnce() already uses internally (e.g.
// "unsupported-layout", "oversized", "colorspace", "no-mips",
// "stb-recommend-ktx2", "stb-16bit") so the caller can apply the identical
// once-per-(debugName,category) dedup this function's own synchronous
// sibling (loadKtx2Bytes()/loadStbBytes()) already provides.
struct DecodedTextureWarning {
    std::string category;
    std::string message;  // RX_LOG_WARN-ready; %s-free, already formatted
};

struct TextureDecodeResult {
    enum class Outcome : uint8_t {
        Ready,         // levels/format/width/height populated -- safe to register as a real texture
        Checkerboard,  // decode succeeded structurally but this content is not uploadable (unsupported
                       // layout/oversized/unsupported stored format) -- caller falls back, no register() call
        Failed,        // decode itself failed (corrupt container, stb failure, empty bytes)
    };
    Outcome outcome = Outcome::Failed;
    std::string failureReason;  // meaningful for Failed/Checkerboard only

    std::vector<DecodedTextureWarning> warnings;

    TextureRole role = TextureRole::GenericData;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    // [Phase 5 Task 6] True for a supported cubemap KTX2 -- `levels` then
    // holds 6 * perFaceMipLevels entries (every face's DecodedTextureLevel
    // ::faceIndex populated 0..5), never a mix of cube and non-cube
    // entries. Always false for every pre-Task-6 producer of this struct
    // (plain 2D KTX2, stb LDR/HDR) -- `levels.size()` alone still gives
    // the correct per-face-and-per-texture mip count either way (divide
    // by 6 only when this is true).
    bool isCube = false;
    std::vector<DecodedTextureLevel> levels;
};

// `maxImageDimension2D`: the device's VkPhysicalDeviceLimits field (or a
// synthetic test value) -- same role as TextureCache's own stored
// `maxImageDimension2D_`, passed explicitly since this function has no
// TextureCache instance to read it from. `isFormatSupported`: same
// FormatSupportQuery contract as planTranscodeFormat() above.
//
// [Phase 5 Task 6] Dispatch order: looksLikeKtx2(bytes) routes to the
// KTX2 path (now cube-aware, TextureDecodeResult::isCube) FIRST;
// otherwise stbi_is_hdr_from_memory(bytes) routes to the NEW
// decodeStbImageHdr()-backed float path (VK_FORMAT_R16G16B16A16_SFLOAT,
// per the phase's environment-upload-format ruling) BEFORE the plain
// 8-bit stb branch ever gets a chance to silently tonemap it (see
// decodeStbImageHdr()'s own header comment); everything else falls to
// the unchanged 8-bit stb (PNG/JPG) path.
TextureDecodeResult decodeTextureForUpload(std::span<const std::byte> bytes, TextureRole role,
                                            uint32_t maxImageDimension2D, const FormatSupportQuery& isFormatSupported);

}  // namespace rx::asset
