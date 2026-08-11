#include <doctest/doctest.h>
#include <rx_core/profile.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Compile-and-run coverage for rx_core/profile.h [task-3 binding
// constraint: "profile.h no-op compile test: a TU using all macros with
// TRACY off AND on"]. This single TU is unconditional -- it always compiles
// and always runs, in BOTH of this task's verified configurations (RX_TRACY
// ON, the default dev-preset build, and RX_TRACY OFF, the extra verification
// build in the report) -- TRACY_ENABLE's own value (propagated transitively
// from rx_core's PUBLIC compile definition, see src/rx_core/CMakeLists.txt)
// is what silently switches every macro below between a real Tracy call and
// a bare no-op; this file never spells out `#ifdef TRACY_ENABLE` itself,
// which is the whole point -- a caller of these macros never needs to know
// or care which build it is in.

namespace {

int zoneScopedFunction(int value) {
    RX_ZONE;
    return value * 2;
}

int zoneNamedFunction(int value) {
    RX_ZONE_NAMED("zoneNamedFunction");
    return value + 1;
}

int dynamicNameFunction(const char* dynamicLabel, size_t labelLen) {
    RX_ZONE_NAMED("dynamicNameFunction");
    RX_ZONE_DYNAMIC_NAME(dynamicLabel, labelLen);
    return static_cast<int>(labelLen);
}

}  // namespace

TEST_CASE("RX_ZONE compiles and runs (no-op or real zone depending on TRACY_ENABLE)") {
    CHECK(zoneScopedFunction(21) == 42);
}

TEST_CASE("RX_ZONE_NAMED compiles and runs") {
    CHECK(zoneNamedFunction(41) == 42);
}

TEST_CASE("RX_ZONE_DYNAMIC_NAME compiles and runs alongside RX_ZONE_NAMED") {
    const std::string dynamicPassName = "graph_pass_example";
    CHECK(dynamicNameFunction(dynamicPassName.data(), dynamicPassName.size()) ==
          static_cast<int>(dynamicPassName.size()));
}

TEST_CASE("RX_FRAME_MARK compiles and runs in a loop, as every sample's frame loop does") {
    for (int frame = 0; frame < 3; ++frame) {
        RX_ZONE;
        RX_FRAME_MARK;
    }
    CHECK(true);
}

TEST_CASE("RX_PLOT compiles and runs with integral and floating-point values") {
    // Tracy's own TracyPlot forwards to an overload set (PlotData(const
    // char*, int64_t/float/double)) -- a plain `int` is ambiguous between
    // them (an equally-good implicit conversion to any of the three), so
    // this deliberately casts to int64_t explicitly, matching how any real
    // caller (e.g. a future pool-byte-budget or draw-count plot) must.
    for (int i = 0; i < 5; ++i) {
        RX_PLOT("profile_test.counter", static_cast<int64_t>(i));
        RX_PLOT("profile_test.ratio", static_cast<double>(i) / 2.0);
    }
    CHECK(true);
}
