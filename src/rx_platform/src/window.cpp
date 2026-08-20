#include <rx_platform/window.h>
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
// SDL3/SDL.h does not pull in the Vulkan-interop declarations
// (SDL_Vulkan_GetInstanceExtensions, SDL_Vulkan_CreateSurface); SDL3 keeps
// those in their own header.
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <string_view>
#include <utility>

#if defined(SDL_PLATFORM_LINUX)
// [Phase 4 Task 17 follow-up, Issue #74] dlopen/dlsym only -- see
// installX11ErrorHandlerOnce()'s own comment below for why this engine
// never links against -lX11 directly.
#include <dlfcn.h>
#endif

namespace rx::platform {

namespace {

#if defined(SDL_PLATFORM_LINUX)
// [Phase 4 Task 17 follow-up, Issue #74] Minimal, ABI-stable mirror of the
// fields of Xlib's real `XErrorEvent` (<X11/Xlib.h>) that
// installX11ErrorHandlerOnce()'s handler actually reads. Deliberately NOT
// including the real header and NOT link-time-depending on libX11 (this
// engine resolves the one Xlib entry point it needs the exact same way
// SDL3 itself does -- SDL_x11dyn's own dlopen-based dynamic loading -- so a
// Wayland-only environment with no libX11.so.6 installed at all is never
// made to need one just because this engine links against SDL3). The X11
// wire protocol's error-packet layout has been frozen since X11R1 -- this
// struct's layout is not expected to change (the same "empirically stable
// protocol/ABI constant" tolerance this codebase already extends to e.g.
// the VkResult classifiers in rx_rhi_vk's isSurfaceLossResult()).
struct XErrorEventAbi {
    int type;
    void* display;
    unsigned long resourceid;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
};

using XErrorHandlerFn = int (*)(void* display, XErrorEventAbi* event);
using XSetErrorHandlerFn = XErrorHandlerFn (*)(XErrorHandlerFn);

// The handler XSetErrorHandler() reported as previously installed (normally
// Xlib's own default, fatal handler) -- non-BadWindow errors are forwarded
// to it unchanged below, so this fix stays narrowly scoped to the ONE
// error code it targets and every other X11 protocol error still behaves
// exactly as it always has (loud, fatal -- a genuine, unexpected protocol
// bug should stay visible, not be silently swallowed by this fix).
std::atomic<XErrorHandlerFn> g_previousX11ErrorHandler{nullptr};

int handleX11Error(void* display, XErrorEventAbi* event) {
    // [Issue #74 round review] Deliberately NOT gated on a "has some window
    // already been abandoned" temporal flag -- see isIgnorableX11Error()'s
    // own comment (window.h) for the full record of why that was tried and
    // reverted this round: it reproduced Issue #74's own original crash,
    // because this race's first BadWindow routinely fires before this
    // engine has ever abandoned anything.
    if (event != nullptr && isIgnorableX11Error(event->error_code)) {
        RX_LOG_WARN(
            "rx_platform: X11 BadWindow protocol error suppressed (resource=0x{:x}, request_code={}, "
            "minor_code={}, serial={}) -- almost certainly a third-party window destroy (e.g. a window-manager "
            "close, or a raw XDestroyWindow()) racing an in-flight SDL/X11 call; this engine's own "
            "Device::isSurfaceLost() detection (rx_rhi_vk/device.h) will notice and handle the underlying "
            "surface loss on its own next check [Issue #74]",
            event->resourceid, static_cast<int>(event->request_code), static_cast<int>(event->minor_code),
            event->serial);
        return 0;
    }
    XErrorHandlerFn previous = g_previousX11ErrorHandler.load();
    if (previous != nullptr) {
        return previous(display, event);
    }
    return 0;
}

// [Phase 4 Task 17 follow-up, Issue #74] One-shot (per PROCESS, not per
// Window) -- mirrors logWaylandMinimizeLimitationOnce()'s own established
// atomic-guarded pattern. Root cause (reproduced directly with gdb-level
// instrumentation against real NVIDIA/Xcb hardware, see the Issue #74
// report): a third-party window destroy can happen at ANY point across the
// whole session -- not just during this engine's own present-loop teardown
// -- and Xlib's DEFAULT error handler is installed for the ENTIRE process
// lifetime the moment SDL3's X11 backend opens its display connection
// (confirmed by inspecting SDL3's own X11 video-driver source: it only
// EVER installs a scoped custom handler transiently, during its own
// window-manager capability probe at video-driver init, restoring the
// default handler immediately afterward -- SDL3 provides no general
// protection against this class of race). Rather than chasing and guarding
// every individual SDL-internal call site that might touch a since-
// destroyed window handle (mouse-capture engage/disengage, focus-dispatch
// grab re-arm, cursor redraw, and any future SDL-internal call this
// engine's own code never directly calls) -- an open-ended, whack-a-mole
// list -- this installs ONE process-wide non-fatal handler for the ONE
// well-defined error code (BadWindow) that class of race always produces,
// regardless of which specific X11 request triggered it.
//
// [Issue #74 round review] The handler stays armed for the rest of the
// process's life -- there is deliberately no "disarm" call anywhere. A
// round-review finding (LOW, Approved, not blocking) flagged this as a
// theoretical future multi-window risk: an XID-agnostic handler could
// silently downgrade a genuine BadWindow bug in a SECOND, still-alive
// window into a permanent warning, if this engine ever grows past one
// Window per process. A follow-up gate -- suppress BadWindow only once
// Window::abandonNativeHandle() had fired at least once -- was implemented
// and empirically tested against this exact scenario this round, and
// REVERTED: it reproduced Issue #74's own original fatal crash three times
// in a row on real NVIDIA hardware (window-destruction independently
// confirmed each time via `xdotool getwindowname` failing post-close), all
// on the Workshop scene's mid-load-close case. Root cause of why a
// temporal gate cannot work here: this race's first BadWindow routinely
// arrives before abandonNativeHandle() is EVER called on anything (that
// only happens reactively, once the present loop's own swapchain-
// recreation path notices the surface is gone -- which has not even
// started running yet when e.g. Window::setRelativeMouseMode(), called
// once immediately after scene load and before the loop begins, races a
// window that died during that load). There is no signal earlier than "a
// Window exists" to arm on, and arming at Window::create() time is
// indistinguishable from always-armed for any realistic single-process
// session -- so this stays permanently armed once installed, exactly as
// originally shipped. Safe under this engine's CURRENT architecture (one
// Window per process, ImGui multi-viewport explicitly disabled --
// rx_debug_ui/overlay.h) -- genuine per-window scoping is the correct fix
// if/when multi-window support is ever added, not a temporal gate.
void installX11ErrorHandlerOnce(const char* videoDriver) {
    static std::atomic<bool> installed{false};

    if (!shouldInstallX11ErrorHandler(videoDriver)) {
        return;
    }

    bool expected = false;
    if (!installed.compare_exchange_strong(expected, true)) {
        return;  // Already installed earlier this process -- idempotent.
    }

    // Resolved via dlopen/dlsym, exactly like SDL3's own SDL_x11dyn.c loads
    // every Xlib entry point it calls. By the time this runs
    // (Window::create(), immediately after SDL_Init(SDL_INIT_VIDEO)
    // succeeds with the x11 driver selected), SDL3 has already dlopen'd
    // libX11.so.6 into this process to create the x11 video driver in the
    // first place -- this call simply attaches to that SAME already-mapped
    // library (POSIX dlopen: re-opening an already-loaded soname bumps its
    // refcount and returns the existing handle, it does not reload or
    // duplicate it), so this is safe and correct regardless of which flags
    // SDL3's own internal dlopen call used. Never dlclose()'d -- reclaimed
    // by the OS at process exit either way, exactly like this engine's own
    // established "never explicitly freed before exit" precedent for e.g.
    // Window::abandonNativeHandle()'s own native handle.
    void* libX11 = dlopen("libX11.so.6", RTLD_NOW);
    if (libX11 == nullptr) {
        RX_LOG_WARN(
            "rx_platform: dlopen(libX11.so.6) failed installing the non-fatal BadWindow handler: {} -- a "
            "third-party window destroy racing an in-flight X11 call may still terminate the process [Issue #74]",
            dlerror());
        return;
    }

    auto setErrorHandler = reinterpret_cast<XSetErrorHandlerFn>(dlsym(libX11, "XSetErrorHandler"));
    if (setErrorHandler == nullptr) {
        RX_LOG_WARN(
            "rx_platform: dlsym(XSetErrorHandler) failed: {} -- non-fatal BadWindow handler not installed "
            "[Issue #74]",
            dlerror());
        return;
    }

    g_previousX11ErrorHandler.store(setErrorHandler(&handleX11Error));
}
#endif  // defined(SDL_PLATFORM_LINUX)

}  // namespace

bool shouldInstallX11ErrorHandler(const char* videoDriver) {
    return videoDriver != nullptr && std::string_view(videoDriver) == "x11";
}

bool isIgnorableX11Error(unsigned char errorCode) {
    // <X11/X.h>: `#define BadWindow 3` -- see this function's own header
    // comment (window.h) for why BadWindow specifically, and why by error
    // code alone rather than by request code.
    constexpr unsigned char kX11BadWindow = 3;
    return errorCode == kX11BadWindow;
}

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

std::optional<Window> Window::create(const char* title, int width, int height, bool visible, bool resizable) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            RX_LOG_WARN("SDL_Init(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
            return std::nullopt;
        }
    }
    // [Phase 4 Task 20, gate ruling #14] SDL3's joystick/gamepad subsystem
    // SILENTLY DROPS input state updates while the app is "in the
    // background" (no window has OS input focus) -- SDL_hints.h's own doc
    // for this hint: default "0" = "Disable joystick & gamecontroller
    // input events when the application is in the background." This is
    // NOT a corner case for this engine: (a) every headless/CI test
    // window in this codebase is created hidden (never focused) --
    // reproduced directly this task: a real SDL_Joystick's virtual-button
    // state changes were invisible through SDL_GetJoystickButton() with
    // this hint at its default, and became visible immediately once set
    // to "1", with the ONLY variable changed being this hint; (b) the
    // ticket's own acceptance bar is Steam Deck drivability, where
    // Gamescope/Steam's overlay routinely steals focus from a running
    // game -- shipping this at SDL's default would make gamepad input go
    // silently dead on exactly that platform. Set unconditionally, before
    // SDL_INIT_GAMEPAD comes up, so this class's gamepad surface behaves
    // identically focused or not on every platform.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    // SDL_INIT_GAMEPAD implies SDL_INIT_JOYSTICK (SDL_init.h's own doc) --
    // one call brings up both subsystems poll()/pumpEvents()'s hot-plug
    // handling need. Best-effort: logged, not fatal to Window::create() --
    // a platform/driver without a joystick backend still gets a fully
    // working window, just with poll() permanently reporting
    // GamepadState{connected=false} (the same degrade-gracefully shape
    // the rest of this class already uses for e.g. a video driver that
    // reports no Vulkan surface extensions).
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            RX_LOG_WARN("SDL_InitSubSystem(SDL_INIT_GAMEPAD) failed: {} -- gamepad input will be unavailable",
                        SDL_GetError());
        }
    }

    // [Phase 4 Task 17, FG7, gate ruling #25 row 3] One-shot, process-wide
    // -- see logWaylandMinimizeLimitationOnce()'s own comment (window.h).
    // SDL_GetPlatform() is valid to call once SDL_INIT_VIDEO has succeeded
    // above (it is documented safe to call before SDL_Init() too, but this
    // keeps the call site right where video is known to be up).
    logWaylandMinimizeLimitationOnce(SDL_GetPlatform());

