#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <cstdint>
#include <utility>
#include <vector>

namespace rx::rhi {

namespace {

// [Phase 4 Task 10, spec D24(c), matrix-issue27 row 17] RX_PLOT() (rx_core/
// profile.h) requires a compile-time string literal per plot -- there is no
// runtime-named-plot variant -- so the five fixed categories get one named
// plot each unconditionally, and heaps get a bounded, hand-unrolled set of
// literal names (0..7; RADV APUs expose a handful, per matrix-issue27 row
// 11 -- 8 is generous headroom over that, VK_MAX_MEMORY_HEAPS's real
// ceiling of 16 is not worth hand-unrolling further for hardware this
// project has never observed). A heap index beyond 7 is silently not
// plotted (Tracy visualization is a nice-to-have, not correctness-bearing
// -- report() itself is unaffected and still returns every heap). Compiles
// to nothing at all when TRACY_ENABLE is off (RX_PLOT's own contract).
void publishTracyPlots(const RxMemoryReport& report) {
    RX_PLOT("Memory/Category/GeometryPool", static_cast<int64_t>(report.category(MemoryCategory::GeometryPool).bytes));
    RX_PLOT("Memory/Category/Texture", static_cast<int64_t>(report.category(MemoryCategory::Texture).bytes));
    RX_PLOT("Memory/Category/Transient", static_cast<int64_t>(report.category(MemoryCategory::Transient).bytes));
    RX_PLOT("Memory/Category/Staging", static_cast<int64_t>(report.category(MemoryCategory::Staging).bytes));
    RX_PLOT("Memory/Category/Internal", static_cast<int64_t>(report.category(MemoryCategory::Internal).bytes));

    for (size_t i = 0; i < report.heaps.size() && i < 8; ++i) {
        const MemoryHeapReport& heap = report.heaps[i];
        switch (i) {
            case 0:
                RX_PLOT("Memory/Heap0/UsageBytes", static_cast<int64_t>(heap.usageBytes));
                RX_PLOT("Memory/Heap0/BudgetBytes", static_cast<int64_t>(heap.budgetBytes));
                break;
            case 1:
                RX_PLOT("Memory/Heap1/UsageBytes", static_cast<int64_t>(heap.usageBytes));
                RX_PLOT("Memory/Heap1/BudgetBytes", static_cast<int64_t>(heap.budgetBytes));
                break;
            case 2:
                RX_PLOT("Memory/Heap2/UsageBytes", static_cast<int64_t>(heap.usageBytes));
                RX_PLOT("Memory/Heap2/BudgetBytes", static_cast<int64_t>(heap.budgetBytes));
                break;
            case 3:
                RX_PLOT("Memory/Heap3/UsageBytes", static_cast<int64_t>(heap.usageBytes));
                RX_PLOT("Memory/Heap3/BudgetBytes", static_cast<int64_t>(heap.budgetBytes));
                break;
            case 4:
                RX_PLOT("Memory/Heap4/UsageBytes", static_cast<int64_t>(heap.usageBytes));
                RX_PLOT("Memory/Heap4/BudgetBytes", static_cast<int64_t>(heap.budgetBytes));
                break;
            case 5:
                RX_PLOT("Memory/Heap5/UsageBytes", static_cast<int64_t>(heap.usageBytes));
                RX_PLOT("Memory/Heap5/BudgetBytes", static_cast<int64_t>(heap.budgetBytes));
                break;
            case 6:
                RX_PLOT("Memory/Heap6/UsageBytes", static_cast<int64_t>(heap.usageBytes));
                RX_PLOT("Memory/Heap6/BudgetBytes", static_cast<int64_t>(heap.budgetBytes));
                break;
            case 7:
                RX_PLOT("Memory/Heap7/UsageBytes", static_cast<int64_t>(heap.usageBytes));
                RX_PLOT("Memory/Heap7/BudgetBytes", static_cast<int64_t>(heap.budgetBytes));
                break;
            default:
                break;
        }
    }
}

}  // namespace

std::optional<Allocator> Allocator::create(Context& context, Device& device, VkDeviceSize forcedHeapSizeLimitBytes) {
    return createRaw(device.physicalDevice(), device.device(), context.instance(),
                      device.memoryBudgetExtensionEnabled(), forcedHeapSizeLimitBytes,
                      device.supportsBufferDeviceAddress());
}

std::optional<Allocator> Allocator::createRaw(VkPhysicalDevice physicalDevice, VkDevice device, VkInstance instance,
                                               bool memoryBudgetExtensionEnabled,
                                               VkDeviceSize forcedHeapSizeLimitBytes,
                                               bool bufferDeviceAddressEnabled) {
    // Only the two entry points VMA's dynamic-loading path actually
    // requires (see buffer.h's comment above the VMA_STATIC_VULKAN_FUNCTIONS
    // / VMA_DYNAMIC_VULKAN_FUNCTIONS defines for why nothing else is
    // hand-filled here). volk exposes both as real global function pointers
    // once volkInitialize()/volkLoadInstance() (Context::create) have run --
    // volkLoadInstance() is what populates vkGetDeviceProcAddr too (verified
    // against volk.c:253; see buffer.h's comment for the exact call chain).
    // Device::create()'s volkLoadDevice() call populates a separate
    // per-device function table and is not what this global pointer relies
    // on.
    VmaVulkanFunctions vulkanFunctions{};
    vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo createInfo{};
    createInfo.physicalDevice = physicalDevice;
    createInfo.device = device;
    createInfo.instance = instance;
    createInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    createInfo.pVulkanFunctions = &vulkanFunctions;

    // [Phase 4 Task 10, spec D24(b)] LOCKSTEP requirement (see this
    // function's own header comment in buffer.h for the full correction):
    // this flag must agree exactly with whether VK_EXT_memory_budget was
    // actually enabled on `device`. VMA itself does NOT runtime-assert
    // this (its nearby VMA_ASSERT is compile-time-macro-gated, not a
    // device-state check) -- `Device::memoryBudgetExtensionEnabled()` is
    // this engine's own single source of truth instead, which is why
    // Allocator::create(Context&, Device&) always reads it directly rather
    // than trusting a caller-supplied value.
    if (memoryBudgetExtensionEnabled) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }

