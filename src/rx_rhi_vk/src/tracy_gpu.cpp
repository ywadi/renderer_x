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
    //
    // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT [hotfix, found post-
    // closure by Task 4's implementer -- VUID-vkBeginCommandBuffer-
    // commandBuffer-00050]: load-bearing, not a style choice. Read
    // directly against the vendored v0.14.0 TracyVulkan.hpp before fixing
    // this: VkCtx's constructor (both the plain and the calibrated
    // overload) ALWAYS issues one begin/end/submit/waitIdle cycle on
    // `cmd` first (for CreateQueryPool()'s own reset), then -- ONLY when
    // `m_timeDomain` is still VK_TIME_DOMAIN_DEVICE_EXT at that point,
    // i.e. FindAvailableTimeDomains() did NOT find a host-comparable
    // domain (VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT on Linux) among
    // whatever vkGetPhysicalDeviceCalibrateableTimeDomainsEXT reported for
    // THIS physical device -- re-begins the SAME `cmd` two more times in a
    // row for the timestamp-write and re-reset steps. A command buffer
    // that has already reached the executable state (every begin after
    // the first) can only be re-begun without an explicit
    // vkResetCommandBuffer/vkResetCommandPool call if the POOL it came
    // from carries this flag (Vulkan spec: "If commandBuffer was
    // allocated from a VkCommandPool which did not have the
    // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT flag set,
    // commandBuffer must be in the initial state"). This is exactly
    // Tracy's own documented contract for the buffer it's handed ("must
    // be in the initial state and be able to be reset... will rerecord
    // and submit it to the queue multiple times") -- the fix is the flag
    // this contract asks for, not a workaround.
    //
    // NOT fixed by resetting the pool ourselves "per collect cycle"
    // instead: this pool is scoped to ONE createGpuProfileContext() call
    // (created, used, destroyed within this function, well before any
    // real per-frame RX_GPU_COLLECT ever runs on a caller-owned command
    // buffer elsewhere) -- there is no cycle of OUR OWN to insert a reset
    // into. Every extra vkBeginCommandBuffer call above happens INSIDE
    // Tracy's own constructor, synchronously, with no seam this project's
    // code could hook a reset into even if it wanted to; the pool's own
    // creation flag is the only lever available, and it is also the
    // textbook-correct one for a command buffer a library reuses
    // internally without the caller's involvement.
    //
    // Whether this branch is ever taken is driver-dependent, not always
    // exercised: confirmed directly on this dev machine that the NVIDIA
    // proprietary driver DOES report VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT
    // (taking the single-begin, bug-free path), while Mesa's llvmpipe/
    // lavapipe software rasterizer does NOT (only VK_TIME_DOMAIN_DEVICE_EXT,
    // forcing the multi-begin path this flag fixes) -- see
    // task-3-report.md's hotfix section for the reproduction and why this
    // task's own original verification missed it.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
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
