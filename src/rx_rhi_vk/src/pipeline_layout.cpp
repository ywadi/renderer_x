#include <rx_rhi_vk/pipeline_layout.h>

#include <rx_rhi_vk/bindless.h>
#include <rx_core/log.h>

#include <algorithm>
#include <array>
#include <utility>

namespace rx::rhi {

namespace {

// Every Vulkan 1.0+ implementation must support at least this many bytes of
// push-constant storage (`VkPhysicalDeviceLimits::maxPushConstantsSize`'s
// guaranteed minimum) -- confirmed as the *exact* limit (not just the
// floor) on the Steam Deck's RADV driver [R:B2], so this engine budgets to
// it rather than to whatever a given desktop GPU happens to report.
constexpr uint32_t kPushConstantBudgetBytes = 128;

// `startIndex` lets callers skip index 0 when it is a caller-owned external
// layout (build()'s `externalSet0` parameter / PipelineLayoutBundle's own
// `externalSet0_` flag) -- see both those comments for why this bundle must
// never destroy that handle.
void destroySetLayouts(VkDevice device, const std::vector<VkDescriptorSetLayout>& setLayouts, size_t startIndex = 0) {
    for (size_t i = startIndex; i < setLayouts.size(); ++i) {
        if (setLayouts[i] != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, setLayouts[i], nullptr);
        }
    }
}

// The fixed binding-number -> descriptor-type shape build()'s `externalSet0`
// path validates a shader's reflected set-0 bindings against -- see
// pipeline_layout.h's comment on build() for the full rationale. This is
// deliberately BindlessTable's own scheme (bindless.h), not an independent
// invention: `externalSet0` only has one real producer in this codebase
// (BindlessTable::descriptorSetLayout()), and validating against anything
// else would just be a different, unrelated shape that happens to also not
// match the real handle build() is about to substitute in.
struct ExpectedBindlessSlot {
    uint32_t binding;
    VkDescriptorType type;
};

// [Phase 4 Stage 2 Task 22 fix round, F1] Grew from 3 to 4 slots: binding 3
// (kComparisonSamplerBinding) is BindlessTable's own OPTIONAL fourth slot
// (Capacities::comparisonSamplers -- see that field's own header comment).
// [Phase 5 Task 10, #46] Grew from 4 to 5 slots: binding 4
// (kCubeSampledImageBinding) is BindlessTable's own OPTIONAL fifth slot
// (Capacities::cubeImages -- see that field's own header comment).
// Recognizing either is additive and backward-compatible: a shader that
// never declares a set-0 binding 3 or 4 (every non-material/pre-Task-10
// shader in this codebase) is unaffected, since this list is only ever
// walked against bindings the shader's OWN reflection actually produced.
constexpr std::array<ExpectedBindlessSlot, 5> kExpectedExternalSet0Shape{{
    {BindlessTable::kSampledImageBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {BindlessTable::kSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER},
    {BindlessTable::kStorageBufferBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {BindlessTable::kComparisonSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER},
    {BindlessTable::kCubeSampledImageBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
}};

// See build()'s comment for exactly what "subset-compatible shape" means
// here. Returns false having already logged the specific reason via
// RX_LOG_ERROR -- callers should treat that as the full diagnostic, not add
// their own generic message on top.
bool validateExternalSet0Shape(const std::vector<const rx::shader::ShaderLayoutInfo::Binding*>& set0Bindings) {
    for (const auto* binding : set0Bindings) {
        const ExpectedBindlessSlot* expected = nullptr;
        for (const auto& slot : kExpectedExternalSet0Shape) {
            if (slot.binding == binding->binding) {
                expected = &slot;
                break;
            }
        }
        if (expected == nullptr) {
            RX_LOG_ERROR(
                "rx_rhi_vk::PipelineLayoutBuilder::build: reflected set-0 binding {} has no counterpart in the "
                "external bindless-table layout (known slots: {}=SAMPLED_IMAGE, {}=SAMPLER, {}=STORAGE_BUFFER, "
                "{}=COMPARISON_SAMPLER, {}=CUBE_SAMPLED_IMAGE); rejecting",
                binding->binding, BindlessTable::kSampledImageBinding, BindlessTable::kSamplerBinding,
                BindlessTable::kStorageBufferBinding, BindlessTable::kComparisonSamplerBinding,
                BindlessTable::kCubeSampledImageBinding);
            return false;
        }
        if (binding->type != expected->type) {
            RX_LOG_ERROR(
                "rx_rhi_vk::PipelineLayoutBuilder::build: reflected set-0 binding {} declares descriptor type {} "
                "but the external bindless-table layout's matching slot is type {}; rejecting",
                binding->binding, static_cast<int>(binding->type), static_cast<int>(expected->type));
            return false;
        }
        if (!binding->unboundedArray && binding->count > PipelineLayoutBuilder::kUnboundedArrayDescriptorCapacity) {
            RX_LOG_ERROR(
                "rx_rhi_vk::PipelineLayoutBuilder::build: reflected set-0 binding {} declares a bounded count of "
                "{}, exceeding this builder's generic capacity ceiling of {}; rejecting",
                binding->binding, binding->count, PipelineLayoutBuilder::kUnboundedArrayDescriptorCapacity);
            return false;
        }
    }
    return true;
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
        externalSet0_ = other.externalSet0_;

        other.setLayouts.clear();
        other.layout = VK_NULL_HANDLE;
        other.device_ = VK_NULL_HANDLE;
        other.externalSet0_ = false;
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
    destroySetLayouts(device_, setLayouts, externalSet0_ ? 1 : 0);
}

std::optional<PipelineLayoutBundle> PipelineLayoutBuilder::build(VkDevice device,
                                                                  const rx::shader::ShaderLayoutInfo& layoutInfo,
                                                                  VkDescriptorSetLayout externalSet0) {
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
    // Forced up to at least 1 when `externalSet0` is supplied: the caller
    // wants set 0 wired to that handle regardless of whether this shader
    // itself declares anything there (a shader with zero set-0 bindings is
    // a valid, if degenerate, "subset" of the bindless table's shape).
    uint32_t maxSet = 0;
    for (const auto& binding : layoutInfo.bindings) {
        maxSet = std::max(maxSet, binding.set);
    }
    size_t setCount = layoutInfo.bindings.empty() ? 0 : static_cast<size_t>(maxSet) + 1;
    if (externalSet0 != VK_NULL_HANDLE) {
        setCount = std::max(setCount, size_t{1});
    }

    std::vector<std::vector<const rx::shader::ShaderLayoutInfo::Binding*>> bindingsBySet(setCount);
    for (const auto& binding : layoutInfo.bindings) {
        bindingsBySet[binding.set].push_back(&binding);
    }

    if (externalSet0 != VK_NULL_HANDLE && !validateExternalSet0Shape(bindingsBySet[0])) {
        return std::nullopt;  // validateExternalSet0Shape already logged why.
    }

    std::vector<VkDescriptorSetLayout> setLayouts;
    setLayouts.reserve(setCount);
    for (size_t setIndex = 0; setIndex < setCount; ++setIndex) {
        // Substitution: set 0 becomes the caller's own layout verbatim, no
        // vkCreateDescriptorSetLayout call at all -- see build()'s comment
        // in pipeline_layout.h for the full ownership contract.
        if (setIndex == 0 && externalSet0 != VK_NULL_HANDLE) {
            setLayouts.push_back(externalSet0);
            continue;
        }

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
            // setLayouts[0] is `externalSet0` here only if setIndex reached
            // this point after already pushing it above (setIndex > 0) --
            // never destroy it.
            destroySetLayouts(device, setLayouts, externalSet0 != VK_NULL_HANDLE ? 1 : 0);
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
        destroySetLayouts(device, setLayouts, externalSet0 != VK_NULL_HANDLE ? 1 : 0);
        return std::nullopt;
    }

    PipelineLayoutBundle bundle(device);
    bundle.setLayouts = std::move(setLayouts);
    bundle.layout = pipelineLayout;
    bundle.externalSet0_ = (externalSet0 != VK_NULL_HANDLE);
    return bundle;
}

}  // namespace rx::rhi
