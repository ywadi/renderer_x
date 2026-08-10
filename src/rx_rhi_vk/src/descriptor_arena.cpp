#include <rx_rhi_vk/descriptor_arena.h>

#include <rx_core/log.h>

#include <utility>

namespace rx::rhi {

DescriptorArena::DescriptorArena(DescriptorArena&& other) noexcept : DescriptorArena() { *this = std::move(other); }

DescriptorArena& DescriptorArena::operator=(DescriptorArena&& other) noexcept {
    if (this != &other) {
        destroyAll();

        device_ = other.device_;
        pools_ = std::move(other.pools_);
        currentFrame_ = other.currentFrame_;

        other.device_ = VK_NULL_HANDLE;
        other.pools_.clear();
        other.currentFrame_ = 0;
    }
    return *this;
}

DescriptorArena::~DescriptorArena() { destroyAll(); }

void DescriptorArena::destroyAll() {
    for (VkDescriptorPool pool : pools_) {
        if (pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, pool, nullptr);
        }
    }
    pools_.clear();
}

std::optional<DescriptorArena> DescriptorArena::create(VkDevice device, uint32_t framesInFlight,
                                                         Capacities capacities) {
    if (framesInFlight == 0) {
        RX_LOG_ERROR("rx::rhi::DescriptorArena::create: framesInFlight must be > 0");
        return std::nullopt;
    }
    if (capacities.maxSets == 0 || capacities.uniformBuffers == 0) {
        RX_LOG_ERROR(
            "rx::rhi::DescriptorArena::create: maxSets/uniformBuffers must be > 0 (got maxSets={}, "
            "uniformBuffers={})",
            capacities.maxSets, capacities.uniformBuffers);
        return std::nullopt;
    }

    std::vector<VkDescriptorPool> pools;
    pools.reserve(framesInFlight);
    for (uint32_t i = 0; i < framesInFlight; ++i) {
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, capacities.uniformBuffers};

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = capacities.maxSets;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;

        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkResult result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool);
        if (result != VK_SUCCESS) {
            RX_LOG_ERROR(
                "rx::rhi::DescriptorArena::create: vkCreateDescriptorPool failed for frame slot {}: VkResult={}", i,
                static_cast<int>(result));
            for (VkDescriptorPool created : pools) {
                vkDestroyDescriptorPool(device, created, nullptr);
            }
            return std::nullopt;
        }
        pools.push_back(pool);
    }

    DescriptorArena arena;
    arena.device_ = device;
    arena.pools_ = std::move(pools);
    return arena;
}

void DescriptorArena::beginFrame(uint32_t frameIndex) {
    currentFrame_ = frameIndex % static_cast<uint32_t>(pools_.size());
    VkResult result = vkResetDescriptorPool(device_, pools_[currentFrame_], 0);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("rx::rhi::DescriptorArena::beginFrame: vkResetDescriptorPool failed for slot {}: VkResult={}",
                     currentFrame_, static_cast<int>(result));
    }
}

VkDescriptorSet DescriptorArena::allocate(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pools_[currentFrame_];
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(device_, &allocInfo, &set);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("rx::rhi::DescriptorArena::allocate: vkAllocateDescriptorSets failed for frame slot {}: "
                     "VkResult={}",
                     currentFrame_, static_cast<int>(result));
        return VK_NULL_HANDLE;
    }
    return set;
}

}  // namespace rx::rhi
