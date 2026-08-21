#include <rx_material/param_arena_factory.h>

#include <rx_core/log.h>

#include <algorithm>
#include <utility>

namespace rx::material {

std::optional<DemandSizedMaterialParamArena> createDemandSizedMaterialParamArena(VkDevice device,
                                                                                   uint32_t materialCount) {
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &uboBinding;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout) != VK_SUCCESS) {
        RX_LOG_ERROR("rx::material::createDemandSizedMaterialParamArena: vkCreateDescriptorSetLayout failed");
        return std::nullopt;
    }

    const uint32_t sets = std::max<uint32_t>(materialCount, 1);
    const rx::rhi::DescriptorArena::Capacities capacities{/*maxSets=*/sets, /*uniformBuffers=*/sets};
    auto arena = rx::rhi::DescriptorArena::create(device, /*framesInFlight=*/1, capacities);
    if (!arena.has_value()) {
        RX_LOG_ERROR("rx::material::createDemandSizedMaterialParamArena: DescriptorArena::create failed for {} "
                     "material(s)",
                     materialCount);
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        return std::nullopt;
    }

    RX_LOG_INFO("rx::material::createDemandSizedMaterialParamArena: material-params descriptor arena sized for {} "
                "material(s)",
                sets);
    return DemandSizedMaterialParamArena{setLayout, std::move(*arena)};
}

}  // namespace rx::material
