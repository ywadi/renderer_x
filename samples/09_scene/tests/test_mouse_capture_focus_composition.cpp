// samples/09_scene/tests/test_mouse_capture_focus_composition.cpp -- proves
// Issue #33's requirement 1 explicitly: "confirm re-arm on focus-gain
// composes with your toggle state". [Phase 5 Task 5, ticket #41 row 3]
// Drives the ENGINE-OWNED rx::platform::MouseCaptureToggle (promoted from
// this sample's own former mouse_capture.h) through this SAMPLE's own
// composition logic -- this file's own scope is proving runPresent()'s own
// "apply the transition to the real Window" call sequence, not the toggle
// primitive itself (that is src/rx_platform/tests/test_mouse_capture_toggle.cpp's
// job now). Unlike that device-free suite, THIS file deliberately links a
// real rx::platform::Window (headless/hidden, no VkDevice needed -- same
// "Window::create/destroy lifecycle succeeds under any video driver"
// posture src/rx_platform/tests/window_test.cpp's own first TEST_CASE
// already establishes) and drives it through the EXACT call sequence
// runPresent() (main.cpp) uses:
//   1. rx::platform::MouseCaptureToggle's own toggle methods change
//      captured() intent (Esc / click-to-recapture) -- pure, no Window.
//   2. main.cpp applies that transition to the real Window with
//      `window.setRelativeMouseMode(state.captured())`, ONLY when
//      captured() actually changed (see main.cpp's own
//      "[Issue #33] Apply exactly one real Window transition per frame"
//      comment) -- reproduced verbatim below as applyCaptureTransition().
// window.cpp's own pumpEvents() FOCUS_LOST/FOCUS_GAINED handling (gate
// matrix row 1) is already generically proven by window_test.cpp's own
// "FOCUS_LOST pauses ... FOCUS_GAINED resumes ... unconditionally re-arms"
// TEST_CASE -- this file does not re-prove that mechanism in isolation. It
// proves the COMPOSITION: that main.cpp's OWN toggle-driven
// setRelativeMouseMode() calls correctly set window.h's own
// relativeModeWanted_ (via relativeMouseModeWanted()) BEFORE a focus-loss/
// gain cycle, so that cycle's re-arm decision reflects the CURRENT capture
// toggle state, not a stale one -- e.g. releasing via Esc and THEN
// alt-tabbing away and back must NOT silently re-engage capture the app no
// longer wants.
#include <rx_platform/mouse_capture_toggle.h>

#include <doctest/doctest.h>
#include <rx_platform/window.h>
#include <SDL3/SDL.h>

using rx::platform::MouseCaptureToggle;

namespace {

// Mirrors runPresent()'s own "capturedBeforePump -> pump -> apply on
// change" sequence (main.cpp) exactly -- see that function's own
// "[Issue #33] Apply exactly one real Window transition per frame" comment.
// Pulled out here so this test file states the exact contract once instead
// of inlining it at every call site below.
void applyCaptureTransition(rx::platform::Window& window, const MouseCaptureToggle& state, bool capturedBefore) {
    if (state.captured() != capturedBefore) {
        window.setRelativeMouseMode(state.captured());
    }
}

// window_test.cpp's own established synthetic-event-injection idiom
// (SDL_PushEvent + pumpEvents()) -- reused verbatim rather than
// reinvented, per this project's own "prefer the established pattern"
// convention.
void pushWindowEvent(SDL_EventType type, SDL_WindowID windowId) {
    SDL_Event event{};
    event.window.type = type;
    event.window.windowID = windowId;
    REQUIRE(SDL_PushEvent(&event));
}

}  // namespace

TEST_CASE("Issue #33 composition: releasing via Esc clears relativeMouseModeWanted(), and a SUBSEQUENT focus-loss/"
          "gain cycle correctly does NOT re-arm capture the app no longer wants [requirement 1: re-arm-on-"
          "focus-gain composes with the toggle state]") {
    auto window = rx::platform::Window::create("rx_samples9_capture_compose_test", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    const SDL_WindowID windowId = SDL_GetWindowID(window->sdlWindow());
    window->pumpEvents();

    MouseCaptureToggle state;  // captured by default.
    // runPresent()'s own startup call (main.cpp, "[Issue #33] Engage the
    // platform relative-mouse-capture facility") is UNCONDITIONAL, not
    // gated through applyCaptureTransition()'s "only on change" guard
    // below -- there is no "before" frame to diff against yet.
    window->setRelativeMouseMode(state.captured());
    REQUIRE(window->relativeMouseModeWanted());

    // Esc -> released.
    bool capturedBefore = state.captured();
    state.toggleOnEscPressed();
    applyCaptureTransition(*window, state, capturedBefore);
    REQUIRE_FALSE(state.captured());
    CHECK_FALSE(window->relativeMouseModeWanted());  // the toggle's own setRelativeMouseMode(false) took effect.

    // Alt-tab away and back WHILE RELEASED.
    pushWindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST, windowId);
    window->pumpEvents();
    pushWindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, windowId);
    window->pumpEvents();

    // Must NOT have been silently re-armed -- window.cpp's own FOCUS_GAINED
    // handling only re-arms `if (relativeModeWanted_)`, and this composition
    // is exactly what makes that condition correctly false here.
    CHECK_FALSE(window->relativeMouseModeWanted());
}