    // [Phase 4 Stage 1 Task 12, spec D26.4] Same LOCKSTEP requirement as
    // the memory-budget flag above: this must agree exactly with whether
    // `bufferDeviceAddress` was actually enabled on `device` --
    // `Device::supportsBufferDeviceAddress()` is the single source of
    // truth (see this function's own header comment in buffer.h for the
    // exact VMA enforcement this flag gates: a buffer created with
    // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT against an allocator that
    // never set this flag trips a VMA-side assert/failure).
    if (bufferDeviceAddressEnabled) {
        createInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    // [Phase 4 Task 10, D24(d) OOM-path testing] TEST-ONLY heap-size
    // ceiling -- see this function's own header comment in buffer.h.
    // `heapLimits` must outlive the vmaCreateAllocator() call below (VMA
    // reads through the pointer during that call only, but does not copy
    // the array beyond it), so it stays in this function's scope, not a
    // narrower block.
    std::vector<VkDeviceSize> heapLimits;
    if (forcedHeapSizeLimitBytes != VK_WHOLE_SIZE) {
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
        heapLimits.assign(memProps.memoryHeapCount, forcedHeapSizeLimitBytes);
        createInfo.pHeapSizeLimit = heapLimits.data();
    }

    VmaAllocator allocator = VK_NULL_HANDLE;
    VkResult result = vmaCreateAllocator(&createInfo, &allocator);
    if (result != VK_SUCCESS) {
        // [Phase 4 Task 10, matrix-issue27 row 5] Named and distinguishable
        // from "some other init step failed", per this function's own
        // header comment on why this site is NOT exercised via a forced
        // vmaCreateAllocator failure in tests (VMA_ASSERT on a null
        // physicalDevice/device/instance would abort the whole test
        // process, not fail gracefully -- see oom_handling_test.cpp's own
        // comment). classifyAllocationFailure() is still the single source
        // of truth for the kind name here, exercised device-free by that
        // same test file.
        RX_LOG_ERROR("Allocator::createRaw: vmaCreateAllocator failed (site=AllocatorCreation): VkResult={} ({})",
                     static_cast<int>(result), allocationFailureKindName(classifyAllocationFailure(result)));
        return std::nullopt;
    }

    const MemoryBudgetSource budgetSource =
        memoryBudgetExtensionEnabled ? MemoryBudgetSource::RealExtension : MemoryBudgetSource::HeapSizeFallback;
    return Allocator(allocator, physicalDevice, budgetSource);
}

std::optional<Buffer> Allocator::createHostVisibleBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                           MemoryCategory category) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};
    VkResult result =
        vmaCreateBuffer(allocator_, &bufferInfo, &allocCreateInfo, &buffer, &allocation, &allocationInfo);
    if (result != VK_SUCCESS) {
        noteAllocationFailure("Allocator::createHostVisibleBuffer", category, result);
        return std::nullopt;
    }

    accounting_->record(category, allocationInfo.size);
    return Buffer(allocator_, buffer, allocation, allocationInfo.pMappedData, size, /*directPathCapable=*/false,
                  accounting_, category, allocationInfo.size);
}

