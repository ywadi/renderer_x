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
    VkSwapchainKHR swapchain() const { return swapchain_; }
    const std::vector<VkImage>& swapchainImages() const { return swapchainImages_; }
    VkFormat swapchainFormat() const { return swapchainFormat_; }
    VkExtent2D swapchainExtent() const { return swapchainExtent_; }

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

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{0, 0};
};

}  // namespace rx::rhi
