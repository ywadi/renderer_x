#pragma once
#include <volk.h>
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

    static std::optional<Context> create(std::vector<const char*> requiredExtensions, bool enableValidation);

    VkInstance instance() const { return instance_; }
    VkDebugUtilsMessengerEXT debugMessenger() const { return debugMessenger_; }
    bool hasValidationErrors() const { return *errorCount_ > 0; }

private:
    Context(VkInstance instance, VkDebugUtilsMessengerEXT messenger, std::shared_ptr<int> errorCount)
        : instance_(instance), debugMessenger_(messenger), errorCount_(std::move(errorCount)) {}

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    std::shared_ptr<int> errorCount_;
};

}  // namespace rx::rhi
