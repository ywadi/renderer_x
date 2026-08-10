#pragma once

// VMA function-loading mode: hand VMA only the two entry points volk exposes
// as real global function pointers (vkGetInstanceProcAddr/
// vkGetDeviceProcAddr, populated by volkInitialize()/volkLoadInstance()/
// volkLoadDevice() -- see rx_rhi_vk/context.h and device.h) and let VMA's
// own dynamic-loading path (VMA_DYNAMIC_VULKAN_FUNCTIONS) resolve every
// other Vulkan entry point itself via those two. Do NOT hand-fill a partial
// VmaVulkanFunctions table instead (the classic copy-pasted Vulkan-1.0-era
// sample): with VmaAllocatorCreateInfo::vulkanApiVersion ==
// VK_API_VERSION_1_3 (see allocator.cpp), VMA additionally needs the `*2`
// variants -- vkGetBufferMemoryRequirements2, vkBindBufferMemory2,
// vkGetPhysicalDeviceMemoryProperties2, vkGetDeviceBufferMemoryRequirements,
// etc. -- which a hand-filled 1.0-only table omits; VMA's internal
// ValidateVulkanFunctions() asserts (and behaves incorrectly in builds where
// asserts are compiled out) if any function required by the requested API
// version is still null after import. Letting VMA's own dynamic importer
// fetch everything past the two required entry points means it always asks
// for exactly the set its own vulkanApiVersion/extension flags require, so
// this can never drift out of sync with VMA's own requirements.
//
// Both macros are defined here, above the only #include of
// <vk_mem_alloc.h> in this pair of headers, per VMA's own requirement that
// they be visible to every translation unit that includes the header
// (declarations here, VMA_IMPLEMENTATION in src/vma_impl.cpp -- see that
// file for why it must be the only VMA_IMPLEMENTATION TU in the program).
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <optional>

namespace rx::rhi {

class Context;
class Device;

// A single VkBuffer + VmaAllocation, allocated host-visible and
// persistently mapped (VMA_ALLOCATION_CREATE_MAPPED_BIT) by
// Allocator::createHostVisibleBuffer(). Move-only RAII: destroys both via
// vmaDestroyBuffer on destruction or move-assignment.
//
// A Buffer must not outlive the Allocator (and, beneath it, the Device and
// Context) it was created from: vmaDestroyBuffer() needs the same
// VmaAllocator handle the buffer was allocated from to still be valid.
// Declaring the owning Allocator (and Device/Context) before any Buffer
// created from it in the same scope, per the usual RAII/reverse-destruction
// discipline already used by Context/Device in this library, is sufficient.
class Buffer {
public:
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer();

    VkBuffer handle() const { return buffer_; }
    void* mappedData() const { return mappedData_; }
    VkDeviceSize size() const { return size_; }

    // Wraps vmaFlushAllocation()/vmaInvalidateAllocation() -- the API gap
    // flagged by Phase 1 Task 3's review: that task's readback test had no
    // way to reach the underlying VmaAllocation to invalidate it, and had
    // to fall back to verifying (once, as a test precondition) that every
    // HOST_VISIBLE memory type on the device was also HOST_COHERENT
    // instead. These two calls close that gap for real.
    //
    // Call flush() after writing to mappedData() whenever the GPU will
    // read those bytes next (e.g. this Buffer is about to be used as a
    // vkCmdCopyBuffer/vkCmdCopyBufferToImage source). Call invalidate()
    // before reading mappedData() whenever the GPU may have written to
    // this Buffer since the last time the CPU read it (e.g. this Buffer
    // is a transfer-destination readback buffer and a copy into it was
    // just submitted and waited on). `size` of VK_WHOLE_SIZE (the
    // default) covers everything from `offset` to the end of this
    // Buffer's allocation, matching VMA's own sentinel semantics.
    //
    // Both are correct AND cheap to call unconditionally, even on a
    // memory type that turns out to already be HOST_COHERENT: VMA's own
    // implementation checks the underlying memory type first and skips
    // the real vkFlushMappedMemoryRanges/vkInvalidateMappedMemoryRanges
    // call entirely when it isn't needed. So callers should always call
    // the appropriate one around a CPU<->GPU handoff on mapped memory
    // rather than trying to detect/assume coherence themselves the way
    // Phase 1's readback test had to. No-ops if this Buffer is invalid
    // (default-constructed or moved-from). Logs (RX_LOG_ERROR) and
    // otherwise no-ops on the rare underlying VkResult failure.
    void flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;
    void invalidate(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;

private:
    friend class Allocator;

    Buffer() = default;
    Buffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation, void* mappedData, VkDeviceSize size)
        : allocator_(allocator), buffer_(buffer), allocation_(allocation), mappedData_(mappedData), size_(size) {}

