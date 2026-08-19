#include <rx_platform/window.h>
#include <rx_core/log.h>
// SDL3/SDL.h does not pull in the Vulkan-interop declarations
// (SDL_Vulkan_GetInstanceExtensions, SDL_Vulkan_CreateSurface); SDL3 keeps
// those in their own header.
#include <SDL3/SDL_vulkan.h>

#include <atomic>
#include <string_view>

namespace rx::platform {

void logWaylandMinimizeLimitationOnce(const char* platformName) {
    // See this function's own header comment (window.h) for the full
    // rationale. Only a "Wayland" match ever flips this -- an X11/Windows
    // call never consumes the one-shot budget, so a later genuine Wayland
    // call (regardless of how many non-Wayland calls preceded it, e.g. in
    // a multi-platform test binary) still logs exactly once.
    static std::atomic<bool> alreadyLogged{false};

    if (platformName == nullptr || std::string_view(platformName) != "Wayland") {
        return;
    }

    bool expected = false;
    if (alreadyLogged.compare_exchange_strong(expected, true)) {
        RX_LOG_INFO(
            "rx_platform: running under Wayland -- SDL3 cannot positively detect a real, compositor-driven "
            "window minimize here (SDL_EVENT_WINDOW_MINIMIZED/RESTORED and SDL_GetWindowFlags()'s MINIMIZED bit "
            "only reflect this process's OWN SDL_MinimizeWindow() calls on Wayland, never the window manager's -- "
            "see github.com/libsdl-org/SDL/issues/13473 and wiki.libsdl.org/SDL3/README-wayland). This engine's "
            "own suspended-present guard (rx::rhi::Device::recreateSwapchain()) is unaffected -- it is driven by "
            "the queried Vulkan surface extent, not these events -- but any HOST code that separately listens for "
            "these same SDL events/flags to drive its own pause behavior will not see them fire on a real "
            "minimize here.");
    }
}

std::optional<Window> Window::create(const char* title, int width, int height, bool visible) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            RX_LOG_WARN("SDL_Init(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
            return std::nullopt;
        }
    }

    // [Phase 4 Task 17, FG7, gate ruling #25 row 3] One-shot, process-wide
    // -- see logWaylandMinimizeLimitationOnce()'s own comment (window.h).
    // SDL_GetPlatform() is valid to call once SDL_INIT_VIDEO has succeeded
    // above (it is documented safe to call before SDL_Init() too, but this
    // keeps the call site right where video is known to be up).
    logWaylandMinimizeLimitationOnce(SDL_GetPlatform());

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

Window::Window(Window&& other) noexcept
    : window_(other.window_),
      minimizedEventObserved_(other.minimizedEventObserved_),
      lastPixelSizeEvent_(other.lastPixelSizeEvent_) {
    other.window_ = nullptr;
    other.minimizedEventObserved_ = false;
    other.lastPixelSizeEvent_ = VkExtent2D{0, 0};
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (window_) {
            SDL_DestroyWindow(window_);
        }
        window_ = other.window_;
        minimizedEventObserved_ = other.minimizedEventObserved_;
        lastPixelSizeEvent_ = other.lastPixelSizeEvent_;
        other.window_ = nullptr;
        other.minimizedEventObserved_ = false;
        other.lastPixelSizeEvent_ = VkExtent2D{0, 0};
    }
    return *this;
}

Window::~Window() {
    if (window_) {
        SDL_DestroyWindow(window_);
    }
}

void Window::pumpEvents() {
    // [Phase 4 Task 17, FG7, gate ruling #25] Filtered to THIS window's own
    // SDL_WindowID -- an event for a different window (only possible if the
    // embedding process opened more than one) is still drained from SDL's
    // process-wide queue (SDL_PollEvent always pops the front of that
    // single queue, regardless of which window it targets) but never
    // touches this Window's own observed state. The `event.window.windowID`
    // read only ever happens INSIDE a case already known (by `event.type`)
    // to be one of SDL_WindowEvent's own variants, never against an
    // arbitrary/unrelated event's union member.
    const SDL_WindowID thisWindowId = SDL_GetWindowID(window_);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Size is read ONLY from SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED's
        // data1/data2 -- the one window event whose payload SDL3's own
        // SDL_events.h header comment documents AS a size (gate
        // matrix-issue25 row 1). SDL_EVENT_WINDOW_MINIMIZED/RESTORED carry
        // no size payload at all and must never be treated as one -- see
        // window_test.cpp's device-free test asserting exactly this.
        switch (event.type) {
            case SDL_EVENT_WINDOW_MINIMIZED:
                if (event.window.windowID == thisWindowId) {
                    minimizedEventObserved_ = true;
                }
                break;
            case SDL_EVENT_WINDOW_RESTORED:
                if (event.window.windowID == thisWindowId) {
                    minimizedEventObserved_ = false;
                }
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (event.window.windowID == thisWindowId) {
                    lastPixelSizeEvent_.width = static_cast<uint32_t>(event.window.data1);
                    lastPixelSizeEvent_.height = static_cast<uint32_t>(event.window.data2);
                }
                break;
            default:
                break;
        }
    }
}

bool Window::setFullscreen(bool fullscreen) {
    if (!SDL_SetWindowFullscreen(window_, fullscreen)) {
        RX_LOG_WARN("SDL_SetWindowFullscreen({}) failed: {}", fullscreen, SDL_GetError());
        return false;
    }
    // See this method's own header comment (window.h): the request is
    // documented asynchronous on some platforms -- sync before returning so
    // a caller's immediate readback (flags, size, or a swapchain
    // recreation) sees the already-settled state.
    if (!SDL_SyncWindow(window_)) {
        RX_LOG_WARN("SDL_SyncWindow after SDL_SetWindowFullscreen({}) failed: {}", fullscreen, SDL_GetError());
    }
    return true;
}

bool Window::isFullscreen() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
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
