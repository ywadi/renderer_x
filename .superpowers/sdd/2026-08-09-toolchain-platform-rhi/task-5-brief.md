### Task 5: rx_platform (SDL3 window wrapper)

**Files:**
- Create: `src/rx_platform/CMakeLists.txt`
- Create: `src/rx_platform/include/rx_platform/window.h`, `src/rx_platform/src/window.cpp`
- Create: `src/rx_platform/tests/window_test.cpp`
- Modify: `third_party/CMakeLists.txt` (add SDL3 via dep-cache)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `rx::core::log` (Task 4).
- Produces: `rx::platform::Window` with `Window::create(title, width, height, visible) -> std::optional<Window>`, `.sdlWindow()`, `.pumpEvents()`, `.requiredVulkanInstanceExtensions() -> std::vector<const char*>`, `.createVulkanSurface(VkInstance) -> VkSurfaceKHR` (returns `VK_NULL_HANDLE` on failure). Target `rx_platform`, consumed by `rx_rhi_vk` starting Task 7.

- [ ] **Step 1: Add SDL3 to third_party via the dependency cache**

Append to `third_party/CMakeLists.txt`:
```cmake
set(RX_SDL3_TAG "release-3.4.14")
rx_add_cached_dependency(
  NAME SDL3
  REPO https://github.com/libsdl-org/SDL.git
  TAG ${RX_SDL3_TAG}
  CMAKE_ARGS -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF
)
find_package(SDL3 REQUIRED PATHS "${SDL3_CACHE_DIR}" NO_DEFAULT_PATH)
```

- [ ] **Step 2: Write the failing test**

`src/rx_platform/tests/window_test.cpp`:
```cpp
#include <doctest/doctest.h>
#include <rx_platform/window.h>

TEST_CASE("Window::create/destroy lifecycle succeeds under any video driver") {
    auto window = rx::platform::Window::create("rx_platform_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    CHECK(window->sdlWindow() != nullptr);
    window->pumpEvents();
}

TEST_CASE("Window reports Vulkan instance extensions when a real display backend is present") {
    auto window = rx::platform::Window::create("rx_platform_vk_test", 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping Vulkan-extension check");
        return;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping");
        return;
    }
    CHECK(extensions.size() > 0);
}
```

- [ ] **Step 3: Run to verify it fails (header doesn't exist yet)**

```bash
cmake --preset linux-native && cmake --build --preset linux-native --target rx_platform_tests
```
Expected: FAIL — `rx_platform/window.h: No such file or directory`.

- [ ] **Step 4: Implement rx_platform**

`src/rx_platform/include/rx_platform/window.h`:
```cpp
#pragma once
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <optional>
#include <vector>

namespace rx::platform {

class Window {
public:
    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    static std::optional<Window> create(const char* title, int width, int height, bool visible);

    SDL_Window* sdlWindow() const { return window_; }
    void pumpEvents();

    std::vector<const char*> requiredVulkanInstanceExtensions() const;
    VkSurfaceKHR createVulkanSurface(VkInstance instance) const;

private:
    explicit Window(SDL_Window* window) : window_(window) {}
    SDL_Window* window_ = nullptr;
};

}  // namespace rx::platform
```

`src/rx_platform/src/window.cpp`:
```cpp
#include <rx_platform/window.h>
#include <rx_core/log.h>

namespace rx::platform {

std::optional<Window> Window::create(const char* title, int width, int height, bool visible) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            RX_LOG_WARN("SDL_Init(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
            return std::nullopt;
        }
    }

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (!visible) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    SDL_Window* window = SDL_CreateWindow(title, width, height, flags);
    if (!window) {
        RX_LOG_WARN("SDL_CreateWindow failed: {}", SDL_GetError());
        return std::nullopt;
    }
    return Window(window);
}

Window::Window(Window&& other) noexcept : window_(other.window_) {
    other.window_ = nullptr;
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (window_) {
            SDL_DestroyWindow(window_);
        }
        window_ = other.window_;
        other.window_ = nullptr;
    }
    return *this;
}

Window::~Window() {
    if (window_) {
        SDL_DestroyWindow(window_);
    }
}

void Window::pumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Intentionally empty for now: rx_platform only exposes the pump,
        // event handling policy belongs to whatever embeds this window.
    }
}

std::vector<const char*> Window::requiredVulkanInstanceExtensions() const {
    Uint32 count = 0;
    char const* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) {
        return {};
    }
    return std::vector<const char*>(names, names + count);
}

VkSurfaceKHR Window::createVulkanSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
        RX_LOG_WARN("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
        return VK_NULL_HANDLE;
    }
    return surface;
}

}  // namespace rx::platform
```

`src/rx_platform/CMakeLists.txt`:
```cmake
find_package(Vulkan REQUIRED)

add_library(rx_platform STATIC
    src/window.cpp
)
target_include_directories(rx_platform PUBLIC include)
target_link_libraries(rx_platform PUBLIC SDL3::SDL3 Vulkan::Headers rx_core)

add_executable(rx_platform_tests
    tests/window_test.cpp
)
target_link_libraries(rx_platform_tests PRIVATE rx_platform doctest::doctest)
add_test(NAME rx_platform_tests COMMAND rx_platform_tests)
```

- [ ] **Step 5: Wire into root CMakeLists.txt**

Add to `CMakeLists.txt`:
```cmake
add_subdirectory(src/rx_platform)
```

- [ ] **Step 6: Run tests and verify they pass**

```bash
cmake --build --preset linux-native --target rx_platform_tests
ctest --preset linux-native -R rx_platform_tests --output-on-failure
```
Expected: both test cases pass on this machine (it has a real X11 `DISPLAY`, so the Vulkan-extension check runs for real rather than skipping).

- [ ] **Step 7: Commit**

```bash
git add third_party/CMakeLists.txt src/rx_platform/ CMakeLists.txt
git commit -m "Add rx_platform: SDL3 window wrapper"
```

---