    void destroyAll();

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    void* mappedData_ = nullptr;
    VkDeviceSize size_ = 0;
};

// Owns a VmaAllocator built against a specific VkInstance/VkPhysicalDevice/
// VkDevice triple. Move-only RAII: destroys the underlying VmaAllocator via
// vmaDestroyAllocator on destruction or move-assignment.
class Allocator {
public:
    Allocator(Allocator&&) noexcept;
    Allocator& operator=(Allocator&&) noexcept;
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;
    ~Allocator();

    // Convenience overload for the common case: builds against the
    // VkInstance/VkPhysicalDevice/VkDevice a Context+Device already own.
    // Delegates to createRaw() below -- both share one implementation.
    static std::optional<Allocator> create(Context& context, Device& device);

    // Builds a VmaAllocator directly from raw handles, with no dependency
    // on rx::rhi::Context/Device -- e.g. for tooling or tests that want an
    // allocator without constructing either.
    static std::optional<Allocator> createRaw(VkPhysicalDevice physicalDevice, VkDevice device, VkInstance instance);

    // Allocates a VkBuffer with `usage`, backed by host-visible, persistently
    // mapped memory (VMA_MEMORY_USAGE_AUTO +
    // VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
    // VMA_ALLOCATION_CREATE_MAPPED_BIT) -- suitable for CPU-written,
    // GPU-read data such as vertex/index/uniform buffers updated every
    // frame. Returns std::nullopt on any failure (logged via RX_LOG_ERROR).
    std::optional<Buffer> createHostVisibleBuffer(VkDeviceSize size, VkBufferUsageFlags usage);

    // Allocates a VkBuffer with `usage`, backed by DEVICE_LOCAL memory with
    // no host-visibility request at all (VMA_MEMORY_USAGE_AUTO, no
    // HOST_ACCESS_* flags, no MAPPED_BIT) -- the resulting Buffer's
    // mappedData() is always nullptr. Suitable for GPU-only resources
    // written exactly once via a real transfer (rx::rhi::Uploader) and
    // read many times afterward, such as rx::rhi::MeshBuffers' vertex/
    // index buffers -- never for anything the CPU writes directly.
    // Returns std::nullopt on any failure (logged via RX_LOG_ERROR).
    std::optional<Buffer> createDeviceLocalBuffer(VkDeviceSize size, VkBufferUsageFlags usage);

    // Result of createUploadRingBuffer() below: the allocated staging
    // Buffer, plus whether VMA actually placed it in memory that is BOTH
    // DEVICE_LOCAL and HOST_VISIBLE (the ReBAR/integrated-memory "direct"
    // fast path [R:C1]) as opposed to falling back to plain HOST_VISIBLE
    // system memory (the non-ReBAR desktop case). `deviceLocal` is a
    // one-time, informational classification for logging/diagnostics --
    // rx::rhi::Uploader's copy code (vkCmdCopyBuffer/
    // vkCmdCopyBufferToImage from this buffer) is identical either way;
    // only the underlying memory's physical location and copy bandwidth
    // differ.
    struct UploadBufferResult {
        Buffer buffer;
        bool deviceLocal = false;
    };

    // Allocates a VkBuffer with `usage` (typically just
    // VK_BUFFER_USAGE_TRANSFER_SRC_BIT -- this is always a copy *source*,
    // never a real destination resource) via VMA_MEMORY_USAGE_AUTO +
    // VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
    // VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
    // VMA_ALLOCATION_CREATE_MAPPED_BIT -- the exact ReBAR-aware pattern
    // [R:C1] documents: VMA tries a memory type that is both DEVICE_LOCAL
    // and HOST_VISIBLE first, falling back to plain HOST_VISIBLE system
    // memory if none exists or allocation from it fails. Which one was
    // actually obtained is reported via the returned `deviceLocal` flag
    // (checked once via vmaGetMemoryTypeProperties -- no physical-device
    // handle needed here). Returns std::nullopt on any failure (logged).
    std::optional<UploadBufferResult> createUploadRingBuffer(VkDeviceSize size, VkBufferUsageFlags usage);

    // Raw VmaAllocator handle -- needed by rx::rhi::Texture2D::create()
    // (texture.h, a separate header/TU) to call vmaCreateImage/
    // vmaDestroyImage directly, mirroring what this Allocator's own
    // createHostVisibleBuffer()/createDeviceLocalBuffer() do internally
    // for buffers. Deliberately not wrapped in an Allocator::createImage()
    // method here: that would make buffer.h/buffer.cpp know about
    // Texture2D (format-feature queries, mip-level computation), which
    // belongs entirely in texture.h/texture.cpp instead. Not intended for
    // ordinary callers -- prefer the create*Buffer() methods above for
    // buffers, and Texture2D::create() for images.
    VmaAllocator raw() const { return allocator_; }

private:
    Allocator() = default;
    explicit Allocator(VmaAllocator allocator) : allocator_(allocator) {}

    void destroyAll();

    VmaAllocator allocator_ = VK_NULL_HANDLE;
};

}  // namespace rx::rhi
