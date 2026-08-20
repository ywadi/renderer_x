// This is the only test translation unit in rx_platform_tests (unlike
// rx_core_tests, which shares a separate tests/doctest_main.cpp across
// several test files), so it both implements doctest's runtime and
// provides main() directly.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <rx_core/debug_checks.h>
#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>
#include <rx_platform/window.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// [Phase 4 Task 20, gate ruling #14] Attaches a fully-populated virtual
// gamepad and owns its lifecycle. axis_mask/button_mask use the
// SDL_GamepadAxis/SDL_GamepadButton enum values directly as bit indices
// (SDL_joystick.h's own doc comment); this task additionally confirmed
// EMPIRICALLY (the gate-mandated smoke check, see task-20-report.md) that
// a virtual joystick's raw axis/button INDEX equals the corresponding
// SDL_GamepadAxis/SDL_GamepadButton enum value for a device declared
// SDL_JOYSTICK_TYPE_GAMEPAD -- not just assumed from the doc comment.
// setAxis()/setButton() both call the real SDL_PumpEvents() themselves:
// SDL_SetJoystickVirtualAxis()'s own doc states values are "not applied
// until the next call to SDL_UpdateJoysticks" (which SDL_PumpEvents calls
// internally) -- a real, non-obvious gotcha this task's smoke check
// found. Calling raw SDL_PumpEvents() here is safe with respect to a
// caller's own Window::pumpEvents() sequencing: SDL_PumpEvents() only
// GATHERS new events into the queue, it never DEQUEUES anything (that is
// SDL_PollEvent's job, which only Window::pumpEvents() calls) -- so it
// cannot cause a GAMEPAD_ADDED/REMOVED event sitting in the queue to be
// lost out from under a Window that hasn't drained it yet.
struct VirtualGamepad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;

    explicit VirtualGamepad(const char* name = "rx_platform_test_pad") {
        // [Fix round, found during this task's own smoke checking] SDL3's
        // gamepad-mapping cache keys its NAME lookup by (VID, PID, ...),
        // not by SDL_VirtualJoystickDesc.name alone -- every VirtualGamepad
        // in this file left vendor_id/product_id at their SDL_INIT_INTERFACE
        // zero default, so a SECOND virtual pad attached in the same
        // process reused the FIRST one's cached mapping-string name via
        // SDL_GetGamepadName(), silently ignoring its own `name` field
        // (reproduced directly: every gamepad test after the first in a
        // full-suite run logged the FIRST test's pad name, never its own).
        // A per-instance unique product_id keeps each attached pad's own
        // mapping-cache entry (and therefore its own name) distinct. This
        // is purely a virtual-joystick TEST artifact of sharing one
        // process across many attach/detach cycles -- production callers
        // read SDL_GetGamepadName() against real hardware, which always
        // has genuine distinct VID/PID values.
        static std::atomic<Uint16> nextProductId{1};
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0x0001;
        desc.product_id = nextProductId.fetch_add(1, std::memory_order_relaxed);
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
        desc.name = name;
        id = SDL_AttachVirtualJoystick(&desc);
        if (id != 0) {
            joystick = SDL_OpenJoystick(id);
        }
    }
    VirtualGamepad(const VirtualGamepad&) = delete;
    VirtualGamepad& operator=(const VirtualGamepad&) = delete;
    ~VirtualGamepad() { reset(); }

    // Detaches early (on demand, e.g. to test a specific device
    // disconnecting mid-test) -- idempotent, so the destructor's own call
    // is always safe even after an explicit reset() already ran.
    void reset() {
        if (joystick != nullptr) {
            SDL_CloseJoystick(joystick);
            joystick = nullptr;
        }
        if (id != 0) {
            SDL_DetachVirtualJoystick(id);
            id = 0;
        }
    }

    void setAxis(SDL_GamepadAxis axis, Sint16 value) const {
        REQUIRE(SDL_SetJoystickVirtualAxis(joystick, static_cast<int>(axis), value));
        SDL_PumpEvents();
    }
    void setButton(SDL_GamepadButton button, bool down) const {
        REQUIRE(SDL_SetJoystickVirtualButton(joystick, static_cast<int>(button), down));
        SDL_PumpEvents();
    }
};

// [Phase 4 Task 20] Some environments -- this project's own dev machine
// included, discovered empirically during this task -- have a REAL
// gamepad already connected before any test runs (this dev machine has a
// physically-plugged-in USB "Generic X-Box pad", confirmed via
// /proc/bus/input/devices -- not a test artifact). CI's own bare runner
// is not expected to have one, but these tests are written to be CORRECT
// regardless of that, never assuming a clean gamepad environment --
// mirrors this file's own existing "delta-based, not absolute-baseline"
// WM/driver-conditional convention (see e.g. the borderless-fullscreen
// and cursor-confine tests). Window::poll()'s single-active rule (gate
// matrix row 9) selects the LOWEST connected SDL_JoystickID process-wide
// -- a real device discovered before this test's own virtual pad(s)
// ALWAYS has a lower ID (SDL's instance-ID counter is monotonic and never
// reused within a process), so it would win selection every time,
// starving this test's own driven values out of poll()'s output entirely.
// Returns every SDL_JoystickID SDL_GetGamepads() currently reports that
// is NOT one of `mine`.
std::vector<SDL_JoystickID> foreignGamepadIds(std::initializer_list<SDL_JoystickID> mine) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    std::vector<SDL_JoystickID> foreign;
    for (int i = 0; i < count; ++i) {
        bool isMine = false;
        for (SDL_JoystickID m : mine) {
            if (ids[i] == m) {
                isMine = true;
                break;
            }
        }
        if (!isMine) {
            foreign.push_back(ids[i]);
        }
    }
    SDL_free(ids);
    return foreign;
}