#if defined(SDL_PLATFORM_LINUX)
    // [Phase 4 Task 17 follow-up, Issue #74] See installX11ErrorHandlerOnce()'s
    // own comment above for the full rationale. Must run after
    // SDL_Init(SDL_INIT_VIDEO) above (SDL_GetCurrentVideoDriver() is only
    // valid once video is initialized) and before this function hands back
    // a live window: the race this closes can start from this engine's very
    // first SDL_CreateWindow()/SDL_ShowWindow() call onward, not just during
    // later teardown -- reproduced directly (Issue #74) racing SDL's own
    // internal focus-dispatch handling during window creation itself.
    installX11ErrorHandlerOnce(SDL_GetCurrentVideoDriver());
#endif

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (!visible) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    if (resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
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
      nativeHandleAbandoned_(other.nativeHandleAbandoned_),
      minimizedEventObserved_(other.minimizedEventObserved_),
      lastPixelSizeEvent_(other.lastPixelSizeEvent_),
      relativeModeWanted_(other.relativeModeWanted_),
      focusLost_(other.focusLost_),
      accumMouseDeltaX_(other.accumMouseDeltaX_),
      accumMouseDeltaY_(other.accumMouseDeltaY_),
      gamepads_(std::move(other.gamepads_)) {
    other.window_ = nullptr;
    other.nativeHandleAbandoned_ = false;
    other.minimizedEventObserved_ = false;
    other.lastPixelSizeEvent_ = VkExtent2D{0, 0};
    other.relativeModeWanted_ = false;
    other.focusLost_ = false;
    other.accumMouseDeltaX_ = 0.0f;
    other.accumMouseDeltaY_ = 0.0f;
    // gamepads_ ownership moved out via std::move above -- other.gamepads_
    // is left in its moved-from (empty, per std::unordered_map's own
    // guarantee) state, so other's eventual destructor closes nothing this
    // Window now owns.
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        // [Phase 4 Task 17 follow-up, Issue #73] See abandonNativeHandle()'s
        // own comment (window.h) -- skip SDL_DestroyWindow() for a handle
        // already known abandoned, exactly like the destructor below.
        if (window_ && !nativeHandleAbandoned_) {
            SDL_DestroyWindow(window_);
        }
        closeAllGamepads();
        window_ = other.window_;
        nativeHandleAbandoned_ = other.nativeHandleAbandoned_;
        minimizedEventObserved_ = other.minimizedEventObserved_;
        lastPixelSizeEvent_ = other.lastPixelSizeEvent_;
        relativeModeWanted_ = other.relativeModeWanted_;
        focusLost_ = other.focusLost_;
        accumMouseDeltaX_ = other.accumMouseDeltaX_;
        accumMouseDeltaY_ = other.accumMouseDeltaY_;
        gamepads_ = std::move(other.gamepads_);
        other.window_ = nullptr;
        other.nativeHandleAbandoned_ = false;
        other.minimizedEventObserved_ = false;
        other.lastPixelSizeEvent_ = VkExtent2D{0, 0};
        other.relativeModeWanted_ = false;
        other.focusLost_ = false;
        other.accumMouseDeltaX_ = 0.0f;
        other.accumMouseDeltaY_ = 0.0f;
        // other.gamepads_ already emptied by std::move above.
    }
    return *this;
}

