// compare_rgba8_png -- issue #31: a small, reusable host tool applying
// samples/common's own D17 tolerance-gate algorithm
// (rx::samples::compareToReference) to two ARBITRARY PNG files, rather
// than a sample's own live render vs. a committed reference. Introduced
// specifically to verify KHR_draco_mesh_compression's render-equivalence
// gate -- comparing sample_08_gltf_viewer's own --write-references output
// for a Draco-compressed scene against its uncompressed twin (see
// tools/gen_gltf_compression_fixtures/main.cpp's cube_draco.gltf /
// cube_draco_reference.gltf) -- without inventing a second reference-
// regeneration mechanism (gate ruling #15): this tool never writes or
// updates any COMMITTED file, it only reads two already-rendered PNGs and
// reports whether they match within tolerance. Deliberately generic (not
// Draco-specific) so any future "do these two renders agree" question can
// reuse it instead of hand-rolling another comparison.
//
// Usage: compare_rgba8_png <imageA.png> <imageB.png> [toleranceOutOf255=4] [failingBudgetFraction=0.005]
// Exit 0 on PASS, 1 on FAIL or load error.
#include <reference_gate.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <imageA.png> <imageB.png> [toleranceOutOf255=4] [failingBudgetFraction=0.005]\n",
                     argv[0]);
        return 1;
    }
    const std::string pathA = argv[1];
    const std::string pathB = argv[2];
    const int tolerance = argc > 3 ? std::atoi(argv[3]) : 4;
    const double budget = argc > 4 ? std::atof(argv[4]) : 0.005;

    auto imageA = rx::samples::loadRgba8Png(pathA);
    if (!imageA.has_value()) {
        std::fprintf(stderr, "compare_rgba8_png: failed to load '%s'\n", pathA.c_str());
        return 1;
    }
    auto imageB = rx::samples::loadRgba8Png(pathB);
    if (!imageB.has_value()) {
        std::fprintf(stderr, "compare_rgba8_png: failed to load '%s'\n", pathB.c_str());
        return 1;
    }

    rx::samples::GateResult result = rx::samples::compareToReference(
        imageA->rgba8.data(), imageA->width, imageA->height, imageB->rgba8.data(), imageB->width, imageB->height,
        tolerance, budget);

    std::printf("compare_rgba8_png: '%s' vs '%s': failingPixels=%llu/%llu (%.4f%%) tolerance=+-%d/255 budget=%.4f%% pass=%s\n",
                pathA.c_str(), pathB.c_str(), static_cast<unsigned long long>(result.failingPixelCount),
                static_cast<unsigned long long>(result.totalPixelCount), result.failingPixelFraction * 100.0, tolerance,
                budget * 100.0, result.passed ? "true" : "false");
    if (!result.passed && result.hasFirstMismatch) {
        std::printf("compare_rgba8_png: first mismatch at (%u,%u), delta rgba=(%d,%d,%d,%d)\n", result.firstMismatchX,
                    result.firstMismatchY, result.firstMismatchDelta[0], result.firstMismatchDelta[1],
                    result.firstMismatchDelta[2], result.firstMismatchDelta[3]);
    }
    return result.passed ? 0 : 1;
}