// [gate ruling #25 row 3 / Phase 4 Task 20] Shared LogForwardSink capture
// helper -- used by both logWaylandMinimizeLimitationOnce's own test
// (Task 17) and the gamepad gyro log-don't-drop test (Task 20) below.
// `allMessages` (not just `lastMessage`) exists specifically because the
// gyro test can race a real foreign gamepad's OWN "gamepad connected" log
// line (see foreignGamepadIds()'s own comment) -- searching the full
// accumulated capture is robust to interleaving order; `lastMessage` is
// kept for the Wayland test's own single-expected-call usage.
struct CapturedLog {
    std::mutex mutex;
    int callCount = 0;
    std::string lastMessage;
    std::string allMessages;
};

void captureLogCallback(int32_t /*severity*/, const char* /*category*/, const char* message, void* userData) {
    auto* captured = static_cast<CapturedLog*>(userData);
    std::lock_guard<std::mutex> lock(captured->mutex);
    captured->callCount++;
    captured->lastMessage = message != nullptr ? message : "";
    captured->allMessages += captured->lastMessage;
    captured->allMessages += '\n';
}

struct ForwardCallbackGuard {
    std::shared_ptr<rx::core::log::LogForwardSink> sink;
    ~ForwardCallbackGuard() { (void)sink->set(nullptr, nullptr); }
};

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

// ===== Deadzone math [Phase 4 Task 20, gate ruling #14, matrix rows 5/6]
// =========================================================================
// Pure math, zero SDL dependency at all (rx_platform/input.h has no SDL
// include) -- device-free by construction, not merely "runs under a dummy
// driver."
TEST_CASE("applyStickDeadzone(): below-threshold on-axis point is exactly zero") {
    // 4000 raw on one axis alone: mag = 4000/32768 ~= 0.1221, well under
    // the default deadzone ratio (8000/32768 ~= 0.2441).
    auto result = rx::platform::applyStickDeadzone(4000.0f, 0.0f);
    CHECK(result.x == doctest::Approx(0.0f));
    CHECK(result.y == doctest::Approx(0.0f));
}

TEST_CASE("applyStickDeadzone(): centered stick (0,0) is exactly zero -- no NaN/div-by-zero") {
    auto result = rx::platform::applyStickDeadzone(0.0f, 0.0f);
    CHECK(result.x == 0.0f);
    CHECK(result.y == 0.0f);
}

TEST_CASE("applyStickDeadzone(): diagonal-snap discrimination -- a point whose PER-AXIS values are both below "
          "the naive 8000 per-axis threshold, but whose COMBINED radial magnitude exceeds the deadzone radius, "
          "is non-zero under the correct radial formula -- proving a naive independent-per-axis-clamp check "
          "(which would zero BOTH axes here) is wrong") {
    // (6000, 6000): magnitude = 6000*sqrt(2) ~= 8485.28, /32768 ~= 0.2589
    // -- ABOVE the 0.2441 deadzone ratio, so the radial formula's output
    // must be non-zero. A naive per-axis clamp ("if abs(axis) < 8000:
    // axis = 0", applied independently per axis) would zero BOTH axes
    // here (6000 < 8000 on each) -- the exact "snap to cardinal
    // directions" anti-pattern the matrix's first-tier precedent
    // (Sutphin, Microsoft XInput) names.
    auto result = rx::platform::applyStickDeadzone(6000.0f, 6000.0f);
    CHECK(result.x > 0.0f);
    CHECK(result.y > 0.0f);
    const bool naivePerAxisWouldZeroBoth = (std::fabs(6000.0f) < 8000.0f) && (std::fabs(6000.0f) < 8000.0f);
    CHECK(naivePerAxisWouldZeroBoth);  // documents exactly what the (wrong) naive check would conclude here
}

TEST_CASE("applyStickDeadzone(): MANDATORY (8500,8500) discrimination case [gate hardening block] -- a naive "
          "unscaled-per-axis-clamp implementation ALSO calls this point non-zero (8500 clears the naive 8000 "
          "per-axis threshold on both axes), so a bare zero/non-zero test cannot tell the two implementations "
          "apart; only the exact scaled OUTPUT VALUE can") {
    // (8500, 8500): raw magnitude = 8500*sqrt(2) ~= 12020.815, mag ~=
    // 0.36685. Correct scaled-radial formula: scaledMag =
    // (0.36685 - 0.24414) / (1 - 0.24414) ~= 0.16235, direction
    // (0.70711, 0.70711) -> output ~= (0.11480, 0.11480), output
    // magnitude ~= 0.16235.
    auto result = rx::platform::applyStickDeadzone(8500.0f, 8500.0f);
    CHECK(result.x == doctest::Approx(0.11480f).epsilon(0.002));
    CHECK(result.y == doctest::Approx(0.11480f).epsilon(0.002));
    const float outputMagnitude = std::sqrt(result.x * result.x + result.y * result.y);
    CHECK(outputMagnitude == doctest::Approx(0.16235f).epsilon(0.002));

    // The naive-unscaled-per-axis-clamp comparator this case discriminates
    // against, spelled out explicitly (never actually used by
    // applyStickDeadzone() -- documents what a WRONG implementation would
    // have produced at this exact point: raw/32768 per axis, unscaled,
    // since neither axis individually trips the naive 8000 threshold).
    const float naiveX = (std::fabs(8500.0f) < 8000.0f) ? 0.0f : 8500.0f / 32768.0f;
    CHECK(naiveX == doctest::Approx(0.25940f).epsilon(0.002));
    CHECK(result.x != doctest::Approx(naiveX).epsilon(0.02));  // NOT equivalent -- the whole point of this case
}

