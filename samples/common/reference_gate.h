#pragma once
// samples/common/reference_gate.h -- D17's tolerance-based pixel gate
// [Phase 4 Stage 1 Task 16, spec D17: "Content gates (sample 08/09 headless)
// compare against committed reference PNGs rendered on lavapipe at 256x256:
// per-channel tolerance (+-4/255) with a failing-pixel budget (<0.5%), plus
// the existing analytic probes where applicable. References are
// lavapipe-only (CI's driver); local GPU runs report divergence as info, not
// failure. Reference regeneration is an explicit documented script, never
// automatic."] -- a small, reusable helper deliberately NOT inlined into
// sample 08's own main.cpp: this exact mechanism is reused by sample 09
// later (D17's own text), so it lives here, in `samples/common`, the one
// place both samples can link against without duplicating it.
//
// Thread-affinity: none (plain host-side byte comparison / PNG codec calls,
// no Vulkan/GPU object touched here at all -- callers pass already-read-back
// RGBA8 pixel buffers).
//
// Depends on stb_image.h (declarations only -- the real
// STB_IMAGE_IMPLEMENTATION translation unit already exists exactly once, in
// rx_rhi_vk/src/stb_impl.cpp, which every sample already links transitively)
// for loadRgba8Png(), and provides its OWN sole STB_IMAGE_WRITE_IMPLEMENTATION
// translation unit (reference_gate.cpp) for writeRgba8Png() -- no other file
// in this repository defines STB_IMAGE_WRITE_IMPLEMENTATION; matching this
// project's own "exactly one" discipline for the read half (see
// rx_rhi_vk/src/stb_impl.cpp's own header comment).

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rx::samples {

// Loads `path` as an RGBA8 image (forcing 4 channels via stb_image's own
// `desired_channels` parameter, regardless of the PNG's real channel count)
// -- returns std::nullopt (no exception, no crash) on any failure (missing
// file, corrupt/non-image data, unsupported format), so a caller can turn
// that into its own loud, named diagnostic rather than stb_image's own
// generic failure string leaking through uninterpreted.
struct LoadedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba8;  // width * height * 4 bytes, row-major, no padding.
};
[[nodiscard]] std::optional<LoadedImage> loadRgba8Png(const std::filesystem::path& path);

// Writes `rgba8` (width * height * 4 bytes, row-major) as a PNG to `path`.
// Returns false (logged) on any failure (bad path, stb_image_write's own
// internal I/O error). Used by both `tools/regen_references.sh` (via a
// sample's own `--write-reference` escape hatch) and any future diagnostic
// dump a caller wants (e.g. saving a MISMATCHING render alongside its
// expected reference for visual debugging).
[[nodiscard]] bool writeRgba8Png(const std::filesystem::path& path, uint32_t width, uint32_t height,
                                  const uint8_t* rgba8);

// D17's own tolerance-gate algorithm: per-channel (R/G/B/A, all four --
// alpha included since a wrong-alpha render is still a real content bug,
// even though every sample this gate covers today renders fully opaque)
// absolute difference must be <= `toleranceOutOf255` for a pixel to count
// as MATCHING; the total count of non-matching pixels, as a fraction of
// `width*height`, must be < `failingPixelBudgetFraction` for the whole
// image to PASS. Returns a detailed result (not just a bool) so a caller
// can log a real, specific diagnostic ("N/`width*height` pixels exceeded
// tolerance, M% > 0.5% budget") rather than a bare "gate failed".
struct GateResult {
    bool passed = false;
    uint64_t failingPixelCount = 0;
    uint64_t totalPixelCount = 0;
    double failingPixelFraction = 0.0;
    // First mismatching pixel's (x, y) and per-channel delta -- purely
    // diagnostic (logged on failure), not part of the pass/fail decision.
    bool hasFirstMismatch = false;
    uint32_t firstMismatchX = 0;
    uint32_t firstMismatchY = 0;
    std::array<int, 4> firstMismatchDelta{0, 0, 0, 0};
};

// `rendered`/`reference` must both be `width*height*4`-byte RGBA8 buffers
// of the IDENTICAL width/height (a size mismatch is itself a hard failure,
// reported via `GateResult{false, ...}` with `totalPixelCount` set from
// `reference`'s own dimensions -- never a crash/out-of-bounds read).
[[nodiscard]] GateResult compareToReference(const uint8_t* rendered, uint32_t renderedWidth, uint32_t renderedHeight,
                                             const uint8_t* reference, uint32_t referenceWidth,
                                             uint32_t referenceHeight, int toleranceOutOf255 = 4,
                                             double failingPixelBudgetFraction = 0.005);

}  // namespace rx::samples
