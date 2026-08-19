// This is the only test translation unit in rx_platform_tests (unlike
// rx_core_tests, which shares a separate tests/doctest_main.cpp across
// several test files), so it both implements doctest's runtime and
// provides main() directly.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <rx_platform/window.h>
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <SDL3/SDL.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace {

// A real desktop's window manager (this repo's own dev machine, not just
// CI's bare Xvfb) delivers genuine SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
// events for a just-created window ASYNCHRONOUSLY -- with real, observed
// latency across more than one pumpEvents() call, not necessarily all
// present by the very first drain (reproduced directly during this task's
// own development: a single upfront pumpEvents() call was not sufficient
// to observe a stable baseline). Repolls until two consecutive drains
// report the identical size (quiescent), bounded so a genuinely
// never-settling backend still terminates promptly rather than hanging a
// test.
VkExtent2D drainUntilQuiescent(rx::platform::Window& window) {
    VkExtent2D previous = window.lastPixelSizeEvent();
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        window.pumpEvents();
        VkExtent2D current = window.lastPixelSizeEvent();
        if (current.width == previous.width && current.height == previous.height) {
            return current;
        }
        previous = current;
    }
    return previous;
}

}  // namespace

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

// ===== Window-event-observed state [Phase 4 Task 17, FG7, gate ruling #25]
// =========================================================================
// Device-free, two-tier test design's TIER 1 (matrix row 6): SDL_PushEvent
// synthesizes a MINIMIZED/RESTORED/PIXEL_SIZE_CHANGED sequence that flows
// through Window::pumpEvents() exactly like a real one -- per SDL3's own
// SDL_PushEvent doc ("pushing device input events onto the queue doesn't
// modify the state of the device within SDL"), this exercises
// Window's OWN state machine built on top of these events in complete
// isolation, with no Vulkan/Device object anywhere in this file. The
// AUTHORITATIVE suspended-present guard (extent-query-driven) lives in
// rx::rhi::Device::recreateSwapchain() and has its own GPU test elsewhere
// (rx_rhi_vk/tests/window_state_test.cpp) -- this file only proves the
// optimization/logging signal's own bookkeeping is correct.
TEST_CASE("Window::pumpEvents() flips minimizedEventObserved() on MINIMIZED/RESTORED and reads size ONLY from "
          "PIXEL_SIZE_CHANGED's data1/data2 -- never from MINIMIZED/RESTORED") {
    auto window = rx::platform::Window::create("rx_platform_event_state_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    const SDL_WindowID windowId = SDL_GetWindowID(window->sdlWindow());
    REQUIRE(windowId != 0);

    // Immediately after construction, with pumpEvents() never yet called,
    // lastPixelSizeEvent()/minimizedEventObserved() reflect only the
    // in-class defaults (nothing has been drained yet) -- true regardless
    // of environment, since nothing SDL-side has been processed at all.
    CHECK_FALSE(window->minimizedEventObserved());
    CHECK(window->lastPixelSizeEvent().width == 0);
    CHECK(window->lastPixelSizeEvent().height == 0);

    // From here on, this test is DELTA-based rather than assuming an exact
    // baseline: on a real desktop (this repo's own dev machine, not just
    // CI's bare Xvfb), SDL_CreateWindow() genuinely enqueues real window-
    // manager-driven events of its own (an initial
    // SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED reporting the real granted size,
    // sometimes more than one as the WM negotiates/settles) that keep
    // trickling in across the first several pumpEvents() calls,
    // independent of and racing against whatever this test pushes
    // synthetically -- reproduced directly during this task's own
    // development. Asserting "MINIMIZED/RESTORED never move the size" as a
    // DELTA against whatever the size happened to be immediately
    // beforehand is what makes this test correct under both a real desktop
    // and a bare headless backend, rather than flaking on the former.
    const VkExtent2D baselineSize = drainUntilQuiescent(*window);

    // MINIMIZED flips the flag true; carries no size payload -- the size
    // must stay exactly at whatever it was immediately beforehand.
    SDL_Event minimizedEvent{};
    minimizedEvent.window.type = SDL_EVENT_WINDOW_MINIMIZED;
    minimizedEvent.window.windowID = windowId;
    REQUIRE(SDL_PushEvent(&minimizedEvent));
    window->pumpEvents();
    CHECK(window->minimizedEventObserved());
    CHECK(window->lastPixelSizeEvent().width == baselineSize.width);
    CHECK(window->lastPixelSizeEvent().height == baselineSize.height);

    // PIXEL_SIZE_CHANGED is the ONLY event whose data1/data2 this class
    // ever reads as a size -- must not disturb minimizedEventObserved(),
    // and MUST overwrite the size to exactly the injected value (42x17 is
    // deliberately far from any real window size this test's own 64x64
    // window or its WM could plausibly report, so a pass here cannot be
    // explained by a coincidental real event instead of the synthetic
    // one).
    SDL_Event sizeEvent{};
    sizeEvent.window.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
    sizeEvent.window.windowID = windowId;
    sizeEvent.window.data1 = 42;
    sizeEvent.window.data2 = 17;
    REQUIRE(SDL_PushEvent(&sizeEvent));
    window->pumpEvents();
    CHECK(window->minimizedEventObserved());  // unchanged by the size event
    CHECK(window->lastPixelSizeEvent().width == 42);
    CHECK(window->lastPixelSizeEvent().height == 17);

    // RESTORED flips the flag back false; must NOT touch the last observed
    // size (RESTORED carries no size payload either) -- still exactly
    // {42, 17} from the synthetic PIXEL_SIZE_CHANGED above.
    SDL_Event restoredEvent{};
    restoredEvent.window.type = SDL_EVENT_WINDOW_RESTORED;
    restoredEvent.window.windowID = windowId;
    REQUIRE(SDL_PushEvent(&restoredEvent));
    window->pumpEvents();
    CHECK_FALSE(window->minimizedEventObserved());
    CHECK(window->lastPixelSizeEvent().width == 42);
    CHECK(window->lastPixelSizeEvent().height == 17);
}

TEST_CASE("Window::pumpEvents() ignores window events targeting a different SDL_WindowID") {
    auto window = rx::platform::Window::create("rx_platform_event_isolation_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());

    // A bogus windowID that (overwhelmingly likely) does not belong to any
    // real window in this process -- proves cross-window isolation without
    // needing to actually open a second Window.
    constexpr SDL_WindowID kBogusWindowId = 0xFFFFFFFEu;
    SDL_Event event{};
    event.window.type = SDL_EVENT_WINDOW_MINIMIZED;
    event.window.windowID = kBogusWindowId;
    REQUIRE(SDL_PushEvent(&event));
    window->pumpEvents();

    CHECK_FALSE(window->minimizedEventObserved());
}

// ===== logWaylandMinimizeLimitationOnce [gate ruling #25 row 3] ===========
// Device-free: no live Wayland session needed. platformName is a caller-
// supplied parameter specifically so this is testable this way (see
// window.h's own comment) -- captures the forwarded log via the same
// LogForwardSink mechanism rx_core/tests/log_test.cpp already uses.
namespace {

struct CapturedLog {
    std::mutex mutex;
    int callCount = 0;
    std::string lastMessage;
};

void captureLogCallback(int32_t /*severity*/, const char* /*category*/, const char* message, void* userData) {
    auto* captured = static_cast<CapturedLog*>(userData);
    std::lock_guard<std::mutex> lock(captured->mutex);
    captured->callCount++;
    captured->lastMessage = message != nullptr ? message : "";
}

struct ForwardCallbackGuard {
    std::shared_ptr<rx::core::log::LogForwardSink> sink;
    ~ForwardCallbackGuard() { (void)sink->set(nullptr, nullptr); }
};

}  // namespace

TEST_CASE("logWaylandMinimizeLimitationOnce fires exactly once for a mocked \"Wayland\" platform name and never "
          "for X11/Windows") {
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();
    CapturedLog captured;
    REQUIRE(sink->set(&captureLogCallback, &captured));
    ForwardCallbackGuard guard{sink};

    // Non-Wayland platform names never consume the one-shot budget and
    // never log.
    rx::platform::logWaylandMinimizeLimitationOnce("Windows");
    rx::platform::logWaylandMinimizeLimitationOnce("Linux");  // SDL_GetPlatform()'s own X11 return value.
    rx::platform::logWaylandMinimizeLimitationOnce(nullptr);
    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        CHECK(captured.callCount == 0);
    }

    // A real "Wayland" match logs exactly once, mentioning Wayland.
    rx::platform::logWaylandMinimizeLimitationOnce("Wayland");
    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        CHECK(captured.callCount == 1);
        CHECK(captured.lastMessage.find("Wayland") != std::string::npos);
    }

    // One-shot: a second (and third) "Wayland" call does not log again.
    rx::platform::logWaylandMinimizeLimitationOnce("Wayland");
    rx::platform::logWaylandMinimizeLimitationOnce("Wayland");
    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        CHECK(captured.callCount == 1);
    }
}

