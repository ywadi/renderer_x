#include <rx_rhi_vk/device.h>
#include <rx_core/log.h>
#include <VkBootstrap.h>
#include <array>
#include <utility>
#include <vector>

namespace rx::rhi {

namespace {

void destroySurface(const Context& context, VkSurfaceKHR surface) {
    vkb::destroy_surface(context.vkbInstance(), surface);
}

// One entry per bit this Device requires from VkPhysicalDeviceVulkan12Features
// [Phase 2 spec Fixed decision #5, R:B1/B2] -- kept as a table of
// (name, accessor) pairs rather than ten separate `if`s so the "loud
// startup error naming the missing feature" diagnostic below and the
// feature-request struct below it can never silently drift apart.
struct RequiredVulkan12Feature {
    const char* name;
    VkBool32 (*get)(const VkPhysicalDeviceVulkan12Features&);
};

constexpr std::array<RequiredVulkan12Feature, 10> kRequiredDescriptorIndexingFeatures{{
    {"descriptorIndexing", [](const VkPhysicalDeviceVulkan12Features& f) { return f.descriptorIndexing; }},
    {"runtimeDescriptorArray", [](const VkPhysicalDeviceVulkan12Features& f) { return f.runtimeDescriptorArray; }},
    {"descriptorBindingPartiallyBound",
     [](const VkPhysicalDeviceVulkan12Features& f) { return f.descriptorBindingPartiallyBound; }},
    {"descriptorBindingVariableDescriptorCount",
     [](const VkPhysicalDeviceVulkan12Features& f) { return f.descriptorBindingVariableDescriptorCount; }},
    {"descriptorBindingSampledImageUpdateAfterBind",
     [](const VkPhysicalDeviceVulkan12Features& f) { return f.descriptorBindingSampledImageUpdateAfterBind; }},
    {"descriptorBindingStorageImageUpdateAfterBind",
     [](const VkPhysicalDeviceVulkan12Features& f) { return f.descriptorBindingStorageImageUpdateAfterBind; }},
    {"descriptorBindingStorageBufferUpdateAfterBind",
     [](const VkPhysicalDeviceVulkan12Features& f) { return f.descriptorBindingStorageBufferUpdateAfterBind; }},
    {"descriptorBindingUpdateUnusedWhilePending",
     [](const VkPhysicalDeviceVulkan12Features& f) { return f.descriptorBindingUpdateUnusedWhilePending; }},
    {"shaderSampledImageArrayNonUniformIndexing",
     [](const VkPhysicalDeviceVulkan12Features& f) { return f.shaderSampledImageArrayNonUniformIndexing; }},
    {"shaderStorageBufferArrayNonUniformIndexing",
     [](const VkPhysicalDeviceVulkan12Features& f) { return f.shaderStorageBufferArrayNonUniformIndexing; }},
}};

// Called only after vkb::PhysicalDeviceSelector::select() has already
// failed, to turn its generic "no suitable device" error into a loud,
// specific one when the actual cause is a missing descriptor-indexing
// feature bit: vk-bootstrap's own error message never names which
// required feature a candidate device lacked, which is not good enough
// for a hard startup requirement [task-3-brief.md]. Enumerates every
// physical device this instance can see and, for each, reports by name
// any of kRequiredDescriptorIndexingFeatures it does not support. If no
// device is missing any of these bits, says so explicitly instead of
// staying silent -- the real cause of the selection failure is then
// something else entirely (surface support, queue families, the 1.1/1.3
// feature sets, API version), and misattributing it to descriptor
// indexing would send whoever reads this log down the wrong path.
void logDescriptorIndexingFeatureGaps(VkInstance instance) {
    uint32_t deviceCount = 0;
    if (vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) {
        RX_LOG_ERROR("Device::create: no Vulkan physical devices are visible to this instance at all");
        return;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()) != VK_SUCCESS) {
        return;
    }

    bool anyGapReported = false;
    for (VkPhysicalDevice physicalDevice : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);

        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

        for (const auto& required : kRequiredDescriptorIndexingFeatures) {
            if (!required.get(features12)) {
                RX_LOG_ERROR(
                    "Device::create: physical device '{}' is missing required descriptor-indexing "
                    "feature '{}' [Phase 2 spec Fixed decision #5, R:B1/B2]",
                    properties.deviceName, required.name);
                anyGapReported = true;
            }
        }
    }

    if (!anyGapReported) {
        RX_LOG_ERROR(
            "Device::create: physical device selection failed, but every visible device already "
            "supports all required descriptor-indexing features -- the actual cause is something "
            "else (surface support, queue families, Vulkan 1.1/1.3 feature requirements, or API "
            "version)");
    }
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

    // Descriptor-indexing feature set for bindless resources (Task 3 --
    // rx_rhi_vk/bindless.h's BindlessTable and PipelineLayoutBuilder's
    // (Task 2) unbounded-array bindings both require these enabled at
    // device-creation time) [Phase 2 spec Fixed decision #5, R:B1/B2].
    // None of these bits are guaranteed just by requiring Vulkan 1.3 --
    // every one remains an individually-optional feature -- but all ten
    // are confirmed present on this project's stated floor hardware (Steam
    // Deck RADV) via the Vulkan Roadmap 2022 profile, and on every desktop
    // GPU/driver this project has been developed and tested against
    // (verified directly: NVIDIA RTX 2080 proprietary driver, and
    // llvmpipe/lavapipe). set_required_features_12() below makes
    // vkb::PhysicalDeviceSelector::select() require all ten from any
    // candidate device; logDescriptorIndexingFeatureGaps() (this file,
    // above) turns a resulting selection failure into a loud, specific
    // error naming exactly which bit(s) are missing, rather than
    // vk-bootstrap's own generic "no suitable device" message.
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

    vkb::PhysicalDeviceSelector selector(context.vkbInstance());
    auto physResult = selector.set_surface(surface)
                          .set_minimum_version(1, 3)
                          .set_required_features_11(features11)
                          .set_required_features_12(features12)
                          .set_required_features_13(features13)
                          .select();
    if (!physResult) {
        RX_LOG_ERROR("vkb::PhysicalDeviceSelector::select failed: {}", physResult.error().message());
        logDescriptorIndexingFeatureGaps(context.instance());
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
