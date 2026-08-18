#include <rx_rhi_vk/memory_report.h>
#include <cstdio>

namespace rx::rhi {

const char* memoryCategoryName(MemoryCategory category) {
    switch (category) {
        case MemoryCategory::GeometryPool:
            return "GeometryPool";
        case MemoryCategory::Texture:
            return "Texture";
        case MemoryCategory::Transient:
            return "Transient";
        case MemoryCategory::Staging:
            return "Staging";
        case MemoryCategory::Internal:
            return "Internal";
        default:
            return "UNKNOWN";
    }
}

const char* memoryBudgetSourceName(MemoryBudgetSource source) {
    switch (source) {
        case MemoryBudgetSource::RealExtension:
            return "RealExtension";
        case MemoryBudgetSource::HeapSizeFallback:
            return "HeapSizeFallback";
        default:
            return "UNKNOWN";
    }
}

const char* allocationFailureKindName(AllocationFailureKind kind) {
    switch (kind) {
        case AllocationFailureKind::None:
            return "None";
        case AllocationFailureKind::OutOfDeviceMemory:
            return "OutOfDeviceMemory";
        case AllocationFailureKind::OutOfHostMemory:
            return "OutOfHostMemory";
        case AllocationFailureKind::Other:
            return "Other";
        default:
            return "UNKNOWN";
    }
}

AllocationFailureKind classifyAllocationFailure(VkResult result) {
    switch (result) {
        case VK_SUCCESS:
            return AllocationFailureKind::None;
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return AllocationFailureKind::OutOfDeviceMemory;
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return AllocationFailureKind::OutOfHostMemory;
        default:
            return AllocationFailureKind::Other;
    }
}

void MemoryAccounting::record(MemoryCategory category, uint64_t bytes) {
    Counter& counter = counters_[static_cast<size_t>(category)];
    counter.bytes.fetch_add(bytes, std::memory_order_relaxed);
    counter.count.fetch_add(1, std::memory_order_relaxed);
}

void MemoryAccounting::release(MemoryCategory category, uint64_t bytes) {
    Counter& counter = counters_[static_cast<size_t>(category)];
    counter.bytes.fetch_sub(bytes, std::memory_order_relaxed);
    counter.count.fetch_sub(1, std::memory_order_relaxed);
}

MemoryCategoryStats MemoryAccounting::snapshot(MemoryCategory category) const {
    const Counter& counter = counters_[static_cast<size_t>(category)];
    return MemoryCategoryStats{counter.bytes.load(std::memory_order_relaxed),
                                counter.count.load(std::memory_order_relaxed)};
}

bool MemoryAccounting::hasOutstanding() const {
    for (const Counter& counter : counters_) {
        if (counter.count.load(std::memory_order_relaxed) != 0) {
            return true;
        }
    }
    return false;
}

std::string summarizeMemoryReport(const RxMemoryReport& report) {
    std::string out = "categories={";
    for (size_t i = 0; i < kMemoryCategoryCount; ++i) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s%s=%llu/%u", i == 0 ? "" : ",",
                      memoryCategoryName(static_cast<MemoryCategory>(i)),
                      static_cast<unsigned long long>(report.categories[i].bytes), report.categories[i].count);
        out += buf;
    }
    out += "} heaps={";
    for (size_t i = 0; i < report.heaps.size(); ++i) {
        const MemoryHeapReport& heap = report.heaps[i];
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s#%u:usage=%llu/budget=%llu(of %llu)", i == 0 ? "" : ",", heap.heapIndex,
                      static_cast<unsigned long long>(heap.usageBytes), static_cast<unsigned long long>(heap.budgetBytes),
                      static_cast<unsigned long long>(heap.heapSizeBytes));
        out += buf;
    }
    out += "} budgetSource=";
    out += memoryBudgetSourceName(report.budgetSource);
    return out;
}

}  // namespace rx::rhi
