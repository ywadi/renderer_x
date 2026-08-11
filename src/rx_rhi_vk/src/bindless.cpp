#include <rx_rhi_vk/bindless.h>

#include <rx_core/debug_checks.h>
#include <rx_core/log.h>

#include <array>
#include <utility>

namespace rx::rhi {

namespace {

// Every binding in this table's set is update-after-bind + partially-bound;
// only the last binding (kStorageBufferBinding) may additionally carry
// VARIABLE_DESCRIPTOR_COUNT -- Vulkan requires that flag be used on at most
// the final binding of a set (VUID-VkDescriptorSetLayoutCreateInfo-pNext
// via VkDescriptorSetLayoutBindingFlagsCreateInfo's own documented
// constraint).
constexpr VkDescriptorBindingFlags kCommonBindingFlags =
    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

void destroyPoolAndLayout(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout setLayout) {
    if (pool != VK_NULL_HANDLE) {
        // Destroying the pool implicitly frees the one VkDescriptorSet
        // allocated from it -- this table's pool is never created with
        // VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, so there is no
        // separate vkFreeDescriptorSets step.
        vkDestroyDescriptorPool(device, pool, nullptr);
    }
    if (setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
}

}  // namespace

BindlessTable::BindlessTable(BindlessTable&& other) noexcept : BindlessTable() {
    *this = std::move(other);
}

BindlessTable& BindlessTable::operator=(BindlessTable&& other) noexcept {
    if (this != &other) {
        destroyAll();

        device_ = other.device_;
        setLayout_ = other.setLayout_;
        pool_ = other.pool_;
        set_ = other.set_;
        capacities_ = other.capacities_;
        sampledImages_ = std::move(other.sampledImages_);
        samplers_ = std::move(other.samplers_);
        storageBuffers_ = std::move(other.storageBuffers_);

        other.device_ = VK_NULL_HANDLE;
        other.setLayout_ = VK_NULL_HANDLE;
        other.pool_ = VK_NULL_HANDLE;
        other.set_ = VK_NULL_HANDLE;
        other.capacities_ = Capacities{};
    }
    return *this;
}

BindlessTable::~BindlessTable() {
    destroyAll();
}

void BindlessTable::destroyAll() {
    destroyPoolAndLayout(device_, pool_, setLayout_);
}

std::optional<BindlessTable> BindlessTable::create(VkPhysicalDevice physicalDevice, VkDevice device,
                                                     Capacities capacities) {
    // --- Zero-capacity guard -------------------------------------------
    if (capacities.sampledImages == 0 || capacities.samplers == 0 || capacities.storageBuffers == 0) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::create: all three capacities must be > 0 (got sampledImages={}, "
            "samplers={}, storageBuffers={}); a zero-count binding is not a valid way to express "
            "\"unused\" in this table's fixed three-binding layout",
            capacities.sampledImages, capacities.samplers, capacities.storageBuffers);
        return std::nullopt;
    }

    // --- Defensive capacity pre-check against real device limits -------
    // Done BEFORE any Vulkan object is created: an absurd capacity request
    // (far beyond what this physical device actually supports) must fail
    // as a clean, logged std::nullopt here, not as a validation error or
    // driver-dependent behavior from vkCreateDescriptorSetLayout/
    // vkCreateDescriptorPool.
    VkPhysicalDeviceVulkan12Properties props12{};
    props12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &props12;
    vkGetPhysicalDeviceProperties2(physicalDevice, &props2);

    struct CapacityCheck {
        uint32_t requested;
        uint32_t limit;
        const char* capacityName;
        const char* limitName;
    };
    const std::array<CapacityCheck, 3> checks{{
        {capacities.sampledImages, props12.maxDescriptorSetUpdateAfterBindSampledImages, "sampledImages",
         "maxDescriptorSetUpdateAfterBindSampledImages"},
        {capacities.samplers, props12.maxDescriptorSetUpdateAfterBindSamplers, "samplers",
         "maxDescriptorSetUpdateAfterBindSamplers"},
        {capacities.storageBuffers, props12.maxDescriptorSetUpdateAfterBindStorageBuffers, "storageBuffers",
         "maxDescriptorSetUpdateAfterBindStorageBuffers"},
    }};
    for (const auto& check : checks) {
        if (check.requested > check.limit) {
            RX_LOG_ERROR(
                "rx::rhi::BindlessTable::create: requested {} capacity {} exceeds this device's {} limit "
                "of {}; rejecting",
                check.capacityName, check.requested, check.limitName, check.limit);
            return std::nullopt;
        }
    }

    // --- Set layout ------------------------------------------------------
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = kSampledImageBinding;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = capacities.sampledImages;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;

    bindings[1].binding = kSamplerBinding;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = capacities.samplers;
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;

    bindings[2].binding = kStorageBufferBinding;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = capacities.storageBuffers;
    bindings[2].stageFlags = VK_SHADER_STAGE_ALL;

