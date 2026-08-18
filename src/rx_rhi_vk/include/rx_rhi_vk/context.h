#pragma once
#include <volk.h>
// vkb::Instance is stored by value below, so its full definition is
// required here -- a forward declaration is not possible for a by-value
// member.
#include <VkBootstrap.h>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace rx::rhi {

class Context {
public:
    Context(Context&&) noexcept;
    Context& operator=(Context&&) noexcept;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context();

    // WARNING -- process-wide vk-bootstrap caching hazard, not specific to
    // this Context: the pinned vk-bootstrap commit
    // (556b79b165386f6c1a18362d30f2a076fdaa2778) resolves instance-level
    // Vulkan function pointers (vkGetPhysicalDeviceSurfaceSupportKHR and
    // friends for PhysicalDeviceSelector; vkCreateDebugUtilsMessengerEXT /
    // vkDestroyDebugUtilsMessengerEXT for enableValidation=true; likely
    // others too -- anything resolved through its internal `detail::
    // vulkan_functions()` singleton) from whichever vkb::Instance is built
    // *first* in the process, and never refreshes them afterward -- there
    // is no public API to reset the cache, and destroying that first
    // Instance does not do it either. Building a Context with a narrower
    // capability set than a later one needs -- fewer extensions (e.g.
    // headless, no VK_KHR_surface) *or* enableValidation=false when a later
    // Context in the same process needs true -- poisons that later
    // Context/Device's calls with null function pointers cached from the
    // first, narrower instance. Depending on which function pointer is
    // null, this shows up either as a segfault (confirmed via gdb: null
    // fp_vkGetPhysicalDeviceSurfaceSupportKHR, VkBootstrap.cpp:1103, when a
    // later PhysicalDeviceSelector needs surface support the first instance
    // never enabled) or as a clean-looking `Context::create` failure
    // ("failed_create_debug_messenger", when a later enableValidation=true
    // Context needs a debug messenger the first, unvalidated instance never
    // requested). rx_rhi_vk_tests' tests/doctest_main.cpp works around this
    // today by warming the cache with the single broadest instance (real
    // window extensions when available, enableValidation=true) the test
    // binary needs, before any test runs. There is no equivalent guard in
    // production code: whoever implements Context/Device recreation
    // (device-lost recovery, editor hot-reload, or any other flow that
    // could build more than one Context across a single process's
    // lifetime) needs to either keep every Context's capabilities
    // (extensions *and* enableValidation) consistent, or replicate that
    // same warm-up-with-the-broadest-instance-first pattern.
    static std::optional<Context> create(std::vector<const char*> requiredExtensions, bool enableValidation);

    VkInstance instance() const { return vkbInstance_.instance; }
    VkDebugUtilsMessengerEXT debugMessenger() const { return vkbInstance_.debug_messenger; }
    const vkb::Instance& vkbInstance() const { return vkbInstance_; }
    bool hasValidationErrors() const { return *errorCount_ > 0; }

    // Process-lifetime tally of unfiltered validation errors observed from
    // debugCallback() across EVERY Context ever created in this process --
    // deliberately NOT scoped to any one Context/errorCount_ instance the
    // way hasValidationErrors() above is. A GPU test fixture's own
    // CHECK_FALSE(fixture->context.hasValidationErrors()) only observes
    // errors raised before that line runs; a validation error raised while
    // the fixture's OWN destructor chain tears down afterward (the
    // sampler-leak / unflushed-upload class -- VUID-vkDestroyDevice-
    // device-00378 and friends) still prints via debugCallback's
    // RX_LOG_ERROR, but nothing re-checks THAT Context's (now-dying)
    // errorCount_ again, and once the fixture is fully destroyed
    // errorCount_ itself is gone. This counter survives that: it is read
    // by every GPU test binary's main() (each doctest_main*.cpp under
    // src/*/tests/) AFTER doctest::Context::run() returns -- i.e. after
    // every TEST_CASE's fixture, including the very last one, has already
    // been fully destroyed -- to catch exactly that class of teardown-time
    // error and force a nonzero process exit code. Filtering (what counts
    // as a known false positive vs. a real error) is unchanged: this only
    // tallies the SAME real-error branch that already increments a
    // per-Context errorCount_ in context.cpp's debugCallback().
    static std::size_t processValidationErrorCount();

private:
    Context(vkb::Instance vkbInstance, std::shared_ptr<int> errorCount)
        : vkbInstance_(std::move(vkbInstance)), errorCount_(std::move(errorCount)) {}

    vkb::Instance vkbInstance_{};
    std::shared_ptr<int> errorCount_;
};

}  // namespace rx::rhi
