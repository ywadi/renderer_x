#include <rx_rhi_vk/texture.h>
#include <rx_core/log.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace rx::rhi {

namespace {

// floor(log2(max(w, h))) + 1 -- the number of mip levels needed to reach a
// final 1x1 level from `extent`. `extent` of {0, *}/{*, 0} (degenerate,
// should never happen for a real texture) is defensively clamped to 1
// level rather than calling std::log2(0).
uint32_t computeMaxMipLevels(VkExtent2D extent) {
    uint32_t maxDim = std::max(extent.width, extent.height);
    if (maxDim == 0) {
        return 1;
    }
    return static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(maxDim)))) + 1;
}

// Depth/depth-stencil formats need DEPTH_BIT (and STENCIL_BIT for the
// combined ones) as their view's aspect mask instead of COLOR_BIT. None of
// Task 4's own tests exercise a depth format (they're all color UNORM),
// but Task 6's brief already calls out "Depth buffer via Texture2D" as a
// near-term consumer, so this is handled correctly from the start rather
// than assumed-color-only.
VkImageAspectFlags aspectMaskForFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// A per-mip-level-range version of rx::rhi::transitionImage (command.h):
// same maximally-conservative ALL_COMMANDS/MEMORY_READ|WRITE barrier
// (correct for any transition at the cost of over-synchronizing --
// appropriate here for the same reason command.h's own comment gives:
// this is setup/upload code, not a steady-state per-frame path), just
// parametrized over an arbitrary [baseLevel, baseLevel+levelCount) range
// instead of always covering VK_REMAINING_MIP_LEVELS -- which is exactly
// what recordMipChainBlit() below needs and transitionImage() itself
// cannot express.
void transitionLevelRange(VkCommandBuffer cmd, VkImage image, uint32_t baseLevel, uint32_t levelCount,
                           VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = baseLevel;
    barrier.subresourceRange.levelCount = levelCount;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo depInfo{};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &depInfo);
}

}  // namespace

std::optional<Texture2D> Texture2D::create(VkPhysicalDevice physicalDevice, VkDevice device, Allocator& allocator,
                                            VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                                            uint32_t requestedMipLevels) {
    uint32_t maxPossible = computeMaxMipLevels(extent);
    uint32_t mipLevels = requestedMipLevels == 0 ? maxPossible : std::min(requestedMipLevels, maxPossible);

    if (mipLevels > 1) {
        VkFormatProperties formatProps{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &formatProps);
        // Three bits, not just BLIT_DST (see this method's declaration
        // comment in texture.h for why SRC is equally required) [R:C2]:
        // BLIT_SRC_BIT/BLIT_DST_BIT for the blit itself, PLUS
        // SAMPLED_IMAGE_FILTER_LINEAR_BIT -- vkCmdBlitImage with
        // VK_FILTER_LINEAR (recordMipChainBlit()'s filter choice) also
        // requires the SOURCE format to support linear filtering under
        // the tiling in use (VUID-vkCmdBlitImage-filter-02001: "If filter
        // is VK_FILTER_LINEAR, ... must contain
        // VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT"), a
        // requirement independent of the two blit bits above and missed
        // by this task's own first implementation (flagged in review).
        constexpr VkFormatFeatureFlags kBlitChainFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                                              VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((formatProps.optimalTilingFeatures & kBlitChainFeatures) != kBlitChainFeatures) {
            RX_LOG_WARN(
                "Texture2D::create: format {} does not support BLIT_SRC+BLIT_DST+FILTER_LINEAR under optimal "
                "tiling on this device -- falling back to a single mip level instead of the requested {} [R:C2]",
                static_cast<int>(format), mipLevels);
            mipLevels = 1;
        }
    }

    // Level 0 always needs to be a transfer destination (Uploader's
    // initial pixel copy); every level but the last additionally needs
    // to be a transfer source once mipLevels > 1 (each level but the last
    // is a vkCmdBlitImage source for the next one). Callers must not add
    // either bit themselves -- see the header comment.
    VkImageUsageFlags finalUsage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (mipLevels > 1) {
        finalUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = finalUsage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // No HOST_ACCESS_* flags requested at all -- VMA_MEMORY_USAGE_AUTO
    // with no host-visibility request resolves to DEVICE_LOCAL memory,
    // exactly right for an optimal-tiling sampled image nothing on the
    // CPU side ever maps directly (mirrors Allocator::
    // createDeviceLocalBuffer's own reasoning in buffer.cpp).
    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkResult result = vmaCreateImage(allocator.raw(), &imageInfo, &allocCreateInfo, &image, &allocation, nullptr);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("vmaCreateImage failed: VkResult={}", static_cast<int>(result));
        return std::nullopt;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectMaskForFormat(format);
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    result = vkCreateImageView(device, &viewInfo, nullptr, &view);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("vkCreateImageView failed: VkResult={}", static_cast<int>(result));
        vmaDestroyImage(allocator.raw(), image, allocation);
        return std::nullopt;
    }

    return Texture2D(allocator.raw(), device, image, allocation, view, extent, format, mipLevels);
}