Window::~Window() {
    closeAllGamepads();
    // [Phase 4 Task 17 follow-up, Issue #73] See abandonNativeHandle()'s
    // own comment (window.h) -- skip SDL_DestroyWindow() for a handle
    // already known abandoned: there is nothing valid left to destroy, and
    // calling it anyway issues further native requests against an
    // already-invalid native ID (reproduced directly as an unhandled X11
    // BadWindow protocol error that Xlib's own default error handler
    // treats as fatal to the whole process).
    if (window_ && !nativeHandleAbandoned_) {
        SDL_DestroyWindow(window_);
    }
}

void Window::closeAllGamepads() {
    for (auto& [id, gamepad] : gamepads_) {
        (void)id;
        SDL_CloseGamepad(gamepad);
    }
    gamepads_.clear();
}

void Window::pumpEvents(const std::function<void(const SDL_Event&)>& preDispatch) {
    RX_ASSERT_MAIN_THREAD("Window::pumpEvents");

    // [Phase 4 Task 17, FG7, gate ruling #25] Filtered to THIS window's own
    // SDL_WindowID -- an event for a different window (only possible if the
    // embedding process opened more than one) is still drained from SDL's
    // process-wide queue (SDL_PollEvent always pops the front of that
    // single queue, regardless of which window it targets) but never
    // touches this Window's own observed state. The `event.window.windowID`
    // read only ever happens INSIDE a case already known (by `event.type`)
    // to be one of SDL_WindowEvent's own variants, never against an
    // arbitrary/unrelated event's union member. [Phase 4 Task 20] The same
    // isolation now applies to SDL_EVENT_MOUSE_MOTION's own `windowID`
    // field. Gamepad device events carry no windowID at all (gamepads are
    // process-global, not per-window) so GAMEPAD_ADDED/REMOVED are handled
    // unconditionally.
    const SDL_WindowID thisWindowId = SDL_GetWindowID(window_);

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // [Phase 4 Task 21, gate ruling #16] Every drained event reaches
        // `preDispatch` FIRST, unconditionally, before any of this method's
        // own case handling below -- see pumpEvents()'s own header comment
        // (window.h) for why this order is load-bearing (ImGui's own
        // internal IO state, e.g. WantCaptureMouse, must already be current
        // by the time anything downstream reads it this frame).
        if (preDispatch) {
            preDispatch(event);
        }

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

            // [Phase 4 Task 20, gate ruling #14, matrix row 1] Pauses
            // mouse-delta accumulation without assuming anything about
            // what SDL/the OS did to relative-mode capture on its own --
            // see setRelativeMouseMode()'s header comment.
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (event.window.windowID == thisWindowId) {
                    focusLost_ = true;
                }
                break;

            // Unconditional re-arm: if the app still wants relative mode,
            // ask SDL for it again regardless of whether SDL silently
            // cleared it on its own during the just-ended focus loss --
            // idempotent and safe either way (gate matrix row 1).
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (event.window.windowID == thisWindowId) {
                    focusLost_ = false;
                    if (relativeModeWanted_) {
                        if (!SDL_SetWindowRelativeMouseMode(window_, true)) {
                            RX_LOG_WARN("SDL_SetWindowRelativeMouseMode(true) re-arm on focus-gain failed: {}",
                                        SDL_GetError());
                        }
                    }
                }
                break;

            // [Phase 4 Task 20, gate ruling #14, matrix row 2] Raw
            // per-event delta summation -- consumeMouseDelta() is the
            // consume-and-reset read side. Never accumulated while
            // focus-lost (see FOCUS_LOST/FOCUS_GAINED above) -- the event
            // is still drained from the queue either way, just not summed.
            case SDL_EVENT_MOUSE_MOTION:
                if (event.motion.windowID == thisWindowId && !focusLost_) {
                    accumMouseDeltaX_ += event.motion.xrel;
                    accumMouseDeltaY_ += event.motion.yrel;
                }
                break;

            // [Phase 4 Task 20, gate ruling #14, matrix row 4] Open-on-
            // ADDED -- inserted into gamepads_ keyed by SDL_JoystickID.
            // Also the gyro log-don't-drop probe (matrix row 8/10, SDL
            // issue #9148): queried and logged once per gamepad-open, not
            // consumed anywhere -- purely diagnosable-not-silent.
            case SDL_EVENT_GAMEPAD_ADDED: {
                const SDL_JoystickID id = event.gdevice.which;
                SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
                if (gamepad == nullptr) {
                    RX_LOG_WARN("SDL_OpenGamepad({}) failed: {}", id, SDL_GetError());
                    break;
                }
                gamepads_[id] = gamepad;
                const char* name = SDL_GetGamepadName(gamepad);
                const bool hasGyro = SDL_GamepadHasSensor(gamepad, SDL_SENSOR_GYRO);
                RX_LOG_INFO(
                    "rx_platform: gamepad connected id={} name=\"{}\" hasGyroSensor={} [Phase 4 Task 20, gate "
                    "ruling #14 -- gyro is log-don't-drop this phase, SDL issue #9148: undetectable on Steam Deck "
                    "at this project's pinned SDL3 version]",
                    id, name != nullptr ? name : "<unknown>", hasGyro);
                break;
            }

            // Close-then-erase, synchronously, in this same event-handling
            // step -- never deferred (gate matrix row 4's own hazard
            // note: a stale handle outliving its REMOVED event risks a
            // future SDL_JoystickID reuse colliding with it).
            case SDL_EVENT_GAMEPAD_REMOVED: {
                const SDL_JoystickID id = event.gdevice.which;
                auto it = gamepads_.find(id);
                if (it != gamepads_.end()) {
                    SDL_CloseGamepad(it->second);
                    gamepads_.erase(it);
                }
                break;
            }

            default:
                break;
        }
    }
}

