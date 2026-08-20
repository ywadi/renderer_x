#include <rx_rhi_vk/storage_image.h>

#include <rx_core/log.h>

#include <utility>

namespace rx::rhi {

namespace {

// Same small format-bucket helper as texture.cpp's own aspectMaskForFormat()
// -- an independent copy for the same reason that one documents itself as
// one (not exported past its own .cpp).
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

// The VkImageViewType a [baseArrayLayer, baseArrayLayer+layerCount) request
// against a `cube`-created image resolves to -- see viewForSubresource()'s
// own header comment for the full rule.
VkImageViewType viewTypeFor(uint32_t layerCount, bool cube) {
    if (layerCount == 1) {
        return VK_IMAGE_VIEW_TYPE_2D;
    }
    if (cube && layerCount % 6 == 0) {
        return layerCount == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }
    return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
}

}  // namespace

std::optional<StorageImage> StorageImage::create(VkPhysicalDevice /*physicalDevice*/, VkDevice device,
                                                   Allocator& allocator, VkExtent2D extent, VkFormat format,
                                                   VkImageUsageFlags usage, uint32_t mipLevels, uint32_t arrayLayers,
                                                   bool cube, MemoryCategory category) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = arrayLayers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // VK_IMAGE_USAGE_STORAGE_BIT unconditionally (see this method's own
    // header comment) plus VK_IMAGE_USAGE_TRANSFER_SRC_BIT unconditionally
    // too -- mirrors Texture2D::create()'s own "usage is ORed with
    // TRANSFER_DST_BIT unconditionally... callers should NOT add it
    // themselves" precedent (texture.h/texture.cpp), for the copy-OUT
    // direction instead of copy-in: rx_graph's own Pass API (resources.h)
    // has no way for a StorageImageOutput declaration to request an extra
    // usage bit the way BufferDesc::usage lets a storage buffer ask for
    // TRANSFER_SRC_BIT explicitly (ImageDesc carries no `usage` field at
    // all -- see that struct's own comment), so a compute-written storage
    // image could otherwise NEVER be read back or copied out of by any
    // caller -- a real, concrete need this ticket's own GPU test already
    // has (host-visible value-assertion readback) and every realistic
    // future compute consumer (a baked cubemap copied into its final
    // resting place, a mip chain blitted out) would hit too.
    imageInfo.usage = usage | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // No HOST_ACCESS_* flags requested -- VMA_MEMORY_USAGE_AUTO with no
    // host-visibility request resolves to DEVICE_LOCAL memory, same
    // reasoning as Texture2D::create()'s own identical choice.
    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};
    VkResult result =
        vmaCreateImage(allocator.raw(), &imageInfo, &allocCreateInfo, &image, &allocation, &allocationInfo);
    if (result != VK_SUCCESS) {
        allocator.noteAllocationFailure("StorageImage::create(vmaCreateImage)", category, result);
        return std::nullopt;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = viewTypeFor(arrayLayers, cube);
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectMaskForFormat(format);
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = arrayLayers;

    VkImageView fullView = VK_NULL_HANDLE;
    result = vkCreateImageView(device, &viewInfo, nullptr, &fullView);
    if (result != VK_SUCCESS) {
        allocator.noteNonMemoryFailure("StorageImage::create(vkCreateImageView)", result);
        vmaDestroyImage(allocator.raw(), image, allocation);
        return std::nullopt;
    }

    allocator.accounting()->record(category, allocationInfo.size);
    return StorageImage(allocator.raw(), device, image, allocation, fullView, extent, format, mipLevels, arrayLayers,
                         cube, allocator.accounting(), category, allocationInfo.size);
}

