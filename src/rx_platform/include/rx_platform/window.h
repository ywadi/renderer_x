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

    // Drains SDL's process-wide event queue (SDL_PollEvent) -- callers that
    // also need to react to OTHER event types (quit, input, ...) still poll
    // those themselves; this only owns draining the queue's window-state
    // events into the observed state below [Phase 4 Task 17, FG7, gate
    // ruling #25]. Thread-affinity (D5): main-thread-only, matching every
    // other SDL-touching method here -- SDL's own event queue is itself
    // main-thread-affine on most backends.
    void pumpEvents();

    // ---- Window-event-observed state [Phase 4 Task 17, FG7, gate ruling
    // #25] -- OPTIMIZATION/LOGGING SIGNAL ONLY. Read this comment before
    // using either accessor below: the AUTHORITATIVE suspended-present
    // decision is rx::rhi::Device::recreateSwapchain()'s own
    // extent-query-driven guard (device.h), never these two -- per the
    // design correction recorded in gate/rulings-2026-08-18.md #25 (matrix
    // conflict 1). On Wayland, SDL3 does not fire
    // SDL_EVENT_WINDOW_MINIMIZED/RESTORED on a real, compositor-driven
    // minimize AT ALL -- verified directly against SDL issue
    // github.com/libsdl-org/SDL/issues/13473 and
    // wiki.libsdl.org/SDL3/README-wayland, both fetched 2026-08-18 -- so a
    // caller that gated suspend/resume on minimizedEventObserved() alone
    // would silently never suspend there. These exist for optimization
    // (skip a re-query when nothing has changed) and logging/HUD use only.
    // Updated exclusively by pumpEvents() above, filtered to THIS window's
    // own SDL_WindowID (a pushed/real event for a different window is
    // drained from the queue but never touches this Window's own state).
    bool minimizedEventObserved() const { return minimizedEventObserved_; }

    // The size most recently reported by an SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
    // event's own data1/data2 payload -- per SDL3's own SDL_events.h header
    // comment, this is the ONLY window event whose payload IS a size (gate
    // matrix-issue25 row 1); {0, 0} before the first such event is ever
    // observed for this window. Never written by MINIMIZED/RESTORED or any
    // other window event -- see pumpEvents()'s own implementation and the
    // device-free test in window_test.cpp that proves exactly this
    // (MINIMIZED/RESTORED must never move this value).
    VkExtent2D lastPixelSizeEvent() const { return lastPixelSizeEvent_; }

    // ---- Borderless-desktop fullscreen toggle [Phase 4 Task 17, FG7, gate
    // ruling #25 row 4] --------------------------------------------------
    // `SDL_SetWindowFullscreen(window_, fullscreen)` with NO prior
    // `SDL_SetWindowFullscreenMode()` call -- SDL3's own documented meaning
    // of "borderless fullscreen desktop mode" (SDL_video.h: "By default a
    // window in fullscreen state uses borderless fullscreen desktop mode,
    // but a specific exclusive display mode can be set using
    // SDL_SetWindowFullscreenMode()"). Exclusive fullscreen (a real
    // SDL_DisplayMode) is FG7b -- out of this ticket's scope, never
    // requested by this function. Followed unconditionally by
    // `SDL_SyncWindow()` before returning: SDL3's own doc states this
    // request "is asynchronous and the new fullscreen state may not have
    // been applied immediately upon the return of this function" -- without
    // the sync, a caller that immediately reads back the new size/flags
    // (SDL_GetWindowFlags()/SDL_GetWindowSizeInPixels(), or this engine's
    // own swapchain-recreation path) would race the real transition. This
    // is what makes it safe for a caller to treat this call and any
    // subsequent swapchain recreation as happening against the ALREADY
    // -settled new state. Returns false (logged) only if
    // SDL_SetWindowFullscreen() itself fails; SDL_SyncWindow()'s own
    // (rare) failure is logged but does not itself turn a successful
    // SDL_SetWindowFullscreen() into a false return, matching this
    // project's existing "best-effort sync, log don't fail the caller for
    // it" convention.
    bool setFullscreen(bool fullscreen);

    // SDL_GetWindowFlags() & SDL_WINDOW_FULLSCREEN -- true in EITHER
    // borderless or exclusive fullscreen (this engine only ever REQUESTS
    // borderless via setFullscreen() above, but reports the flag as-is);
    // false in windowed mode.
    bool isFullscreen() const;

    std::vector<const char*> requiredVulkanInstanceExtensions() const;
    VkSurfaceKHR createVulkanSurface(VkInstance instance) const;

private:
    explicit Window(SDL_Window* window) : window_(window) {}
    SDL_Window* window_ = nullptr;

    // [Phase 4 Task 17, FG7] See minimizedEventObserved()/
    // lastPixelSizeEvent()'s own comments above.
    bool minimizedEventObserved_ = false;
    VkExtent2D lastPixelSizeEvent_{0, 0};
};

// [Phase 4 Task 17, FG7, gate ruling #25 row 3] One-shot (per PROCESS, not
// per Window), diagnosable-not-silent acknowledgment of a real platform
// gap: on Wayland, this engine has NO reliable way to detect a real,
// compositor-driven window minimize -- not via SDL_EVENT_WINDOW_MINIMIZED/
// RESTORED (verified: they only fire for SDL_MinimizeWindow()'s OWN calls
// there, never a real window-manager-driven minimize -- SDL issue #13473),
// not via SDL_GetWindowFlags()'s MINIMIZED bit (same root cause, same
// verified source), and not via the Vulkan surface's own currentExtent
// (Wayland reports the (0xFFFFFFFF, 0xFFFFFFFF) "surface determines its
// own size" sentinel there, never a real (0, 0) the way Win32/Xcb/Xlib
// do). This engine's OWN suspended-present guard
// (rx::rhi::Device::recreateSwapchain()) still degrades safely regardless
// (a Wayland surface simply never reports 0x0, so it is never treated as
// suspended and normal presentation continues even while genuinely
// minimized) -- this log exists purely so a HOST engine/game built on top
// of this one, that separately listens for these same SDL events/flags
// itself expecting minimize-driven pause behavior, gets a diagnosable
// answer in the log instead of silent non-firing.
//
// Called with `platformName` == SDL_GetPlatform()'s own return value at
// its real call site (Window::create(), window.cpp) rather than calling
// SDL_GetPlatform() internally, purely for testability: see
// window_test.cpp's device-free test asserting this fires exactly once for
// a mocked "Wayland" platform name and never fires for "Windows"/"Linux"
// (the X11 case) -- no live Wayland session is needed to prove the logic.
// Only a real "Wayland" match (case-sensitive, matching SDL_GetPlatform()'s
// own documented return values) ever consumes the one-shot budget; calls
// with any other platform name are a no-op every time, not just once.
void logWaylandMinimizeLimitationOnce(const char* platformName);

}  // namespace rx::platform
