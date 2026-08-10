#pragma once

// VMA function-loading mode: hand VMA only the two entry points volk exposes
// as real global function pointers (vkGetInstanceProcAddr/
// vkGetDeviceProcAddr, both populated by volkInitialize()/volkLoadInstance()
// -- NOT volkLoadDevice(), verified directly against the vendored volk.c:
// volkLoadInstance() calls volkGenLoadInstance(), which is what assigns the
// global `vkGetDeviceProcAddr` pointer (volk.c:253); volkLoadDevice() only
// populates a per-device VolkDeviceTable, never touching that global. See
// rx_rhi_vk/context.h (volkInitialize()/volkLoadInstance()) and device.h
// (volkLoadDevice(), irrelevant to this global) -- and let VMA's
// own dynamic-loading path (VMA_DYNAMIC_VULKAN_FUNCTIONS) resolve every
// other Vulkan entry point itself via those two. Do NOT hand-fill a partial
// VmaVulkanFunctions table instead (the classic copy-pasted Vulkan-1.0-era
// sample): with VmaAllocatorCreateInfo::vulkanApiVersion ==
// VK_API_VERSION_1_3 (see buffer.cpp), VMA additionally needs the `*2`
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

    // True only for a Buffer created by Allocator::createDeviceLocalBuffer()
    // whose resulting VMA memory type turned out to be BOTH
    // VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT and
    // VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT -- the ReBAR/unified-memory
    // "direct upload" classification [R:C1]. rx::rhi::Uploader::
    // uploadToBuffer() checks this (together with mappedData() != nullptr)
    // to decide whether it can memcpy straight into this Buffer and skip
    // the staging ring entirely. Always false for a Buffer created by
    // createHostVisibleBuffer() (that factory never requests the
    // ALLOW_TRANSFER_INSTEAD_BIT preference in the first place -- see its
    // own comment) regardless of what its real memory type happens to be;
    // this is a deliberate, hardcoded false, not a measurement, precisely
    // so a test can force the non-direct branch deterministically without
    // depending on any specific hardware's classification (see
    // upload_test.cpp).
    bool directPathCapable() const { return directPathCapable_; }

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
    Buffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation, void* mappedData, VkDeviceSize size,
           bool directPathCapable = false)
        : allocator_(allocator),
          buffer_(buffer),
          allocation_(allocation),
          mappedData_(mappedData),
          size_(size),
          directPathCapable_(directPathCapable) {}

    void destroyAll();

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    void* mappedData_ = nullptr;
    VkDeviceSize size_ = 0;
    bool directPathCapable_ = false;
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

    // Allocates a VkBuffer with `usage` -- intended for a real
    // device-consuming role (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
    // _INDEX_BUFFER_BIT, _STORAGE_BUFFER_BIT, ...; TRANSFER_DST_BIT alone
    // does NOT count, see below) -- via VMA_MEMORY_USAGE_AUTO +
    // VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
    // VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
    // VMA_ALLOCATION_CREATE_MAPPED_BIT -- the real ReBAR-aware
    // direct-upload pattern [R:C1], applied to the actual destination
    // resource rather than a staging buffer (see the CRITICAL fix note
    // below for why that distinction is load-bearing).
    //
    // **Why `usage` must carry a real device-consuming bit for this to do
    // anything:** verified directly against this project's vendored VMA
    // 3.4.0 source (`vk_mem_alloc.h`). `VmaBufferImageUsage::
    // ContainsDeviceAccess()` masks OUT `TRANSFER_SRC_BIT`/`TRANSFER_DST_BIT`
    // and returns whether anything else remains -- a buffer whose usage is
    // ONLY transfer bits reports `ContainsDeviceAccess() == false`.
    // `FindMemoryPreferences()`'s `VMA_MEMORY_USAGE_AUTO` branch only adds
    // `DEVICE_LOCAL_BIT | HOST_VISIBLE_BIT` to its *preferred* flags when
    // `ContainsDeviceAccess() == true` (the `deviceAccess` local) AND
    // `ALLOW_TRANSFER_INSTEAD_BIT` is set; when `ContainsDeviceAccess()`
    // is false, that same code path instead pushes `DEVICE_LOCAL_BIT` into
    // *not-preferred* -- actively steering AWAY from the memory this
    // pattern is supposed to land in. This is exactly the mistake this
    // task's own first implementation made on `createUploadRingBuffer()`'s
    // TRANSFER_SRC-only staging buffer (fixed below) and exactly why this
    // method requires a real device-consuming usage bit to be worth
    // calling with these flags at all on hardware that actually has a
    // choice to steer away from.
    //
    // **Correction (Phase 2 Task 8, verified against a real CI failure --
    // not a hypothetical):** "not-preferred" is a soft tie-breaking signal
    // among *multiple candidate* memory types, not a hard exclusion. On a
    // backend whose Vulkan memory types don't include any HOST_VISIBLE
    // type that ISN'T also DEVICE_LOCAL (verified directly: GitHub Actions'
    // `ubuntu-latest` lavapipe/llvmpipe build reports exactly this, even
    // though this same test passed against this project's development
    // machine's own local lavapipe build -- Mesa/llvmpipe's exposed memory
    // types are not identical across builds/versions), there is nothing
    // better to steer toward instead, and the one remaining valid memory
    // type still gets selected -- which CAN be both DEVICE_LOCAL and
    // HOST_VISIBLE even for a `TRANSFER_DST_BIT`-only usage. So a caller
    // passing only transfer bits here is NOT guaranteed
    // `directPathCapable() == false` "no matter the hardware" (the
    // previous, now-corrected claim here) -- only "on hardware that has a
    // genuinely separate non-host-visible DEVICE_LOCAL memory pool to
    // steer into instead." A test that needs to deterministically force
    // (not just usually get) the staging branch regardless of backend must
    // use `createHostVisibleBuffer()` below instead, whose
    // `directPathCapable()` is a hardcoded `false` by construction, not a
    // measurement -- see `src/rx_rhi_vk/tests/upload_test.cpp`.
    //
    // On a ReBAR-enabled desktop GPU or unified-memory APU, this resolves
    // to memory that is BOTH DEVICE_LOCAL and HOST_VISIBLE
    // (`Buffer::directPathCapable()` true, `mappedData()` non-null) --
    // callers (rx::rhi::Uploader::uploadToBuffer(), rx::rhi::MeshBuffers)
    // can then write directly into it and skip a staging copy entirely.
    // Everywhere else it resolves to plain DEVICE_LOCAL-only memory
    // (`directPathCapable()` false, `mappedData()` null -- VMA does not
    // map memory it cannot map), identical to this method's behavior
    // before this task's Critical review fix. Returns std::nullopt on any
    // failure (logged via RX_LOG_ERROR).
    std::optional<Buffer> createDeviceLocalBuffer(VkDeviceSize size, VkBufferUsageFlags usage);

    // Allocates a VkBuffer with `usage` (typically just
    // VK_BUFFER_USAGE_TRANSFER_SRC_BIT -- this is always a copy *source*,
    // never a real destination resource) via VMA_MEMORY_USAGE_AUTO +
    // VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
    // VMA_ALLOCATION_CREATE_MAPPED_BIT -- always resolves to plain
    // HOST_VISIBLE memory, exactly VMA's own "Simple staging buffer"
    // pattern [R:C1]. The resulting Buffer's directPathCapable() is
    // always false: a TRANSFER_SRC-only usage can never make
    // ContainsDeviceAccess() true (see createDeviceLocalBuffer()'s own
    // comment above), so there is no point requesting
    // ALLOW_TRANSFER_INSTEAD_BIT here at all -- an earlier version of this
    // method did, and it was structurally inert (this task's own Critical
    // review finding: the flag never changed which memory type this
    // buffer landed in, on any hardware). rx::rhi::Uploader's internal
    // staging ring buffer is exactly this: a plain, always-staging
    // resource; the direct-upload optimization belongs on the
    // *destination* (createDeviceLocalBuffer() above), not here. Returns
    // std::nullopt on any failure (logged).
    std::optional<Buffer> createUploadRingBuffer(VkDeviceSize size, VkBufferUsageFlags usage);

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
