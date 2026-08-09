#include <rx_rhi_vk/context.h>
#include <rx_core/log.h>
#include <VkBootstrap.h>
#include <memory>

namespace rx::rhi {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void* userData) {
    auto* errorCount = static_cast<int*>(userData);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        RX_LOG_ERROR("[vulkan validation] {}", data->pMessage);
        (*errorCount)++;
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