TEST_CASE("Issue #33 composition: recapturing (click or Esc) sets relativeMouseModeWanted() again, and a "
          "SUBSEQUENT focus-loss/gain cycle DOES re-arm capture, composing correctly with window.cpp's own "
          "unconditional re-arm-if-wanted rule [requirement 1, the positive case]") {
    auto window = rx::platform::Window::create("rx_samples9_capture_compose_test2", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    const SDL_WindowID windowId = SDL_GetWindowID(window->sdlWindow());
    window->pumpEvents();

    MouseCaptureToggle state;
    window->setRelativeMouseMode(state.captured());  // runPresent()'s own unconditional startup engagement.
    REQUIRE(window->relativeMouseModeWanted());

    // Esc -> released -> recapture via viewport click, all before any focus event.
    bool capturedBefore = state.captured();
    state.toggleOnEscPressed();
    applyCaptureTransition(*window, state, capturedBefore);
    REQUIRE_FALSE(window->relativeMouseModeWanted());

    capturedBefore = state.captured();
    state.recaptureOnViewportClick();
    applyCaptureTransition(*window, state, capturedBefore);
    REQUIRE(state.captured());
    CHECK(window->relativeMouseModeWanted());

    // Alt-tab away and back WHILE CAPTURED (again).
    pushWindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST, windowId);
    window->pumpEvents();
    pushWindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, windowId);
    window->pumpEvents();

    // Composes correctly the other way too -- capture the app currently
    // wants survives the focus cycle (window.cpp's own re-arm branch,
    // driven off the state THIS toggle sequence produced, not a value the
    // test seeded directly).
    CHECK(window->relativeMouseModeWanted());
}

TEST_CASE("Issue #33 composition: a steady-state frame where captured() did NOT change is a harmless no-op re-"
          "apply (matches runPresent()'s own per-frame call in the common case -- most frames touch neither Esc "
          "nor a recapture click) -- interleaved between the toggle and the focus cycle to prove it does not "
          "disturb the composition either way") {
    auto window = rx::platform::Window::create("rx_samples9_capture_compose_test3", 64, 64, /*visible=*/false);
    REQUIRE(window.has_value());
    const SDL_WindowID windowId = SDL_GetWindowID(window->sdlWindow());
    window->pumpEvents();

    MouseCaptureToggle state;
    window->setRelativeMouseMode(state.captured());  // runPresent()'s own unconditional startup engagement.
    REQUIRE(window->relativeMouseModeWanted());

    bool capturedBefore = state.captured();
    state.toggleOnEscPressed();  // released.
    applyCaptureTransition(*window, state, capturedBefore);
    REQUIRE_FALSE(window->relativeMouseModeWanted());

    // A frame where nothing changed (state.captured() unchanged) --
    // applyCaptureTransition() is a pure no-op here, exactly like
    // runPresent()'s own per-frame call in the steady state.
    capturedBefore = state.captured();
    applyCaptureTransition(*window, state, capturedBefore);
    CHECK_FALSE(window->relativeMouseModeWanted());  // still released -- no-op re-apply didn't flip anything.

    pushWindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST, windowId);
    window->pumpEvents();
    pushWindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, windowId);
    window->pumpEvents();
    CHECK_FALSE(window->relativeMouseModeWanted());
}

// [Revert-discrimination] The genuinely load-bearing part of
// applyCaptureTransition() (above) is the `!=` in
// `state.captured() != capturedBefore`: an inverted (`==`) condition would
// call Window::setRelativeMouseMode() only on NO-OP frames and SKIP it on
// every REAL transition, so a released toggle would never actually reach
// the real Window at all. Verified directly by mutation (not left as a
// permanent test, since proving it requires editing that very function to
// inject the bug): temporarily flipping that operator to `==` and
// rebuilding turns the two composition TEST_CASEs above from all-passing
// into failing at their first post-toggle `relativeMouseModeWanted()`
// assertion (both stay wrongly stuck at the initial captured=true state,
// since the Esc-driven transition is exactly the "captured() changed"
// frame the inverted condition treats as a no-op instead) -- confirming
// those two TEST_CASEs already discriminate this exact class of bug,
// without needing a dedicated third one for it.
