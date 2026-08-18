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

#include <rx_rhi_vk/memory_report.h>
#include <memory>
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
//
// Thread-affinity (D5, Phase 4): a Buffer is always CREATED by one of
// Allocator's main-thread-only factory methods below (same
// GPU-object-mutation convention as BindlessTable/Uploader/DeletionQueue --
// see docs/threading.md), so its move/destroy (which record()/release()
// into the category ledger, memory_report.h [Phase 4 Task 10]) follow the
// same affinity in every path this codebase has today. flush()/
// invalidate() touch only this Buffer's own mapped memory, not shared
// state, and carry no additional restriction beyond that.
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

    // [Phase 4 Task 10, spec D24(a)] The category this Buffer's allocated
    // bytes are attributed to in whichever MemoryAccounting ledger created
    // it -- see memory_report.h. `allocatedBytes()` is the real VMA-
    // reported allocation size (VmaAllocationInfo::size, which may be
    // larger than size() due to alignment/block-granularity padding) --
    // the exact byte count record()'d at creation and release()'d at
    // destruction, so the two always net to zero.
    MemoryCategory category() const { return category_; }
    VkDeviceSize allocatedBytes() const { return allocatedBytes_; }

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
           bool directPathCapable = false, std::shared_ptr<MemoryAccounting> accounting = nullptr,
           MemoryCategory category = MemoryCategory::Internal, VkDeviceSize allocatedBytes = 0)
        : allocator_(allocator),
          buffer_(buffer),
          allocation_(allocation),
          mappedData_(mappedData),
          size_(size),
          directPathCapable_(directPathCapable),
          accounting_(std::move(accounting)),
          category_(category),
          allocatedBytes_(allocatedBytes) {}

    void destroyAll();

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    void* mappedData_ = nullptr;
    VkDeviceSize size_ = 0;
    bool directPathCapable_ = false;

    // [Phase 4 Task 10] Category-attributed accounting (D24(a)) -- a
    // Buffer holds its own shared_ptr to the ledger it recorded into so
    // release() on destruction is correct even if the owning Allocator has
    // since moved (the shared_ptr, not the Allocator object's own
    // lifetime, is what the ledger's survival depends on). Null for a
    // default-constructed or moved-from Buffer -- destroyAll() guards on
    // buffer_ != VK_NULL_HANDLE before touching it, same as every other
    // teardown here.
    std::shared_ptr<MemoryAccounting> accounting_;
    MemoryCategory category_ = MemoryCategory::Internal;
    VkDeviceSize allocatedBytes_ = 0;
};

