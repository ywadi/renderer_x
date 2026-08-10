#include <doctest/doctest.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <VkBootstrap.h>
#include <cstdint>
#include <utility>

// Pure headless test -- no rx::platform::Window, no VkSurfaceKHR, no
// swapchain anywhere in this file, exactly like clear_color_test.cpp and
// pipeline_layout_test.cpp (safe to run in any order alongside the windowed
// Device/Buffer tests in this binary because tests/doctest_main.cpp warms
// vk-bootstrap's process-wide instance-function cache before any TEST_CASE
// runs -- see the comment there, and the Phase 2 plan's Global Constraints
// on the vk-bootstrap landmine, for why this test joins rx_rhi_vk_tests
// rather than a new executable).
namespace {

// Same descriptor-indexing feature set Device::create() now enables
// process-wide (device.cpp) -- built locally here rather than routed
// through rx::rhi::Device itself because Device::create() requires a real
// VkSurfaceKHR/windowed swapchain, and this file's tests are deliberately
// pure headless (per task-3-brief.md's "Tests (headless device)"), the
// same reasoning pipeline_layout_test.cpp's own local copy documents.
struct HeadlessBindlessFixture {
    rx::rhi::Context context;
    vkb::Device vkbDevice;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkQueue graphicsQueue;
    uint32_t graphicsQueueFamily;
};

HeadlessBindlessFixture makeHeadlessBindlessFixture() {
    auto context = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;

    // synchronization2 is required here too, unlike pipeline_layout_test.cpp's
    // otherwise-identical fixture: this file's makeRealSampledImage() below
    // calls rx::rhi::transitionImage(), which records a
    // vkCmdPipelineBarrier2 (VUID-vkCmdPipelineBarrier2-synchronization2-03848
    // requires the feature enabled, not just the device's API version to be
    // >= 1.3 -- discovered by this test failing validation-clean without
    // it). dynamicRendering is included alongside it for the same reason
    // clear_color_test.cpp's headless fixture enables both together.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    // No set_surface()/defer_surface_initialization() -- see
    // clear_color_test.cpp's comment on Context::create({}, ...) selection
    // for why that's correct for a genuinely headless instance, not merely
    // a workaround.
    vkb::PhysicalDeviceSelector selector(context->vkbInstance());
    auto physResult = selector.set_minimum_version(1, 3)
                          .set_required_features_12(features12)
                          .set_required_features_13(features13)
                          .select();
    REQUIRE(physResult.has_value());

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    REQUIRE(deviceResult.has_value());
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    auto graphicsQueueResult = vkbDevice.get_queue(vkb::QueueType::graphics);
    REQUIRE(graphicsQueueResult.has_value());
    auto graphicsQueueIndexResult = vkbDevice.get_queue_index(vkb::QueueType::graphics);
    REQUIRE(graphicsQueueIndexResult.has_value());

    return HeadlessBindlessFixture{std::move(*context), vkbDevice, vkbDevice.device,
                                    vkbDevice.physical_device.physical_device, graphicsQueueResult.value(),
                                    graphicsQueueIndexResult.value()};
}

rx::rhi::BindlessTable::Capacities makeTestCapacities() {
    return rx::rhi::BindlessTable::Capacities{/*sampledImages=*/1024, /*samplers=*/16, /*storageBuffers=*/256};
}

// A real 1x1 sampled image (transitioned to SHADER_READ_ONLY_OPTIMAL) plus
// a real sampler -- deliberately raw Vulkan (mirroring clear_color_test.cpp's
// own raw-image pattern) rather than routed through a not-yet-existing
// Texture2D helper (that's Task 4's job). Caller owns teardown via
// destroy(), which must run before the device that created these resources
// is destroyed.
struct RealSampledImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;

