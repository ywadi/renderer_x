#pragma once
#include <volk.h>
// vkb::Instance is stored by value below, so its full definition is
// required here -- a forward declaration is not possible for a by-value
// member.
#include <VkBootstrap.h>
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

    VkInstance instance() const { return vkbInstance_.instance; }
    VkDebugUtilsMessengerEXT debugMessenger() const { return vkbInstance_.debug_messenger; }
    const vkb::Instance& vkbInstance() const { return vkbInstance_; }
    bool hasValidationErrors() const { return *errorCount_ > 0; }

private:
    Context(vkb::Instance vkbInstance, std::shared_ptr<int> errorCount)
        : vkbInstance_(std::move(vkbInstance)), errorCount_(std::move(errorCount)) {}

    vkb::Instance vkbInstance_{};
    std::shared_ptr<int> errorCount_;
};

}  // namespace rx::rhi
