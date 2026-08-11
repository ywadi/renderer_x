#include <rx_material/instance.h>

#include <rx_core/log.h>
#include <rx_core/profile.h>

#include <cstring>
#include <utility>

namespace rx::material {

std::optional<ParamArena> ParamArena::create(VkDevice device, rx::rhi::Allocator& allocator,
                                              uint32_t framesInFlight) {
    if (framesInFlight == 0) {
        RX_LOG_ERROR("rx::material::ParamArena::create: framesInFlight must be > 0");
        return std::nullopt;
    }

    rx::rhi::DescriptorArenaCapacities capacities{/*maxSets=*/kMaxInstancesPerFrame,
                                                   /*uniformBuffers=*/kMaxInstancesPerFrame};
    auto descriptorArena = rx::rhi::DescriptorArena::create(device, framesInFlight, capacities);
    if (!descriptorArena.has_value()) {
        return std::nullopt;
    }

    std::vector<rx::rhi::Buffer> buffers;
    buffers.reserve(framesInFlight);
    for (uint32_t i = 0; i < framesInFlight; ++i) {
        auto buffer = allocator.createHostVisibleBuffer(kBytesPerFrame, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        if (!buffer.has_value()) {
            RX_LOG_ERROR("rx::material::ParamArena::create: createHostVisibleBuffer failed for frame slot {}", i);
            return std::nullopt;
        }
        buffers.push_back(std::move(*buffer));
    }

    ParamArena arena;
    arena.device_ = device;
    arena.descriptorArena_ = std::move(*descriptorArena);
    arena.buffers_ = std::move(buffers);
    arena.cursors_.assign(framesInFlight, 0);
    return arena;
}

void ParamArena::beginFrame(uint32_t frameIndex) {
    RX_ZONE;
    currentFrame_ = frameIndex % static_cast<uint32_t>(buffers_.size());
    cursors_[currentFrame_] = 0;
    descriptorArena_->beginFrame(frameIndex);
}

VkDescriptorSet ParamArena::writeAndAllocate(VkDescriptorSetLayout setLayout, const void* data, size_t size) {
    if (data == nullptr || size == 0) {
        RX_LOG_ERROR("rx::material::ParamArena::writeAndAllocate: data/size must be non-null/non-zero");
        return VK_NULL_HANDLE;
    }

    rx::rhi::Buffer& buffer = buffers_[currentFrame_];
    VkDeviceSize& cursor = cursors_[currentFrame_];

    VkDeviceSize alignedOffset = (cursor + (kUniformBufferAlignment - 1)) & ~(kUniformBufferAlignment - 1);
    VkDeviceSize end = alignedOffset + static_cast<VkDeviceSize>(size);
    if (end > buffer.size()) {
        RX_LOG_ERROR(
            "rx::material::ParamArena::writeAndAllocate: frame slot {} exhausted ({} of {} bytes already used, {} "
            "more requested)",
            currentFrame_, alignedOffset, buffer.size(), size);
        return VK_NULL_HANDLE;
    }

    std::memcpy(static_cast<uint8_t*>(buffer.mappedData()) + alignedOffset, data, size);
    buffer.flush(alignedOffset, static_cast<VkDeviceSize>(size));
    cursor = end;

    VkDescriptorSet set = descriptorArena_->allocate(setLayout);
    if (set == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;  // already logged by DescriptorArena::allocate().
    }

    VkDescriptorBufferInfo bufferInfo{buffer.handle(), alignedOffset, static_cast<VkDeviceSize>(size)};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;  // this engine's own fixed binding-0-within-set-1 convention [D6/D8] -- see
                            // material_system.cpp's kMaterialParamBlockBinding comment for the citation.
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return set;
}

namespace detail {

const void* debugFrameBufferData(const ParamArena& arena, uint32_t frameIndex) {
    if (arena.buffers_.empty()) {
        return nullptr;
    }
    uint32_t slot = frameIndex % static_cast<uint32_t>(arena.buffers_.size());
    return arena.buffers_[slot].mappedData();
}

}  // namespace detail

}  // namespace rx::material
