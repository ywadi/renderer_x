### Task 6: rx_rhi_vk::Context (instance + validation layers)

**Files:**
- Create: `src/rx_rhi_vk/CMakeLists.txt`
- Create: `src/rx_rhi_vk/include/rx_rhi_vk/context.h`, `src/rx_rhi_vk/src/context.cpp`
- Create: `src/rx_rhi_vk/tests/context_test.cpp`
- Modify: `third_party/CMakeLists.txt` (add volk via FetchContent, vk-bootstrap via dep-cache)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `rx::core::log` (Task 4).
- Produces: `rx::rhi::Context` with `Context::create(requiredExtensions, enableValidation) -> std::optional<Context>`, `.instance()`, `.hasValidationErrors()` (true if the debug messenger ever reported an error/warning during this Context's lifetime). Target `rx_rhi_vk`, consumed by every later RHI task.

- [ ] **Step 1: Add volk and vk-bootstrap to third_party**

Append to `third_party/CMakeLists.txt`:
```cmake
set(RX_VOLK_TAG "vulkan-sdk-1.4.357.0")
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG ${RX_VOLK_TAG}
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(volk)

set(RX_VK_BOOTSTRAP_COMMIT "556b79b165386f6c1a18362d30f2a076fdaa2778")
rx_add_cached_dependency(
  NAME vk-bootstrap
  REPO https://github.com/charles-lunarg/vk-bootstrap.git
  TAG ${RX_VK_BOOTSTRAP_COMMIT}
  CMAKE_ARGS -DVK_BOOTSTRAP_TEST=OFF
)
find_package(vk-bootstrap REQUIRED PATHS "${vk-bootstrap_CACHE_DIR}" NO_DEFAULT_PATH)
```

- [ ] **Step 2: Write the failing test**

`src/rx_rhi_vk/tests/context_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <rx_rhi_vk/context.h>

TEST_CASE("Context::create succeeds with no required extensions and reports no validation errors") {
    auto ctx = rx::rhi::Context::create({}, /*enableValidation=*/true);
    REQUIRE(ctx.has_value());
    CHECK(ctx->instance() != VK_NULL_HANDLE);
    CHECK_FALSE(ctx->hasValidationErrors());
}
```

- [ ] **Step 3: Run to verify it fails**

```bash
cmake --preset linux-native && cmake --build --preset linux-native --target rx_rhi_vk_tests
```
Expected: FAIL — `rx_rhi_vk/context.h: No such file or directory`.

- [ ] **Step 4: Implement Context**

`src/rx_rhi_vk/include/rx_rhi_vk/context.h`:
```cpp
#pragma once
#include <volk.h>
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
```

`src/rx_rhi_vk/src/context.cpp`:
```cpp
#include <rx_rhi_vk/context.h>
#include <rx_core/log.h>
#include <VkBootstrap.h>
#include <memory>

namespace rx::rhi {

namespace {

VkBool32 debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
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
```

`src/rx_rhi_vk/CMakeLists.txt`:
```cmake
add_library(rx_rhi_vk STATIC
    src/context.cpp
    ${volk_SOURCE_DIR}/volk.c
)
target_include_directories(rx_rhi_vk PUBLIC include ${volk_SOURCE_DIR})
target_link_libraries(rx_rhi_vk PUBLIC rx_core vk-bootstrap::vk-bootstrap)
target_compile_definitions(rx_rhi_vk PUBLIC VK_NO_PROTOTYPES)

add_executable(rx_rhi_vk_tests
    tests/context_test.cpp
)
target_link_libraries(rx_rhi_vk_tests PRIVATE rx_rhi_vk doctest::doctest)
add_test(NAME rx_rhi_vk_tests COMMAND rx_rhi_vk_tests)
```

- [ ] **Step 5: Wire into root CMakeLists.txt**

Add to `CMakeLists.txt`:
```cmake
add_subdirectory(src/rx_rhi_vk)
```

- [ ] **Step 6: Run tests and verify they pass**

```bash
cmake --build --preset linux-native --target rx_rhi_vk_tests
ctest --preset linux-native -R rx_rhi_vk_tests --output-on-failure
```
Expected: instance creation succeeds and `hasValidationErrors()` is false.

- [ ] **Step 7: Commit**

```bash
git add third_party/CMakeLists.txt src/rx_rhi_vk/ CMakeLists.txt
git commit -m "Add rx_rhi_vk::Context: Vulkan instance and validation layers"
```

---

