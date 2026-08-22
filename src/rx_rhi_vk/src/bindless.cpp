#include <rx_rhi_vk/bindless.h>

#include <rx_core/debug_checks.h>
#include <rx_core/log.h>

#include <array>
#include <utility>
#include <vector>

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
        comparisonSamplers_ = std::move(other.comparisonSamplers_);
        cubeImages_ = std::move(other.cubeImages_);
        genericStorageBuffers_ = std::move(other.genericStorageBuffers_);
        clusterLightBuffers_ = std::move(other.clusterLightBuffers_);

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
    // [Phase 4 Stage 2 Task 22 fix round, F1; Phase 5 Task 10, #46 widened
    // this from a binary (3-or-4-binding) choice to four possible shapes]
    // Binding 3 (kComparisonSamplerBinding) and binding 4
    // (kCubeSampledImageBinding) are EACH independently CONDITIONAL: present
    // only when their own capacity is > 0 -- see those fields' own header
    // comments for why (every caller that leaves either at 0 gets this
    // table's original, byte-identical layout for that slot). Only the LAST
    // binding in `pBindings`'s own array ORDER may carry
    // VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT [Vulkan spec
    // constraint] -- built here as a plain ordered list (SampledImage,
    // Sampler, StorageBuffer, then ComparisonSampler/CubeImage if present,
    // in that fixed priority order) so whichever of the two optional slots
    // is actually present LAST always receives the flag, covering all four
    // (comparisonSamplers x cubeImages) present/absent combinations
    // uniformly -- binding NUMBERS are never renumbered/compacted (a table
    // with cubeImages>0 but comparisonSamplers==0 legitimately has bindings
    // {0,1,2,4}, skipping 3 entirely; Vulkan does not require contiguous
    // binding numbers within one set).
    const bool hasComparisonSamplers = capacities.comparisonSamplers > 0;
    const bool hasCubeImages = capacities.cubeImages > 0;
    // [Phase 5 Task 15, #51] Two more independently-optional bindings,
    // appended to the SAME fixed priority order (after cubeImages) so
    // whichever is actually present LAST still receives the
    // VARIABLE_DESCRIPTOR_COUNT flag -- see this function's own comment
    // above for the general rule this extends.
    const bool hasGenericStorageBuffers = capacities.genericStorageBuffers > 0;
    const bool hasClusterLightBuffers = capacities.clusterLightBuffers > 0;

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(7);

    VkDescriptorSetLayoutBinding sampledImageBinding{};
    sampledImageBinding.binding = kSampledImageBinding;
    sampledImageBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sampledImageBinding.descriptorCount = capacities.sampledImages;
    sampledImageBinding.stageFlags = VK_SHADER_STAGE_ALL;
    bindings.push_back(sampledImageBinding);

    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = kSamplerBinding;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerBinding.descriptorCount = capacities.samplers;
    samplerBinding.stageFlags = VK_SHADER_STAGE_ALL;
    bindings.push_back(samplerBinding);

    VkDescriptorSetLayoutBinding storageBufferBinding{};
    storageBufferBinding.binding = kStorageBufferBinding;
    storageBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    storageBufferBinding.descriptorCount = capacities.storageBuffers;
    storageBufferBinding.stageFlags = VK_SHADER_STAGE_ALL;
    bindings.push_back(storageBufferBinding);

    if (hasComparisonSamplers) {
        VkDescriptorSetLayoutBinding comparisonSamplerBinding{};
        comparisonSamplerBinding.binding = kComparisonSamplerBinding;
        comparisonSamplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        comparisonSamplerBinding.descriptorCount = capacities.comparisonSamplers;
        comparisonSamplerBinding.stageFlags = VK_SHADER_STAGE_ALL;
        bindings.push_back(comparisonSamplerBinding);
    }
    if (hasCubeImages) {
        VkDescriptorSetLayoutBinding cubeImageBinding{};
        cubeImageBinding.binding = kCubeSampledImageBinding;
        cubeImageBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        cubeImageBinding.descriptorCount = capacities.cubeImages;
        cubeImageBinding.stageFlags = VK_SHADER_STAGE_ALL;
        bindings.push_back(cubeImageBinding);
    }
    if (hasGenericStorageBuffers) {
        VkDescriptorSetLayoutBinding genericStorageBufferBinding{};
        genericStorageBufferBinding.binding = kGenericStorageBufferBinding;
        genericStorageBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        genericStorageBufferBinding.descriptorCount = capacities.genericStorageBuffers;
        genericStorageBufferBinding.stageFlags = VK_SHADER_STAGE_ALL;
        bindings.push_back(genericStorageBufferBinding);
    }
    if (hasClusterLightBuffers) {
        VkDescriptorSetLayoutBinding clusterLightBufferBinding{};
        clusterLightBufferBinding.binding = kClusterLightBufferBinding;
        clusterLightBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        clusterLightBufferBinding.descriptorCount = capacities.clusterLightBuffers;
        clusterLightBufferBinding.stageFlags = VK_SHADER_STAGE_ALL;
        bindings.push_back(clusterLightBufferBinding);
    }

    const uint32_t bindingCount = static_cast<uint32_t>(bindings.size());
    std::vector<VkDescriptorBindingFlags> bindingFlags(bindingCount, kCommonBindingFlags);
    // The variable-count slot is whichever capacity `bindings.back()`
    // actually is -- since `bindings` is built in the fixed priority order
    // above, this is ALWAYS the last-present of (clusterLightBuffers,
    // genericStorageBuffers, cubeImages, comparisonSamplers, storageBuffers),
    // matching this function's own header comment.
    bindingFlags.back() |= VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    const uint32_t variableCount = hasClusterLightBuffers    ? capacities.clusterLightBuffers
                                    : hasGenericStorageBuffers ? capacities.genericStorageBuffers
                                    : hasCubeImages             ? capacities.cubeImages
                                    : hasComparisonSamplers     ? capacities.comparisonSamplers
                                                                 : capacities.storageBuffers;

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
    std::vector<VkDescriptorPoolSize> poolSizes;
    poolSizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacities.sampledImages});
    poolSizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER, capacities.samplers});
    poolSizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, capacities.storageBuffers});
    if (hasComparisonSamplers) {
        // Same descriptor TYPE as binding 1 (VK_DESCRIPTOR_TYPE_SAMPLER --
        // Vulkan draws no type-level distinction between a comparison and
        // an ordinary sampler; only the underlying VkSampler's own
        // `compareEnable` and the consuming shader's `SamplerComparisonState`
        // vs `SamplerState` declaration differ) -- a SEPARATE pool-size
        // entry, not folded into binding 1's, since each
        // VkDescriptorPoolSize is scoped to the (type, count) pair a pool
        // must reserve, and this table tracks the two counts independently.
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER, capacities.comparisonSamplers});
    }
    if (hasCubeImages) {
        // Same descriptor TYPE as binding 0 (VK_DESCRIPTOR_TYPE_SAMPLED_
        // IMAGE -- Vulkan draws no type-level distinction between a cube
        // and an ordinary sampled image; only the shader-side `TextureCube`
        // vs `Texture2D` declaration and the registered VkImageView's own
        // VK_IMAGE_VIEW_TYPE differ) -- a SEPARATE pool-size entry, same
        // reasoning as comparisonSamplers above.
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacities.cubeImages});
    }
    if (hasGenericStorageBuffers) {
        // [Phase 5 Task 15, #51] Same descriptor TYPE as binding 2
        // (VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) -- a separate pool-size entry,
        // same reasoning as comparisonSamplers/cubeImages above.
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, capacities.genericStorageBuffers});
    }
    if (hasClusterLightBuffers) {
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, capacities.clusterLightBuffers});
    }

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
    // `variableCount` was already derived above, alongside the matching
    // bindingFlags.back() flag -- see this function's own comment there.
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableCountInfo{};
    variableCountInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    variableCountInfo.descriptorSetCount = 1;
    variableCountInfo.pDescriptorCounts = &variableCount;

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