// ===== Borderless-fullscreen toggle [Phase 4 Task 17, FG7, gate ruling #25
// row 4] -- SDL-level only (no Vulkan/Device here; the real windowed<->
// fullscreen<->windowed GPU test, including the swapchain-recreation path,
// lives in rx_rhi_vk/tests/window_state_test.cpp). ======================
TEST_CASE("Window::setFullscreen(true) enters borderless-desktop fullscreen (SDL_GetWindowFullscreenMode() == "
          "nullptr); toggled back to windowed") {
    auto window = rx::platform::Window::create("rx_platform_fullscreen_test", 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping fullscreen check");
        return;
    }
    CHECK_FALSE(window->isFullscreen());

    if (!window->setFullscreen(true)) {
        MESSAGE("SDL_SetWindowFullscreen(true) failed on this video driver (e.g. dummy/offscreen), skipping the "
                 "rest of this check");
        return;
    }
    CHECK(window->isFullscreen());
    // Borderless, not exclusive [gate ruling #25 row 4's own acceptance
    // criterion]: setFullscreen() never calls SDL_SetWindowFullscreenMode(),
    // so SDL3's own documented borderless-desktop signal is a nullptr mode.
    CHECK(SDL_GetWindowFullscreenMode(window->sdlWindow()) == nullptr);

    CHECK(window->setFullscreen(false));
    CHECK_FALSE(window->isFullscreen());
}
