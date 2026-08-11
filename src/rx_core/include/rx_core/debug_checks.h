#pragma once

// debug_checks.h -- lightweight, dev-time runtime assertions for this
// engine's main-thread-only threading contract (docs/threading.md, spec
// D5) [Phase 4 Task 7 fix round 1, review Medium finding: "no runtime
// guard against a main-thread-only API called from chunk >= 1 -- silent
// corruption, not a loud failure, is the realistic failure mode"; spec D4's
// own "Chunk-0 main-thread guarantee" paragraph (adjudicated 2026-08-11)
// requires exactly this: "main-thread-only subsystems carry debug
// thread-affinity assertions (dev-preset-active regardless of NDEBUG)"].
//
// Guarded by its OWN CMake option, RX_DEBUG_CHECKS (default ON in both dev
// presets -- src/rx_core/CMakeLists.txt/CMakePresets.json) --
// DELIBERATELY NOT `assert()`/NDEBUG: both this project's presets compile
// with `-DNDEBUG` (RelWithDebInfo -- confirmed directly,
// task-5-review.md's own "NDEBUG assert honesty disclosure" finding: a
// bare `assert()` in this codebase is silently compiled away in EVERY
// build this project's own CMakePresets.json ever produces, dev or CI).
// RX_DEBUG_CHECKS is this engine's own, independent switch, so a real
// violation is caught in the SAME builds every sample/test/CI run already
// uses, never only in a hypothetical Debug configuration nobody here
// builds.
//
// RX_ASSERT_MAIN_THREAD(context) -- place as the very FIRST statement of
// any main-thread-only mutator (docs/threading.md's own "Main-thread-only"
// list): compares the calling thread's identity against a lazily-captured
// "this process's main thread" identity (see assertMainThreadImpl()'s own
// comment in debug_checks.cpp for exactly how, and the one limitation that
// choice carries, disclosed there rather than hidden). On a mismatch: logs
// an ERROR naming `context` and calls the currently-installed violation
// hook (production default: `std::abort()`, after logging -- a silent
// data race is worse than a loud crash); on a match: a no-op. `context`
// should name the API (e.g. `"MaterialSystem::bindInstance"`) so the log
// line is immediately actionable without needing a backtrace.
//
// When RX_DEBUG_CHECKS is OFF, this macro expands to nothing at all --
// zero symbols, zero runtime cost, matching `RX_ZONE`'s identical
// OFF-compiles-to-nothing contract (`rx_core/profile.h`) -- so a
// main-thread-only mutator's own call site never needs its own
// `#ifdef RX_DEBUG_CHECKS`; it reads identically in every configuration.
#ifdef RX_DEBUG_CHECKS

namespace rx::core::debug {

// Real implementation backing RX_ASSERT_MAIN_THREAD below -- never call
// this directly; the macro exists specifically so every guarded call site
// reads identically regardless of RX_DEBUG_CHECKS, matching every other
// engine-wide instrumentation macro's own convention (RX_ZONE et al.,
// rx_core/profile.h).
void assertMainThreadImpl(const char* context);

namespace detail {

// Test-only seam -- NOT part of the stable public contract (mirrors
// rx::material::detail::debugCompileCount()'s own carve-out convention:
// "exposed only so ...'s own unit tests ... can drive the ... state
// machine directly").
//
// Called by assertMainThreadImpl() on every violation, IN PLACE OF the
// real default (log, then std::abort()) -- installing one is how a test
// observes a violation without crashing the test process. `nullptr` (the
// default state, and what every test must restore before returning, e.g.
// via a scope guard) restores the real abort-on-violation production
// behavior.
//
// CONTRACT a hook must honor: `assertMainThreadImpl()` itself enforces
// NOTHING beyond calling whichever hook is currently installed -- it does
// not itself prevent the violating call from continuing into whatever
// main-thread-only mutation it was about to perform. The production
// default hook satisfies this by never returning at all (`std::abort()`).
// A TEST-installed hook that instead records-and-returns is only safe to
// use in a test that has independently ensured no OTHER thread can be
// concurrently touching the same guarded state during that one violating
// call -- every test that installs one (`debug_checks_test.cpp`,
// `test_material_system.cpp`'s own chunked-guard case) documents, at its
// own call site, exactly why that holds for that specific test.
using ViolationHook = void (*)(const char* context);
void setViolationHookForTests(ViolationHook hook);

}  // namespace detail

}  // namespace rx::core::debug

#define RX_ASSERT_MAIN_THREAD(context) ::rx::core::debug::assertMainThreadImpl(context)

#else

#define RX_ASSERT_MAIN_THREAD(context)

#endif  // RX_DEBUG_CHECKS
