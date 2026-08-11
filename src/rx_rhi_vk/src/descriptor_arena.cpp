#include <rx_rhi_vk/descriptor_arena.h>

#include <rx_core/log.h>
#include <rx_core/profile.h>

#include <utility>

namespace rx::rhi {

DescriptorArena::DescriptorArena(DescriptorArena&& other) noexcept : DescriptorArena() { *this = std::move(other); }

DescriptorArena& DescriptorArena::operator=(DescriptorArena&& other) noexcept {
    if (this != &other) {
        destroyAll();

        device_ = other.device_;
        pools_ = std::move(other.pools_);
        capacities_ = other.capacities_;
        allocatedSets_ = std::move(other.allocatedSets_);
        allocatedUniformBuffers_ = std::move(other.allocatedUniformBuffers_);
        currentFrame_ = other.currentFrame_;

        other.device_ = VK_NULL_HANDLE;
        other.pools_.clear();
        other.allocatedSets_.clear();
        other.allocatedUniformBuffers_.clear();
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
    arena.capacities_ = capacities;
    arena.allocatedSets_.assign(framesInFlight, 0);
    arena.allocatedUniformBuffers_.assign(framesInFlight, 0);
    return arena;
}

void DescriptorArena::beginFrame(uint32_t frameIndex) {
    RX_ZONE;
    currentFrame_ = frameIndex % static_cast<uint32_t>(pools_.size());
    VkResult result = vkResetDescriptorPool(device_, pools_[currentFrame_], 0);
    if (result != VK_SUCCESS) {
        RX_LOG_ERROR("rx::rhi::DescriptorArena::beginFrame: vkResetDescriptorPool failed for slot {}: VkResult={}",
                     currentFrame_, static_cast<int>(result));
    }
    // Reset this slot's own arena-enforced budget tracking regardless of
    // the vkResetDescriptorPool result above -- a failed reset is already
    // logged as an error by this class (nothing more this method can do
    // about it), and every VkDescriptorSet the pool held is conceptually
    // gone either way once beginFrame() has been called for this slot.
    allocatedSets_[currentFrame_] = 0;
    allocatedUniformBuffers_[currentFrame_] = 0;
}

VkDescriptorSet DescriptorArena::allocate(VkDescriptorSetLayout layout, uint32_t uniformBufferDescriptorCount) {
    // Arena-enforced budget check FIRST -- see descriptor_arena.h's
    // class-level BUDGETS ARE ARENA-ENFORCED comment for why this cannot be
    // left to the driver: vkAllocateDescriptorSets is never even called
    // once either ceiling would be exceeded, so "documented limit ->
    // VK_NULL_HANDLE" holds deterministically on every driver, including
    // ones (lavapipe/Mesa) that legally never detect real pool exhaustion
    // themselves.
    if (allocatedSets_[currentFrame_] + 1 > capacities_.maxSets) {
        RX_LOG_ERROR(
            "rx::rhi::DescriptorArena::allocate: arena-enforced maxSets budget exhausted for frame slot {} ({} of "
            "{} sets already allocated this reset cycle)",
            currentFrame_, allocatedSets_[currentFrame_], capacities_.maxSets);
        return VK_NULL_HANDLE;
    }
    if (allocatedUniformBuffers_[currentFrame_] + uniformBufferDescriptorCount > capacities_.uniformBuffers) {
        RX_LOG_ERROR(
            "rx::rhi::DescriptorArena::allocate: arena-enforced uniformBuffers budget exhausted for frame slot {} "
            "({} of {} UBO descriptors already allocated this reset cycle, {} more requested)",
            currentFrame_, allocatedUniformBuffers_[currentFrame_], capacities_.uniformBuffers,
            uniformBufferDescriptorCount);
        return VK_NULL_HANDLE;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pools_[currentFrame_];
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(device_, &allocInfo, &set);
    if (result != VK_SUCCESS) {
        // Genuine driver-level failure fallback path (e.g. real pool
        // fragmentation, VK_ERROR_FRAGMENTED_POOL) -- this arena's own
        // budget check above found room, but the driver still could not
        // satisfy the request. Logged distinctly from the arena-enforced
        // rejections above so the two cases are never confused when
        // reading logs.
        RX_LOG_ERROR("rx::rhi::DescriptorArena::allocate: vkAllocateDescriptorSets failed for frame slot {}: "
                     "VkResult={}",
                     currentFrame_, static_cast<int>(result));
        return VK_NULL_HANDLE;
    }

    allocatedSets_[currentFrame_] += 1;
    allocatedUniformBuffers_[currentFrame_] += uniformBufferDescriptorCount;
    return set;
}

}  // namespace rx::rhi
