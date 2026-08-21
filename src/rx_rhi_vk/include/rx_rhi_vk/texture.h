#pragma once
// volk first: with VK_NO_PROTOTYPES set (rx_rhi_vk's PUBLIC compile
// definition, buffer.h's own comment on VMA's dynamic-loading path
// explains why), <vulkan/vulkan.h> declares no real Vulkan functions at
// all -- volk.h is what actually declares them, as `extern PFN_vkFoo vkFoo;`
// globals populated by volkLoadDevice()/volkLoadInstance() (see context.h/
// device.h). Every direct Vulkan call this header's .cpp makes
// (vkCreateImageView, vkCmdBlitImage, vkCmdPipelineBarrier2, ...) needs
// this; buffer.h/buffer.cpp never needed it because they only ever call
// VMA's own vmaCreateBuffer()-style functions, never a raw vkFoo directly.
#include <volk.h>
// Reuses buffer.h's include of <vk_mem_alloc.h> (with
// VMA_STATIC_VULKAN_FUNCTIONS=0 / VMA_DYNAMIC_VULKAN_FUNCTIONS=1 already
// defined there) rather than repeating those defines here -- VMA requires
// them visible before its header is included, but only ever once
// per-TU-chain; buffer.h already does this. See buffer.h's own comment for
// the full rationale. This also brings in rx::rhi::Allocator, needed below.
#include <rx_rhi_vk/buffer.h>
#include <optional>

namespace rx::rhi {

// A single VMA-backed VkImage + VkImageView, always 2D, always a single
// array layer. Move-only RAII: destroys the view then the image
// (vmaDestroyImage, which also frees the VmaAllocation) on destruction or
// move-assignment.
//
// Same lifetime discipline as Buffer (buffer.h): a Texture2D must not
// outlive the Allocator (and beneath it, Device/Context) it was created
// from.
//
// Thread-affinity (D5, Phase 4): create() is main-thread-only, same
// GPU-object-mutation convention as Allocator's own factory methods
// (buffer.h) -- see docs/threading.md. recordMipChainBlit() records onto a
// caller-supplied VkCommandBuffer and carries whatever affinity that
// command buffer's own recording thread already has (main thread, or a
// chunked pass's own worker per the "Worker-allowed" section of
// docs/threading.md) -- it touches no shared Texture2D/Allocator state
// itself.
class Texture2D {
public:
    Texture2D(Texture2D&&) noexcept;
    Texture2D& operator=(Texture2D&&) noexcept;
    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;
    ~Texture2D();

    // Pass 0 for `requestedMipLevels` to auto-compute a full mip chain
    // down to 1x1 (floor(log2(max(extent.width, extent.height))) + 1);
    // pass a specific count to request exactly that many (clamped down to
    // the maximum the extent actually supports, silently -- requesting
    // more levels than an extent can produce is a caller-side rounding
    // convenience, not an error worth failing over).
    //
    // Before honoring any mipLevels > 1 request, this queries
    // `physicalDevice`'s VK_FORMAT_FEATURE_BLIT_SRC_BIT |
    // VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
    // support for `format` under optimal tiling. The brief's own
    // shorthand names only the DST bit, but two more bits are equally
    // required for this Texture2D's actual recordMipChainBlit()
    // implementation to be valid, not just "nice to check": SRC is needed
    // because the blit chain reads FROM level N (via BLIT_SRC_BIT) to
    // write TO level N+1 (via BLIT_DST_BIT), and FILTER_LINEAR is needed
    // because recordMipChainBlit() calls vkCmdBlitImage with
    // VK_FILTER_LINEAR, which Vulkan separately requires the SOURCE
    // format to support under this same optimal-tiling feature set
    // (VUID-vkCmdBlitImage-filter-02001) -- missing that third bit was a
    // real gap this task's review caught, not a hypothetical one. If any
    // of the three is unsupported, this silently falls back to a single
    // mip level and logs RX_LOG_WARN naming the format [R:C2] -- never a
    // hard failure, since a texture with no mips is still a perfectly
    // usable texture.
    //
    // `usage` is ORed with VK_IMAGE_USAGE_TRANSFER_DST_BIT unconditionally
    // (level 0 always needs to be a copy destination for
    // rx::rhi::Uploader::uploadToImage()) and additionally with
    // VK_IMAGE_USAGE_TRANSFER_SRC_BIT whenever the resulting mip count
    // ends up > 1 (every level but the last needs to be a blit source).
    // Callers should NOT add either of those bits themselves; this
    // function already does.
    //
    // `physicalDevice`/`device` are used for the format-feature query
    // above and for vkCreateImageView/vkDestroyImageView respectively --
    // the same kind of brief-shorthand elaboration Task 3's
    // BindlessTable::create() made for its own capacity pre-check (see
    // that class's own header comment and task-3-report.md), for the same
    // reason: a clean, logged single-mip fallback cannot be implemented
    // without a real feature query, and an Allocator alone exposes
    // neither a VkPhysicalDevice nor a VkDevice to get one from.
    //
    // `category` [Phase 4 Task 10, spec D24(a)] attributes this image's
    // allocated bytes in `allocator`'s accounting ledger (Allocator::report()
    // reads it) -- default MemoryCategory::Internal, same rationale as
    // Allocator::createHostVisibleBuffer()'s own default (buffer.h).
    // Returns std::nullopt (logged) on any image/view creation failure. On
    // the vmaCreateImage step specifically, a VK_ERROR_OUT_OF_DEVICE_MEMORY
    // failure is classified/report-attached via `allocator`
    // (Allocator::lastAllocationFailureKind()/lastAllocationFailureReport(),
    // buffer.h [D24(d)]); a LATER vkCreateImageView failure (the image
    // itself already having been created successfully) is a DIFFERENT,
    // deliberately-not-memory-attributed failure class -- see
    // Allocator::noteNonMemoryFailure()'s own comment for why
    // (matrix-issue27 row 9: these two must stay distinguishable).
    static std::optional<Texture2D> create(VkPhysicalDevice physicalDevice, VkDevice device, Allocator& allocator,
                                            VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                                            uint32_t requestedMipLevels = 0,
                                            MemoryCategory category = MemoryCategory::Internal);

