#pragma once
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <rx_platform/input.h>
#include <optional>
#include <unordered_map>
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
    // also need to react to OTHER event types (quit, ImGui, ...) still poll
    // those themselves; this owns draining the queue's window-state events
    // into the observed state below [Phase 4 Task 17, FG7, gate ruling #25],
    // AND [Phase 4 Task 20, gate ruling #14] the mouse-delta accumulation
    // (SDL_EVENT_MOUSE_MOTION), gamepad hot-plug lifecycle
    // (SDL_EVENT_GAMEPAD_ADDED/REMOVED), and the focus-loss/gain handling
    // that pauses mouse-delta accumulation and unconditionally re-arms
    // relative mouse mode on focus regain -- see consumeMouseDelta()/
    // setRelativeMouseMode()/poll()'s own comments below for exactly what
    // each does. This is this engine's single event-dispatch owner: Task
    // 21's ImGui overlay feeds every event to
    // ImGui_ImplSDL3_ProcessEvent() FIRST at this same call site, then lets
    // this class's own handling run, and the platform-input accumulators
    // gate on ImGui's WantCaptureMouse/WantCaptureKeyboard flags at the
    // CALLER level (rulings-2026-08-18.md #14/#16) -- pumpEvents() itself
    // has no ImGui dependency.
    // Thread-affinity (D5, Phase 4): main-thread-only -- see
    // docs/threading.md. SDL's own event queue is itself main-thread-affine
    // on most backends; this method now also carries a dev-time
    // RX_ASSERT_MAIN_THREAD guard [Phase 4 Task 20], matching every other
    // guarded main-thread-only mutator in this codebase.
    void pumpEvents();

    // ---- Relative mouse mode + mouse-delta accumulation [Phase 4 Task 20,
    // gate ruling #14, matrix rows 1/2] ------------------------------------
    // setRelativeMouseMode(true) hides+captures the cursor and puts SDL
    // into continuous relative-motion reporting
    // (SDL_SetWindowRelativeMouseMode). The ENABLED intent is tracked
    // independently of whatever SDL/the OS did internally -- gate ruling:
    // "track the app-requested relative-mode state independently of SDL's"
    // -- relativeMouseModeWanted() below reflects THIS engine's own last
    // request, never a live SDL_GetWindowRelativeMouseMode() query. Returns
    // SDL_SetWindowRelativeMouseMode()'s own success/failure (logged on
    // failure, matching setFullscreen()'s established best-effort
    // convention) -- relativeModeWanted_ is updated regardless of that
    // result, because pumpEvents()'s FOCUS_GAINED handling below needs to
    // know what the app WANTS independent of whether SDL granted it the
    // last time it was asked.
    //
    // On SDL_EVENT_WINDOW_FOCUS_LOST (handled inside pumpEvents()),
    // mouse-delta accumulation is paused -- no assumption is made about
    // what SDL/the window manager did to the OS-level capture (SDL's own
    // docs never state relative mode auto-clears on focus loss, unlike
    // SDL_CaptureMouse, which explicitly does -- gate matrix row 1's own
    // documented gap). On SDL_EVENT_WINDOW_FOCUS_GAINED, if the app still
    // wants relative mode (relativeModeWanted_), SDL_SetWindowRelativeMouseMode(
    // window, true) is unconditionally RE-issued -- idempotent, safe
    // whether or not SDL had silently cleared it, and correct regardless of
    // which OS/compositor answer is true.
    // Thread-affinity (D5, Phase 4): main-thread-only -- see docs/threading.md.
    bool setRelativeMouseMode(bool enabled);
    bool relativeMouseModeWanted() const;

    // Consumes (returns, then resets to {0,0}) the mouse motion accumulated
    // from every SDL_EVENT_MOUSE_MOTION event (targeting THIS window --
    // same cross-window isolation as the Task 17 window-state events)
    // drained by pumpEvents() since the last call to this method -- never
    // leaks across frames (gate matrix row 2's consume-and-reset
    // algorithm: `accumX += event.motion.xrel` etc. inside pumpEvents()'s
    // existing full-drain loop; correctness rests on that loop draining the
    // queue completely before this is ever called, already true today).
    // While this window has been focus-lost and not yet focus-regained (see
    // setRelativeMouseMode()'s comment above), motion events are still
    // drained from the queue but NOT accumulated -- this call still resets
    // the accumulator to {0,0} regardless.
    // Thread-affinity (D5, Phase 4): main-thread-only -- see docs/threading.md.
    MouseDelta consumeMouseDelta();

    // ---- Cursor visibility / confinement [Phase 4 Task 20, gate ruling
    // #14, matrix row 3] ---------------------------------------------------
    // setCursorVisible wraps SDL_ShowCursor()/SDL_HideCursor() -- process-
    // global, not per-window (SDL3's own scoping, not a limitation this
    // class introduces). setCursorConfined wraps SDL_SetWindowMouseGrab --
    // confines the OS cursor to this window WITHOUT altering visibility or
    // motion semantics; a DISTINCT primitive from relative mode above (per
    // SDL_video.h's own doc: "this does NOT grab the cursor" is
    // SDL_SetWindowMouseRect's caveat, but SDL_SetWindowMouseGrab genuinely
    // does grab/confine without touching cursor visibility or capture-mode
    // motion reporting). Both return SDL's own success/failure (logged on
    // failure).
    // Thread-affinity (D5, Phase 4): main-thread-only -- see docs/threading.md.
    bool setCursorVisible(bool visible);
    bool cursorVisible() const;
    bool setCursorConfined(bool confined);
    bool cursorConfined() const;

    // ---- Gamepad [Phase 4 Task 20, gate ruling #14, matrix rows 4-9]
    // ----------------------------------------------------------------------
    // Hot-plug lifecycle is owned entirely by pumpEvents(): on
    // SDL_EVENT_GAMEPAD_ADDED, SDL_OpenGamepad() then insert into an
    // internal SDL_JoystickID-keyed map; on SDL_EVENT_GAMEPAD_REMOVED,
    // SDL_CloseGamepad() THEN erase, synchronously, in the same event-
    // handling step (never deferred to a later frame -- a stale handle
    // outliving its REMOVED event is exactly the hazard gate matrix row 4
    // documents). poll() reads a single "active" gamepad, selected as the
    // LOWEST currently-connected SDL_JoystickID among every tracked device
    // (gate matrix row 9's single-active rule) -- deterministic, and the
    // active pad only changes when it itself disconnects. Every stick/
    // trigger field in the returned GamepadState has ALREADY had
    // applyStickDeadzone()/applyTriggerDeadzone() (input.h) applied --
    // never raw SDL axis values. GamepadState{connected=false} (every other
    // field zeroed) when no gamepad is attached at all.
    // Thread-affinity (D5, Phase 4): main-thread-only -- see docs/threading.md.
    GamepadState poll() const;

    // ---- Keyboard [Phase 4 Task 20, gate ruling #14, NEW SCOPE] ----------
    // Minimal keyboard surface over SDL_GetKeyboardState() -- added this
    // task specifically because sample 09's WASD fly-through camera needs
    // it (gate ruling: its absence from the entire Phase 4 planning
    // universe up to this point was an oversight, now closed). Bounds-
    // checked against SDL_GetKeyboardState()'s own reported array length --
    // an out-of-range SDL_Scancode value returns false rather than reading
    // out of bounds.
    // Thread-affinity (D5, Phase 4): main-thread-only -- see docs/threading.md.
    bool isKeyDown(SDL_Scancode key) const;

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

    // [Phase 4 Task 20, gate ruling #14] See setRelativeMouseMode()/
    // consumeMouseDelta()'s own comments above. relativeModeWanted_ is the
    // APP's own last-requested intent (never a live SDL query);
    // focusLost_ gates mouse-delta accumulation while true.
    bool relativeModeWanted_ = false;
    bool focusLost_ = false;
    float accumMouseDeltaX_ = 0.0f;
    float accumMouseDeltaY_ = 0.0f;

    // [Phase 4 Task 20, gate ruling #14, matrix row 4] Owned handle table,
    // keyed by SDL_JoystickID -- see poll()'s own comment above for the
    // open-on-ADDED/close-then-erase-on-REMOVED lifecycle pumpEvents()
    // maintains. Every SDL_Gamepad* here was returned by a successful
    // SDL_OpenGamepad() and is closed exactly once, either by
    // pumpEvents()'s own REMOVED handling or by closeAllGamepads() below
    // (move/destroy paths).
    std::unordered_map<SDL_JoystickID, SDL_Gamepad*> gamepads_;

    // SDL_CloseGamepad() on every entry still in gamepads_, then clears the
    // map -- shared by the destructor and move-assignment's "destroy what
    // this object currently owns before taking over `other`'s state" step
    // (mirrors the existing `if (window_) SDL_DestroyWindow(window_);`
    // pattern immediately below move-assignment's own call site).
    void closeAllGamepads();
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
