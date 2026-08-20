#pragma once
// Same include shape as texture.h -- volk first (VK_NO_PROTOTYPES means
// <vulkan/vulkan.h> alone declares no real functions; see that header's own
// comment), then buffer.h for rx::rhi::Allocator/MemoryCategory.
#include <volk.h>
#include <rx_rhi_vk/buffer.h>

#include <optional>
#include <utility>
#include <vector>

namespace rx::rhi {

// [Task 2, gate ruling RC2: "storage-image API mirrors StorageBuffer
// shapes"; RC2 resource-model extension: "cube/array views"] A VMA-backed
// VkImage supporting an arbitrary mip-level count AND array-layer count
// (including cube/cube-array), backing rx_graph's new
// Pass::addStorageImageOutput()/addStorageImageInput() resource kind.
//
// Deliberately a NEW, purpose-built type -- NOT an extension of Texture2D
// (texture.h), which is "always 2D, always a single array layer" by
// design and used throughout MaterialSystem/every sample on exactly that
// invariant; growing it to carry array/cube shape would be an invasive,
// high-blast-radius change to a class this task's own scope does not
// otherwise need to touch (rasterized attachment output stays
// single-mip/single-layer -- see resources.h's PhysicalResource::mipLevels
// comment for why that is a deliberate boundary). Mirrors Texture2D's own
// RAII/creation-parameter conventions wherever they apply unchanged
// (texture.h/texture.cpp).
//
// Owns one "full" VkImageView (covering every mip/layer,
// VK_REMAINING_MIP_LEVELS/VK_REMAINING_ARRAY_LAYERS -- exactly Texture2D::
// view()'s own shape) created at construction time, plus a small internal
// cache of narrower, subresource-scoped views built on demand via
// viewForSubresource(). Every view this object ever creates is destroyed
// together, in its own destructor -- a caller never owns or destroys a
// VkImageView either accessor returns.
//
// Move-only RAII, same lifetime discipline as Texture2D/Buffer: a
// StorageImage must not outlive the Allocator (and beneath it,
// Device/Context) it was created from.
class StorageImage {
public:
    StorageImage(StorageImage&&) noexcept;
    StorageImage& operator=(StorageImage&&) noexcept;
    StorageImage(const StorageImage&) = delete;
    StorageImage& operator=(const StorageImage&) = delete;
    ~StorageImage();

    // `usage` is ORed with VK_IMAGE_USAGE_STORAGE_BIT unconditionally --
    // this type exists specifically to back a storage-image binding, so
    // callers should not add that bit themselves (mirrors Texture2D::
    // create()'s own "usage is ORed with TRANSFER_DST_BIT, don't add it
    // yourself" convention). `cube` requires `arrayLayers` to be a
    // positive multiple of 6 [VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT's own
    // Vulkan-spec precondition] -- a CALLER-SIDE precondition (rx_graph's
    // RenderGraph::compile() already validates this before Executor ever
    // reaches here -- render_graph.cpp), not re-validated in this
    // constructor; passing an invalid combination is undefined-by-contract
    // here, exactly like Texture2D::createForPresuppliedMips()'s own
    // "mipLevels must be >= 1, a caller-side precondition" note.
    //
    // Returns std::nullopt (logged, classified via `allocator` exactly
    // like Texture2D::create()'s own vmaCreateImage/vkCreateImageView
    // failure split) on any creation failure.
    static std::optional<StorageImage> create(VkPhysicalDevice physicalDevice, VkDevice device, Allocator& allocator,
                                                VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                                                uint32_t mipLevels, uint32_t arrayLayers, bool cube,
                                                MemoryCategory category = MemoryCategory::Internal);

    VkImage image() const { return image_; }
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    uint32_t mipLevels() const { return mipLevels_; }
    uint32_t arrayLayers() const { return arrayLayers_; }
    bool cube() const { return cube_; }
    VkDeviceSize allocatedBytes() const { return allocatedBytes_; }

    // A view covering the WHOLE resource (every mip, every layer) --
    // created once at construction time, exactly like Texture2D::view().
    // viewType is 2D for a single-layer image, 2DArray for a multi-layer
    // non-cube one, Cube for an exactly-6-layer cube-created one, or
    // CubeArray otherwise.
    VkImageView fullView() const { return fullView_; }

    // Returns (creating and caching on first request) a VkImageView scoped
    // EXACTLY to [baseMipLevel, baseMipLevel+levelCount) x
    // [baseArrayLayer, baseArrayLayer+layerCount). `levelCount`/`layerCount`
    // must already be concrete (no kRemainingMipLevels/kRemainingArrayLayers
    // sentinel -- rx_graph::RenderGraph::compile() resolves every declared
    // Subresource to concrete counts before Executor ever calls this; see
    // rx_graph/resources.h's Subresource/render_graph.cpp's
    // resolveSubresource()). A request whose range covers the whole
    // resource returns fullView() itself, with no extra view created or
    // cached.
    //
    // viewType is chosen from THIS REQUEST's own layerCount (a single-face
    // view of a cube image is legitimately plain VK_IMAGE_VIEW_TYPE_2D --
    // Vulkan has no "one face of a cube" cube-shaped view type -- even
    // though cube() is true for the image as a whole): layerCount == 1 ->
    // Texture2D-shaped 2D view; layerCount > 1 and (!cube() || layerCount
    // % 6 != 0) -> 2D array view; layerCount > 1, cube(), and layerCount %
    // 6 == 0 -> Cube view (layerCount == 6) or CubeArray view (layerCount
    // > 6).
    //
    // Returns VK_NULL_HANDLE (logged) on a vkCreateImageView failure --
    // every view successfully created up to that point (including
    // fullView()) remains valid and owned by this object regardless.
    VkImageView viewForSubresource(uint32_t baseMipLevel, uint32_t levelCount, uint32_t baseArrayLayer,
                                    uint32_t layerCount);

private:
    struct SubresourceKey {
        uint32_t baseMipLevel = 0;
        uint32_t levelCount = 0;
        uint32_t baseArrayLayer = 0;
        uint32_t layerCount = 0;

        bool operator==(const SubresourceKey&) const = default;
    };

    StorageImage() = default;
    StorageImage(VmaAllocator allocator, VkDevice device, VkImage image, VmaAllocation allocation,
                 VkImageView fullView, VkExtent2D extent, VkFormat format, uint32_t mipLevels, uint32_t arrayLayers,
                 bool cube, std::shared_ptr<MemoryAccounting> accounting, MemoryCategory category,
                 VkDeviceSize allocatedBytes);

    void destroyAll();

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView fullView_ = VK_NULL_HANDLE;
    VkExtent2D extent_{0, 0};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    uint32_t mipLevels_ = 1;
    uint32_t arrayLayers_ = 1;
    bool cube_ = false;

    std::shared_ptr<MemoryAccounting> accounting_;
    MemoryCategory category_ = MemoryCategory::Internal;
    VkDeviceSize allocatedBytes_ = 0;

    // Small linear-scan cache -- a real pass graph declares at most a
    // handful of distinct subresource ranges against any one storage
    // image (a per-face bake, a per-mip chain), the same "plainest correct
    // structure, no cleverness with no measured benefit" precedent this
    // codebase already follows elsewhere (e.g. rx_graph's own
    // TransientPool::pinned_).
    std::vector<std::pair<SubresourceKey, VkImageView>> subresourceViews_;
};

}  // namespace rx::rhi