    // [Phase 4 Stage 1 Task 14, gate matrix-issue03 N4/gate ruling #3]
    // Creates a Texture2D whose FULL mip chain is supplied by the caller
    // with real per-level data (a KTX2 container's own levels, or any
    // other pre-mipped source) -- see Uploader::uploadImageMips() below,
    // this factory's actual consumer. Unlike create() above,
    // `mipLevels` is honored EXACTLY, with NO blit-format-feature probe
    // and no fallback-to-1 clamp: that probe exists ONLY to gate whether
    // runtime vkCmdBlitImage mip GENERATION (recordMipChainBlit()) is
    // safe to attempt, and a Texture2D built through this factory must
    // NEVER have recordMipChainBlit() called against it (nothing here
    // ever needs to blit-generate a level that already has real data).
    // Usage carries `usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT` ONLY --
    // never TRANSFER_SRC_BIT, since nothing ever blits FROM an image
    // created this way; this also means a block-compressed format that
    // does not support BLIT_SRC/BLIT_DST/FILTER_LINEAR under optimal
    // tiling (common for compressed formats, irrelevant here) still
    // creates successfully, as long as it supports SAMPLED_IMAGE +
    // TRANSFER_DST -- the two bits this factory's own usage flags
    // actually need.
    //
    // `mipLevels` must be >= 1 (a caller-side precondition, not
    // defensively clamped here -- the KTX2 container this factory's real
    // caller reads from always reports at least 1 level by construction).
    //
    // Same failure/category-attribution contract as create() above
    // (std::nullopt + Allocator::noteAllocationFailure()/
    // noteNonMemoryFailure() on vmaCreateImage/vkCreateImageView failure
    // respectively).
    static std::optional<Texture2D> createForPresuppliedMips(VkPhysicalDevice physicalDevice, VkDevice device,
                                                               Allocator& allocator, VkExtent2D extent,
                                                               VkFormat format, VkImageUsageFlags usage,
                                                               uint32_t mipLevels,
                                                               MemoryCategory category = MemoryCategory::Texture);

    // [Phase 5 Task 6, ticket #42, gate matrix-p5t06-ktx2-cubemap-hdr row
    // 7] Cube-aware sibling of createForPresuppliedMips() above -- SAME
    // "fully caller-supplied mip chain, no blit generation, no
    // blit-format-feature probe" contract (see that factory's own header
    // comment for the full rationale), but creates a 6-array-layer image
    // with VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT set and a
    // VK_IMAGE_VIEW_TYPE_CUBE view instead of a plain 2D one -- mirrors
    // this project's own established StorageImage::create() cube-flag/
    // view-type-resolution precedent (storage_image.cpp's viewTypeFor()),
    // reused here rather than reinvented for the sampled-image case.
    // `faceExtent` is ONE face's own width/height (Vulkan's own cube-image
    // rule: all 6 faces of one cube image always share the same extent).
    // `mipLevels` applies per-face, exactly like createForPresuppliedMips()'s
    // own `mipLevels` parameter. Same failure/category-attribution
    // contract as both factories above.
    static std::optional<Texture2D> createCubeForPresuppliedMips(VkPhysicalDevice physicalDevice, VkDevice device,
                                                                    Allocator& allocator, VkExtent2D faceExtent,
                                                                    VkFormat format, VkImageUsageFlags usage,
                                                                    uint32_t mipLevels,
                                                                    MemoryCategory category = MemoryCategory::Texture);

