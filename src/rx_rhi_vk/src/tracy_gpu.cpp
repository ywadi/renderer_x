#include <rx_rhi_vk/tracy_gpu.h>

#include <rx_core/log.h>
#include <rx_rhi_vk/device.h>

namespace rx::rhi {

#ifdef TRACY_ENABLE

GpuProfileContext createGpuProfileContext(Device& device) {
    // Short-lived setup pool/buffer for the calibration submissions
    // TracyVkContext(Calibrated)'s own constructor issues synchronously --
    // see this function's own header comment (tracy_gpu.h) for why neither
    // is retained past this call.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = device.graphicsQueueFamily();

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device.device(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_rhi_vk: createGpuProfileContext: vkCreateCommandPool failed");
        return nullptr;
    }

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = pool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device.device(), &cmdAllocInfo, &cmd) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_rhi_vk: createGpuProfileContext: vkAllocateCommandBuffers failed");
        vkDestroyCommandPool(device.device(), pool, nullptr);
        return nullptr;
    }

    // Direct-symbol form (TRACY_VK_USE_SYMBOL_TABLE is never defined by this
    // project): Tracy calls bare `vkFoo(...)` names internally, which
    // resolve to volk's own already-loaded global function-pointer
    // variables of the identical names -- exactly the calling convention
    // every other raw Vulkan call in this codebase already relies on under
    // VK_NO_PROTOTYPES + volk (e.g. executor.cpp's vkCmdBeginDebugUtilsLabelEXT
    // null-checked-before-use pattern). `vkGetPhysicalDeviceCalibrateableTimeDomainsEXT`/
    // `vkGetCalibratedTimestampsEXT` are volk-loaded globals too (loaded by
    // volkLoadInstance()/volkLoadDevice(), both already called before any
    // Device exists -- see context.cpp/device.cpp), supplied directly by
    // name per the manual's own "retrieve the following function pointers"
    // instruction (Tracy user manual, "Calibrated context").
    GpuProfileContext ctx = nullptr;
    if (device.calibratedTimestampsEnabled()) {
        ctx = TracyVkContextCalibrated(device.physicalDevice(), device.device(), device.graphicsQueue(), cmd,
                                        vkGetPhysicalDeviceCalibrateableTimeDomainsEXT, vkGetCalibratedTimestampsEXT);
        RX_LOG_INFO("rx_rhi_vk: Tracy GPU context created with calibrated timestamps (VK_EXT_calibrated_timestamps)");
    } else {
        ctx = TracyVkContext(device.physicalDevice(), device.device(), device.graphicsQueue(), cmd);
        RX_LOG_INFO("rx_rhi_vk: Tracy GPU context created without calibrated timestamps (extension not enabled)");
    }

    // The command buffer TracyVkContext(Calibrated) just consumed is left
    // in the executable state (its own manual's own documented contract);
    // safe to free/destroy the whole temporary pool now regardless.
    vkFreeCommandBuffers(device.device(), pool, 1, &cmd);
    vkDestroyCommandPool(device.device(), pool, nullptr);

    if (ctx == nullptr) {
        RX_LOG_ERROR("rx_rhi_vk: createGpuProfileContext: Tracy context creation returned null");
    }
    return ctx;
}

#else

GpuProfileContext createGpuProfileContext(Device& /*device*/) { return nullptr; }

#endif  // TRACY_ENABLE

}  // namespace rx::rhi
