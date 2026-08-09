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