bool Window::setRelativeMouseMode(bool enabled) {
    RX_ASSERT_MAIN_THREAD("Window::setRelativeMouseMode");
    // Tracked regardless of the SDL call's own success -- see this
    // method's header comment: pumpEvents()'s FOCUS_GAINED handling needs
    // to know what the app WANTS, independent of whether SDL granted the
    // last request.
    relativeModeWanted_ = enabled;
    if (!SDL_SetWindowRelativeMouseMode(window_, enabled)) {
        RX_LOG_WARN("SDL_SetWindowRelativeMouseMode({}) failed: {}", enabled, SDL_GetError());
        return false;
    }
    return true;
}

bool Window::relativeMouseModeWanted() const {
    RX_ASSERT_MAIN_THREAD("Window::relativeMouseModeWanted");
    return relativeModeWanted_;
}

MouseDelta Window::consumeMouseDelta() {
    RX_ASSERT_MAIN_THREAD("Window::consumeMouseDelta");
    MouseDelta delta{accumMouseDeltaX_, accumMouseDeltaY_};
    accumMouseDeltaX_ = 0.0f;
    accumMouseDeltaY_ = 0.0f;
    return delta;
}

bool Window::setCursorVisible(bool visible) {
    RX_ASSERT_MAIN_THREAD("Window::setCursorVisible");
    const bool ok = visible ? SDL_ShowCursor() : SDL_HideCursor();
    if (!ok) {
        RX_LOG_WARN("{}() failed: {}", visible ? "SDL_ShowCursor" : "SDL_HideCursor", SDL_GetError());
    }
    return ok;
}