    VkImage image() const { return image_; }
    VkImageView view() const { return view_; }
    VkExtent2D extent() const { return extent_; }
    VkFormat format() const { return format_; }
    uint32_t mipLevels() const { return mipLevels_; }
    MemoryCategory category() const { return category_; }
    VkDeviceSize allocatedBytes() const { return allocatedBytes_; }
    // [Phase 5 Task 6] arrayLayers() is 1 for every Texture2D built via
    // create()/createForPresuppliedMips() and 6 for one built via
    // createCubeForPresuppliedMips() -- isCube() is the same information
    // spelled as a bool, for a caller (TextureCache::registerRealTexture())
    // that just needs to know which of the two factories built this
    // instance without re-deriving it from the layer count.
    uint32_t arrayLayers() const { return arrayLayers_; }
    bool isCube() const { return isCube_; }

    // Records the full mip-chain blit for a texture whose level 0 was
    // just written (e.g. by rx::rhi::Uploader::uploadToImage()) and is
    // currently in VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, with every level
    // 1..mipLevels()-1 still in its initial VK_IMAGE_LAYOUT_UNDEFINED.
    // Issues the per-level VkImageMemoryBarrier2 + vkCmdBlitImage sequence
    // (level 0 -> 1, barrier, 1 -> 2, barrier, ...) and leaves EVERY level
    // in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL on return, ready for
    // sampling. A no-op beyond that same final transition if
    // mipLevels() == 1 (nothing to blit; level 0 alone still needs the
    // TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL barrier).
    //
    // sRGB CAVEAT [R:C2]: vkCmdBlitImage's VK_FILTER_LINEAR box-filters
    // texel VALUES in the image's own stored encoding -- for a UNORM
    // format this is a correct linear-light average; for an SRGB format
    // the same blit instead averages gamma-encoded (perceptual) values
    // directly, which is NOT equivalent to decoding to linear light,
    // averaging, and re-encoding to sRGB. The result is measurably
    // (if subtly) too dark/desaturated versus a "correct" mip chain --
    // this is the exact bug class libktx's own `ktx create` tool shipped
    // and later fixed ("sRGB images being resampled without first
    // decoding to linear", KTX-Software v4.4.2 release notes). This
    // engine accepts that caveat for this phase rather than building a
    // decode/average/re-encode compute pass: the spec's Fixed decision #8
    // restricts Phase 2 samples to UNORM formats specifically so this
    // caveat never actually applies to shipped content. Do not reuse this
    // function unmodified for an SRGB-format Texture2D without accounting
    // for it first.
    void recordMipChainBlit(VkCommandBuffer cmd) const;

private:
    Texture2D() = default;
    // [Phase 5 Task 6] `arrayLayers`/`isCube` default to 1/false so
    // create()/createForPresuppliedMips()'s own `return Texture2D(...)`
    // call sites (texture.cpp) need NO change at all -- zero risk to
    // either existing factory; only createCubeForPresuppliedMips() (the
    // new one) passes 6/true explicitly.
    Texture2D(VmaAllocator allocator, VkDevice device, VkImage image, VmaAllocation allocation, VkImageView view,
               VkExtent2D extent, VkFormat format, uint32_t mipLevels,
               std::shared_ptr<MemoryAccounting> accounting = nullptr,
               MemoryCategory category = MemoryCategory::Internal, VkDeviceSize allocatedBytes = 0,
               uint32_t arrayLayers = 1, bool isCube = false)
        : allocator_(allocator),
          device_(device),
          image_(image),
          allocation_(allocation),
          view_(view),
          extent_(extent),
          format_(format),
          mipLevels_(mipLevels),
          accounting_(std::move(accounting)),
          category_(category),
          allocatedBytes_(allocatedBytes),
          arrayLayers_(arrayLayers),
          isCube_(isCube) {}

    void destroyAll();

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkExtent2D extent_{0, 0};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    uint32_t mipLevels_ = 1;

    // [Phase 4 Task 10] Category-attributed accounting (D24(a)) -- same
    // shape/rationale as Buffer's own accounting_/category_/allocatedBytes_
    // (buffer.h).
    std::shared_ptr<MemoryAccounting> accounting_;
    MemoryCategory category_ = MemoryCategory::Internal;
    VkDeviceSize allocatedBytes_ = 0;

    // [Phase 5 Task 6]
    uint32_t arrayLayers_ = 1;
    bool isCube_ = false;
};

}  // namespace rx::rhi