TEST_CASE("applyTriggerDeadzone(): below-threshold is exactly zero; max reports 1.0; a mid-range value rescales "
          "linearly") {
    CHECK(rx::platform::applyTriggerDeadzone(0.0f) == doctest::Approx(0.0f));
    CHECK(rx::platform::applyTriggerDeadzone(500.0f) == doctest::Approx(0.0f));  // below default ~1000/32767
    CHECK(rx::platform::applyTriggerDeadzone(32767.0f) == doctest::Approx(1.0f));
    // raw=16000: t=16000/32767, deadzone=1000/32767 -> scaled =
    // (16000-1000)/(32767-1000) = 15000/31767 ~= 0.47219.
    CHECK(rx::platform::applyTriggerDeadzone(16000.0f) == doctest::Approx(0.47219f).epsilon(0.002));
}

// ===== Mouse: relative mode, delta accumulation, cursor [Phase 4 Task 20,
// gate ruling #14, matrix rows 1-3] ========================================
TEST_CASE("Window::consumeMouseDelta() sums xrel/yrel across N synthetic MOUSE_MOTION events (consume-and-reset, "
          "gate matrix row 2)") {
    auto window = rx::platform::Window::create("rx_platform_mouse_delta_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    const SDL_WindowID windowId = SDL_GetWindowID(window->sdlWindow());
    REQUIRE(windowId != 0);
    window->pumpEvents();               // drain any stray leftover events first.
    (void)window->consumeMouseDelta();  // reset baseline to {0,0} regardless of what drained above.

    struct Sample {
        float xrel;
        float yrel;
    };
    const Sample samples[] = {{1.5f, -2.25f}, {10.0f, 0.0f}, {-3.75f, 4.0f}};
    float expectedX = 0.0f;
    float expectedY = 0.0f;
    for (const auto& s : samples) {
        SDL_Event event{};
        event.motion.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = windowId;
        event.motion.xrel = s.xrel;
        event.motion.yrel = s.yrel;
        REQUIRE(SDL_PushEvent(&event));
        expectedX += s.xrel;
        expectedY += s.yrel;
    }
    window->pumpEvents();

    auto delta = window->consumeMouseDelta();
    CHECK(delta.x == doctest::Approx(expectedX));
    CHECK(delta.y == doctest::Approx(expectedY));

    // Consume-and-reset: a second call with no new events returns {0,0}.
    auto second = window->consumeMouseDelta();
    CHECK(second.x == 0.0f);
    CHECK(second.y == 0.0f);
}

TEST_CASE("Window::consumeMouseDelta() ignores MOUSE_MOTION events targeting a different SDL_WindowID") {
    auto window = rx::platform::Window::create("rx_platform_mouse_delta_isolation_test", 64, 64, false);
    REQUIRE(window.has_value());
    window->pumpEvents();
    (void)window->consumeMouseDelta();

    constexpr SDL_WindowID kBogusWindowId = 0xFFFFFFFEu;
    SDL_Event event{};
    event.motion.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = kBogusWindowId;
    event.motion.xrel = 500.0f;
    event.motion.yrel = 500.0f;
    REQUIRE(SDL_PushEvent(&event));
    window->pumpEvents();

    auto delta = window->consumeMouseDelta();
    CHECK(delta.x == 0.0f);
    CHECK(delta.y == 0.0f);
}

TEST_CASE("Window::pumpEvents(): FOCUS_LOST pauses mouse-delta accumulation; FOCUS_GAINED resumes it and "
          "unconditionally re-arms relative mode if the app still wants it [gate matrix row 1]") {
    auto window = rx::platform::Window::create("rx_platform_focus_pause_test", 64, 64, false);
    REQUIRE(window.has_value());
    const SDL_WindowID windowId = SDL_GetWindowID(window->sdlWindow());
    window->pumpEvents();
    (void)window->consumeMouseDelta();

    window->setRelativeMouseMode(true);  // best-effort; relativeModeWanted_ tracks true regardless of grant.
    CHECK(window->relativeMouseModeWanted());

    // Baseline: motion accumulates normally before any focus-loss.
    SDL_Event motionBefore{};
    motionBefore.motion.type = SDL_EVENT_MOUSE_MOTION;
    motionBefore.motion.windowID = windowId;
    motionBefore.motion.xrel = 5.0f;
    motionBefore.motion.yrel = 5.0f;
    REQUIRE(SDL_PushEvent(&motionBefore));
    window->pumpEvents();
    auto beforeDelta = window->consumeMouseDelta();
    CHECK(beforeDelta.x == doctest::Approx(5.0f));
    CHECK(beforeDelta.y == doctest::Approx(5.0f));

    // FOCUS_LOST -> motion is drained but NOT accumulated.
    SDL_Event focusLost{};
    focusLost.window.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    focusLost.window.windowID = windowId;
    REQUIRE(SDL_PushEvent(&focusLost));
    window->pumpEvents();

    SDL_Event motionDuring{};
    motionDuring.motion.type = SDL_EVENT_MOUSE_MOTION;
    motionDuring.motion.windowID = windowId;
    motionDuring.motion.xrel = 100.0f;
    motionDuring.motion.yrel = 100.0f;
    REQUIRE(SDL_PushEvent(&motionDuring));
    window->pumpEvents();
    auto duringDelta = window->consumeMouseDelta();
    CHECK(duringDelta.x == 0.0f);
    CHECK(duringDelta.y == 0.0f);

    // FOCUS_GAINED -> resumes; a later motion event accumulates again.
    SDL_Event focusGained{};
    focusGained.window.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    focusGained.window.windowID = windowId;
    REQUIRE(SDL_PushEvent(&focusGained));
    window->pumpEvents();

    SDL_Event motionAfter{};
    motionAfter.motion.type = SDL_EVENT_MOUSE_MOTION;
    motionAfter.motion.windowID = windowId;
    motionAfter.motion.xrel = 7.0f;
    motionAfter.motion.yrel = 7.0f;
    REQUIRE(SDL_PushEvent(&motionAfter));
    window->pumpEvents();
    auto afterDelta = window->consumeMouseDelta();
    CHECK(afterDelta.x == doctest::Approx(7.0f));
    CHECK(afterDelta.y == doctest::Approx(7.0f));
}

TEST_CASE("Window::setRelativeMouseMode()/relativeMouseModeWanted() track APP intent independent of SDL's own "
          "query; SDL_GetWindowRelativeMouseMode() round-trips where the driver actually grants it [gate matrix "
          "row 1]") {
    auto window = rx::platform::Window::create("rx_platform_relative_mode_test", 64, 64, false);
    REQUIRE(window.has_value());
    CHECK_FALSE(window->relativeMouseModeWanted());

    const bool enabledOk = window->setRelativeMouseMode(true);
    CHECK(window->relativeMouseModeWanted());  // app intent updates regardless of SDL's own success.
    if (!enabledOk || !SDL_GetWindowRelativeMouseMode(window->sdlWindow())) {
        MESSAGE(
            "this video driver never actually granted relative mouse mode (e.g. CI's bare Xvfb, no window "
            "manager) -- app-intent tracking (relativeMouseModeWanted()), the unconditional floor this test "
            "already checked above, is still correct regardless; skipping the SDL-level round-trip assertion.");
    } else {
        CHECK(SDL_GetWindowRelativeMouseMode(window->sdlWindow()));
    }

    const bool disabledOk = window->setRelativeMouseMode(false);
    CHECK_FALSE(window->relativeMouseModeWanted());
    if (disabledOk) {
        CHECK_FALSE(SDL_GetWindowRelativeMouseMode(window->sdlWindow()));
    }
}

TEST_CASE("Window::setCursorVisible()/cursorVisible() round-trip through SDL_CursorVisible() [gate matrix row "
          "3]") {
    auto window = rx::platform::Window::create("rx_platform_cursor_visible_test", 64, 64, false);
    REQUIRE(window.has_value());

    REQUIRE(window->setCursorVisible(false));
    CHECK_FALSE(window->cursorVisible());
    CHECK_FALSE(SDL_CursorVisible());

    REQUIRE(window->setCursorVisible(true));
    CHECK(window->cursorVisible());
    CHECK(SDL_CursorVisible());
}

TEST_CASE("Window::setCursorConfined()/cursorConfined() round-trip through SDL_GetWindowMouseGrab() [gate matrix "
          "row 3]") {
    auto window = rx::platform::Window::create("rx_platform_cursor_confined_test", 64, 64, false);
    REQUIRE(window.has_value());
    CHECK_FALSE(window->cursorConfined());

    // Mirrors Task 17's own established "request succeeded but the driver
    // never actually granted it" gating (window_state_test.cpp's
    // fullscreen double-toggle fix round 1): SDL_SetWindowMouseGrab(true)
    // can report success on CI's bare Xvfb (no window manager to reject
    // the request against) while the grab is never actually applied --
    // reproduced directly under xvfb-run during this task. Check the
    // ACTUAL resulting state, not just the call's own return value.
    const bool requestOk = window->setCursorConfined(true);
    if (!requestOk || !window->cursorConfined()) {
        MESSAGE(
            "SDL_SetWindowMouseGrab(true) either failed outright or reported success without this "
            "driver/window-manager actually granting the grab (e.g. CI's bare Xvfb) -- skipping the rest of "
            "this check.");
        return;
    }
    CHECK(SDL_GetWindowMouseGrab(window->sdlWindow()));

    CHECK(window->setCursorConfined(false));
    CHECK_FALSE(window->cursorConfined());
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

// [Phase 4 Task 21, gate ruling #16] Device-free proof of the `preDispatch`
// seam pumpEvents() gained for the ImGui overlay: (a) it fires exactly once
// per drained event, in drain order, and (b) it genuinely runs BEFORE this
// class's own handling of that SAME event -- not merely "at some point
// during the same pumpEvents() call". (b) is the load-bearing half (matches
// ImGui_ImplSDL3_ProcessEvent()'s own real requirement: its IO state must be
// current before anything downstream reads WantCaptureMouse/Keyboard this
// frame) and is proven directly, not inferred: the callback reads
// minimizedEventObserved() for the very MINIMIZED event it was just handed
// and asserts it is STILL false at that moment, only becoming true once
// pumpEvents() itself returns.
TEST_CASE("Window::pumpEvents(preDispatch) invokes the callback once per drained event, strictly BEFORE this "
          "class's own handling of that same event") {
    auto window = rx::platform::Window::create("rx_platform_predispatch_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    const SDL_WindowID windowId = SDL_GetWindowID(window->sdlWindow());
    REQUIRE(windowId != 0);

    // Drain whatever the platform/WM enqueued on its own first (see
    // drainUntilQuiescent()'s own comment above) so the synthetic events
    // pushed below are the only ones this test's own assertions reason
    // about.
    (void)drainUntilQuiescent(*window);

    std::vector<Uint32> observedEventTypes;
    bool minimizedObservedInsideCallback = true;  // deliberately wrong-default: only false proves the ordering.
    auto preDispatch = [&](const SDL_Event& event) {
        observedEventTypes.push_back(event.type);
        if (event.type == SDL_EVENT_WINDOW_MINIMIZED) {
            minimizedObservedInsideCallback = window->minimizedEventObserved();
        }
    };

    SDL_Event minimizedEvent{};
    minimizedEvent.window.type = SDL_EVENT_WINDOW_MINIMIZED;
    minimizedEvent.window.windowID = windowId;
    REQUIRE(SDL_PushEvent(&minimizedEvent));

    SDL_Event sizeEvent{};
    sizeEvent.window.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
    sizeEvent.window.windowID = windowId;
    sizeEvent.window.data1 = 99;
    sizeEvent.window.data2 = 88;
    REQUIRE(SDL_PushEvent(&sizeEvent));

    window->pumpEvents(preDispatch);

    // Both synthetic events reached the callback, in the order they were
    // drained (SDL_PollEvent is FIFO).
    REQUIRE(observedEventTypes.size() >= 2);
    auto minimizedPos = std::find(observedEventTypes.begin(), observedEventTypes.end(), SDL_EVENT_WINDOW_MINIMIZED);
    auto sizePos =
        std::find(observedEventTypes.begin(), observedEventTypes.end(), SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
    REQUIRE(minimizedPos != observedEventTypes.end());
    REQUIRE(sizePos != observedEventTypes.end());
    CHECK(minimizedPos < sizePos);

    // The ordering proof: at the moment the callback saw the MINIMIZED
    // event, Window's OWN handling of that same event had not run yet.
    CHECK_FALSE(minimizedObservedInsideCallback);
    // ... but by the time pumpEvents() returns, it has.
    CHECK(window->minimizedEventObserved());
    CHECK(window->lastPixelSizeEvent().width == 99);
    CHECK(window->lastPixelSizeEvent().height == 88);
}

// Regression guard: every call site written before this task calls
// pumpEvents() with no argument at all -- the default `nullptr` callback
// must behave as a plain no-op check per event (never crash, never alter
// any existing behavior). Reuses the exact MINIMIZED/RESTORED assertions
// from the very first TEST_CASE in this file as a smoke check.
TEST_CASE("Window::pumpEvents() with no preDispatch argument behaves exactly as before (default nullptr is a "
          "no-op)") {
    auto window = rx::platform::Window::create("rx_platform_predispatch_default_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    const SDL_WindowID windowId = SDL_GetWindowID(window->sdlWindow());
    REQUIRE(windowId != 0);
    (void)drainUntilQuiescent(*window);

    SDL_Event minimizedEvent{};
    minimizedEvent.window.type = SDL_EVENT_WINDOW_MINIMIZED;
    minimizedEvent.window.windowID = windowId;
    REQUIRE(SDL_PushEvent(&minimizedEvent));
    window->pumpEvents();  // no preDispatch argument -- must not crash or misbehave.

    CHECK(window->minimizedEventObserved());
}

// ===== logWaylandMinimizeLimitationOnce [gate ruling #25 row 3] ===========
// Device-free: no live Wayland session needed. platformName is a caller-
// supplied parameter specifically so this is testable this way (see
// window.h's own comment) -- captures the forwarded log via the shared
// LogForwardSink helper above (also used by Task 20's gyro log-don't-drop
// test below).
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

// ===== Resizable window flag [Issue #36] ===================================
TEST_CASE("Window::create()'s `resizable` parameter threads through to SDL_WINDOW_RESIZABLE -- defaults to false "
          "(every caller written before this parameter existed keeps its pre-existing non-resizable behavior "
          "unchanged), true when explicitly requested [revert-discrimination: an implementation that ignored the "
          "parameter and always/never set the flag fails one of the three cases below]") {
    auto defaulted = rx::platform::Window::create("rx_platform_resizable_default_test", 64, 64, /*visible=*/false);
    REQUIRE(defaulted.has_value());
    CHECK((SDL_GetWindowFlags(defaulted->sdlWindow()) & SDL_WINDOW_RESIZABLE) == 0);

    auto explicitFalse =
        rx::platform::Window::create("rx_platform_resizable_false_test", 64, 64, /*visible=*/false, /*resizable=*/false);
    REQUIRE(explicitFalse.has_value());
    CHECK((SDL_GetWindowFlags(explicitFalse->sdlWindow()) & SDL_WINDOW_RESIZABLE) == 0);

    auto resizable =
        rx::platform::Window::create("rx_platform_resizable_true_test", 64, 64, /*visible=*/false, /*resizable=*/true);
    REQUIRE(resizable.has_value());
    CHECK((SDL_GetWindowFlags(resizable->sdlWindow()) & SDL_WINDOW_RESIZABLE) != 0);
}

// ===== Gamepad [Phase 4 Task 20, gate ruling #14, matrix rows 4-10] =======
// Every test below drives a REAL SDL_Gamepad through SDL_AttachVirtualJoystick
// (VirtualGamepad helper, top of this file) -- CI-automatable per the gate
// hardening block's TEST-COVERAGE CORRECTION (matrix Conflict #3),
// confirmed working under this project's CI-representative headless driver
// (xvfb-run -a) by this task's own smoke check (task-20-report.md). Tests
// that need to read THIS test's own driven values back through
// Window::poll() first check foreignGamepadIds() (top of file) and skip
// with an honest MESSAGE if a real gamepad is already connected in this
// environment -- see that helper's own comment for why (this dev machine
// itself has one plugged in, discovered during this task).
TEST_CASE("Window::poll(): hot-plug lifecycle -- GAMEPAD_ADDED opens+tracks, GAMEPAD_REMOVED closes+untracks, "
          "synchronously via pumpEvents() [gate matrix row 4]") {
    auto window = rx::platform::Window::create("rx_platform_gamepad_hotplug_test", 64, 64, false);
    REQUIRE(window.has_value());
    window->pumpEvents();

    if (!foreignGamepadIds({}).empty()) {
        MESSAGE(
            "a real gamepad is already connected in this environment (see foreignGamepadIds()'s own comment) "
            "-- Window::poll()'s single-active 'lowest ID' rule always selects it over this test's own virtual "
            "pad, so the connected/disconnected TOGGLE this test wants to observe cannot be reproduced here. "
            "The open-on-ADDED/close-then-erase-on-REMOVED mechanics are still exercised below unconditionally "
            "and are independently proven correct by this task's own gate-mandated smoke check "
            "(task-20-report.md).");
        VirtualGamepad pad("rx_platform_hotplug_pad");
        REQUIRE(pad.id != 0);
        window->pumpEvents();
        pad.reset();
        window->pumpEvents();
        return;
    }

    CHECK_FALSE(window->poll().connected);
    {
        VirtualGamepad pad("rx_platform_hotplug_pad");
        REQUIRE(pad.id != 0);
        window->pumpEvents();
        CHECK(window->poll().connected);
    }
    window->pumpEvents();  // drains the REMOVED event pushed by ~VirtualGamepad() above.
    CHECK_FALSE(window->poll().connected);
}

TEST_CASE("Window::poll(): rapid attach-detach-attach leaves no stale-handle/stale-value confusion [gate matrix "
          "row 4]") {
    auto window = rx::platform::Window::create("rx_platform_gamepad_rapid_test", 64, 64, false);
    REQUIRE(window.has_value());
    window->pumpEvents();

    if (!foreignGamepadIds({}).empty()) {
        MESSAGE(
            "a real gamepad is already connected in this environment -- skipping (see foreignGamepadIds()'s "
            "own comment); the hot-plug open/close mechanics this test would otherwise re-exercise are already "
            "covered unconditionally by the hot-plug lifecycle test above.");
        return;
    }

    CHECK_FALSE(window->poll().connected);

    {
        VirtualGamepad first("rx_platform_rapid_pad_1");
        REQUIRE(first.id != 0);
        window->pumpEvents();
        first.setButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
        CHECK(window->poll().connected);
        CHECK(window->poll().buttons.south);
    }
    window->pumpEvents();
    CHECK_FALSE(window->poll().connected);

    {
        VirtualGamepad second("rx_platform_rapid_pad_2");
        REQUIRE(second.id != 0);
        window->pumpEvents();
        // A freshly-attached device starts with south NOT pressed --
        // proves this isn't reading a stale cached value left over from
        // `first`'s handle/state.
        CHECK(window->poll().connected);
        CHECK_FALSE(window->poll().buttons.south);
    }
    window->pumpEvents();
    CHECK_FALSE(window->poll().connected);
}

TEST_CASE("Window::poll(): single-active-gamepad rule selects the LOWEST connected SDL_JoystickID; disconnecting "
          "it re-selects the next-lowest [gate matrix row 9]") {
    auto window = rx::platform::Window::create("rx_platform_gamepad_active_test", 64, 64, false);
    REQUIRE(window.has_value());
    window->pumpEvents();

    VirtualGamepad podA("rx_platform_active_pad_a");
    REQUIRE(podA.id != 0);
    window->pumpEvents();
    VirtualGamepad podB("rx_platform_active_pad_b");
    REQUIRE(podB.id != 0);
    window->pumpEvents();
    REQUIRE(podA.id != podB.id);

    if (!foreignGamepadIds({podA.id, podB.id}).empty()) {
        MESSAGE(
            "a real gamepad is already connected in this environment -- it always has a LOWER SDL_JoystickID "
            "than these freshly-attached virtual pads (SDL's instance-ID counter is monotonic and never reused "
            "within a process), so it -- not either of this test's own pads -- would win poll()'s 'lowest ID' "
            "selection every time, making this test's assertions unobservable here. Skipping (see "
            "foreignGamepadIds()'s own comment).");
        return;
    }

    VirtualGamepad& lowerPad = (podA.id < podB.id) ? podA : podB;
    VirtualGamepad& higherPad = (podA.id < podB.id) ? podB : podA;

    lowerPad.setButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
    higherPad.setButton(SDL_GAMEPAD_BUTTON_SOUTH, false);
    higherPad.setButton(SDL_GAMEPAD_BUTTON_EAST, true);
    {
        auto state = window->poll();
        CHECK(state.connected);
        CHECK(state.buttons.south);  // active == lower-ID pad
        CHECK_FALSE(state.buttons.east);
    }

    // Detach the active (lower-ID) pad -- the higher-ID one must become
    // active, reflecting ITS OWN (deliberately different) button state.
    lowerPad.reset();
    window->pumpEvents();
    {
        auto state = window->poll();
        CHECK(state.connected);
        CHECK_FALSE(state.buttons.south);
        CHECK(state.buttons.east);
    }
}

TEST_CASE("Window::poll(): stick axes go through applyStickDeadzone() end-to-end, including the MANDATORY "
          "(8500,8500) discrimination case, via a real virtual joystick [gate hardening block]") {
    auto window = rx::platform::Window::create("rx_platform_gamepad_axis_test", 64, 64, false);
    REQUIRE(window.has_value());
    window->pumpEvents();

    VirtualGamepad pad("rx_platform_axis_pad");
    REQUIRE(pad.id != 0);
    window->pumpEvents();

    if (!foreignGamepadIds({pad.id}).empty()) {
        MESSAGE(
            "a real gamepad is already connected in this environment and outranks this test's own virtual pad "
            "for poll()'s 'lowest ID' selection -- skipping the poll()-mediated assertions (see "
            "foreignGamepadIds()'s own comment). The deadzone MATH itself, including this exact (8500,8500) "
            "case, is unconditionally covered by the pure-math applyStickDeadzone() tests above, and the "
            "SDL_GetGamepadAxis() wiring/index-mapping was independently confirmed by this task's own smoke "
            "check (task-20-report.md).");
        return;
    }

    pad.setAxis(SDL_GAMEPAD_AXIS_LEFTX, 8500);
    pad.setAxis(SDL_GAMEPAD_AXIS_LEFTY, 8500);
    {
        auto state = window->poll();
        REQUIRE(state.connected);
        CHECK(state.leftStick.x == doctest::Approx(0.11480f).epsilon(0.01));
        CHECK(state.leftStick.y == doctest::Approx(0.11480f).epsilon(0.01));
    }

    pad.setAxis(SDL_GAMEPAD_AXIS_RIGHTX, 4000);
    pad.setAxis(SDL_GAMEPAD_AXIS_RIGHTY, 0);
    {
        auto state = window->poll();
        CHECK(state.rightStick.x == 0.0f);
        CHECK(state.rightStick.y == 0.0f);
    }
}

TEST_CASE("Window::poll(): trigger axes go through applyTriggerDeadzone() end-to-end -- SDL's own RAW "
          "joystick-axis trigger rest value is SDL_JOYSTICK_AXIS_MIN, not 0 [gate matrix row 6]") {
    auto window = rx::platform::Window::create("rx_platform_gamepad_trigger_test", 64, 64, false);
    REQUIRE(window.has_value());
    window->pumpEvents();

    VirtualGamepad pad("rx_platform_trigger_pad");
    REQUIRE(pad.id != 0);
    window->pumpEvents();

    if (!foreignGamepadIds({pad.id}).empty()) {
        MESSAGE(
            "a real gamepad is already connected in this environment and outranks this test's own virtual pad "
            "for poll()'s 'lowest ID' selection -- skipping (see foreignGamepadIds()'s own comment). The "
            "deadzone math itself is unconditionally covered by the pure-math applyTriggerDeadzone() tests "
            "above.");
        return;
    }

    // Per SDL_SetJoystickVirtualAxis()'s own doc: "when sending trigger
    // axes... a trigger at rest would have the value of
    // SDL_JOYSTICK_AXIS_MIN" at the RAW joystick-axis level -- SDL's
    // gamepad mapping layer translates that to SDL_GetGamepadAxis()'s
    // documented 0=released convention this project's poll() consumes.
    pad.setAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    CHECK(window->poll().leftTrigger == doctest::Approx(0.0f));

    pad.setAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    CHECK(window->poll().rightTrigger == doctest::Approx(1.0f));
}

TEST_CASE("Window::poll(): the full discrete button surface (D-pad x4, four face buttons, both shoulders, "
          "START) reflects each button independently -- SOUTH/EAST/WEST/NORTH, never A/B [gate ruling #14, "
          "matrix row 7]") {
    auto window = rx::platform::Window::create("rx_platform_gamepad_buttons_test", 64, 64, false);
    REQUIRE(window.has_value());
    window->pumpEvents();

    VirtualGamepad pad("rx_platform_buttons_pad");
    REQUIRE(pad.id != 0);
    window->pumpEvents();

    if (!foreignGamepadIds({pad.id}).empty()) {
        MESSAGE(
            "a real gamepad is already connected in this environment and outranks this test's own virtual pad "
            "for poll()'s 'lowest ID' selection -- skipping (see foreignGamepadIds()'s own comment). The "
            "SDL_GetGamepadButton()/SDL_GamepadButton enum wiring was independently confirmed by this task's "
            "own smoke check (task-20-report.md).");
        return;
    }

    struct Case {
        SDL_GamepadButton sdlButton;
        const char* name;
        bool rx::platform::GamepadButtons::*field;
    };
    const Case cases[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, "dpadUp", &rx::platform::GamepadButtons::dpadUp},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "dpadDown", &rx::platform::GamepadButtons::dpadDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "dpadLeft", &rx::platform::GamepadButtons::dpadLeft},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "dpadRight", &rx::platform::GamepadButtons::dpadRight},
        {SDL_GAMEPAD_BUTTON_SOUTH, "south", &rx::platform::GamepadButtons::south},
        {SDL_GAMEPAD_BUTTON_EAST, "east", &rx::platform::GamepadButtons::east},
        {SDL_GAMEPAD_BUTTON_WEST, "west", &rx::platform::GamepadButtons::west},
        {SDL_GAMEPAD_BUTTON_NORTH, "north", &rx::platform::GamepadButtons::north},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "leftShoulder", &rx::platform::GamepadButtons::leftShoulder},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "rightShoulder", &rx::platform::GamepadButtons::rightShoulder},
        {SDL_GAMEPAD_BUTTON_START, "start", &rx::platform::GamepadButtons::start},
    };

    for (const auto& underTest : cases) {
        pad.setButton(underTest.sdlButton, true);
        auto state = window->poll();
        INFO("button under test: ", underTest.name);
        CHECK(state.buttons.*underTest.field);
        // Every OTHER field in the struct stays false -- proves this
        // isn't a stuck-true bug that would make every CHECK above pass
        // vacuously.
        for (const auto& other : cases) {
            if (other.field == underTest.field) {
                continue;
            }
            CHECK_FALSE(state.buttons.*other.field);
        }
        pad.setButton(underTest.sdlButton, false);
        CHECK_FALSE((window->poll().buttons.*underTest.field));
    }
}

