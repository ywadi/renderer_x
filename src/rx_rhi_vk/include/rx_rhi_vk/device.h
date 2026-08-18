#pragma once
#include <rx_rhi_vk/context.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace rx::rhi {

enum class SwapchainStatus {
    Ok,
    NeedsRecreate,
    DeviceLost,
};

struct AcquireResult {
    SwapchainStatus status;
    uint32_t imageIndex;
};

// Present-mode control [Phase 4 Task 6, spec seed 1]. Only two choices are
// exposed at this layer -- an on/off vsync preference, not a raw
// VkPresentModeKHR -- because the actual mode is driver/surface-dependent
// (see Device::setPresentMode()'s own comment for the exact fallback
// ladder each of these resolves to). Callers that need the concrete mode
// actually in use (for logging or a HUD) read it back via
// Device::presentMode() instead of assuming one from this enum.
enum class PresentMode {
    VsyncOn,
    VsyncOff,
};

// Human-readable name for a VkPresentModeKHR, for logging/HUD use (e.g. a
// sample printing Device::presentMode() once at startup). Covers every
// value Device's own present-mode ladder can select (FIFO, MAILBOX,
// IMMEDIATE) plus FIFO_RELAXED for completeness, with "UNKNOWN" as the
// fallback for anything else (e.g. the shared-present extension modes this
// codebase never requests).
const char* presentModeName(VkPresentModeKHR mode);

// Device owns the logical VkDevice selected/built against a Context's
// vkb::Instance, its graphics and present queues, and a VkSwapchainKHR built
// against a caller-provided VkSurfaceKHR.
//
// Device::create() takes ownership of the VkSurfaceKHR passed to it,
// unconditionally: on success the returned Device owns and destroys it
// (together with the swapchain and device) on destruction or move-assign;
// on failure Device::create destroys the surface itself before returning
// std::nullopt. Either way, the caller must not destroy that surface handle
// itself once create() has been called with it.
//
// Thread-affinity (D5, Phase 4): create()/acquireNextImage()/present()/
// setPresentMode()/recreateSwapchain() are main-thread-only -- this Device
// owns the swapchain/present loop's own state, the same convention every
// existing caller of these methods (every sample's frame loop) already
// follows. Every other accessor (physicalDevice()/device()/queues/
// memoryBudgetExtensionEnabled()/calibratedTimestampsEnabled()/etc.) is a
// read-only snapshot, safe to call from any thread holding a valid
// reference -- see docs/threading.md.
class Device {
public:
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

    static std::optional<Device> create(Context& context, VkSurfaceKHR surface);

    // Task 3 (rx_graph's Executor): the same VkInstance handle this Device
    // was built against (Context::instance(), stashed at create() time --
    // see the private instance_ member below). Added because
    // Executor::create(Device&) needs to build its own
    // rx::rhi::Allocator::createRaw(physicalDevice, device, instance) from
    // a Device alone, with no separate Context& in its signature, and
    // every other accessor on this class already exposes exactly this
    // kind of already-stored handle (physicalDevice(), device(), ...) the
    // same trivial way.
    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    uint32_t graphicsQueueFamily() const { return graphicsQueueFamily_; }
    VkQueue presentQueue() const { return presentQueue_; }

    // Optional DEDICATED transfer queue [Phase 4 Task 11, spec D25, gate
    // ruling #28 row 10]: acquired via vk-bootstrap's
    // `get_dedicated_queue(QueueType::transfer)` -- NEVER
    // `require_dedicated_transfer_queue()`, which would make PHYSICAL
    // DEVICE SELECTION itself fail outright on hardware lacking one (the
    // wrong primitive for an optional/fallback feature; verified against
    // vk-bootstrap's own `has_dedicated_transfer_queue()` definition: a
    // family that supports transfer but NOT graphics/compute, which many
    // devices -- including this project's own dev/CI hardware -- simply
    // do not expose). A missing dedicated queue is not a Device::create()
    // failure; it is a graceful, logged degrade (see create()) --
    // `hasDedicatedTransferQueue()` reports which case this Device landed
    // in; `transferQueue()`/`transferQueueFamily()` are only meaningful
    // when it returns true (VK_NULL_HANDLE/0 otherwise -- callers must
    // check the flag first, not assume a nonzero handle).
    //
    // ACQUISITION ONLY this phase -- nothing in this codebase submits to
    // transferQueue() yet (cross-queue submission and queue-family
    // ownership-transfer barriers stay streaming-phase policy, per the
    // toolchain-platform-rhi-design registry's "Upload/transfer asynchrony
    // policy" entry). The accessor existing and being correctly populated
    // (or correctly absent) is this phase's entire acceptance bar.
    bool hasDedicatedTransferQueue() const { return hasDedicatedTransferQueue_; }
    VkQueue transferQueue() const { return transferQueue_; }
    uint32_t transferQueueFamily() const { return transferQueueFamily_; }

