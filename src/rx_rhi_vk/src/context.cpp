#include <rx_rhi_vk/context.h>
#include <rx_core/log.h>
#include <VkBootstrap.h>
#include <memory>
#include <string_view>

namespace rx::rhi {

namespace {

// Known false positive, narrowly scoped: vk-bootstrap's InstanceBuilder
// unconditionally enables VK_KHR_portability_enumeration (the extension and
// the corresponding VkInstanceCreateInfo::flags bit) whenever the Vulkan
// *loader* reports it available, with no InstanceBuilder API to opt out
// (checked directly against vk-bootstrap's header: only
// PhysicalDeviceSelector::disable_portability_subset() exists, a different,
// device-level knob -- nothing at the instance level). On a host whose
// installed VK_LAYER_KHRONOS_validation predates that extension (verified:
// this machine's apt-packaged layer is 1.3.204, with no newer apt candidate
// available), the layer doesn't recognize it and reports two messages for
// this one root cause:
//   1. a WARNING that the extension "is not supported by this layer"
//   2. an ERROR misreporting VkInstanceCreateInfo::flags as invalid
//      (VUID-VkInstanceCreateInfo-flags-zerobitmask), because the layer
//      doesn't know the extension defines a legal nonzero flag bit
// Both are matched on their distinctive, specific text/VUID -- not a broad
// "ignore anything portability-related" -- so a genuine, different
// validation failure is never silently swallowed. (VUID_Undefined, the
// warning's own "VUID", is a generic Khronos placeholder used by many
// unrelated informational messages, so it is deliberately NOT used as a
// match key on its own; the extension name plus its exact phrase is used
// instead.)
bool isKnownPortabilityEnumerationLayerBug(const char* message) {
    if (message == nullptr) {
        return false;
    }
    const std::string_view msg(message);
    if (msg.find("VUID-VkInstanceCreateInfo-flags-zerobitmask") != std::string_view::npos) {
        return true;
    }
    return msg.find("VK_KHR_portability_enumeration") != std::string_view::npos &&
           msg.find("is not supported by this layer") != std::string_view::npos;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* userData) {
    auto* errorCount = static_cast<int*>(userData);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (isKnownPortabilityEnumerationLayerBug(data->pMessage)) {
            RX_LOG_WARN("[vulkan validation] (known false positive: validation layer predates VK_KHR_portability_enumeration) {}", data->pMessage);
        } else {
            RX_LOG_ERROR("[vulkan validation] {}", data->pMessage);
            (*errorCount)++;
        }
    } else {
        RX_LOG_INFO("[vulkan validation] {}", data->pMessage);
    }
    return VK_FALSE;
}

}  // namespace

std::optional<Context> Context::create(std::vector<const char*> requiredExtensions, bool enableValidation) {
    rx::core::log::init();

    if (volkInitialize() != VK_SUCCESS) {
        RX_LOG_ERROR("volkInitialize failed");
        return std::nullopt;
    }

    auto errorCount = std::make_shared<int>(0);

    vkb::InstanceBuilder builder;
    builder.set_app_name("renderer_x")
        .require_api_version(1, 3, 0)
        .set_headless(requiredExtensions.empty());

    for (const char* ext : requiredExtensions) {
        builder.enable_extension(ext);
    }

    if (enableValidation) {
        builder.request_validation_layers()
            .set_debug_callback(debugCallback)
            .set_debug_callback_user_data_pointer(errorCount.get());
    }

    auto result = builder.build();
    if (!result) {
        RX_LOG_ERROR("vkb::InstanceBuilder::build failed: {}", result.error().message());
        return std::nullopt;
    }

    vkb::Instance vkbInstance = result.value();
    volkLoadInstance(vkbInstance.instance);

    return Context(vkbInstance.instance, vkbInstance.debug_messenger, errorCount);
}

Context::Context(Context&& other) noexcept
    : instance_(other.instance_), debugMessenger_(other.debugMessenger_), errorCount_(std::move(other.errorCount_)) {
    other.instance_ = VK_NULL_HANDLE;
    other.debugMessenger_ = VK_NULL_HANDLE;
}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        if (instance_ != VK_NULL_HANDLE) {
            if (debugMessenger_ != VK_NULL_HANDLE) {
                vkb::destroy_debug_utils_messenger(instance_, debugMessenger_);
            }
            vkDestroyInstance(instance_, nullptr);
        }
        instance_ = other.instance_;
        debugMessenger_ = other.debugMessenger_;
        errorCount_ = std::move(other.errorCount_);
        other.instance_ = VK_NULL_HANDLE;
        other.debugMessenger_ = VK_NULL_HANDLE;
    }
    return *this;
}

Context::~Context() {
    if (instance_ != VK_NULL_HANDLE) {
        if (debugMessenger_ != VK_NULL_HANDLE) {
            vkb::destroy_debug_utils_messenger(instance_, debugMessenger_);
        }
        vkDestroyInstance(instance_, nullptr);
    }
}

}  // namespace rx::rhi