// Owns a VmaAllocator built against a specific VkInstance/VkPhysicalDevice/
// VkDevice triple. Move-only RAII: destroys the underlying VmaAllocator via
// vmaDestroyAllocator on destruction or move-assignment.
//
// Thread-affinity (D5, Phase 4): create()/createRaw()/createHostVisibleBuffer()/
// createDeviceLocalBuffer()/createUploadRingBuffer()/setCurrentFrameIndex()/
// noteAllocationFailure()/noteNonMemoryFailure() are main-thread-only -- see
// docs/threading.md's `rx::rhi::Allocator` entry [Phase 4 Task 10]. Not
// [guarded] (no RX_ASSERT_MAIN_THREAD): unlike BindlessTable/Uploader/
// DeletionQueue, none of Allocator's own state is unsafe to touch from a
// worker in the sense that guard exists for (MemoryAccounting's counters
// are atomic; a chunk >= 1 caller could not corrupt them) -- the
// main-thread-only convention here matches every other GPU-object-mutation
// entry point's existing scoping, not a new hazard this type introduces.
// report()/accounting()/lastAllocationFailureKind()/
// lastAllocationFailureReport()/raw() are read-only snapshots, safe to call
// from any thread that already has a valid reference to this Allocator.
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
    // `VK_EXT_memory_budget` enablement is NOT a parameter here: it is
    // pulled from `device.memoryBudgetExtensionEnabled()` automatically
    // (device.h), which is itself set opportunistically at Device::create()
    // time -- the two must stay in lockstep (see createRaw()'s own comment
    // for exactly what enforces that and what does not), and this overload
    // is what keeps them so for every ordinary caller.
    // `forcedHeapSizeLimitBytes` is a TEST-ONLY escape hatch (default
    // VK_WHOLE_SIZE = no limit): forwarded verbatim to createRaw() -- see
    // that function's own comment for what it does and why [Phase 4 Task
    // 10, D24(d) OOM-path testing].
    // [Phase 4 Stage 1 Task 12, spec D26.4] `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`
    // enablement is likewise NOT a parameter here: it is pulled from
    // `device.supportsBufferDeviceAddress()` automatically (device.h),
    // itself set opportunistically at Device::create() time -- the two
    // must stay in LOCKSTEP, exactly like the memory-budget flag above
    // (same rationale, same enforcement: this overload always reads
    // Device's own accessor directly rather than trusting a caller-
    // supplied value).
    static std::optional<Allocator> create(Context& context, Device& device,
                                            VkDeviceSize forcedHeapSizeLimitBytes = VK_WHOLE_SIZE);

    // Builds a VmaAllocator directly from raw handles, with no dependency
    // on rx::rhi::Context/Device -- e.g. for tooling or tests that want an
    // allocator without constructing either.
    //
    // `memoryBudgetExtensionEnabled` [Phase 4 Task 10, spec D24(b), gate
    // ruling #27]: pass true ONLY if `VK_EXT_memory_budget` was actually
    // enabled on `device` at logical-device-creation time (Device::create()
    // does this opportunistically via `enable_extension_if_present()` --
    // see device.cpp). This flag drives whether
    // `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` is set below.
    //
    // CORRECTION (fix round 1, reviewer M1) -- an earlier version of this
    // comment claimed VMA itself runtime-asserts if this flag is true but
    // the device extension was not really enabled, citing
    // vk_mem_alloc.h:13365-13368. Re-read directly: that `VMA_ASSERT` sits
    // behind `#if !(VMA_MEMORY_BUDGET) || !(VMA_GET_PHYSICAL_DEVICE_PROPERTIES2)`
    // -- a COMPILE-TIME macro gate (whether VMA's own headers detect the
    // extension's symbols at BUILD time), not a runtime check against
    // whether the extension was enabled on `device`. Since this project's
    // vendored Vulkan headers are current, that macro compiles to
    // always-true here, so the assert can never fire regardless of what
    // this parameter is passed. There is NO VMA-side runtime enforcement of
    // this lockstep at all: `Device::memoryBudgetExtensionEnabled()`
    // (device.h) is the SINGLE SOURCE OF TRUTH this engine relies on
    // instead -- `Allocator::create(Context&, Device&)` above always reads
    // it directly rather than trusting a caller-supplied value, which is
    // what actually keeps production callers honest. Passing a mismatched
    // `true` here directly (only reachable via this raw, test/tooling-only
    // overload) would not assert -- it would make VMA chain a
    // `VkPhysicalDeviceMemoryBudgetPropertiesEXT` onto a
    // `vkGetPhysicalDeviceMemoryProperties2` query for an extension struct
    // the device never actually enabled, which is undefined per the Vulkan
    // spec (and a validation-layer VUID hit under sync validation) --
    // reason enough on its own not to pass true here without a real,
    // verified enablement.
    //
    // When true, `report()` below reports MemoryBudgetSource::RealExtension
    // and its per-heap `budgetBytes` comes from the real
    // `vmaGetHeapBudgets()` driver query; when false,
    // MemoryBudgetSource::HeapSizeFallback and `budgetBytes` is this
    // engine's own `VkMemoryHeap::size` estimate instead (matrix-issue27
    // row 2's exact fallback wording -- deliberately NOT VMA's own internal
    // 80%-of-heap-size heuristic, which is an unversioned VMA implementation
    // detail this engine does not want a silent dependency on).
    //
    // `forcedHeapSizeLimitBytes` [Phase 4 Task 10, D24(d) OOM-path testing]:
    // TEST-ONLY. When not VK_WHOLE_SIZE (the default -- no limit), every
    // memory heap on `physicalDevice` is capped to this many bytes via
    // `VmaAllocatorCreateInfo::pHeapSizeLimit` (VMA's own documented,
    // host-side "test how your program behaves with limited memory"
    // mechanism -- vk_mem_alloc.h's `\\page heap_memory_limit` docs) --
    // deterministically forces `VK_ERROR_OUT_OF_DEVICE_MEMORY` on the next
    // allocation that would exceed it, with NO dependency on actually
    // exhausting real GPU memory or on any specific driver's
    // overallocation behavior. This is the "tiny mock/--budget-override
    // allocator ceiling" the plan's own Task 10 steps call for
    // (matrix-issue27 rows 6-9) -- see oom_handling_test.cpp.
    // `bufferDeviceAddressEnabled` [Phase 4 Stage 1 Task 12, spec D26.4]:
    // pass true ONLY if `bufferDeviceAddress` was actually enabled on
    // `device` at logical-device-creation time (Device::create() does
    // this opportunistically via `enable_extension_features_if_present()`
    // -- see device.cpp). Mirrors `memoryBudgetExtensionEnabled` exactly:
    // when true, `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` is set
    // below, which is what makes it legal to later create a buffer with
    // `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` against this allocator
    // (verified directly against vendored VMA 3.4.0 source: creating such
    // a buffer without this flag having been set at allocator-creation
    // time trips a `VMA_ASSERT`/`VK_ERROR_INITIALIZATION_FAILED` --
    // vk_mem_alloc.h:14440-14445 -- both the allocator flag and the
    // buffer usage bit are creation-time-only and immutable for their
    // respective object's lifetime, exactly why this is opportunistic-
    // but-lockstep, never retrofittable later). `rx::asset::GeometryPool`
    // (src/rx_asset) is this phase's sole consumer of the resulting
    // capability.
    static std::optional<Allocator> createRaw(VkPhysicalDevice physicalDevice, VkDevice device, VkInstance instance,
                                               bool memoryBudgetExtensionEnabled = false,
                                               VkDeviceSize forcedHeapSizeLimitBytes = VK_WHOLE_SIZE,
                                               bool bufferDeviceAddressEnabled = false);

    // Allocates a VkBuffer with `usage`, backed by host-visible, persistently
    // mapped memory (VMA_MEMORY_USAGE_AUTO +
    // VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
    // VMA_ALLOCATION_CREATE_MAPPED_BIT) -- suitable for CPU-written,
    // GPU-read data such as vertex/index/uniform buffers updated every
    // frame. `category` [Phase 4 Task 10, D24(a)] attributes this
    // allocation's bytes in the accounting ledger `report()` reads --
    // defaults to MemoryCategory::Internal for callers that don't yet
    // categorize themselves (this default is deliberately generic, not a
    // guess: a caller that legitimately knows its role, e.g. Uploader's
    // staging ring buffer below, passes its real category explicitly).
    // Returns std::nullopt on any failure (logged via RX_LOG_ERROR); on
    // VK_ERROR_OUT_OF_DEVICE_MEMORY specifically, the failure is
    // additionally classified and report-attached -- see
    // lastAllocationFailureKind()/lastAllocationFailureReport() below
    // [D24(d)].
    std::optional<Buffer> createHostVisibleBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                   MemoryCategory category = MemoryCategory::Internal);

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
    // before this task's Critical review fix. `category` [Phase 4 Task 10,
    // D24(a)] -- see createHostVisibleBuffer()'s own comment above; same
    // default and same rationale. Returns std::nullopt on any failure
    // (logged via RX_LOG_ERROR); OOM-classified/report-attached on
    // VK_ERROR_OUT_OF_DEVICE_MEMORY specifically [D24(d)].
    std::optional<Buffer> createDeviceLocalBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                   MemoryCategory category = MemoryCategory::Internal);

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
    // *destination* (createDeviceLocalBuffer() above), not here. `category`
    // [Phase 4 Task 10, D24(a)] defaults to MemoryCategory::Staging -- this
    // buffer IS the staging ring, by construction, so unlike the two
    // factories above there is no honest "uncategorized" default here;
    // Uploader::create() (upload.cpp) relies on this default rather than
    // passing it explicitly. Returns std::nullopt on any failure (logged);
    // OOM-classified/report-attached on VK_ERROR_OUT_OF_DEVICE_MEMORY
    // specifically [D24(d)].
    std::optional<Buffer> createUploadRingBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                  MemoryCategory category = MemoryCategory::Staging);

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

    // Shared accounting ledger this Allocator's own create*Buffer()
    // methods record into -- exposed so rx::rhi::Texture2D::create()
    // (texture.h/.cpp, a separate TU that already takes `Allocator&`) can
    // record()/release() into the SAME ledger `report()` below reads,
    // without texture.cpp needing to befriend or duplicate any of this
    // class's internals. Not intended for ordinary callers.
    const std::shared_ptr<MemoryAccounting>& accounting() const { return accounting_; }

    // [Phase 4 Task 10, spec D24(c)] Snapshots the current category ledger
    // (accounting()) plus a fresh vmaGetHeapBudgets() query into one
    // RxMemoryReport, and pushes each field to a named Tracy plot as a side
    // effect (D24(c): "feeding HUD + Tracy plots") -- see buffer.cpp for
    // the exact per-heap budgetBytes computation (RealExtension vs
    // HeapSizeFallback). Cheap enough to call every frame (vmaGetHeapBudgets
    // itself is documented O(1) -- vk_mem_alloc.h:18103-18106) but ONLY
    // fresh if setCurrentFrameIndex() below has been kept current -- see
    // that method's own comment for why.
    RxMemoryReport report() const;

    // [Phase 4 Task 10, gate ruling #27, matrix-issue27 row 3] Thin wrapper
    // over vmaSetCurrentFrameIndex(). MUST be called once per rendered
    // frame for report()'s per-heap usage/budget numbers (when
    // MemoryBudgetSource::RealExtension) to stay fresh: verified directly
    // against the vendored VMA 3.4.0 source that the real
    // VK_EXT_memory_budget driver query only re-fires when this is called,
    // or lazily once 30 allocate/free operations have accumulated since the
    // last fetch (vk_mem_alloc.h:14787) -- a scene that goes many frames
    // without a single VMA alloc/free would otherwise report a stale
    // budget the whole time. FrameSync::advanceFrame() (frame_sync.h/.cpp)
    // is the wired, once-per-frame call site: pass this Allocator's
    // address to advanceFrame() and it calls this automatically; calling
    // it directly is also fine for a caller that doesn't use FrameSync.
    // No-op cost when MemoryBudgetSource::HeapSizeFallback (VMA's own
    // non-extension budget path recomputes fresh on every
    // vmaGetHeapBudgets() call regardless, per the same source read) --
    // safe to call unconditionally either way.
    void setCurrentFrameIndex(uint32_t frameIndex);

    // [Phase 4 Task 10, fix round 1, reviewer C1] Test/diagnostic accessor
    // -- production frame-loop code has no need for it, same carve-out
    // convention as DeletionQueue::pendingCount() (deletion_queue.h).
    // Cumulative count of real setCurrentFrameIndex() calls this Allocator
    // has ever received, incremented unconditionally at the top of that
    // method (buffer.cpp) regardless of MemoryBudgetSource. This is the
    // DISCRIMINATING signal memory_budget_test.cpp's staleness-wiring test
    // asserts on: unlike comparing report()'s own budget/usage numbers
    // before and after a refresh (which can pass vacuously on a quiet
    // driver where the real value never moves either way -- see that
    // test's own comment), this counter proves the call fired at all,
    // completely independent of whether the underlying VK_EXT_memory_budget
    // query happened to return a different number.
    uint64_t setCurrentFrameIndexCallCount() const { return setCurrentFrameIndexCallCount_; }

    // [Phase 4 Task 10, spec D24(d)] What kind of failure the MOST RECENT
    // createHostVisibleBuffer()/createDeviceLocalBuffer()/
    // createUploadRingBuffer() call ended in (AllocationFailureKind::None
    // if it succeeded, or hasn't been called yet). `lastAllocationFailureReport()`
    // is the RxMemoryReport snapshot captured at the exact moment of that
    // failure -- the "report attached" half of "loud, named failure with
    // the report attached" (both are also inlined into the RX_LOG_ERROR
    // line itself via summarizeMemoryReport(), memory_report.h). Texture2D::
    // create()'s vmaCreateImage failure path (texture.cpp) updates these
    // same two via noteAllocationFailure() below; its DIFFERENT
    // vkCreateImageView failure class deliberately does NOT (see
    // noteNonMemoryFailure() below) -- matrix-issue27 row 9's "these two
    // failure paths must be distinguishable" requirement.
    AllocationFailureKind lastAllocationFailureKind() const { return lastFailureKind_; }
    const RxMemoryReport& lastAllocationFailureReport() const { return lastFailureReport_; }

    // Classifies `result`, snapshots report() into lastAllocationFailureReport(),
    // and logs one RX_LOG_ERROR naming `siteName`/the classified kind/
    // `category`/the report summary -- the single formatting implementation
    // every one of createHostVisibleBuffer()/createDeviceLocalBuffer()/
    // createUploadRingBuffer()/Texture2D::create()'s vmaCreateImage branch
    // calls on failure, so the "loud, named, report-attached" log shape
    // (D24(d)) can never drift between call sites. Public so texture.cpp
    // (which only holds an `Allocator&`, not friendship) can call it too.
    void noteAllocationFailure(const char* siteName, MemoryCategory category, VkResult result);

    // [Phase 4 Task 10, matrix-issue27 row 9] For a failure class that is
    // NOT a memory-budget event -- today, only Texture2D::create()'s
    // vkCreateImageView failure (a device/API-misuse-class error, distinct
    // from the vmaCreateImage failure two lines earlier in that same
    // function, which DOES go through noteAllocationFailure() above).
    // Logs a clearly-labeled RX_LOG_ERROR naming `siteName`/`result` but
    // deliberately does NOT touch lastAllocationFailureKind()/
    // lastAllocationFailureReport() or the accounting ledger -- exactly
    // what keeps the two failure classes distinguishable to a caller
    // inspecting Allocator state after either.
    void noteNonMemoryFailure(const char* siteName, VkResult result);

private:
    Allocator() = default;
    explicit Allocator(VmaAllocator allocator, VkPhysicalDevice physicalDevice, MemoryBudgetSource budgetSource)
        : allocator_(allocator),
          physicalDevice_(physicalDevice),
          budgetSource_(budgetSource),
          accounting_(std::make_shared<MemoryAccounting>()) {}

    void destroyAll();

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    MemoryBudgetSource budgetSource_ = MemoryBudgetSource::HeapSizeFallback;
    std::shared_ptr<MemoryAccounting> accounting_;
    AllocationFailureKind lastFailureKind_ = AllocationFailureKind::None;
    RxMemoryReport lastFailureReport_;
    uint64_t setCurrentFrameIndexCallCount_ = 0;
};

}  // namespace rx::rhi