TEST_CASE("Window::pumpEvents(): GAMEPAD_ADDED logs the gamepad's name + SDL_GamepadHasSensor(GYRO) result "
          "[gate ruling #14, log-don't-drop, SDL issue #9148]") {
    rx::core::log::init();
    auto sink = rx::core::log::forwardSink();
    CapturedLog captured;
    REQUIRE(sink->set(&captureLogCallback, &captured));
    ForwardCallbackGuard guard{sink};

    auto window = rx::platform::Window::create("rx_platform_gamepad_gyro_log_test", 64, 64, false);
    REQUIRE(window.has_value());
    window->pumpEvents();

    VirtualGamepad pad("rx_platform_gyro_log_pad");
    REQUIRE(pad.id != 0);
    window->pumpEvents();

    std::lock_guard<std::mutex> lock(captured.mutex);
    CHECK(captured.callCount >= 1);
    // Searches the full accumulated capture (not just the LAST message):
    // a real, already-connected gamepad in this environment (see
    // foreignGamepadIds()'s own comment -- this dev machine has one) logs
    // its OWN "gamepad connected" line too, and queue/drain ordering
    // between the two is not guaranteed -- this test cares specifically
    // about the ONE LINE naming this test's own pad, not whichever
    // happened to log last.
    const auto podLineStart = captured.allMessages.find("rx_platform_gyro_log_pad");
    REQUIRE(podLineStart != std::string::npos);
    const auto lineStart = captured.allMessages.rfind('\n', podLineStart) + 1;  // npos+1 == 0: start-of-string
    const auto lineEnd = captured.allMessages.find('\n', podLineStart);
    const std::string podLine = captured.allMessages.substr(lineStart, lineEnd - lineStart);
    CHECK(podLine.find("gamepad connected") != std::string::npos);
    // A virtual joystick declares zero sensors (VirtualGamepad's desc
    // above never sets nsensors/sensors) -- SDL_GamepadHasSensor(GYRO)
    // genuinely reports false for it, so this also proves the log line
    // reflects a REAL query result, not a hardcoded string.
    CHECK(podLine.find("hasGyroSensor=false") != std::string::npos);
}