std::optional<Buffer> Allocator::createDeviceLocalBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                           MemoryCategory category) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // ALLOW_TRANSFER_INSTEAD_BIT here (not on createUploadRingBuffer()'s
    // staging buffer) is this task's Critical review fix -- see this
    // method's own header comment in buffer.h for the exact VMA
    // FindMemoryPreferences()/ContainsDeviceAccess() mechanics this
    // depends on `usage` actually carrying a device-consuming bit for.
    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};
    VkResult result =
        vmaCreateBuffer(allocator_, &bufferInfo, &allocCreateInfo, &buffer, &allocation, &allocationInfo);
    if (result != VK_SUCCESS) {
        noteAllocationFailure("Allocator::createDeviceLocalBuffer", category, result);
        return std::nullopt;
    }

    VkMemoryPropertyFlags memoryFlags = 0;
    vmaGetMemoryTypeProperties(allocator_, allocationInfo.memoryType, &memoryFlags);
    bool directPathCapable = (memoryFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                              (memoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

    accounting_->record(category, allocationInfo.size);
    return Buffer(allocator_, buffer, allocation, allocationInfo.pMappedData, size, directPathCapable, accounting_,
                  category, allocationInfo.size);
}

std::optional<Buffer> Allocator::createUploadRingBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                          MemoryCategory category) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // Deliberately NO ALLOW_TRANSFER_INSTEAD_BIT -- see this method's own
    // header comment in buffer.h: for a TRANSFER_SRC-only usage (this
    // buffer's real-world use, per rx::rhi::Uploader), that flag is
    // structurally inert (verified directly against vendored VMA 3.4.0
    // source) and an earlier version of this method carried it as dead,
    // misleading weight -- this task's own Critical review finding.
    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocationInfo{};
    VkResult result =
        vmaCreateBuffer(allocator_, &bufferInfo, &allocCreateInfo, &buffer, &allocation, &allocationInfo);
    if (result != VK_SUCCESS) {
        noteAllocationFailure("Allocator::createUploadRingBuffer", category, result);
        return std::nullopt;
    }

    // directPathCapable left at its default (false) -- this Buffer is
    // never a direct-upload destination candidate, by construction.
    accounting_->record(category, allocationInfo.size);
    return Buffer(allocator_, buffer, allocation, allocationInfo.pMappedData, size, /*directPathCapable=*/false,
                  accounting_, category, allocationInfo.size);
}