    // True iff VK_EXT_calibrated_timestamps was present on the selected
    // physical device AND successfully enabled on the logical device this
    // Device wraps (see device.cpp's own comment on the optional, guarded
    // enable_extension_if_present() call in create()) [Phase 4 Stage 0
    // Task 3]. This class has no idea Tracy exists -- it only reports
    // whether this one, ordinarily-optional Vulkan extension is live;
    // rx_rhi_vk/tracy_gpu.h is the sole consumer that gives this fact any
    // profiling meaning (TracyVkContextCalibrated vs plain TracyVkContext).
    bool calibratedTimestampsEnabled() const { return calibratedTimestampsEnabled_; }

    // True iff `VK_EXT_memory_budget` was present on the selected physical
    // device AND successfully enabled on the logical device this Device
    // wraps [Phase 4 Task 10, spec D24(b), gate ruling #27] -- same
    // opportunistic, guarded `enable_extension_if_present()` pattern as
    // calibratedTimestampsEnabled() just above (see device.cpp's own
    // comment on that call). rx::rhi::Allocator::create(Context&, Device&)
    // (buffer.h) reads this to decide whether to request
    // VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT -- the two must stay in
    // LOCKSTEP. This is enforced entirely by THIS being the single source
    // of truth Allocator::create() always reads directly (buffer.h has the
    // full correction on why VMA itself does not runtime-enforce this),
    // rather than Allocator guessing or re-querying the extension itself.
    bool memoryBudgetExtensionEnabled() const { return memoryBudgetExtensionEnabled_; }

    VkSwapchainKHR swapchain() const { return swapchain_; }
    const std::vector<VkImage>& swapchainImages() const { return swapchainImages_; }
    VkFormat swapchainFormat() const { return swapchainFormat_; }
    VkExtent2D swapchainExtent() const { return swapchainExtent_; }

    // Records the present mode this Device should build its NEXT swapchain
    // against. Does NOT itself touch the live swapchain or recreate
    // anything -- recreateSwapchain() (the same path every caller already
    // drives on SwapchainStatus::NeedsRecreate, e.g. a window resize) is
    // the sole place a present-mode change actually takes effect, per this
    // task's explicit constraint against inventing a second recreation
    // flow. A caller that wants the change applied immediately (e.g. a
    // sample honoring a --vsync flag at startup, before its first frame)
    // must call recreateSwapchain(surface) itself right after this call.
    //
    // The mode actually built is resolved from `mode` against what this
    // surface/device supports, via the ladder in device.cpp's
    // selectPresentMode(): VsyncOn always resolves to FIFO (the one
    // present mode the Vulkan spec guarantees for every surface).
    // VsyncOff prefers MAILBOX (uncapped, no tearing), then IMMEDIATE
    // (uncapped, tearing possible), and only falls back to FIFO -- logging
    // a one-line warning when it does, since that silently keeps vsync
    // effectively on despite the caller asking for it off -- if this
    // surface supports neither.
    void setPresentMode(PresentMode mode);

    // The actual VkPresentModeKHR currently in use, as reported back by
    // vk-bootstrap after the ladder above resolved the most recent
    // setPresentMode() request (or, before any setPresentMode() call, the
    // explicit FIFO default create() builds every Device with -- see that
    // function's own comment). For logging/HUD use; presentModeName()
    // above turns this into a readable string.
    VkPresentModeKHR presentMode() const { return actualPresentMode_; }

    // Acquires the next available swapchain image, signaling `signal` once
    // it is safe to render into it. VK_SUBOPTIMAL_KHR is treated as success
    // here: the image is still valid to render into and present, so
    // recreation is deferred to present()'s return value instead.
    AcquireResult acquireNextImage(VkSemaphore signal);

    // Presents `imageIndex`, waiting on `wait` first unless it is
    // VK_NULL_HANDLE.
    SwapchainStatus present(uint32_t imageIndex, VkSemaphore wait);

    // Waits for the device to go idle, destroys the current swapchain, and
    // rebuilds it against `surface` (refreshing images/format/extent).
    // Does not take ownership of `surface` and does not touch the surface
    // this Device already owns; in the expected usage the same surface
    // handle is passed again on every call.
    //
    // Also the sole place the present mode most recently recorded via
    // setPresentMode() is actually applied [Phase 4 Task 6] -- every
    // caller of this function (resize/NeedsRecreate handling, or a sample
    // applying --vsync at startup) gets that behavior for free, with no
    // separate "apply present mode" path to call.
    bool recreateSwapchain(VkSurfaceKHR surface);

private:
    Device() = default;

    void destroyAll();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily_ = 0;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkQueue transferQueue_ = VK_NULL_HANDLE;
    uint32_t transferQueueFamily_ = 0;
    bool hasDedicatedTransferQueue_ = false;
    bool calibratedTimestampsEnabled_ = false;
    bool memoryBudgetExtensionEnabled_ = false;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{0, 0};

    // Desired mode recorded by setPresentMode(), consulted by
    // recreateSwapchain() (and create()'s own equivalent inline logic) the
    // next time either builds a swapchain. Defaults to VsyncOn to match
    // create()'s explicit FIFO default -- see that function's comment.
    PresentMode desiredPresentMode_ = PresentMode::VsyncOn;
    // What the ladder actually resolved `desiredPresentMode_` to, as
    // reported back by vk-bootstrap. Returned by presentMode().
    VkPresentModeKHR actualPresentMode_ = VK_PRESENT_MODE_FIFO_KHR;
};

}  // namespace rx::rhi
