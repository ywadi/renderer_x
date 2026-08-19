#include "reference_gate.h"

#include <rx_core/log.h>

// Declarations only -- the real STB_IMAGE_IMPLEMENTATION translation unit
// already exists exactly once, in rx_rhi_vk/src/stb_impl.cpp, linked
// transitively by every sample (see this file's own header comment).
#include <stb_image.h>

// This file is the SOLE STB_IMAGE_WRITE_IMPLEMENTATION translation unit in
// this repository -- see reference_gate.h's own header comment.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cmath>
#include <cstring>

namespace rx::samples {

std::optional<LoadedImage> loadRgba8Png(const std::filesystem::path& path) {
    int width = 0;
    int height = 0;
    int channelsInFile = 0;
    // desired_channels=4: always force RGBA8 regardless of the PNG's own
    // real channel count (a reference authored/exported as RGB-only, for
    // instance, still compares cleanly against a renderer that always
    // produces RGBA8 readback).
    stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &channelsInFile, 4);
    if (pixels == nullptr) {
        RX_LOG_ERROR("reference_gate: stbi_load failed for '{}': {}", path.string(), stbi_failure_reason());
        return std::nullopt;
    }

    LoadedImage image;
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    image.rgba8.resize(byteCount);
    std::memcpy(image.rgba8.data(), pixels, byteCount);
    stbi_image_free(pixels);
    return image;
}

bool writeRgba8Png(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint8_t* rgba8) {
    if (width == 0 || height == 0 || rgba8 == nullptr) {
        RX_LOG_ERROR("reference_gate: writeRgba8Png called with a zero dimension or null buffer");
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const int strideBytes = static_cast<int>(width) * 4;
    const int result = stbi_write_png(path.string().c_str(), static_cast<int>(width), static_cast<int>(height),
                                       /*comp=*/4, rgba8, strideBytes);
    if (result == 0) {
        RX_LOG_ERROR("reference_gate: stbi_write_png failed for '{}'", path.string());
        return false;
    }
    return true;
}

GateResult compareToReference(const uint8_t* rendered, uint32_t renderedWidth, uint32_t renderedHeight,
                               const uint8_t* reference, uint32_t referenceWidth, uint32_t referenceHeight,
                               int toleranceOutOf255, double failingPixelBudgetFraction) {
    GateResult result;
    result.totalPixelCount = static_cast<uint64_t>(referenceWidth) * static_cast<uint64_t>(referenceHeight);

    if (renderedWidth != referenceWidth || renderedHeight != referenceHeight) {
        RX_LOG_ERROR(
            "reference_gate: dimension mismatch -- rendered {}x{} vs reference {}x{} (never comparable pixel-by-"
            "pixel; this is a hard failure, not a tolerance question)",
            renderedWidth, renderedHeight, referenceWidth, referenceHeight);
        result.passed = false;
        result.failingPixelCount = result.totalPixelCount;
        result.failingPixelFraction = 1.0;
        return result;
    }
    if (rendered == nullptr || reference == nullptr || result.totalPixelCount == 0) {
        result.passed = false;
        return result;
    }

    for (uint32_t y = 0; y < referenceHeight; ++y) {
        for (uint32_t x = 0; x < referenceWidth; ++x) {
            const size_t offset = (static_cast<size_t>(y) * referenceWidth + x) * 4;
            bool pixelMatches = true;
            std::array<int, 4> delta{0, 0, 0, 0};
            for (int c = 0; c < 4; ++c) {
                const int a = static_cast<int>(rendered[offset + static_cast<size_t>(c)]);
                const int b = static_cast<int>(reference[offset + static_cast<size_t>(c)]);
                delta[static_cast<size_t>(c)] = a - b;
                if (std::abs(a - b) > toleranceOutOf255) {
                    pixelMatches = false;
                }
            }
            if (!pixelMatches) {
                ++result.failingPixelCount;
                if (!result.hasFirstMismatch) {
                    result.hasFirstMismatch = true;
                    result.firstMismatchX = x;
                    result.firstMismatchY = y;
                    result.firstMismatchDelta = delta;
                }
            }
        }
    }

    result.failingPixelFraction =
        static_cast<double>(result.failingPixelCount) / static_cast<double>(result.totalPixelCount);
    result.passed = result.failingPixelFraction < failingPixelBudgetFraction;
    return result;
}

}  // namespace rx::samples
