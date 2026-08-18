#include <rx_rhi_vk/device.h>
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <VkBootstrap.h>
#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace rx::rhi {

const char* presentModeName(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_FIFO_KHR:
            return "FIFO";
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return "MAILBOX";
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return "IMMEDIATE";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return "FIFO_RELAXED";
        default:
            return "UNKNOWN";
    }
}

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

// Present-mode fallback ladder [Phase 4 Task 6, spec seed 1, R:present].
// Resolves `desired` against the present modes this physical device
// actually reports supporting for `surface` (queried directly via
// vkGetPhysicalDeviceSurfacePresentModesKHR, per this task's resolution --
// VK_KHR_surface is always enabled at instance level by the time this
// runs, since every Device is built from a real windowing surface, so the
// function pointer is already loaded by Context::create()'s
// volkLoadInstance() call).
//
// PresentMode::VsyncOn always resolves to FIFO with no query needed: FIFO
// is the one present mode the Vulkan spec guarantees every surface
// supports (spec 34.1, "Presentation Mode Support"), so asking the driver
// first would only ever confirm what the spec already promises.
//
// PresentMode::VsyncOff prefers MAILBOX (uncapped framerate, no tearing),
// then IMMEDIATE (uncapped, tearing possible), and only falls back to
// FIFO -- logging a one-line warning when it does, since that silently
// keeps vsync effectively on despite the caller asking for it off -- when
// this surface supports neither. The returned mode is always one this
// surface has already been confirmed to support, so the caller can hand
// it straight to vkb::SwapchainBuilder::set_desired_present_mode()
// without relying on vk-bootstrap's own (different, undocumented-here)
// fallback behavior.
VkPresentModeKHR selectPresentMode(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, PresentMode desired) {
    if (desired == PresentMode::VsyncOn) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> supportedModes(modeCount);
    if (modeCount > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &modeCount, supportedModes.data());
    }

    auto supports = [&supportedModes](VkPresentModeKHR mode) {
        return std::find(supportedModes.begin(), supportedModes.end(), mode) != supportedModes.end();
    };

    if (supports(VK_PRESENT_MODE_MAILBOX_KHR)) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (supports(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    RX_LOG_WARN(
        "Device: PresentMode::VsyncOff requested but this surface supports neither MAILBOX nor IMMEDIATE -- "
        "falling back to FIFO (vsync stays effectively on)");
    return VK_PRESENT_MODE_FIFO_KHR;
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
    // timelineSemaphore [Phase 4 Task 11, spec D25 as amended by gate
    // ruling RC4]: rx::rhi::Uploader's ticket-completion primitive
    // (upload.cpp) needs VK_SEMAPHORE_TYPE_TIMELINE, which -- despite
    // `VK_KHR_timeline_semaphore`'s functionality being fully promoted
    // into Vulkan 1.2 core, confirmed against the Khronos registry man
    // page -- is still a FEATURE BIT that must be explicitly requested at
    // device-creation time, not something API version alone turns on
    // (`vkCreateSemaphore` with `VK_SEMAPHORE_TYPE_TIMELINE` otherwise
    // hard-fails VUID-VkSemaphoreTypeCreateInfo-timelineSemaphore-03252 --
    // caught empirically by this task's own test suite before this line
    // existed). Required unconditionally, matching every other bit in this
    // struct: verified present on both this project's own dev drivers
    // (NVIDIA proprietary and Mesa llvmpipe/lavapipe, `vulkaninfo`) and
    // mandated by the Vulkan Roadmap 2022 profile this project's Steam
    // Deck RADV floor hardware already targets -- no optionality/fallback
    // needed the way VK_EXT_calibrated_timestamps/VK_EXT_memory_budget
    // (genuinely optional extensions, not core-promoted feature bits) get
    // below.
    features12.timelineSemaphore = VK_TRUE;

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

    // VK_EXT_calibrated_timestamps: optional, guarded -- enabled at device
    // creation only when the selected physical device actually supports it
    // [Phase 4 Stage 0 Task 3, spec D3]. `enable_extension_if_present()` is
    // vk-bootstrap's own documented "make it enabled on the device IF
    // present, otherwise a no-op" call (VkBootstrap.h) -- this is not a
    // required feature (unlike the Vulkan 1.1/1.2/1.3 feature sets above):
    // a device lacking it is still fully supported, just without GPU-side
    // calibrated Tracy zones (rx_rhi_vk/tracy_gpu.h falls back to plain
    // TracyVkContext in that case -- see that file's own comment). This
    // class has no idea Tracy exists; it only reports the plain fact via
    // calibratedTimestampsEnabled() below. Empirically checked directly on
    // this dev machine's two available drivers as part of this task: BOTH
    // the NVIDIA proprietary driver and Mesa's llvmpipe/lavapipe software
    // rasterizer report `VK_EXT_calibrated_timestamps` (verified via
    // `vulkaninfo`) -- lavapipe support was flagged [R:threading] as
    // "unverified" going into this task; it is verified present here.
    const bool calibratedTimestampsEnabled =
        physResult.value().enable_extension_if_present(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

    // VK_EXT_memory_budget: optional, guarded, same pattern as
    // calibratedTimestampsEnabled directly above [Phase 4 Task 10, spec
    // D24(b), gate ruling #27]. A device lacking it is still fully
    // supported -- rx::rhi::Allocator (buffer.h/.cpp) falls back to a
    // heap-size budget estimate (MemoryBudgetSource::HeapSizeFallback)
    // instead of the real driver-reported budget. Its instance-level
    // dependency (VK_KHR_get_physical_device_properties2, or core Vulkan
    // >= 1.1) is already satisfied unconditionally: this file's `selector`
    // above requires >= 1.3 (set_minimum_version(1, 3)), and
    // Context::create() (context.cpp) requires instance API version 1.3.0
    // too (`require_api_version(1, 3, 0)`) -- both comfortably subsume the
    // 1.1 floor, so no separate instance extension needs enabling here.
    // Empirically verified present on both drivers this project develops
    // against (vulkaninfo, `VK_ICD_FILENAMES` pointed at each ICD in
    // isolation): NVIDIA's proprietary driver AND Mesa llvmpipe/lavapipe
    // (the software rasterizer CI runs against) both report
    // `VK_EXT_memory_budget` -- so the RealExtension path, not just the
    // fallback, is exercised for real in CI, not merely local dev.
    const bool memoryBudgetExtensionEnabled =
        physResult.value().enable_extension_if_present(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

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

    // Optional dedicated transfer queue [Phase 4 Task 11, spec D25, gate
    // ruling #28 row 10]: `get_dedicated_queue()` fails gracefully via its
    // own `Result` (not a hard PhysicalDeviceSelector-level requirement,
    // unlike `require_dedicated_transfer_queue()`, which this task
    // deliberately never calls) on hardware with no queue family that
    // supports transfer but NOT graphics/compute. A missing dedicated
    // queue is not a Device::create() failure -- it is a graceful, logged
    // degrade (below); every caller falls back to graphicsQueue() for any
    // future transfer work, not exercised this phase (acquisition only --
    // see device.h's own comment on hasDedicatedTransferQueue()).
    VkQueue transferQueue = VK_NULL_HANDLE;
    uint32_t transferQueueFamily = 0;
    bool hasDedicatedTransferQueue = false;
    auto transferQueueResult = vkbDevice.get_dedicated_queue(vkb::QueueType::transfer);
    auto transferQueueIndexResult = vkbDevice.get_dedicated_queue_index(vkb::QueueType::transfer);
    if (transferQueueResult && transferQueueIndexResult) {
        transferQueue = transferQueueResult.value();
        transferQueueFamily = transferQueueIndexResult.value();
        hasDedicatedTransferQueue = true;
    }

    vkb::SwapchainBuilder swapchainBuilder(vkbDevice, surface);
    // Present-mode control [Phase 4 Task 6, spec seed 1]: explicit FIFO
    // default (PresentMode::VsyncOn), not vk-bootstrap's own implicit
    // preference (MAILBOX if the surface supports it, else FIFO). This is
    // a BEHAVIORAL CHANGE from every Device built before this task: a
    // sample that happened to run against a MAILBOX-capable surface
    // previously got MAILBOX with no code anywhere having chosen it; now
    // it gets FIFO unless something explicitly opts out via
    // setPresentMode(PresentMode::VsyncOff) + recreateSwapchain() (see
    // those methods' own comments) -- e.g. a sample's --vsync off flag.
    // No availability query is needed for FIFO specifically -- see
    // selectPresentMode()'s own comment above on why the spec already
    // guarantees it for every surface.
    swapchainBuilder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
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
    dev.transferQueue_ = transferQueue;
    dev.transferQueueFamily_ = transferQueueFamily;
    dev.hasDedicatedTransferQueue_ = hasDedicatedTransferQueue;
    RX_LOG_INFO("Device::create: dedicated transfer queue {} [Phase 4 Task 11, spec D25] (acquisition only -- "
                "nothing submits to it this phase)",
                hasDedicatedTransferQueue
                    ? "ACQUIRED"
                    : "not present on this physical device -- falling back to the graphics queue for any future "
                      "transfer work");
    dev.calibratedTimestampsEnabled_ = calibratedTimestampsEnabled;
    RX_LOG_INFO("Device::create: VK_EXT_calibrated_timestamps {} on the selected physical device",
                calibratedTimestampsEnabled ? "ENABLED" : "not present -- falling back to uncalibrated GPU zones");
    dev.memoryBudgetExtensionEnabled_ = memoryBudgetExtensionEnabled;
    RX_LOG_INFO("Device::create: VK_EXT_memory_budget {} on the selected physical device",
                memoryBudgetExtensionEnabled ? "ENABLED" : "not present -- memory reports fall back to a heap-size "
                                                            "budget estimate");
    dev.swapchain_ = vkbSwapchain.swapchain;
    dev.swapchainImages_ = imagesResult.value();
    dev.swapchainFormat_ = vkbSwapchain.image_format;
    dev.swapchainExtent_ = vkbSwapchain.extent;
    dev.desiredPresentMode_ = PresentMode::VsyncOn;
    dev.actualPresentMode_ = vkbSwapchain.present_mode;
    RX_LOG_INFO("Device::create: present mode in use: {} (explicit default, PresentMode::VsyncOn)",
                presentModeName(dev.actualPresentMode_));
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
        transferQueue_ = other.transferQueue_;
        transferQueueFamily_ = other.transferQueueFamily_;
        hasDedicatedTransferQueue_ = other.hasDedicatedTransferQueue_;
        calibratedTimestampsEnabled_ = other.calibratedTimestampsEnabled_;
        memoryBudgetExtensionEnabled_ = other.memoryBudgetExtensionEnabled_;
        swapchain_ = other.swapchain_;
        swapchainImages_ = std::move(other.swapchainImages_);
        swapchainFormat_ = other.swapchainFormat_;
        swapchainExtent_ = other.swapchainExtent_;
        desiredPresentMode_ = other.desiredPresentMode_;
        actualPresentMode_ = other.actualPresentMode_;

        other.instance_ = VK_NULL_HANDLE;
        other.physicalDevice_ = VK_NULL_HANDLE;
        other.device_ = VK_NULL_HANDLE;
        other.surface_ = VK_NULL_HANDLE;
        other.graphicsQueue_ = VK_NULL_HANDLE;
        other.graphicsQueueFamily_ = 0;
        other.presentQueue_ = VK_NULL_HANDLE;
        other.transferQueue_ = VK_NULL_HANDLE;
        other.transferQueueFamily_ = 0;
        other.hasDedicatedTransferQueue_ = false;
        other.calibratedTimestampsEnabled_ = false;
        other.memoryBudgetExtensionEnabled_ = false;
        other.swapchain_ = VK_NULL_HANDLE;
        other.swapchainImages_.clear();
        other.swapchainFormat_ = VK_FORMAT_UNDEFINED;
        other.swapchainExtent_ = VkExtent2D{0, 0};
        other.desiredPresentMode_ = PresentMode::VsyncOn;
        other.actualPresentMode_ = VK_PRESENT_MODE_FIFO_KHR;
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
    // FrameSync's frame loop begins here every iteration [Phase 4 Stage 0
    // Task 3, spec D3: "FrameSync acquire/present" zone placement --
    // FrameSync itself (frame_sync.h/.cpp) owns no acquire/present method
    // of its own; these two Device functions are the real acquire/present
    // calls every sample's present loop drives its FrameSync fences/
    // semaphores around].
    RX_ZONE;
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
    // See acquireNextImage()'s own comment -- the matching "present" half
    // of FrameSync's own acquire/present zone pair.
    RX_ZONE;
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

void Device::setPresentMode(PresentMode mode) {
    // See this method's own header comment: this only records `mode` for
    // the next recreateSwapchain() call to apply -- no swapchain touched
    // here, no second recreation flow invented.
    desiredPresentMode_ = mode;
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

    // Present-mode control [Phase 4 Task 6]: resolve the mode most
    // recently recorded via setPresentMode() (VsyncOn by default, matching
    // create()'s own default) against what this surface actually supports
    // -- see selectPresentMode()'s comment for the exact ladder -- and
    // hand the already-verified-available result straight to
    // set_desired_present_mode(). This is the sole place that ladder
    // executes for a live Device: create() above inlines the VsyncOn-only
    // half of it (no query needed for FIFO), and every present-mode change
    // after creation, whether from a real resize/NeedsRecreate or a
    // caller-driven setPresentMode() + recreateSwapchain() pair, flows
    // through here.
    VkPresentModeKHR selectedPresentMode = selectPresentMode(physicalDevice_, surface, desiredPresentMode_);

    vkb::SwapchainBuilder swapchainBuilder(physicalDevice_, device_, surface);
    swapchainBuilder.set_desired_present_mode(selectedPresentMode);
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
    actualPresentMode_ = vkbSwapchain.present_mode;
    RX_LOG_INFO("Device::recreateSwapchain: present mode in use: {} ({})", presentModeName(actualPresentMode_),
                desiredPresentMode_ == PresentMode::VsyncOn ? "PresentMode::VsyncOn" : "PresentMode::VsyncOff");
    return true;
}

}  // namespace rx::rhi
