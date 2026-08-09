#include <rx_rhi_vk/device.h>
#include <rx_core/log.h>
#include <VkBootstrap.h>
#include <utility>

namespace rx::rhi {

namespace {

void destroySurface(const Context& context, VkSurfaceKHR surface) {
    vkb::destroy_surface(context.vkbInstance(), surface);
}

}  // namespace

std::optional<Device> Device::create(Context& context, VkSurfaceKHR surface) {
    // shaderDrawParameters (promoted from VK_KHR_shader_draw_parameters to
    // Vulkan 1.1 core) is required by any HLSL/Slang vertex shader that
    // reads SV_VertexID: to reproduce HLSL's zero-based SV_VertexID exactly
    // (unaffected by a nonzero firstVertex/vkCmdDraw base), Slang's SPIR-V
    // backend emits `gl_VertexIndex - gl_BaseVertex`, which declares
    // OpCapability DrawParameters and a BaseVertex BuiltIn input -- verified
    // directly via `spirv-dis` on samples/01_triangle's compiled
    // triangle.vert.spv. Without this feature enabled, vkCreateShaderModule
    // on any such shader is a validation error
    // (VUID-VkShaderModuleCreateInfo-pCode-01091: "capability was declared,
    // but none of the requirements were met") even though several drivers
    // silently tolerate it -- exactly the "driver-tolerated but not
    // spec-valid" gap this task's triangle gate exists to close. Universally
    // available (core since Vulkan 1.1, no extension string needed here
    // since set_minimum_version is already >= 1.1), so requiring it costs
    // nothing on any target this project cares about.
    VkPhysicalDeviceVulkan11Features features11{};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.shaderDrawParameters = VK_TRUE;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(context.vkbInstance());
    auto physResult = selector.set_surface(surface)
                          .set_minimum_version(1, 3)
                          .set_required_features_11(features11)
                          .set_required_features_13(features13)
                          .select();
    if (!physResult) {
        RX_LOG_ERROR("vkb::PhysicalDeviceSelector::select failed: {}", physResult.error().message());
        destroySurface(context, surface);
        return std::nullopt;
    }

    auto deviceResult = vkb::DeviceBuilder(physResult.value()).build();
    if (!deviceResult) {
        RX_LOG_ERROR("vkb::DeviceBuilder::build failed: {}", deviceResult.error().message());
        destroySurface(context, surface);
        return std::nullopt;
    }
    vkb::Device vkbDevice = deviceResult.value();
    volkLoadDevice(vkbDevice.device);

    auto graphicsQueueResult = vkbDevice.get_queue(vkb::QueueType::graphics);
    if (!graphicsQueueResult) {
        RX_LOG_ERROR("vkb::Device::get_queue(graphics) failed: {}", graphicsQueueResult.error().message());
        vkb::destroy_device(vkbDevice);
        destroySurface(context, surface);
        return std::nullopt;
    }

    auto graphicsQueueIndexResult = vkbDevice.get_queue_index(vkb::QueueType::graphics);
    if (!graphicsQueueIndexResult) {
        RX_LOG_ERROR("vkb::Device::get_queue_index(graphics) failed: {}", graphicsQueueIndexResult.error().message());
        vkb::destroy_device(vkbDevice);
        destroySurface(context, surface);
        return std::nullopt;
    }

    auto presentQueueResult = vkbDevice.get_queue(vkb::QueueType::present);
    if (!presentQueueResult) {
        RX_LOG_ERROR("vkb::Device::get_queue(present) failed: {}", presentQueueResult.error().message());
        vkb::destroy_device(vkbDevice);
        destroySurface(context, surface);
        return std::nullopt;
    }

    vkb::SwapchainBuilder swapchainBuilder(vkbDevice, surface);
    auto swapchainResult = swapchainBuilder.build();
    if (!swapchainResult) {
        RX_LOG_ERROR("vkb::SwapchainBuilder::build failed: {}", swapchainResult.error().message());
        vkb::destroy_device(vkbDevice);
        destroySurface(context, surface);
        return std::nullopt;
    }
    vkb::Swapchain vkbSwapchain = swapchainResult.value();

    auto imagesResult = vkbSwapchain.get_images();
    if (!imagesResult) {
        RX_LOG_ERROR("vkb::Swapchain::get_images failed: {}", imagesResult.error().message());
        vkb::destroy_swapchain(vkbSwapchain);
        vkb::destroy_device(vkbDevice);
        destroySurface(context, surface);
        return std::nullopt;
    }

    Device dev;
    dev.instance_ = context.instance();
    dev.physicalDevice_ = vkbDevice.physical_device.physical_device;
    dev.device_ = vkbDevice.device;
    dev.surface_ = surface;
    dev.graphicsQueue_ = graphicsQueueResult.value();
    dev.graphicsQueueFamily_ = graphicsQueueIndexResult.value();
    dev.presentQueue_ = presentQueueResult.value();
    dev.swapchain_ = vkbSwapchain.swapchain;
    dev.swapchainImages_ = imagesResult.value();
    dev.swapchainFormat_ = vkbSwapchain.image_format;
    dev.swapchainExtent_ = vkbSwapchain.extent;
    return dev;
}

Device::Device(Device&& other) noexcept : Device() {
    *this = std::move(other);
}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        destroyAll();