Allocator::Allocator(Allocator&& other) noexcept : Allocator() {
    *this = std::move(other);
}

Allocator& Allocator::operator=(Allocator&& other) noexcept {
    if (this != &other) {
        destroyAll();

        allocator_ = other.allocator_;
        physicalDevice_ = other.physicalDevice_;
        budgetSource_ = other.budgetSource_;
        accounting_ = std::move(other.accounting_);
        lastFailureKind_ = other.lastFailureKind_;
        lastFailureReport_ = other.lastFailureReport_;
        setCurrentFrameIndexCallCount_ = other.setCurrentFrameIndexCallCount_;

        other.allocator_ = VK_NULL_HANDLE;
        other.physicalDevice_ = VK_NULL_HANDLE;
        other.budgetSource_ = MemoryBudgetSource::HeapSizeFallback;
        other.lastFailureKind_ = AllocationFailureKind::None;
        other.lastFailureReport_ = RxMemoryReport{};
        other.setCurrentFrameIndexCallCount_ = 0;
    }
    return *this;
}

Allocator::~Allocator() {
    destroyAll();
}

void Allocator::destroyAll() {
    if (allocator_ != VK_NULL_HANDLE) {
        // [Phase 4 Task 10, spec D24, matrix-issue27 row 16] Teardown
        // leak/balance check -- mirrors DeletionQueue's own "destroyed with
        // N never flushed" destructor warning (deletion_queue.cpp), the
        // same established pattern in this codebase, not a new one. By the
        // time this Allocator is torn down, every Buffer/Texture2D it
        // created must ALREADY have been destroyed (the documented
        // lifetime contract: neither may outlive the Allocator that
        // created them) -- if the ledger is still nonzero here, that
        // contract was violated somewhere, and this is the loud signal
        // rather than a silent leak.
        if (accounting_ && accounting_->hasOutstanding()) {
            RxMemoryReport leaked = report();
            RX_LOG_ERROR(
                "Allocator::destroyAll: torn down with outstanding category-attributed allocations still recorded "
                "(a Buffer/Texture2D created from this Allocator was never destroyed before it) -- {}",
                summarizeMemoryReport(leaked));
        }
        vmaDestroyAllocator(allocator_);
    }
}

Buffer::Buffer(Buffer&& other) noexcept : Buffer() {
    *this = std::move(other);
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroyAll();

        allocator_ = other.allocator_;
        buffer_ = other.buffer_;
        allocation_ = other.allocation_;
        mappedData_ = other.mappedData_;
        size_ = other.size_;
        directPathCapable_ = other.directPathCapable_;
        accounting_ = std::move(other.accounting_);
        category_ = other.category_;
        allocatedBytes_ = other.allocatedBytes_;

        other.allocator_ = VK_NULL_HANDLE;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.mappedData_ = nullptr;
        other.size_ = 0;
        other.directPathCapable_ = false;
        other.category_ = MemoryCategory::Internal;
        other.allocatedBytes_ = 0;
    }
    return *this;
}

Buffer::~Buffer() {
    destroyAll();
}

void Buffer::destroyAll() {
    if (buffer_ != VK_NULL_HANDLE) {
        // [Phase 4 Task 10, spec D24(a)] Release BEFORE vmaDestroyBuffer --
        // order is not load-bearing here (the ledger is independent of the
        // real VMA call), but matches "attribute the byte count for exactly
        // as long as the real allocation is live" precisely.
        if (accounting_) {
            accounting_->release(category_, allocatedBytes_);
        }
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
}

void Buffer::flush(VkDeviceSize offset, VkDeviceSize size) const {
    if (buffer_ == VK_NULL_HANDLE) {
        return;
    }
    VkResult result = vmaFlushAllocation(allocator_, allocation_, offset, size);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("vmaFlushAllocation failed: VkResult={}", static_cast<int>(result));
    }
}