void Texture2D::recordMipChainBlit(VkCommandBuffer cmd) const {
    if (mipLevels_ <= 1) {
        // Nothing to blit -- level 0 alone still needs the
        // TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL transition
        // Uploader::uploadToImage() otherwise relies on this function for.
        transitionLevelRange(cmd, image_, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return;
    }

    int32_t srcWidth = static_cast<int32_t>(extent_.width);
    int32_t srcHeight = static_cast<int32_t>(extent_.height);

    for (uint32_t level = 1; level < mipLevels_; ++level) {
        // Level `level - 1` was just written -- either the original pixel
        // upload (level 0) or the previous loop iteration's blit
        // destination (any later level) -- and is still
        // TRANSFER_DST_OPTIMAL; promote it to a blit source. Level
        // `level` is still its initial UNDEFINED layout; prepare it as
        // this iteration's blit destination.
        transitionLevelRange(cmd, image_, level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionLevelRange(cmd, image_, level, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        int32_t dstWidth = std::max(1, srcWidth / 2);
        int32_t dstHeight = std::max(1, srcHeight / 2);

        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {srcWidth, srcHeight, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {dstWidth, dstHeight, 1};

        // sRGB CAVEAT -- see this method's declaration comment in
        // texture.h before reusing this for an SRGB format.
        vkCmdBlitImage(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        srcWidth = dstWidth;
        srcHeight = dstHeight;
    }

    // Every level but the last is now TRANSFER_SRC_OPTIMAL (promoted
    // above to serve as that iteration's blit source); the last level is
    // still TRANSFER_DST_OPTIMAL (blitted into, never promoted since
    // nothing blits from it). Two range transitions cover the whole mip
    // chain instead of one per level.
    transitionLevelRange(cmd, image_, 0, mipLevels_ - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionLevelRange(cmd, image_, mipLevels_ - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

Texture2D::Texture2D(Texture2D&& other) noexcept : Texture2D() {
    *this = std::move(other);
}

Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
    if (this != &other) {
        destroyAll();

        allocator_ = other.allocator_;
        device_ = other.device_;
        image_ = other.image_;
        allocation_ = other.allocation_;
        view_ = other.view_;
        extent_ = other.extent_;
        format_ = other.format_;
        mipLevels_ = other.mipLevels_;

        other.allocator_ = VK_NULL_HANDLE;
        other.device_ = VK_NULL_HANDLE;
        other.image_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.view_ = VK_NULL_HANDLE;
        other.extent_ = VkExtent2D{0, 0};
        other.format_ = VK_FORMAT_UNDEFINED;
        other.mipLevels_ = 1;
    }
    return *this;
}

Texture2D::~Texture2D() {
    destroyAll();
}

void Texture2D::destroyAll() {
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view_, nullptr);
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image_, allocation_);
    }
}

}  // namespace rx::rhi
