// [Task 3 (#39), matrix row 2: "The chosen format's precision
// characteristics are documented, with a test pinning the format"]
// Device-free: `rx_graph/scene_color.h` is a plain-value header (no live
// Vulkan handle), so pinning its constants needs no real device -- this
// file lives in the `rx_graph_tests` binary (doctest_main.cpp's plain
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN target), not `rx_graph_gpu_tests`.
// The GPU-backed proof that the ruled format actually behaves as
// documented (value survival, precision-asymmetry discrimination, the
// escape hatch, empirical format-support queries) lives in
// test_scene_color_gpu.cpp.
#include <doctest/doctest.h>
#include <rx_graph/scene_color.h>

#include <cstring>

using namespace rx::graph;

TEST_CASE("kHdrFormat is pinned to the ruled process-wide default (B10G11R11_UFLOAT_PACK32)") {
    // [rulings-2026-08-20.md, "T3 (#39)"] "HDR scene-color format B10G11R11
    // (UFLOAT) as the process-wide default" -- a single named engine
    // constant, not the four independent per-sample copies that predated
    // this task (matrix row 2/9).
    CHECK(kHdrFormat == VK_FORMAT_B10G11R11_UFLOAT_PACK32);
}

TEST_CASE("kHdrFormatHighPrecision is pinned to the ruled escape hatch (A16B16G16R16F)") {
    // [rulings-2026-08-20.md, "T3 (#39)"] "...with a documented
    // A16B16G16R16F escape hatch where precision demands" -- Vulkan spells
    // this bit layout VK_FORMAT_R16G16B16A16_SFLOAT (component-order
    // naming convention, same layout the ruling's "A16B16G16R16F" shorthand
    // names).
    CHECK(kHdrFormatHighPrecision == VK_FORMAT_R16G16B16A16_SFLOAT);
}

TEST_CASE("kHdrFormat and kHdrFormatHighPrecision are distinct formats") {
    // Guards against a future edit accidentally collapsing the ruled
    // default and its documented escape hatch onto the same value, which
    // would silently defeat the entire point of having an escape hatch.
    CHECK(kHdrFormat != kHdrFormatHighPrecision);
}

TEST_CASE("kSceneColorResourceName is the canonical \"hdr\" resource name") {
    // [matrix row 6] "the resource name (\"hdr\" or a ruled successor)" --
    // kept as "hdr" (every existing sample already agreed on this by
    // copy-paste convention before this task; no ruling text requires
    // renaming it).
    CHECK(std::strcmp(kSceneColorResourceName, "hdr") == 0);
}