BindlessHandle BindlessTable::registerComparisonSampler(VkSampler sampler) {
    RX_ASSERT_MAIN_THREAD("BindlessTable::registerComparisonSampler");
    if (capacities_.comparisonSamplers == 0) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerComparisonSampler: this table was created with "
            "capacities.comparisonSamplers == 0 -- binding 3 does not exist; rejecting");
        return BindlessHandle{};
    }
    auto internal = comparisonSamplers_.acquire(detail::EmptyPayload{});
    if (internal.index() >= capacities_.comparisonSamplers) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerComparisonSampler: comparison-sampler capacity ({}) already fully "
            "occupied; rejecting",
            capacities_.comparisonSamplers);
        comparisonSamplers_.release(internal);
        return BindlessHandle{};
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    imageInfo.imageView = VK_NULL_HANDLE;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = kComparisonSamplerBinding;
    write.dstArrayElement = internal.index();
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return BindlessHandle(BindlessResourceKind::ComparisonSampler, internal.index(), internal.generation());
}

BindlessHandle BindlessTable::registerCubeSampledImage(VkImageView view, VkImageLayout layout) {
    RX_ASSERT_MAIN_THREAD("BindlessTable::registerCubeSampledImage");
    if (capacities_.cubeImages == 0) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerCubeSampledImage: this table was created with "
            "capacities.cubeImages == 0 -- binding 4 does not exist; rejecting");
        return BindlessHandle{};
    }
    auto internal = cubeImages_.acquire(detail::EmptyPayload{});
    if (internal.index() >= capacities_.cubeImages) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerCubeSampledImage: cube-image capacity ({}) already fully "
            "occupied; rejecting",
            capacities_.cubeImages);
        cubeImages_.release(internal);
        return BindlessHandle{};
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = VK_NULL_HANDLE;
    imageInfo.imageView = view;
    imageInfo.imageLayout = layout;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = kCubeSampledImageBinding;
    write.dstArrayElement = internal.index();
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return BindlessHandle(BindlessResourceKind::CubeImage, internal.index(), internal.generation());
}

