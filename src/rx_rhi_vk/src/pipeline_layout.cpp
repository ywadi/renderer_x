#include <rx_rhi_vk/pipeline_layout.h>

#include <rx_core/log.h>

#include <algorithm>
#include <utility>

namespace rx::rhi {

namespace {

// Every Vulkan 1.0+ implementation must support at least this many bytes of
// push-constant storage (`VkPhysicalDeviceLimits::maxPushConstantsSize`'s
// guaranteed minimum) -- confirmed as the *exact* limit (not just the
// floor) on the Steam Deck's RADV driver [R:B2], so this engine budgets to
// it rather than to whatever a given desktop GPU happens to report.
constexpr uint32_t kPushConstantBudgetBytes = 128;

void destroySetLayouts(VkDevice device, const std::vector<VkDescriptorSetLayout>& setLayouts) {
    for (VkDescriptorSetLayout setLayout : setLayouts) {
        if (setLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        }
    }
}

}  // namespace

PipelineLayoutBundle::PipelineLayoutBundle(PipelineLayoutBundle&& other) noexcept : PipelineLayoutBundle() {
    *this = std::move(other);
}

PipelineLayoutBundle& PipelineLayoutBundle::operator=(PipelineLayoutBundle&& other) noexcept {
    if (this != &other) {
        destroyAll();

        setLayouts = std::move(other.setLayouts);
        layout = other.layout;
        device_ = other.device_;

        other.setLayouts.clear();
        other.layout = VK_NULL_HANDLE;
        other.device_ = VK_NULL_HANDLE;
    }
    return *this;
}

PipelineLayoutBundle::~PipelineLayoutBundle() {
    destroyAll();
}

void PipelineLayoutBundle::destroyAll() {
    if (layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout, nullptr);
    }
    destroySetLayouts(device_, setLayouts);
}

std::optional<PipelineLayoutBundle> PipelineLayoutBuilder::build(VkDevice device,
                                                                  const rx::shader::ShaderLayoutInfo& layoutInfo) {
    // --- Push-constant budget enforcement -----------------------------
    uint32_t totalPushConstantSpan = 0;
    for (const auto& range : layoutInfo.pushRanges) {
        totalPushConstantSpan = std::max(totalPushConstantSpan, range.offset + range.size);
    }
    if (totalPushConstantSpan > kPushConstantBudgetBytes) {
        RX_LOG_ERROR(
            "rx_rhi_vk::PipelineLayoutBuilder::build: push constant footprint of {} bytes exceeds the "
            "{}-byte guaranteed-minimum budget [spec Fixed decision #5, R:B2]; rejecting",
            totalPushConstantSpan, kPushConstantBudgetBytes);
        return std::nullopt;
    }

    // --- Group bindings by set index, gap-filling any skipped index ----
    uint32_t maxSet = 0;
    for (const auto& binding : layoutInfo.bindings) {
        maxSet = std::max(maxSet, binding.set);
    }
    size_t setCount = layoutInfo.bindings.empty() ? 0 : static_cast<size_t>(maxSet) + 1;

    std::vector<std::vector<const rx::shader::ShaderLayoutInfo::Binding*>> bindingsBySet(setCount);
    for (const auto& binding : layoutInfo.bindings) {
        bindingsBySet[binding.set].push_back(&binding);
    }

    std::vector<VkDescriptorSetLayout> setLayouts;
    setLayouts.reserve(setCount);
    for (size_t setIndex = 0; setIndex < setCount; ++setIndex) {
        std::vector<VkDescriptorSetLayoutBinding> vkBindings;
        std::vector<VkDescriptorBindingFlags> bindingFlags;
        bool anyUpdateAfterBind = false;

        vkBindings.reserve(bindingsBySet[setIndex].size());
        bindingFlags.reserve(bindingsBySet[setIndex].size());
        for (const auto* binding : bindingsBySet[setIndex]) {
            VkDescriptorSetLayoutBinding vkBinding{};
            vkBinding.binding = binding->binding;
            vkBinding.descriptorType = binding->type;
            vkBinding.descriptorCount =
                binding->unboundedArray ? kUnboundedArrayDescriptorCapacity : binding->count;
            vkBinding.stageFlags = binding->stages;
            vkBindings.push_back(vkBinding);

            VkDescriptorBindingFlags flags = 0;
            if (binding->unboundedArray) {
                flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
                anyUpdateAfterBind = true;
            }
            bindingFlags.push_back(flags);
        }

        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
        flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
        flagsInfo.pBindingFlags = bindingFlags.empty() ? nullptr : bindingFlags.data();

        VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
        setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayoutInfo.bindingCount = static_cast<uint32_t>(vkBindings.size());
        setLayoutInfo.pBindings = vkBindings.empty() ? nullptr : vkBindings.data();
        if (anyUpdateAfterBind) {
            setLayoutInfo.pNext = &flagsInfo;
            setLayoutInfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        }

        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkResult result = vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout);
        if (result != VK_SUCCESS) {
            RX_LOG_ERROR("rx_rhi_vk::PipelineLayoutBuilder::build: vkCreateDescriptorSetLayout(set {}) failed: "
                         "VkResult={}",
                         setIndex, static_cast<int>(result));
            destroySetLayouts(device, setLayouts);
            return std::nullopt;
        }
        setLayouts.push_back(setLayout);
    }

    // --- Pipeline layout, tying the sets above to the push ranges ------
    std::vector<VkPushConstantRange> vkPushRanges;
    vkPushRanges.reserve(layoutInfo.pushRanges.size());
    for (const auto& range : layoutInfo.pushRanges) {
        VkPushConstantRange vkRange{};
        vkRange.stageFlags = range.stages;
        vkRange.offset = range.offset;
        vkRange.size = range.size;
        vkPushRanges.push_back(vkRange);
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(vkPushRanges.size());
    pipelineLayoutInfo.pPushConstantRanges = vkPushRanges.empty() ? nullptr : vkPushRanges.data();

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkResult pipelineLayoutResult = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
    if (pipelineLayoutResult != VK_SUCCESS) {
        RX_LOG_ERROR("rx_rhi_vk::PipelineLayoutBuilder::build: vkCreatePipelineLayout failed: VkResult={}",
                     static_cast<int>(pipelineLayoutResult));
        destroySetLayouts(device, setLayouts);
        return std::nullopt;
    }

    PipelineLayoutBundle bundle(device);
    bundle.setLayouts = std::move(setLayouts);
    bundle.layout = pipelineLayout;
    return bundle;
}

}  // namespace rx::rhi