VkImageView StorageImage::viewForSubresource(uint32_t baseMipLevel, uint32_t levelCount, uint32_t baseArrayLayer,
                                              uint32_t layerCount) {
    if (baseMipLevel == 0 && levelCount == mipLevels_ && baseArrayLayer == 0 && layerCount == arrayLayers_) {
        return fullView_;
    }

    const SubresourceKey key{baseMipLevel, levelCount, baseArrayLayer, layerCount};
    for (const auto& [existingKey, view] : subresourceViews_) {
        if (existingKey == key) {
            return view;
        }
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = viewTypeFor(layerCount, cube_);
    viewInfo.format = format_;
    viewInfo.subresourceRange.aspectMask = aspectMaskForFormat(format_);
    viewInfo.subresourceRange.baseMipLevel = baseMipLevel;
    viewInfo.subresourceRange.levelCount = levelCount;
    viewInfo.subresourceRange.baseArrayLayer = baseArrayLayer;
    viewInfo.subresourceRange.layerCount = layerCount;

    VkImageView view = VK_NULL_HANDLE;
    const VkResult result = vkCreateImageView(device_, &viewInfo, nullptr, &view);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("rx_rhi_vk: StorageImage::viewForSubresource: vkCreateImageView failed (mip [{}, {}), layer "
                     "[{}, {})): VkResult={}",
                     baseMipLevel, baseMipLevel + levelCount, baseArrayLayer, baseArrayLayer + layerCount,
                     static_cast<int>(result));
        return VK_NULL_HANDLE;
    }

    subresourceViews_.push_back({key, view});
    return view;
}

StorageImage::StorageImage(VmaAllocator allocator, VkDevice device, VkImage image, VmaAllocation allocation,
                            VkImageView fullView, VkExtent2D extent, VkFormat format, uint32_t mipLevels,
                            uint32_t arrayLayers, bool cube, std::shared_ptr<MemoryAccounting> accounting,
                            MemoryCategory category, VkDeviceSize allocatedBytes)
    : allocator_(allocator),
      device_(device),
      image_(image),
      allocation_(allocation),
      fullView_(fullView),
      extent_(extent),
      format_(format),
      mipLevels_(mipLevels),
      arrayLayers_(arrayLayers),
      cube_(cube),
      accounting_(std::move(accounting)),
      category_(category),
      allocatedBytes_(allocatedBytes) {}

StorageImage::StorageImage(StorageImage&& other) noexcept : StorageImage() {
    *this = std::move(other);
}

StorageImage& StorageImage::operator=(StorageImage&& other) noexcept {
    if (this != &other) {
        destroyAll();

        allocator_ = other.allocator_;
        device_ = other.device_;
        image_ = other.image_;
        allocation_ = other.allocation_;
        fullView_ = other.fullView_;
        extent_ = other.extent_;
        format_ = other.format_;
        mipLevels_ = other.mipLevels_;
        arrayLayers_ = other.arrayLayers_;
        cube_ = other.cube_;
        accounting_ = std::move(other.accounting_);
        category_ = other.category_;
        allocatedBytes_ = other.allocatedBytes_;
        subresourceViews_ = std::move(other.subresourceViews_);

        other.allocator_ = VK_NULL_HANDLE;
        other.device_ = VK_NULL_HANDLE;
        other.image_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.fullView_ = VK_NULL_HANDLE;
        other.extent_ = VkExtent2D{0, 0};
        other.format_ = VK_FORMAT_UNDEFINED;
        other.mipLevels_ = 1;
        other.arrayLayers_ = 1;
        other.cube_ = false;
        other.category_ = MemoryCategory::Internal;
        other.allocatedBytes_ = 0;
        other.subresourceViews_.clear();
    }
    return *this;
}

StorageImage::~StorageImage() {
    destroyAll();
}

void StorageImage::destroyAll() {
    if (image_ != VK_NULL_HANDLE && accounting_) {
        accounting_->release(category_, allocatedBytes_);
    }
    for (const auto& [key, view] : subresourceViews_) {
        (void)key;
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view, nullptr);
        }
    }
    if (fullView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, fullView_, nullptr);
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image_, allocation_);
    }
}

}  // namespace rx::rhi
