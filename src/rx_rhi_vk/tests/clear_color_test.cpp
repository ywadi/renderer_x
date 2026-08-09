#include <doctest/doctest.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <VkBootstrap.h>
#include <array>
#include <cstdint>
#include <cstring>

// Pure headless test -- no rx::platform::Window, no VkSurfaceKHR, no
// swapchain anywhere in this file. Exercises CommandContext::runOnce(),
// transitionImage(), and dynamic rendering end to end by clearing a small
// offscreen color image to a known color and reading the result back
// through a host-visible buffer.
//
// Safe to run in any order alongside the windowed Device/Buffer tests in
// this same binary: tests/doctest_main.cpp warms vk-bootstrap's
// process-wide instance-function cache with the broadest instance this
// binary needs (real window extensions when available, validation always
// on) before any TEST_CASE runs -- see the comment there, and the WARNING
// on Context::create() in rx_rhi_vk/context.h, for why a headless-only
// instance built here first would otherwise poison later windowed
// instances with null function pointers.
//
// Teardown ordering is the load-bearing part of this test (see
// task-3-brief.md): the logical VkDevice built below is a raw handle, not
// an rx::rhi::Device, so nothing destroys it automatically. Every RAII
// object that depends on it (CommandContext, Allocator, the readback
// Buffer) is declared inside the inner `{ }` scope below and goes out of
// scope -- destroyed in reverse order, against a still-live device --
// before vkDeviceWaitIdle + the raw image/view/memory destroys + finally
// vkb::destroy_device() run. Nothing here is destroyed after the device.
TEST_CASE("CommandContext::runOnce + transitionImage clear an offscreen image via dynamic rendering") {
    auto ctx = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(ctx.has_value());

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    // Deliberately NOT calling defer_surface_initialization() here, despite
    // the original plan's assumption that it belonged in this headless
    // path. Verified directly against vk-bootstrap's pinned commit
    // (556b79b165386f6c1a18362d30f2a076fdaa2778) source:
    //   - PhysicalDeviceSelector's constructor already sets
    //     criteria.require_present = !instance.headless, so for a
    //     genuinely headless Context (built via Context::create({}, ...),
    //     which calls InstanceBuilder::set_headless(true)) require_present
    //     is already false and select() never needs the surface-present
    //     check that defer_surface_initialization() exists to bypass --
    //     calling it buys nothing here.
    //   - Worse, DeviceBuilder::build() unconditionally adds
    //     VK_KHR_SWAPCHAIN_EXTENSION_NAME to the device's enabled
    //     extensions whenever `physical_device.surface != VK_NULL_HANDLE ||
    //     physical_device.defer_surface_initialization` -- i.e. calling
    //     defer_surface_initialization() on a selector with no surface at
    //     all requests VK_KHR_swapchain anyway, which is a real, reproduced
    //     validation error against a headless instance that never enabled
    //     the prerequisite VK_KHR_surface *instance* extension
    //     (VUID-vkCreateDevice-ppEnabledExtensionNames-01387: "Missing
    //     extension required by the device extension VK_KHR_swapchain:
    //     VK_KHR_surface"). This test never creates a VkSurfaceKHR or
    //     VkSwapchainKHR at all, so there is no later surface attachment to
    //     defer for -- omitting the call is the correct headless-only
    //     selection, not merely a workaround.
    vkb::PhysicalDeviceSelector selector(ctx->vkbInstance());
    auto physResult = selector.set_minimum_version(1, 3).set_required_features_13(features13).select();
    REQUIRE(physResult.has_value());

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    REQUIRE(deviceResult.has_value());
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    VkDevice device = vkbDevice.device;
    VkPhysicalDevice physicalDevice = vkbDevice.physical_device.physical_device;

    auto graphicsQueueResult = vkbDevice.get_queue(vkb::QueueType::graphics);
    REQUIRE(graphicsQueueResult.has_value());
    auto graphicsQueueIndexResult = vkbDevice.get_queue_index(vkb::QueueType::graphics);
    REQUIRE(graphicsQueueIndexResult.has_value());
    VkQueue graphicsQueue = graphicsQueueResult.value();
    uint32_t graphicsQueueFamily = graphicsQueueIndexResult.value();

    constexpr uint32_t kSize = 4;
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize kPixelBytes = static_cast<VkDeviceSize>(kSize) * kSize * 4;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kFormat;
    imageInfo.extent = {kSize, kSize, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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

    // Allocator::createHostVisibleBuffer (Task 2) only ever *requires*
    // VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT for its
    // VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT allocations
    // (verified directly against VMA v3.4.0's FindMemoryPreferences(): the
    // sequential-write path only adds VK_MEMORY_PROPERTY_HOST_CACHED_BIT to
    // outNotPreferredFlags, never requires HOST_COHERENT) -- so nothing
    // guarantees through that API alone that the readback buffer created
    // below ends up host-coherent. Buffer/Allocator's public surface has no
    // way to reach the underlying VmaAllocation to call
    // vmaInvalidateAllocation() explicitly, so instead verify the
    // precondition this test's memcpy-after-vkQueueWaitIdle read relies on:
    // every HOST_VISIBLE memory type this physical device exposes is also
    // HOST_COHERENT (true for every desktop Vulkan driver in practice,
    // never violated in this codebase's target set) -- making that
    // assumption a verified, testable fact rather than a silent one that
    // could someday read stale data on hardware with a truly non-coherent
    // host-visible heap.
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
            REQUIRE((memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0U);
        }
    }

    VkMemoryAllocateInfo memAllocInfo{};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReq.size;
    memAllocInfo.memoryTypeIndex = memoryTypeIndex;

    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(device, &memAllocInfo, nullptr, &imageMemory) == VK_SUCCESS);
    REQUIRE(vkBindImageMemory(device, image, imageMemory, 0) == VK_SUCCESS);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView = VK_NULL_HANDLE;
    REQUIRE(vkCreateImageView(device, &viewInfo, nullptr, &imageView) == VK_SUCCESS);

    std::array<uint8_t, kPixelBytes> pixels{};

    {
        // Inner scope: every RAII object created below is destroyed when
        // this scope closes -- readback Buffer, then Allocator, then
        // CommandContext, in reverse declaration order -- while `device`
        // is still alive. This is what avoids the original plan's
        // use-after-free (it destroyed the device first, then let these
        // same RAII destructors run against a dangling VkDevice).
        auto cmdCtx = rx::rhi::CommandContext::create(device, graphicsQueue, graphicsQueueFamily);
        REQUIRE(cmdCtx.has_value());

        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = imageView;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = VkClearColorValue{{1.0F, 0.0F, 0.0F, 1.0F}};

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = VkRect2D{{0, 0}, {kSize, kSize}};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;

            vkCmdBeginRendering(cmd, &renderingInfo);
            vkCmdEndRendering(cmd);

            rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        });

        auto allocator = rx::rhi::Allocator::createRaw(physicalDevice, device, ctx->instance());
        REQUIRE(allocator.has_value());

        auto readback = allocator->createHostVisibleBuffer(kPixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        REQUIRE(readback.has_value());

        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {kSize, kSize, 1};

            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
        });

        // runOnce() above already vkQueueWaitIdle()'d before returning, and
        // the REQUIRE above confirmed this device's host-visible memory is
        // also host-coherent, so the copy's writes are already visible to
        // this host read with no further barrier or
        // vkInvalidateMappedMemoryRanges needed.
        std::memcpy(pixels.data(), readback->mappedData(), pixels.size());
    }

    vkDeviceWaitIdle(device);
    vkDestroyImageView(device, imageView, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkFreeMemory(device, imageMemory, nullptr);
    vkb::destroy_device(vkbDevice);

    CHECK(pixels[0] == 255);
    CHECK(pixels[1] == 0);
    CHECK(pixels[2] == 0);
    CHECK(pixels[3] == 255);
    CHECK_FALSE(ctx->hasValidationErrors());
}