    std::array<VkDescriptorBindingFlags, 3> bindingFlags{
        kCommonBindingFlags, kCommonBindingFlags, kCommonBindingFlags | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
    flagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    setLayoutInfo.pNext = &flagsInfo;
    setLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    setLayoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkResult setLayoutResult = vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout);
    if (setLayoutResult != VK_SUCCESS) {
        RX_LOG_ERROR("rx::rhi::BindlessTable::create: vkCreateDescriptorSetLayout failed: VkResult={}",
                     static_cast<int>(setLayoutResult));
        return std::nullopt;
    }

    // --- Update-after-bind pool ------------------------------------------
    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacities.sampledImages};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_SAMPLER, capacities.samplers};
    poolSizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, capacities.storageBuffers};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult poolResult = vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool);
    if (poolResult != VK_SUCCESS) {
        RX_LOG_ERROR("rx::rhi::BindlessTable::create: vkCreateDescriptorPool failed: VkResult={}",
                     static_cast<int>(poolResult));
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        return std::nullopt;
    }

    // --- Descriptor set (variable count on the last binding) -------------
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableCountInfo{};
    variableCountInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    variableCountInfo.descriptorSetCount = 1;
    variableCountInfo.pDescriptorCounts = &capacities.storageBuffers;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.pNext = &variableCountInfo;
    allocInfo.descriptorPool = pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &setLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult allocResult = vkAllocateDescriptorSets(device, &allocInfo, &set);
    if (allocResult != VK_SUCCESS) {
        RX_LOG_ERROR("rx::rhi::BindlessTable::create: vkAllocateDescriptorSets failed: VkResult={}",
                     static_cast<int>(allocResult));
        destroyPoolAndLayout(device, pool, setLayout);
        return std::nullopt;
    }

    BindlessTable table;
    table.device_ = device;
    table.setLayout_ = setLayout;
    table.pool_ = pool;
    table.set_ = set;
    table.capacities_ = capacities;
    return table;
}

BindlessHandle BindlessTable::registerSampledImage(VkImageView view, VkImageLayout layout) {
    RX_ASSERT_MAIN_THREAD("BindlessTable::registerSampledImage");
    auto internal = sampledImages_.acquire(detail::EmptyPayload{});
    if (internal.index() >= capacities_.sampledImages) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerSampledImage: sampled-image capacity ({}) already fully "
            "occupied; rejecting",
            capacities_.sampledImages);
        sampledImages_.release(internal);
        return BindlessHandle{};
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = VK_NULL_HANDLE;
    imageInfo.imageView = view;
    imageInfo.imageLayout = layout;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = kSampledImageBinding;
    write.dstArrayElement = internal.index();
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return BindlessHandle(BindlessResourceKind::SampledImage, internal.index(), internal.generation());
}

BindlessHandle BindlessTable::registerSampler(VkSampler sampler) {
    RX_ASSERT_MAIN_THREAD("BindlessTable::registerSampler");
    auto internal = samplers_.acquire(detail::EmptyPayload{});
    if (internal.index() >= capacities_.samplers) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerSampler: sampler capacity ({}) already fully occupied; "
            "rejecting",
            capacities_.samplers);
        samplers_.release(internal);
        return BindlessHandle{};
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    imageInfo.imageView = VK_NULL_HANDLE;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = kSamplerBinding;
    write.dstArrayElement = internal.index();
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return BindlessHandle(BindlessResourceKind::Sampler, internal.index(), internal.generation());
}

BindlessHandle BindlessTable::registerStorageBuffer(VkBuffer buffer, VkDeviceSize range, VkDeviceSize offset) {
    RX_ASSERT_MAIN_THREAD("BindlessTable::registerStorageBuffer");
    auto internal = storageBuffers_.acquire(detail::EmptyPayload{});
    if (internal.index() >= capacities_.storageBuffers) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerStorageBuffer: storage-buffer capacity ({}) already fully "
            "occupied; rejecting",
            capacities_.storageBuffers);
        storageBuffers_.release(internal);
        return BindlessHandle{};
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = offset;
    bufferInfo.range = range;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = kStorageBufferBinding;
    write.dstArrayElement = internal.index();
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return BindlessHandle(BindlessResourceKind::StorageBuffer, internal.index(), internal.generation());
}

void BindlessTable::release(BindlessHandle handle) {
    RX_ASSERT_MAIN_THREAD("BindlessTable::release");
    if (!handle.isValid()) {
        return;
    }
    switch (handle.kind()) {
        case BindlessResourceKind::SampledImage:
            sampledImages_.release(rx::core::Handle<detail::SampledImageSlotTag>(handle.index(), handle.generation()));
            break;
        case BindlessResourceKind::Sampler:
            samplers_.release(rx::core::Handle<detail::SamplerSlotTag>(handle.index(), handle.generation()));
            break;
        case BindlessResourceKind::StorageBuffer:
            storageBuffers_.release(
                rx::core::Handle<detail::StorageBufferSlotTag>(handle.index(), handle.generation()));
            break;
    }
}

}  // namespace rx::rhi