// ===== Keyboard [Phase 4 Task 20, gate ruling #14, NEW SCOPE] =============
TEST_CASE("Window::isKeyDown(): with no real key pressed, every scancode reports false; an out-of-range "
          "SDL_Scancode is bounds-checked, not UB") {
    auto window = rx::platform::Window::create("rx_platform_keyboard_test", 64, 64, false);
    REQUIRE(window.has_value());
    window->pumpEvents();

    CHECK_FALSE(window->isKeyDown(SDL_SCANCODE_UNKNOWN));
    CHECK_FALSE(window->isKeyDown(SDL_SCANCODE_A));
    CHECK_FALSE(window->isKeyDown(SDL_SCANCODE_W));
    CHECK_FALSE(window->isKeyDown(SDL_SCANCODE_SPACE));
    CHECK_FALSE(window->isKeyDown(static_cast<SDL_Scancode>(SDL_SCANCODE_COUNT - 1)));

    // Out-of-range: an absurdly out-of-range value must not read out of
    // SDL_GetKeyboardState()'s own reported array bounds.
    CHECK_FALSE(window->isKeyDown(static_cast<SDL_Scancode>(99999)));
    CHECK_FALSE(window->isKeyDown(static_cast<SDL_Scancode>(-1)));
}

// ===== Thread-affinity guard [Phase 4 Task 20, gate ruling #14 row 11]
// =========================================================================
// Mirrors rx_asset/tests/thread_guard_test.cpp's own documented "a plain
// std::thread stands in for a chunk >= 1 worker" pattern -- legitimate here
// for the identical reason that file gives: rx::platform::Window has no
// rx::task::Scheduler-driven caller in this phase at all, so this tests the
// thread-identity comparison the guard performs, not any real scheduler
// integration. Each worker thread is joined before the next one starts, so
// there is never more than one thread touching this Window's state at a
// time -- the precondition rx::core::debug::detail::ViolationHook's own
// contract comment requires of a test-installed hook that returns normally
// instead of aborting.
#ifdef RX_DEBUG_CHECKS