        instance_ = other.instance_;
        physicalDevice_ = other.physicalDevice_;
        device_ = other.device_;
        surface_ = other.surface_;
        graphicsQueue_ = other.graphicsQueue_;
        graphicsQueueFamily_ = other.graphicsQueueFamily_;
        presentQueue_ = other.presentQueue_;
        swapchain_ = other.swapchain_;
        swapchainImages_ = std::move(other.swapchainImages_);
        swapchainFormat_ = other.swapchainFormat_;
        swapchainExtent_ = other.swapchainExtent_;

        other.instance_ = VK_NULL_HANDLE;
        other.physicalDevice_ = VK_NULL_HANDLE;
        other.device_ = VK_NULL_HANDLE;
        other.surface_ = VK_NULL_HANDLE;
        other.graphicsQueue_ = VK_NULL_HANDLE;
        other.graphicsQueueFamily_ = 0;
        other.presentQueue_ = VK_NULL_HANDLE;
        other.swapchain_ = VK_NULL_HANDLE;
        other.swapchainImages_.clear();
        other.swapchainFormat_ = VK_FORMAT_UNDEFINED;
        other.swapchainExtent_ = VkExtent2D{0, 0};
    }
    return *this;
}

Device::~Device() {
    destroyAll();
}

void Device::destroyAll() {
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
}

AcquireResult Device::acquireNextImage(VkSemaphore signal) {
    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, signal, VK_NULL_HANDLE, &imageIndex);
    switch (result) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR:
            return AcquireResult{SwapchainStatus::Ok, imageIndex};
        case VK_ERROR_OUT_OF_DATE_KHR:
            return AcquireResult{SwapchainStatus::NeedsRecreate, 0};
        case VK_ERROR_DEVICE_LOST:
            RX_LOG_ERROR("vkAcquireNextImageKHR: device lost");
            return AcquireResult{SwapchainStatus::DeviceLost, 0};
        default:
            RX_LOG_ERROR("vkAcquireNextImageKHR failed: VkResult={}", static_cast<int>(result));
            return AcquireResult{SwapchainStatus::NeedsRecreate, 0};
    }
}

SwapchainStatus Device::present(uint32_t imageIndex, VkSemaphore wait) {
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    if (wait != VK_NULL_HANDLE) {
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &wait;
    }
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    VkResult result = vkQueuePresentKHR(presentQueue_, &presentInfo);
    switch (result) {
        case VK_SUCCESS:
            return SwapchainStatus::Ok;
        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_SUBOPTIMAL_KHR:
            return SwapchainStatus::NeedsRecreate;
        case VK_ERROR_DEVICE_LOST:
            RX_LOG_ERROR("vkQueuePresentKHR: device lost");
            return SwapchainStatus::DeviceLost;
        default:
            RX_LOG_ERROR("vkQueuePresentKHR failed: VkResult={}", static_cast<int>(result));
            return SwapchainStatus::NeedsRecreate;
    }
}

bool Device::recreateSwapchain(VkSurfaceKHR surface) {
    if (surface != surface_) {
        // Not necessarily wrong (a caller could legitimately be handing
        // this Device a brand-new surface), but the intended/expected usage
        // is to pass the same surface handle back on every call -- this
        // Device does not take ownership of `surface` here and will not
        // destroy the previously-owned one, so silently swapping it in
        // could leak the old surface if that wasn't the caller's intent.
        RX_LOG_WARN(
            "Device::recreateSwapchain: surface handle changed (previously owned {}, now given {}); the "
            "previous surface will not be destroyed by this call",
            static_cast<void*>(surface_), static_cast<void*>(surface));
    }

    vkDeviceWaitIdle(device_);

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    swapchainImages_.clear();

    vkb::SwapchainBuilder swapchainBuilder(physicalDevice_, device_, surface);
    auto swapchainResult = swapchainBuilder.build();
    if (!swapchainResult) {
        RX_LOG_ERROR("vkb::SwapchainBuilder::build failed during recreate: {}", swapchainResult.error().message());
        return false;
    }
    vkb::Swapchain vkbSwapchain = swapchainResult.value();

    auto imagesResult = vkbSwapchain.get_images();
    if (!imagesResult) {
        RX_LOG_ERROR("vkb::Swapchain::get_images failed during recreate: {}", imagesResult.error().message());
        vkb::destroy_swapchain(vkbSwapchain);
        return false;
    }

    swapchain_ = vkbSwapchain.swapchain;
    swapchainImages_ = imagesResult.value();
    swapchainFormat_ = vkbSwapchain.image_format;
    swapchainExtent_ = vkbSwapchain.extent;
    surface_ = surface;
    return true;
}

}  // namespace rx::rhi
