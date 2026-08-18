#include <rx_asset/texture_cache.h>
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>

namespace rx::asset {

namespace {

// glTF sampler enum values [spec section 5.29 "sampler", quoted in gate
// matrix-issue02 §2A] -- mirrors mesh_asset.h's own SamplerDesc default
// values (10497 == REPEAT).
constexpr uint32_t kGltfRepeat = 10497;
constexpr uint32_t kGltfClampToEdge = 33071;
constexpr uint32_t kGltfMirroredRepeat = 33648;

constexpr uint32_t kGltfNearest = 9728;
constexpr uint32_t kGltfLinear = 9729;
constexpr uint32_t kGltfNearestMipmapNearest = 9984;
constexpr uint32_t kGltfLinearMipmapNearest = 9985;
constexpr uint32_t kGltfNearestMipmapLinear = 9986;
constexpr uint32_t kGltfLinearMipmapLinear = 9987;

VkSamplerAddressMode mapWrap(uint32_t wrap) {
    switch (wrap) {
        case kGltfClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case kGltfMirroredRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case kGltfRepeat:
        default:
            // glTF's own documented default (spec: "REPEAT" when
            // unspecified) -- also the correct fallback for any value
            // this project's own fastgltf-constrained parse could never
            // actually produce.
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFilter mapMagFilter(uint32_t magFilter) {
    // 0 == "unspecified" (mesh_asset.h's own SamplerDesc convention) --
    // glTF's spec text leaves magFilter/minFilter "auto" filtering in
    // this case; this engine's own documented default is LINEAR (the
    // higher-quality common choice; NEAREST would be an equally valid
    // reading of "auto" but this project picks one explicitly rather
    // than leaving it ambiguous).
    return magFilter == kGltfNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

// Canonical minFilter -> (VkFilter, VkSamplerMipmapMode, single-level-only)
// table [G6, matrix-issue03's own "documented in the header" requirement
// -- documented here, texture_cache.h's own getOrCreateSampler() comment
// cross-references this exact table]. TOTAL over the 6 real glTF minFilter
// values plus the 0/unspecified case (glTF default: LINEAR + full mip
// range, this engine's own documented choice for "auto").
struct MinFilterMapping {
    VkFilter filter;
    VkSamplerMipmapMode mipmapMode;
    bool singleLevelOnly;  // true for the two non-"MIPMAP"-named values -- see getOrCreateSampler()'s own maxLod handling
};

MinFilterMapping mapMinFilter(uint32_t minFilter) {
    switch (minFilter) {
        case kGltfNearest:
            return {VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, /*singleLevelOnly=*/true};
        case kGltfLinear:
            return {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, /*singleLevelOnly=*/true};
        case kGltfNearestMipmapNearest:
            return {VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, /*singleLevelOnly=*/false};
        case kGltfLinearMipmapNearest:
            return {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, /*singleLevelOnly=*/false};
        case kGltfNearestMipmapLinear:
            return {VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_LINEAR, /*singleLevelOnly=*/false};
        case kGltfLinearMipmapLinear:
            return {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, /*singleLevelOnly=*/false};
        default:
            // 0/unspecified -- glTF default per this engine's documented
            // choice (see this function's own header comment).
            return {VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, /*singleLevelOnly=*/false};
    }
}

}  // namespace

size_t TextureCache::SamplerKeyHash::operator()(const SamplerKey& key) const {
    size_t h = std::hash<int>()(static_cast<int>(key.wrapS));
    h ^= std::hash<int>()(static_cast<int>(key.wrapT)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>()(static_cast<int>(key.magFilter)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>()(static_cast<int>(key.minFilter)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>()(static_cast<int>(key.mipmapMode)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int32_t>()(key.anisotropyTimes100) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

TextureCache::TextureCache(rx::rhi::Allocator& allocator, rx::rhi::Device& device, rx::rhi::Uploader& uploader,
                            rx::rhi::BindlessTable& bindless, rx::rhi::DeletionQueue& deletionQueue)
    : allocator_(allocator), device_(device), uploader_(uploader), bindless_(bindless), deletionQueue_(deletionQueue) {}

TextureCache::~TextureCache() {
    // [G6] Sampler cache cleanup -- VkSampler is a bare handle, not an
    // RAII type, so nothing else in this class destroys these; skipping
    // this loop is a real, empirically-hit VUID-vkDestroyDevice-device-
    // 00378 ("child objects... must have been destroyed prior to
    // destroying device") the first time this task's own test suite tore
    // a TextureCache down with any sampler still cached.
    for (const auto& [key, sampler] : samplerCache_) {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device_.device(), sampler, nullptr);
        }
    }
}

std::unique_ptr<TextureCache> TextureCache::create(rx::rhi::Allocator& allocator, rx::rhi::Device& device,
                                                     rx::rhi::Uploader& uploader, rx::rhi::BindlessTable& bindless,
                                                     rx::rhi::DeletionQueue& deletionQueue) {
    RX_ASSERT_MAIN_THREAD("TextureCache::create");

    // std::unique_ptr<TextureCache>(new TextureCache(...)) rather than
    // std::make_unique: TextureCache's constructor is private (only
    // create() and this function itself are meant to build one),
    // matching GeometryPool::create()'s own established pattern for the
    // identical reason.
    std::unique_ptr<TextureCache> cache(new TextureCache(allocator, device, uploader, bindless, deletionQueue));

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    cache->maxImageDimension2D_ = props.limits.maxImageDimension2D;
    // 8x default per D10/G6 -- clamped to whatever this device actually
    // advertises (verified in-task alongside the BC format dump,
    // task-14-report.md).
    cache->maxAnisotropy_ = std::min(8.0F, props.limits.maxSamplerAnisotropy);

    // device.supportsSamplerAnisotropy() -- NOT a fresh
    // vkGetPhysicalDeviceFeatures() query here: what matters for
    // VkSamplerCreateInfo::anisotropyEnable's own validity
    // (VUID-VkSamplerCreateInfo-anisotropyEnable-01070) is whether the
    // feature was actually ENABLED on THIS Device's VkDevice at creation
    // time, not merely advertised as physically available -- see
    // Device::supportsSamplerAnisotropy()'s own header comment
    // (device.h) for the real validation error a plain feature-query
    // produced here before this fix.
    cache->anisotropySupported_ = device.supportsSamplerAnisotropy();
    if (!cache->anisotropySupported_) {
        RX_LOG_INFO("rx_asset: TextureCache::create: samplerAnisotropy unsupported on this device -- anisotropic "
                    "filtering cleanly disabled (G6)");
    }

    if (!cache->buildFallbackTextures()) {
        RX_LOG_ERROR("rx_asset: TextureCache::create: failed to build the D11 fallback/utility textures");
        return nullptr;
    }
    return cache;
}

bool TextureCache::isFormatSupported(VkFormat format) const {
    VkFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    vkGetPhysicalDeviceFormatProperties2(device_.physicalDevice(), format, &props);
    constexpr VkFormatFeatureFlags kNeeded =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (props.formatProperties.optimalTilingFeatures & kNeeded) == kNeeded;
}

bool TextureCache::shouldLogOnce(std::string_view debugName, std::string_view category) {
    std::string key;
    key.reserve(debugName.size() + category.size() + 1);
    key.append(debugName);
    key.push_back('|');
    key.append(category);
    return loggedFailures_.insert(std::move(key)).second;
}

TextureHandle TextureCache::registerRealTexture(TextureRole role, VkFormat format, uint32_t width, uint32_t height,
                                                  uint32_t mipLevels,
                                                  std::span<const rx::rhi::Uploader::ImageMipLevel> uploadLevels) {
    auto texture = rx::rhi::Texture2D::createForPresuppliedMips(
        device_.physicalDevice(), device_.device(), allocator_, VkExtent2D{width, height}, format,
        VK_IMAGE_USAGE_SAMPLED_BIT, mipLevels, rx::rhi::MemoryCategory::Texture);
    if (!texture.has_value()) {
        RX_LOG_ERROR("rx_asset: TextureCache: Texture2D::createForPresuppliedMips failed ({}x{}, {} levels, format={})",
                     width, height, mipLevels, static_cast<int>(format));
        return TextureHandle{};
    }

    if (!uploader_.uploadImageMips(*texture, uploadLevels)) {
        RX_LOG_ERROR("rx_asset: TextureCache: Uploader::uploadImageMips failed ({}x{}, {} levels)", width, height,
                     uploadLevels.size());
        return TextureHandle{};
    }

    rx::rhi::BindlessHandle bindlessHandle =
        bindless_.registerSampledImage(texture->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!bindlessHandle.isValid()) {
        RX_LOG_ERROR("rx_asset: TextureCache: BindlessTable::registerSampledImage failed (capacity exhausted?)");
        return TextureHandle{};
    }

    // [FG9] Real VMA-reported bytes (Texture2D::allocatedBytes(), NOT the
    // nominal width*height*bpp request -- may exceed it for alignment/
    // block-granularity padding, matching this codebase's existing
    // accounting convention, e.g. GeometryPool's own vertexBufferAllocatedBytes()
    // comment) -- captured BEFORE `texture` is moved into the Entry below.
    uint64_t allocatedBytes = texture->allocatedBytes();

    Entry entry;
    entry.record.isFallback = false;
    entry.record.role = role;
    entry.record.bindlessIndex = bindlessHandle.index();
    entry.record.width = width;
    entry.record.height = height;
    entry.record.mipLevels = mipLevels;
    entry.record.format = format;
    entry.record.resident = true;
    entry.image = std::move(texture);
    entry.bindlessHandle = bindlessHandle;

    TextureHandle handle = pool_.acquire(std::move(entry));
    TextureCacheStats::RoleStats& roleStats = stats_.byRole[static_cast<size_t>(role)];
    roleStats.bytes += allocatedBytes;
    roleStats.count += 1;
    stats_.totalBytes += allocatedBytes;
    stats_.totalCount += 1;
    return handle;
}

TextureHandle TextureCache::failureFallback(std::string_view debugName, std::string_view reason) {
    if (shouldLogOnce(debugName, "failed")) {
        RX_LOG_ERROR("rx_asset: TextureCache: '{}' failed to load ({}) -- falling back to the D11 checkerboard",
                     debugName, reason);
    }
    return checkerboard_;
}

std::optional<TextureHandle> TextureCache::uploadSolidColor(TextureRole role, std::array<uint8_t, 4> rgba,
                                                               VkFormat format, std::string_view debugName) {
    rx::rhi::Uploader::ImageMipLevel level{};
    level.data = rgba.data();
    level.size = rgba.size();
    level.mipLevel = 0;
    level.extent = VkExtent2D{1, 1};

    TextureHandle handle = registerRealTexture(role, format, 1, 1, 1, std::span<const rx::rhi::Uploader::ImageMipLevel>(&level, 1));
    if (!handle.isValid()) {
        RX_LOG_ERROR("rx_asset: TextureCache: failed to build the D11 utility texture '{}'", debugName);
        return std::nullopt;
    }
    if (Entry* entry = pool_.get(handle)) {
        entry->record.isFallback = true;
    }
    return handle;
}

std::optional<TextureHandle> TextureCache::uploadCheckerboard() {
    constexpr uint32_t kSize = 4;
    constexpr std::array<uint8_t, 4> kMagenta{255, 0, 255, 255};
    constexpr std::array<uint8_t, 4> kBlack{0, 0, 0, 255};

    std::array<uint8_t, kSize * kSize * 4> pixels{};
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const std::array<uint8_t, 4>& c = ((x + y) % 2 == 0) ? kMagenta : kBlack;
            size_t o = (static_cast<size_t>(y) * kSize + x) * 4;
            pixels[o + 0] = c[0];
            pixels[o + 1] = c[1];
            pixels[o + 2] = c[2];
            pixels[o + 3] = c[3];
        }
    }

    rx::rhi::Uploader::ImageMipLevel level{};
    level.data = pixels.data();
    level.size = pixels.size();
    level.mipLevel = 0;
    level.extent = VkExtent2D{kSize, kSize};

    TextureHandle handle = registerRealTexture(TextureRole::GenericData, VK_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1,
                                                std::span<const rx::rhi::Uploader::ImageMipLevel>(&level, 1));
    if (!handle.isValid()) {
        RX_LOG_ERROR("rx_asset: TextureCache: failed to build the D11 checkerboard fallback texture");
        return std::nullopt;
    }
    if (Entry* entry = pool_.get(handle)) {
        entry->record.isFallback = true;
    }
    return handle;
}

bool TextureCache::buildFallbackTextures() {
    auto checker = uploadCheckerboard();
    if (!checker.has_value()) {
        return false;
    }
    checkerboard_ = *checker;

    // D11's own enumeration is exactly THREE utility assets ("1x1 white +
    // flat-normal + neutral-MR") shared across the roles that expect an
    // "as if untextured" (texture-times-factor-equals-factor) sampled
    // value -- built once each, then fanned out by role below, rather
    // than 6 separate near-identical GPU allocations.
    auto white =
        uploadSolidColor(TextureRole::GenericData, {255, 255, 255, 255}, VK_FORMAT_R8G8B8A8_UNORM, "fallback:white");
    // Tangent-space "straight up" normal, UNORM-encoded ((0,0,1) ->
    // (128,128,255)) -- D11's own "flat-normal", never the checkerboard,
    // for an UNBOUND normal slot (a magenta normal map would shade
    // garbage).
    auto flatNormal =
        uploadSolidColor(TextureRole::Normal, {128, 128, 255, 255}, VK_FORMAT_R8G8B8A8_UNORM, "fallback:flat-normal");
    auto neutralMr = uploadSolidColor(TextureRole::MetallicRoughness, {255, 255, 255, 255}, VK_FORMAT_R8G8B8A8_UNORM,
                                       "fallback:neutral-MR");
    if (!white.has_value() || !flatNormal.has_value() || !neutralMr.has_value()) {
        return false;
    }

    roleFallback_[static_cast<size_t>(TextureRole::BaseColor)] = *white;
    roleFallback_[static_cast<size_t>(TextureRole::Emissive)] = *white;
    roleFallback_[static_cast<size_t>(TextureRole::GenericData)] = *white;
    roleFallback_[static_cast<size_t>(TextureRole::Normal)] = *flatNormal;
    roleFallback_[static_cast<size_t>(TextureRole::MetallicRoughness)] = *neutralMr;
    roleFallback_[static_cast<size_t>(TextureRole::Occlusion)] = *neutralMr;

    // [D25] ONE flush()+wait() covering all 4 fallback uploads together --
    // load-bearing, not cosmetic: without this, their upload commands stay
    // RECORDED-but-never-submitted on this Uploader's active command
    // buffer (`recording_` stays true) for as long as no LATER load() call
    // happens to trigger a flush of its own. A TextureCache that is
    // constructed and then torn down without ever loading a REAL texture
    // (every load() call resolving to the checkerboard fallback, e.g. this
    // class's own cubemap/corrupt/missing-byte-source failure-path tests)
    // would otherwise reach ~TextureCache() with those 4 images destroyed
    // while Uploader's OWN destructor still auto-flushes (calls
    // vkEndCommandBuffer on) the very command buffer that recorded copies
    // into them -- reproduced directly, empirically, as a real
    // UNASSIGNED-CoreValidation-DrawState-InvalidCommandBuffer-VkImage
    // validation error the first time this task's own test suite ran
    // without this call.
    uploader_.wait(uploader_.flush());
    return true;
}

TextureDecodeResult TextureCache::decodeForUpload(std::span<const std::byte> bytes, TextureRole role) const {
    // Thread-affinity: NONE -- see this method's own header comment.
    // Deliberately does NOT carry RX_ASSERT_MAIN_THREAD: unlike every
    // other method in this class, calling this from a worker thread is
    // the whole point (the async import pipeline's texture-decode stage).
    return decodeTextureForUpload(bytes, role, maxImageDimension2D_, [this](VkFormat f) { return isFormatSupported(f); });
}

TextureHandle TextureCache::applyDecodeResult(const TextureDecodeResult& decoded, std::string_view debugName) {
    for (const DecodedTextureWarning& w : decoded.warnings) {
        if (shouldLogOnce(debugName, w.category)) {
            RX_LOG_WARN("rx_asset: TextureCache: '{}' {}", debugName, w.message);
        }
    }

    if (decoded.outcome == TextureDecodeResult::Outcome::Failed) {
        return failureFallback(debugName, decoded.failureReason);
    }
    if (decoded.outcome == TextureDecodeResult::Outcome::Checkerboard) {
        return checkerboard_;
    }

    std::vector<rx::rhi::Uploader::ImageMipLevel> uploadLevels;
    uploadLevels.reserve(decoded.levels.size());
    for (const DecodedTextureLevel& lvl : decoded.levels) {
        rx::rhi::Uploader::ImageMipLevel u{};
        u.data = lvl.bytes.data();
        u.size = static_cast<VkDeviceSize>(lvl.bytes.size());
        u.mipLevel = lvl.level;
        u.extent = VkExtent2D{lvl.width, lvl.height};
        uploadLevels.push_back(u);
    }

    TextureHandle handle = registerRealTexture(decoded.role, decoded.format, decoded.width, decoded.height,
                                                static_cast<uint32_t>(decoded.levels.size()), uploadLevels);
    if (!handle.isValid()) {
        return failureFallback(debugName, "GPU upload/bindless registration failed");
    }
    return handle;
}

TextureHandle TextureCache::registerDecoded(const TextureDecodeResult& decoded, std::string_view debugName) {
    RX_ASSERT_MAIN_THREAD("TextureCache::registerDecoded");
    RX_ZONE;
    // No flush()/wait() here -- see this method's own header comment
    // (D25's "poll, never a blocking wait" invariant).
    return applyDecodeResult(decoded, debugName);
}

rx::rhi::UploadTicket TextureCache::flushPendingUploads() {
    RX_ASSERT_MAIN_THREAD("TextureCache::flushPendingUploads");
    return uploader_.flush();
}

bool TextureCache::isUploadComplete(rx::rhi::UploadTicket ticket) const {
    RX_ASSERT_MAIN_THREAD("TextureCache::isUploadComplete");
    return uploader_.isComplete(ticket);
}

TextureHandle TextureCache::loadFromBytes(std::span<const std::byte> bytes, TextureRole role,
                                            std::string_view debugName) {
    RX_ASSERT_MAIN_THREAD("TextureCache::loadFromBytes");
    RX_ZONE;

    // [Task 15] Same decode function the async import pipeline calls from
    // a worker thread (decodeForUpload() above) -- called here inline, on
    // whichever thread this call itself runs on (main, by this method's
    // own guard), immediately followed by the identical GPU-registration
    // tail (applyDecodeResult()) registerDecoded() also uses. One shared
    // implementation for both completion styles, no forked logic.
    TextureDecodeResult decoded = decodeForUpload(bytes, role);
    TextureHandle result = applyDecodeResult(decoded, debugName);

    // [D25] One flush()+wait() per load() call, never per-mip -- makes
    // the returned handle immediately safe to bind/draw (the same "sync
    // convenience" contract GeometryPool::upload()/MeshBuffers::create()
    // already establish). Skipped entirely when `result` is an EXISTING
    // fallback handle (nothing new was uploaded this call -- the D11
    // fallback textures were already flushed once, at buildFallbackTextures()
    // time).
    if (result.isValid() && !(result == checkerboard_)) {
        uploader_.wait(uploader_.flush());
    }
    return result;
}

TextureHandle TextureCache::load(ByteSource& source, const std::string& uri, TextureRole role) {
    RX_ASSERT_MAIN_THREAD("TextureCache::load");
    auto bytes = source.read(uri);
    if (!bytes.has_value()) {
        return failureFallback(uri, "byte source could not resolve URI");
    }
    return loadFromBytes(std::span<const std::byte>(*bytes), role, uri);
}

TextureHandle TextureCache::load(const std::filesystem::path& path, TextureRole role) {
    RX_ASSERT_MAIN_THREAD("TextureCache::load");
    // Zero direct filesystem I/O in THIS translation unit -- wraps a
    // FilesystemByteSource (byte_source.cpp, a different translation
    // unit) exactly like Registry::importGltf's own path-taking
    // convenience overload wraps one for external references.
    FilesystemByteSource fsSource(path.has_parent_path() ? path.parent_path() : std::filesystem::path("."));
    return load(fsSource, path.filename().string(), role);
}

const TextureRecord& TextureCache::resolve(TextureHandle handle) const {
    RX_ASSERT_MAIN_THREAD("TextureCache::resolve");
    if (const Entry* entry = pool_.get(handle)) {
        if (entry->record.resident) {
            return entry->record;
        }
    }
    // Dead handle (never valid, or already reclaimed post-eviction), OR
    // live-but-nonresident (evicted, not yet reclaimed) -- both fall to
    // the checkerboard's own record [D24].
    const Entry* fallback = pool_.get(checkerboard_);
    return fallback->record;  // always resident -- built at create(), never itself evicted
}

TextureHandle TextureCache::fallbackHandle(TextureRole role) const {
    RX_ASSERT_MAIN_THREAD("TextureCache::fallbackHandle");
    return roleFallback_[static_cast<size_t>(role)];
}

TextureHandle TextureCache::checkerboardHandle() const {
    RX_ASSERT_MAIN_THREAD("TextureCache::checkerboardHandle");
    return checkerboard_;
}

VkSampler TextureCache::getOrCreateSampler(const SamplerDesc& desc) {
    RX_ASSERT_MAIN_THREAD("TextureCache::getOrCreateSampler");

    MinFilterMapping minMapping = mapMinFilter(desc.minFilter);
    SamplerKey key;
    key.wrapS = mapWrap(desc.wrapS);
    key.wrapT = mapWrap(desc.wrapT);
    key.magFilter = mapMagFilter(desc.magFilter);
    key.minFilter = minMapping.filter;
    key.mipmapMode = minMapping.mipmapMode;
    float requestedAniso = anisotropySupported_ ? maxAnisotropy_ : 1.0F;
    key.anisotropyTimes100 = static_cast<int32_t>(requestedAniso * 100.0F + 0.5F);

    if (auto it = samplerCache_.find(key); it != samplerCache_.end()) {
        return it->second;
    }

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = key.magFilter;
    info.minFilter = key.minFilter;
    info.mipmapMode = key.mipmapMode;
    info.addressModeU = key.wrapS;
    info.addressModeV = key.wrapT;
    info.addressModeW = key.wrapS;  // glTF has no third wrap axis -- reuse wrapS (2D-only textures, W never sampled)
    info.mipLodBias = 0.0F;
    info.anisotropyEnable = anisotropySupported_ ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = requestedAniso;
    info.compareEnable = VK_FALSE;
    info.compareOp = VK_COMPARE_OP_ALWAYS;
    info.minLod = 0.0F;
    // Forcing mip 0 only for a non-"MIPMAP"-named minFilter [standard
    // Vulkan-sampler technique, not a fabricated one -- clamping maxLod
    // below any real mip 1 boundary makes the sampler ignore every level
    // past 0 regardless of mipmapMode, which Vulkan itself has no
    // separate "off" enum value for]; VK_LOD_CLAMP_NONE (the full range)
    // otherwise.
    info.maxLod = minMapping.singleLevelOnly ? 0.25F : VK_LOD_CLAMP_NONE;
    info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device_.device(), &info, nullptr, &sampler) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_asset: TextureCache::getOrCreateSampler: vkCreateSampler failed");
        return VK_NULL_HANDLE;
    }

    samplerCache_.emplace(key, sampler);
    return sampler;
}

TextureCacheStats TextureCache::stats() const {
    RX_ASSERT_MAIN_THREAD("TextureCache::stats");
    return stats_;
}

size_t TextureCache::samplerCountForTesting() const {
    RX_ASSERT_MAIN_THREAD("TextureCache::samplerCountForTesting");
    return samplerCache_.size();
}

size_t TextureCache::liveTextureCountForTesting() const {
    RX_ASSERT_MAIN_THREAD("TextureCache::liveTextureCountForTesting");
    return pool_.liveCount();
}

void TextureCache::evictForTesting(TextureHandle handle, uint64_t frameIndex) {
    RX_ASSERT_MAIN_THREAD("TextureCache::evictForTesting");
    releaseInternal(handle, frameIndex);
}

void TextureCache::releaseUnpublished(TextureHandle handle, uint64_t frameIndex) {
    RX_ASSERT_MAIN_THREAD("TextureCache::releaseUnpublished");
    releaseInternal(handle, frameIndex);
}

void TextureCache::releaseInternal(TextureHandle handle, uint64_t frameIndex) {
    Entry* entry = pool_.get(handle);
    if (entry == nullptr) {
        return;  // dead handle already -- nothing to evict
    }
    entry->record.resident = false;  // [D24 clauses 1/2] observable immediately

    rx::rhi::BindlessHandle bindlessHandle = entry->bindlessHandle;
    // [D24 clause (c)] Retire the REAL reclaim -- Texture2D destruction
    // (releases the VkImage/VkImageView/VMA allocation NOW, at this
    // fence-confirmed-safe point, not at pool_.release() below, which
    // does NOT itself destroy the stored Entry -- see rx_core/handle.h's
    // own HandlePool::release(), which only flips `alive`/pushes the
    // free list) + BindlessTable slot release + the pool slot release
    // (bumps generation on next reacquire) -- into DeletionQueue, tagged
    // `frameIndex`. Mirrors eviction_contract_test.cpp's own synthetic
    // evict()/clause-(c) sequence, over REAL GPU resources instead of a
    // synthetic stand-in.
    deletionQueue_.retire(
        [this, handle, bindlessHandle]() {
            bindless_.release(bindlessHandle);
            if (Entry* live = pool_.get(handle)) {
                if (live->image.has_value()) {
                    // [FG9] Mirror registerRealTexture()'s own +1 exactly,
                    // in reverse, so stats() balances to zero across
                    // load/evict/teardown (Task 10 accounting-test
                    // pattern).
                    uint64_t allocatedBytes = live->image->allocatedBytes();
                    TextureCacheStats::RoleStats& roleStats = stats_.byRole[static_cast<size_t>(live->record.role)];
                    roleStats.bytes -= allocatedBytes;
                    roleStats.count -= 1;
                    stats_.totalBytes -= allocatedBytes;
                    stats_.totalCount -= 1;
                }
                live->image.reset();  // real VkImage/VkImageView/VMA-allocation destruction happens HERE
            }
            pool_.release(handle);
        },
        frameIndex);
}

}  // namespace rx::asset