    void destroy(VkDevice device) {
        vkDestroySampler(device, sampler, nullptr);
        vkDestroyImageView(device, view, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
};

RealSampledImage makeRealSampledImage(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue,
                                       uint32_t queueFamily) {
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kFormat;
    imageInfo.extent = {1, 1, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    REQUIRE(vkCreateImage(device, &imageInfo, nullptr, &image) == VK_SUCCESS);

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, image, &memReq);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    REQUIRE(memoryTypeIndex != UINT32_MAX);

    VkMemoryAllocateInfo memAllocInfo{};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReq.size;
    memAllocInfo.memoryTypeIndex = memoryTypeIndex;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(device, &memAllocInfo, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    REQUIRE(vkCreateImageView(device, &viewInfo, nullptr, &view) == VK_SUCCESS);

    {
        auto cmdCtx = rx::rhi::CommandContext::create(device, queue, queueFamily);
        REQUIRE(cmdCtx.has_value());
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0F;

    VkSampler sampler = VK_NULL_HANDLE;
    REQUIRE(vkCreateSampler(device, &samplerInfo, nullptr, &sampler) == VK_SUCCESS);

    return RealSampledImage{image, memory, view, sampler};
}

}  // namespace

TEST_CASE("BindlessTable::create builds a valid set/layout and registers real resources with zero validation "
          "errors") {
    HeadlessBindlessFixture fixture = makeHeadlessBindlessFixture();

    {
        auto table = rx::rhi::BindlessTable::create(fixture.physicalDevice, fixture.device, makeTestCapacities());
        REQUIRE(table.has_value());
        CHECK(table->descriptorSetLayout() != VK_NULL_HANDLE);
        CHECK(table->descriptorSet() != VK_NULL_HANDLE);

        RealSampledImage image =
            makeRealSampledImage(fixture.device, fixture.physicalDevice, fixture.graphicsQueue, fixture.graphicsQueueFamily);

        auto imageHandle = table->registerSampledImage(image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK(imageHandle.isValid());
        CHECK(imageHandle.index() == 0);
        CHECK(imageHandle.generation() == 1);
        CHECK(imageHandle.kind() == rx::rhi::BindlessResourceKind::SampledImage);

        auto samplerHandle = table->registerSampler(image.sampler);
        CHECK(samplerHandle.isValid());
        CHECK(samplerHandle.index() == 0);
        CHECK(samplerHandle.generation() == 1);
        CHECK(samplerHandle.kind() == rx::rhi::BindlessResourceKind::Sampler);

        auto allocator = rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());
        auto buffer = allocator->createHostVisibleBuffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        REQUIRE(buffer.has_value());

        auto bufferHandle = table->registerStorageBuffer(buffer->handle(), /*range=*/256);
        CHECK(bufferHandle.isValid());
        CHECK(bufferHandle.index() == 0);
        CHECK(bufferHandle.generation() == 1);
        CHECK(bufferHandle.kind() == rx::rhi::BindlessResourceKind::StorageBuffer);

        // Every RAII/manual resource above must be torn down against a
        // still-live device, before the device itself goes away below --
        // same reverse-destruction discipline clear_color_test.cpp
        // documents. `buffer`/`allocator`/`table` (RAII) go out of scope at
        // the end of this block; `image` (manual) is destroyed explicitly
        // first since it isn't RAII.
        image.destroy(fixture.device);
    }

    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

TEST_CASE("BindlessTable register/release/re-register cycles reuse indices with bumped generations") {
    HeadlessBindlessFixture fixture = makeHeadlessBindlessFixture();

    {
        auto table = rx::rhi::BindlessTable::create(fixture.physicalDevice, fixture.device, makeTestCapacities());
        REQUIRE(table.has_value());

        RealSampledImage image =
            makeRealSampledImage(fixture.device, fixture.physicalDevice, fixture.graphicsQueue, fixture.graphicsQueueFamily);

        // Three registrations of the SAME real view/layout -- this test is
        // about index/generation bookkeeping, not about distinct resource
        // content, so reusing one real (validation-legal) VkImageView
        // across all three slots is deliberate and keeps the test focused.
        auto h0 = table->registerSampledImage(image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        auto h1 = table->registerSampledImage(image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        auto h2 = table->registerSampledImage(image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        REQUIRE(h0.isValid());
        REQUIRE(h1.isValid());
        REQUIRE(h2.isValid());
        CHECK(h0.index() == 0);
        CHECK(h1.index() == 1);
        CHECK(h2.index() == 2);
        CHECK(h0.generation() == 1);
        CHECK(h1.generation() == 1);
        CHECK(h2.generation() == 1);

        // Release the middle slot and re-register: must reuse index 1 with
        // generation bumped to 2 -- the free list is LIFO (rx::core::
        // HandlePool's own documented behavior), so the very next
        // acquisition after releasing exactly one slot must land on that
        // same slot.
        table->release(h1);
        auto h1Again = table->registerSampledImage(image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        REQUIRE(h1Again.isValid());
        CHECK(h1Again.index() == 1);
        CHECK(h1Again.generation() == 2);
        CHECK(h1Again != h1);

        // Releasing an already-released (stale) handle a second time is a
        // documented no-op, not a crash or an error.
        table->release(h1);
        table->release(h1);

        // A brand-new registration after the reuse above must land on a
        // fresh index (3), not clobber anything still live.
        auto h3 = table->registerSampledImage(image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        REQUIRE(h3.isValid());
        CHECK(h3.index() == 3);
        CHECK(h3.generation() == 1);

        image.destroy(fixture.device);
    }

    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

TEST_CASE("BindlessTable descriptor set can be bound in a dispatch-free command buffer with zero validation "
          "errors") {
    HeadlessBindlessFixture fixture = makeHeadlessBindlessFixture();

    {
        auto table = rx::rhi::BindlessTable::create(fixture.physicalDevice, fixture.device, makeTestCapacities());
        REQUIRE(table.has_value());

        RealSampledImage image =
            makeRealSampledImage(fixture.device, fixture.physicalDevice, fixture.graphicsQueue, fixture.graphicsQueueFamily);
        auto imageHandle = table->registerSampledImage(image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        REQUIRE(imageHandle.isValid());
        auto samplerHandle = table->registerSampler(image.sampler);
        REQUIRE(samplerHandle.isValid());

        auto allocator = rx::rhi::Allocator::createRaw(fixture.physicalDevice, fixture.device, fixture.context.instance());
        REQUIRE(allocator.has_value());
        auto buffer = allocator->createHostVisibleBuffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        REQUIRE(buffer.has_value());
        auto bufferHandle = table->registerStorageBuffer(buffer->handle(), /*range=*/256);
        REQUIRE(bufferHandle.isValid());

        // A minimal VkPipelineLayout built directly from this table's own
        // set layout, guaranteed structurally compatible with it (same
        // VkDescriptorSetLayout handle) -- exactly enough for
        // vkCmdBindDescriptorSets to be valid with no pipeline bound and no
        // draw/dispatch recorded, per task-3-brief.md's "bind + no draw"
        // gate.
        VkDescriptorSetLayout setLayout = table->descriptorSetLayout();
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout;

        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        REQUIRE(vkCreatePipelineLayout(fixture.device, &pipelineLayoutInfo, nullptr, &pipelineLayout) == VK_SUCCESS);

        {
            auto cmdCtx = rx::rhi::CommandContext::create(fixture.device, fixture.graphicsQueue, fixture.graphicsQueueFamily);
            REQUIRE(cmdCtx.has_value());

            VkDescriptorSet set = table->descriptorSet();
            cmdCtx->runOnce([&](VkCommandBuffer cmd) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
            });
        }

        vkDestroyPipelineLayout(fixture.device, pipelineLayout, nullptr);
        image.destroy(fixture.device);
    }

    vkDeviceWaitIdle(fixture.device);
    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}

TEST_CASE("BindlessTable::create rejects a capacity beyond this device's real update-after-bind limit with a "
          "clean error, no crash") {
    HeadlessBindlessFixture fixture = makeHeadlessBindlessFixture();

    VkPhysicalDeviceVulkan12Properties props12{};
    props12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &props12;
    vkGetPhysicalDeviceProperties2(fixture.physicalDevice, &props2);

    if (props12.maxDescriptorSetUpdateAfterBindSampledImages == UINT32_MAX) {
        // A driver reporting the "no meaningful limit" sentinel (RADV's
        // documented behavior [R:B2]) genuinely has no capacity this test
        // could request that is "beyond" it -- there is nothing to
        // exercise on such hardware, and that is the correct, honest
        // outcome, not a gap in this test.
        MESSAGE("this device reports an unbounded maxDescriptorSetUpdateAfterBindSampledImages; absurd-capacity "
                "rejection path is not exercisable here");
        vkb::destroy_device(fixture.vkbDevice);
        return;
    }

    rx::rhi::BindlessTable::Capacities absurd{};
    absurd.sampledImages = props12.maxDescriptorSetUpdateAfterBindSampledImages + 1;
    absurd.samplers = 16;
    absurd.storageBuffers = 256;

    auto table = rx::rhi::BindlessTable::create(fixture.physicalDevice, fixture.device, absurd);
    CHECK_FALSE(table.has_value());

    vkb::destroy_device(fixture.vkbDevice);
    CHECK_FALSE(fixture.context.hasValidationErrors());
}