BindlessHandle BindlessTable::registerGenericStorageBuffer(VkBuffer buffer, VkDeviceSize range, VkDeviceSize offset) {
    RX_ASSERT_MAIN_THREAD("BindlessTable::registerGenericStorageBuffer");
    if (capacities_.genericStorageBuffers == 0) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerGenericStorageBuffer: this table was created with "
            "capacities.genericStorageBuffers == 0 -- binding {} does not exist; rejecting",
            kGenericStorageBufferBinding);
        return BindlessHandle{};
    }
    auto internal = genericStorageBuffers_.acquire(detail::EmptyPayload{});
    if (internal.index() >= capacities_.genericStorageBuffers) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerGenericStorageBuffer: genericStorageBuffers capacity ({}) already "
            "fully occupied; rejecting",
            capacities_.genericStorageBuffers);
        genericStorageBuffers_.release(internal);
        return BindlessHandle{};
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = offset;
    bufferInfo.range = range;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = kGenericStorageBufferBinding;
    write.dstArrayElement = internal.index();
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return BindlessHandle(BindlessResourceKind::GenericStorageBuffer, internal.index(), internal.generation());
}

BindlessHandle BindlessTable::registerClusterLightBuffer(VkBuffer buffer, VkDeviceSize range, VkDeviceSize offset) {
    RX_ASSERT_MAIN_THREAD("BindlessTable::registerClusterLightBuffer");
    if (capacities_.clusterLightBuffers == 0) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerClusterLightBuffer: this table was created with "
            "capacities.clusterLightBuffers == 0 -- binding {} does not exist; rejecting",
            kClusterLightBufferBinding);
        return BindlessHandle{};
    }
    auto internal = clusterLightBuffers_.acquire(detail::EmptyPayload{});
    if (internal.index() >= capacities_.clusterLightBuffers) {
        RX_LOG_ERROR(
            "rx::rhi::BindlessTable::registerClusterLightBuffer: clusterLightBuffers capacity ({}) already fully "
            "occupied; rejecting",
            capacities_.clusterLightBuffers);
        clusterLightBuffers_.release(internal);
        return BindlessHandle{};
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = offset;
    bufferInfo.range = range;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = kClusterLightBufferBinding;
    write.dstArrayElement = internal.index();
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return BindlessHandle(BindlessResourceKind::ClusterLightBuffer, internal.index(), internal.generation());
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
        case BindlessResourceKind::ComparisonSampler:
            comparisonSamplers_.release(
                rx::core::Handle<detail::ComparisonSamplerSlotTag>(handle.index(), handle.generation()));
            break;
        case BindlessResourceKind::CubeImage:
            cubeImages_.release(rx::core::Handle<detail::CubeImageSlotTag>(handle.index(), handle.generation()));
            break;
        case BindlessResourceKind::GenericStorageBuffer:
            genericStorageBuffers_.release(
                rx::core::Handle<detail::GenericStorageBufferSlotTag>(handle.index(), handle.generation()));
            break;
        case BindlessResourceKind::ClusterLightBuffer:
            clusterLightBuffers_.release(
                rx::core::Handle<detail::ClusterLightBufferSlotTag>(handle.index(), handle.generation()));
            break;
    }
}

}  // namespace rx::rhi