namespace {

struct GuardViolationCapture {
    std::mutex mutex;
    int callCount = 0;
    std::string lastContext;
};

std::atomic<GuardViolationCapture*> g_activeGuardCapture{nullptr};

void captureGuardViolationHook(const char* context) {
    GuardViolationCapture* capture = g_activeGuardCapture.load(std::memory_order_relaxed);
    if (capture == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->callCount++;
    capture->lastContext = context != nullptr ? context : "";
    // Returns normally rather than aborting -- safe here specifically
    // because every worker thread below is join()'d before the next
    // guarded call starts (see this section's own banner comment).
}

struct GuardHookGuard {
    ~GuardHookGuard() {
        g_activeGuardCapture.store(nullptr, std::memory_order_relaxed);
        rx::core::debug::detail::setViolationHookForTests(nullptr);
    }
};

}  // namespace

TEST_CASE("Window's new input surface (pumpEvents/consumeMouseDelta/poll/setCursorVisible) trips "
          "RX_ASSERT_MAIN_THREAD when called from a worker thread [gate ruling #14 row 11]") {
    auto window = rx::platform::Window::create("rx_platform_thread_guard_test", 64, 64, false);
    REQUIRE(window.has_value());

    GuardViolationCapture capture;
    g_activeGuardCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureGuardViolationHook);
    GuardHookGuard guard;

    std::thread pumpThread([&] { window->pumpEvents(); });
    pumpThread.join();
    std::thread consumeThread([&] { window->consumeMouseDelta(); });
    consumeThread.join();
    std::thread pollThread([&] { window->poll(); });
    pollThread.join();
    std::thread cursorThread([&] { window->setCursorVisible(true); });
    cursorThread.join();

    std::lock_guard<std::mutex> lock(capture.mutex);
    CHECK(capture.callCount == 4);
    CHECK(capture.lastContext == "Window::setCursorVisible");
}

TEST_CASE("Window's new input surface does NOT trip the guard for a call genuinely on the main thread") {
    auto window = rx::platform::Window::create("rx_platform_thread_guard_legal_test", 64, 64, false);
    REQUIRE(window.has_value());

    GuardViolationCapture capture;
    g_activeGuardCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureGuardViolationHook);
    GuardHookGuard guard;

    window->pumpEvents();
    window->consumeMouseDelta();
    window->poll();
    window->setCursorVisible(true);

    std::lock_guard<std::mutex> lock(capture.mutex);
    CHECK(capture.callCount == 0);
}

#endif  // RX_DEBUG_CHECKS