void Buffer::invalidate(VkDeviceSize offset, VkDeviceSize size) const {
    if (buffer_ == VK_NULL_HANDLE) {
        return;
    }
    VkResult result = vmaInvalidateAllocation(allocator_, allocation_, offset, size);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("vmaInvalidateAllocation failed: VkResult={}", static_cast<int>(result));
    }
}

RxMemoryReport Allocator::report() const {
    RxMemoryReport out;
    for (size_t i = 0; i < kMemoryCategoryCount; ++i) {
        out.categories[i] = accounting_->snapshot(static_cast<MemoryCategory>(i));
    }
    out.budgetSource = budgetSource_;

    const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
    vmaGetMemoryProperties(allocator_, &memProps);
    uint32_t heapCount = memProps != nullptr ? memProps->memoryHeapCount : 0;

    std::vector<VmaBudget> budgets(heapCount);
    if (heapCount > 0) {
        vmaGetHeapBudgets(allocator_, budgets.data());
    }

    out.heaps.resize(heapCount);
    for (uint32_t i = 0; i < heapCount; ++i) {
        out.heaps[i].heapIndex = i;
        out.heaps[i].heapSizeBytes = memProps->memoryHeaps[i].size;
        out.heaps[i].usageBytes = budgets[i].usage;
        // [Phase 4 Task 10, matrix-issue27 row 2] budgetSource_ ==
        // HeapSizeFallback: use the raw VkMemoryHeap::size directly, NOT
        // VMA's own internal non-extension fallback (`budgets[i].budget`
        // would still be populated -- verified against the vendored VMA
        // 3.4.0 source that its non-extension branch sets
        // `budget = heapSize * 8 / 10`, an unversioned 80%-of-heap-size
        // implementation detail this engine deliberately does not want a
        // silent dependency on). budgetSource_ == RealExtension: use VMA's
        // own driver-reported `budgets[i].budget` (already MIN'd against
        // heap size internally, per the same source read).
        out.heaps[i].budgetBytes =
            budgetSource_ == MemoryBudgetSource::RealExtension ? budgets[i].budget : memProps->memoryHeaps[i].size;
    }

    publishTracyPlots(out);
    return out;
}

void Allocator::setCurrentFrameIndex(uint32_t frameIndex) {
    // [Phase 4 Task 10, fix round 1, reviewer C1] Incremented BEFORE the
    // real VMA call, unconditionally -- this is the test-only discriminator
    // setCurrentFrameIndexCallCount() (buffer.h) exposes; see that
    // accessor's own comment for why report()'s own budget/usage numbers
    // are not a reliable enough signal on their own.
    ++setCurrentFrameIndexCallCount_;
    vmaSetCurrentFrameIndex(allocator_, frameIndex);
}

void Allocator::noteAllocationFailure(const char* siteName, MemoryCategory category, VkResult result) {
    lastFailureKind_ = classifyAllocationFailure(result);
    lastFailureReport_ = report();
    RX_LOG_ERROR("{} failed: VkResult={} ({}) category={} -- memory report at failure: {}", siteName,
                 static_cast<int>(result), allocationFailureKindName(lastFailureKind_), memoryCategoryName(category),
                 summarizeMemoryReport(lastFailureReport_));
}

void Allocator::noteNonMemoryFailure(const char* siteName, VkResult result) {
    // Deliberately does NOT touch lastFailureKind_/lastFailureReport_/the
    // accounting ledger -- see this method's own header comment in
    // buffer.h: this is what keeps a vmaCreateImage-class failure and a
    // vkCreateImageView-class failure distinguishable to a caller
    // inspecting Allocator state afterward (matrix-issue27 row 9).
    RX_LOG_ERROR("{} failed: VkResult={} -- NOT a memory-budget event (device/API-misuse failure class); memory "
                 "accounting unaffected",
                 siteName, static_cast<int>(result));
}

}  // namespace rx::rhi