bool Window::cursorVisible() const {
    RX_ASSERT_MAIN_THREAD("Window::cursorVisible");
    return SDL_CursorVisible();
}

bool Window::setCursorConfined(bool confined) {
    RX_ASSERT_MAIN_THREAD("Window::setCursorConfined");
    if (!SDL_SetWindowMouseGrab(window_, confined)) {
        RX_LOG_WARN("SDL_SetWindowMouseGrab({}) failed: {}", confined, SDL_GetError());
        return false;
    }
    return true;
}

bool Window::cursorConfined() const {
    RX_ASSERT_MAIN_THREAD("Window::cursorConfined");
    return SDL_GetWindowMouseGrab(window_);
}

GamepadState Window::poll() const {
    RX_ASSERT_MAIN_THREAD("Window::poll");
    GamepadState state{};
    if (gamepads_.empty()) {
        return state;
    }

    // [Phase 4 Task 20, gate ruling #14, matrix row 9] Single-active rule:
    // the LOWEST currently-connected SDL_JoystickID -- deterministic, and
    // re-selected only when that specific pad disconnects (gamepads_ is
    // maintained exclusively by pumpEvents()'s ADDED/REMOVED handling, so
    // a disconnected pad's key is already gone by the time poll() runs).
    auto activeIt =
        std::min_element(gamepads_.begin(), gamepads_.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
    SDL_Gamepad* gamepad = activeIt->second;

    state.connected = true;
    state.leftStick = applyStickDeadzone(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX),
                                          SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
    state.rightStick = applyStickDeadzone(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX),
                                           SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
    state.leftTrigger = applyTriggerDeadzone(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    state.rightTrigger = applyTriggerDeadzone(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));

    state.buttons.dpadUp = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    state.buttons.dpadDown = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    state.buttons.dpadLeft = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    state.buttons.dpadRight = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    state.buttons.south = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
    state.buttons.east = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST);
    state.buttons.west = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST);
    state.buttons.north = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH);
    state.buttons.leftShoulder = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    state.buttons.rightShoulder = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    state.buttons.start = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START);

    return state;
}

bool Window::isKeyDown(SDL_Scancode key) const {
    RX_ASSERT_MAIN_THREAD("Window::isKeyDown");
    int numKeys = 0;
    const bool* state = SDL_GetKeyboardState(&numKeys);
    if (state == nullptr || key < 0 || key >= numKeys) {
        return false;
    }
    return state[key];
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
