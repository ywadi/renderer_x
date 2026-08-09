#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_core/log.h>
#include <utility>

namespace rx::rhi {

std::optional<Allocator> Allocator::create(Context& context, Device& device) {
    return createRaw(device.physicalDevice(), device.device(), context.instance());
}

std::optional<Allocator> Allocator::createRaw(VkPhysicalDevice physicalDevice, VkDevice device, VkInstance instance) {
    // Only the two entry points VMA's dynamic-loading path actually
    // requires (see buffer.h's comment above the VMA_STATIC_VULKAN_FUNCTIONS
    // / VMA_DYNAMIC_VULKAN_FUNCTIONS defines for why nothing else is
    // hand-filled here). volk exposes both as real global function pointers
    // once volkInitialize()/volkLoadInstance() (Context::create) and
    // volkLoadDevice() (Device::create) have run.
    VmaVulkanFunctions vulkanFunctions{};
    vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo createInfo{};
    createInfo.physicalDevice = physicalDevice;
    createInfo.device = device;
    createInfo.instance = instance;
    createInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    createInfo.pVulkanFunctions = &vulkanFunctions;

    VmaAllocator allocator = VK_NULL_HANDLE;
    VkResult result = vmaCreateAllocator(&createInfo, &allocator);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("vmaCreateAllocator failed: VkResult={}", static_cast<int>(result));
        return std::nullopt;
    }

    return Allocator(allocator);
}

std::optional<Buffer> Allocator::createHostVisibleBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
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
        RX_LOG_ERROR("vmaCreateBuffer failed: VkResult={}", static_cast<int>(result));
        return std::nullopt;
    }

    return Buffer(allocator_, buffer, allocation, allocationInfo.pMappedData, size);
}

Allocator::Allocator(Allocator&& other) noexcept : Allocator() {
    *this = std::move(other);
}

Allocator& Allocator::operator=(Allocator&& other) noexcept {
    if (this != &other) {
        destroyAll();

        allocator_ = other.allocator_;
        other.allocator_ = VK_NULL_HANDLE;
    }
    return *this;
}

Allocator::~Allocator() {
    destroyAll();
}

void Allocator::destroyAll() {
    if (allocator_ != VK_NULL_HANDLE) {
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

        other.allocator_ = VK_NULL_HANDLE;
        other.buffer_ = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.mappedData_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

Buffer::~Buffer() {
    destroyAll();
}

void Buffer::destroyAll() {
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
}

}  // namespace rx::rhi
