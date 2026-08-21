// rx_ibl/tests/test_ibl_analytic_gpu.cpp -- Phase 5 Stage 1 Task 9 [#45,
// gate matrix-p5t09-ibl-bake-chain.md acceptance sketch]: value-asserted
// analytic-ground-truth tests for the bake chain. See
// /tmp/.../scratchpad/analytic_notes.md (this task's own working notes,
// reproduced/cited inline below) for the derivations.
//
// KEY ANALYTIC FACTS this file's assertions rest on (both derived
// directly from the PINNED port source's own formulas, CubemapIBL.cpp
// v1.75.0):
//
// 1. UNIFORM-ENVIRONMENT CONSERVATION IS EXACT, not merely
//    approximately-converged Monte Carlo. diffuseIrradiance()'s
//    `Ed() = (1/N) * sum L(dir_i)` -- for a spatially uniform L(dir)=L0,
//    every term is IDENTICALLY L0 (not sampled with varying per-sample
//    weight), so the sum is exactly N*L0 and Ed()=L0 REGARDLESS of N or
//    the sample directions -- zero statistical noise, only fp32 rounding.
//    roughnessFilter()'s `result = accum/weight` where
//    `accum = sum(L(dir_i)*NoL_i)`, `weight = sum(NoL_i)` -- for constant
//    L(dir)=L0, `accum = L0 * sum(NoL_i)` so `result = L0 *
//    sum(NoL_i)/sum(NoL_i) = L0` EXACTLY, again independent of sample
//    count/directions (the weighted average of a constant is that
//    constant, exactly, by cancellation). The equirect->cubemap stage's
//    own bilinear hardware sampling of a uniform source texture also
//    returns that same constant everywhere. This chains: a uniform
//    equirect source bakes to a uniform baseCubemap, uniform
//    irradianceCubemap, and uniform prefilteredCubemap at EVERY mip --
//    all equal to the source L0, to fp16-storage-rounding tolerance only.
//
// 2. DFG (multiscatter) closed-form limit as linearRoughness -> 0, for
//    ANY NoV in (0,1]: DFV_Multiscatter(NoV, ~0) = ((1-NoV)^5, 1.0)
//    exactly (re-derived from DFV_Multiscatter's own formula: at
//    alpha=0, hemisphereImportanceSampleDggx always returns H=(0,0,1)=N,
//    collapsing every per-sample term to a CONSTANT v=0.25 regardless of
//    NoV, so r.y -> 4*0.25=1.0 and r.x -> 4*0.25*(1-NoV)^5). The DFG
//    LUT's own dfg_lut.slang parameterization puts the SMOOTHEST row at
//    y=height-1 (coord=(h-y+0.5)/h, smallest at y=height-1) -- close to
//    but not exactly linearRoughness=0, so this test uses a generous
//    tolerance and a large sample count for that one row specifically.
#include "ibl_gpu_fixture.h"

#include <doctest/doctest.h>
#include <rx_ibl/bake.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace rx::ibl_test;

namespace {

std::vector<float> buildUniformEquirect(uint32_t w, uint32_t h, float r, float g, float b) {
    std::vector<float> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        px[i * 4 + 0] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = 1.0F;
    }
    return px;
}

std::vector<float> buildFlatCubeFaces(uint32_t dim, const std::array<std::array<float, 3>, 6>& colors) {
    std::vector<float> px(static_cast<size_t>(dim) * dim * 4 * 6);
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(dim) * dim; ++i) {
            size_t idx = (static_cast<size_t>(face) * dim * dim + i) * 4;
            px[idx + 0] = colors[face][0];
            px[idx + 1] = colors[face][1];
            px[idx + 2] = colors[face][2];
            px[idx + 3] = 1.0F;
        }
    }
    return px;
}

// Reads texel (px,py) of `tex` at (mip,layer), decoding R16G16B16A16_SFLOAT.
std::array<float, 4> readTexelHalf4(IblGpuFixture& fx, rx::rhi::Texture2D& tex, uint32_t mip, uint32_t layer,
                                      uint32_t dim, uint32_t px, uint32_t py) {
    auto raw = readbackRaw(fx, tex, mip, layer, dim, dim, 8);
    const uint16_t* half = reinterpret_cast<const uint16_t*>(raw.data());
    size_t base = (static_cast<size_t>(py) * dim + px) * 4;
    return {halfToFloat(half[base + 0]), halfToFloat(half[base + 1]), halfToFloat(half[base + 2]),
            halfToFloat(half[base + 3])};
}

// Reads texel (px,py) of the DFG LUT, decoding R16G16_SFLOAT.
std::array<float, 2> readDfgTexel(IblGpuFixture& fx, rx::rhi::Texture2D& tex, uint32_t dim, uint32_t px, uint32_t py) {
    auto raw = readbackRaw(fx, tex, 0, 0, dim, dim, 4);
    const uint16_t* half = reinterpret_cast<const uint16_t*>(raw.data());
    size_t base = (static_cast<size_t>(py) * dim + px) * 2;
    return {halfToFloat(half[base + 0]), halfToFloat(half[base + 1])};
}

}  // namespace

TEST_CASE("IBL Task 9: uniform environment bakes to EXACT irradiance/prefilter conservation at every mip") {
    auto fx = makeIblFixture("rx_ibl_uniform_env");
    if (!fx.has_value()) {
        return;
    }

    // Non-white (per-channel-distinguishable) uniform environment --
    // stronger than a pure-white check: proves channel independence, not
    // just a scalar coincidence.
    constexpr float kR = 0.25F;
    constexpr float kG = 0.5F;
    constexpr float kB = 0.75F;
    auto pixels = buildUniformEquirect(16, 8, kR, kG, kB);
    auto source = uploadTestSource(*fx, 16, 8, 1, pixels);
    REQUIRE(source.has_value());

    rx::ibl::BakeParams params;
    params.baseCubemapFaceSize = 32;
    params.irradianceFaceSize = 8;
    params.irradianceSamples = 256;
    params.prefilteredMipCount = 4;
    params.prefilteredBaseFaceSize = 32;
    params.specularSamples = 64;
    params.dfgLutSize = 32;
    params.dfgSamples = 256;

    auto result = rx::ibl::bakeEnvironment(fx->device, fx->allocator, *fx->cmdCtx, *fx->scheduler, *source, false,
                                             RX_IBL_SHADER_DIR, params, nullptr, "test_analytic_uniform");
    REQUIRE(result.has_value());

    constexpr double kEps = 0.02;  // fp16-storage + bilinear-resample tolerance -- NOT a Monte-Carlo tolerance
                                     // (see this file's own header comment: uniform-environment conservation is
                                     // exact by cancellation, not by convergence).

    // baseCubemap: every face, center texel.
    for (uint32_t face = 0; face < 6; ++face) {
        auto texel = readTexelHalf4(*fx, result->baseCubemap, 0, face, params.baseCubemapFaceSize,
                                      params.baseCubemapFaceSize / 2, params.baseCubemapFaceSize / 2);
        INFO("baseCubemap face=", face, " rgb=(", texel[0], ",", texel[1], ",", texel[2], ")");
        CHECK(static_cast<double>(texel[0]) == doctest::Approx(kR).epsilon(kEps));
        CHECK(static_cast<double>(texel[1]) == doctest::Approx(kG).epsilon(kEps));
        CHECK(static_cast<double>(texel[2]) == doctest::Approx(kB).epsilon(kEps));
    }

    // irradianceCubemap: every face, center texel -- Ed()/PI convention
    // means the STORED value equals L0 directly (see
    // irradiance_convolve.slang's own header comment), not L0*PI.
    for (uint32_t face = 0; face < 6; ++face) {
        auto texel = readTexelHalf4(*fx, result->irradianceCubemap, 0, face, params.irradianceFaceSize,
                                      params.irradianceFaceSize / 2, params.irradianceFaceSize / 2);
        INFO("irradiance face=", face, " rgb=(", texel[0], ",", texel[1], ",", texel[2], ")");
        CHECK(static_cast<double>(texel[0]) == doctest::Approx(kR).epsilon(kEps));
        CHECK(static_cast<double>(texel[1]) == doctest::Approx(kG).epsilon(kEps));
        CHECK(static_cast<double>(texel[2]) == doctest::Approx(kB).epsilon(kEps));
    }

    // prefilteredCubemap: EVERY mip (including mip 0's own passthrough
    // special case), one representative face, center texel.
    for (uint32_t mip = 0; mip < params.prefilteredMipCount; ++mip) {
        const uint32_t mipDim = std::max(1U, params.prefilteredBaseFaceSize >> mip);
        auto texel = readTexelHalf4(*fx, result->prefilteredCubemap, mip, /*face=*/2, mipDim, mipDim / 2, mipDim / 2);
        INFO("prefiltered mip=", mip, " dim=", mipDim, " rgb=(", texel[0], ",", texel[1], ",", texel[2], ")");
        CHECK(static_cast<double>(texel[0]) == doctest::Approx(kR).epsilon(kEps));
        CHECK(static_cast<double>(texel[1]) == doctest::Approx(kG).epsilon(kEps));
        CHECK(static_cast<double>(texel[2]) == doctest::Approx(kB).epsilon(kEps));
    }

    CHECK_FALSE(fx->context.hasValidationErrors());
}

TEST_CASE("IBL Task 9: DFG LUT closed-form limit at the smoothest row -- dfgY->1.0, dfgX->(1-NoV)^5") {
    auto fx = makeIblFixture("rx_ibl_dfg_closed_form");
    if (!fx.has_value()) {
        return;
    }

    // A tiny flat cube source (sourceIsCube path -- this test only cares
    // about the DFG LUT, which is purely procedural and independent of
    // the environment content; cube-source skips the equirect stage
    // entirely, the cheapest path to a valid bake).
    std::array<std::array<float, 3>, 6> colors{};
    for (auto& c : colors) {
        c = {0.5F, 0.5F, 0.5F};
    }
    auto pixels = buildFlatCubeFaces(8, colors);
    auto source = uploadTestSource(*fx, 8, 8, 6, pixels);
    REQUIRE(source.has_value());

    rx::ibl::BakeParams params;
    params.baseCubemapFaceSize = 8;
    params.irradianceFaceSize = 4;
    params.irradianceSamples = 64;
    params.prefilteredMipCount = 2;
    params.prefilteredBaseFaceSize = 8;
    params.specularSamples = 32;
    params.dfgLutSize = 64;
    // Large sample count for THIS test specifically -- the smoothest-row
    // closed form is an asymptotic (linearRoughness ~ 0.0006 at
    // y=height-1, not exactly 0) limit, and GGX importance sampling at
    // very low roughness is a sharply peaked distribution; a generous
    // sample budget keeps residual Monte-Carlo noise well under the
    // assertion tolerance below.
    params.dfgSamples = 4096;

    auto result = rx::ibl::bakeEnvironment(fx->device, fx->allocator, *fx->cmdCtx, *fx->scheduler, *source, true,
                                             RX_IBL_SHADER_DIR, params, nullptr, "test_analytic_dfg");
    REQUIRE(result.has_value());

    const uint32_t smoothestRow = params.dfgLutSize - 1;
    // Two NoV columns away from the unstable NoV~0 edge (this closed
    // form's own derivation requires NoV>0 -- see this file's header
    // comment) -- x=width-1 (NoV~1) and x=3*width/4 (NoV~0.75).
    for (uint32_t x : {params.dfgLutSize - 1, params.dfgLutSize * 3 / 4}) {
        auto dfg = readDfgTexel(*fx, result->dfgLut, params.dfgLutSize, x, smoothestRow);
        const double noV = (static_cast<double>(x) + 0.5) / params.dfgLutSize;
        const double expectedX = std::pow(1.0 - noV, 5.0);
        INFO("x=", x, " NoV~=", noV, " dfg=(", dfg[0], ",", dfg[1], ") expectedX~=", expectedX);
        CHECK(static_cast<double>(dfg[1]) == doctest::Approx(1.0).epsilon(0.08));
        CHECK(static_cast<double>(dfg[0]) == doctest::Approx(expectedX).epsilon(0.15).scale(0.02));
    }

    CHECK_FALSE(fx->context.hasValidationErrors());
}

TEST_CASE("IBL Task 9: mip-0 exactly matches source (passthrough) AND prefiltered contrast monotonically "
          "decreases with roughness (mip-chain roughness mapping monotonicity)") {
    auto fx = makeIblFixture("rx_ibl_monotonicity");
    if (!fx.has_value()) {
        return;
    }

    // One bright face (+Z, index 4), five dark faces -- a NON-uniform
    // environment the "uniform conservation" tests above are
    // architecturally blind to variance/blur behavior on.
    std::array<std::array<float, 3>, 6> colors{};
    for (auto& c : colors) {
        c = {0.05F, 0.05F, 0.05F};
    }
    colors[4] = {1.0F, 1.0F, 1.0F};  // +Z
    constexpr uint32_t kDim = 32;
    auto pixels = buildFlatCubeFaces(kDim, colors);
    auto source = uploadTestSource(*fx, kDim, kDim, 6, pixels);
    REQUIRE(source.has_value());

    rx::ibl::BakeParams params;
    params.baseCubemapFaceSize = kDim;
    params.irradianceFaceSize = 8;
    params.irradianceSamples = 128;
    params.prefilteredMipCount = 5;
    params.prefilteredBaseFaceSize = kDim;
    params.specularSamples = 128;
    params.dfgLutSize = 16;
    params.dfgSamples = 64;

    auto result = rx::ibl::bakeEnvironment(fx->device, fx->allocator, *fx->cmdCtx, *fx->scheduler, *source, true,
                                             RX_IBL_SHADER_DIR, params, nullptr, "test_analytic_monotonicity");
    REQUIRE(result.has_value());

    // --- mip-0 ~= source (CubemapIBL.cpp's own linearRoughness==0
    // literal-passthrough special case, ported exactly) -- checked on the
    // NON-uniform source, at BOTH the bright face's center (far from any
    // boundary) and a dark face's center, so this is a real value match,
    // not a trivial "everything is the same constant" pass. ------------
    {
        auto brightMip0 = readTexelHalf4(*fx, result->prefilteredCubemap, 0, 4, kDim, kDim / 2, kDim / 2);
        INFO("mip0 bright face center rgb=(", brightMip0[0], ",", brightMip0[1], ",", brightMip0[2], ")");
        CHECK(static_cast<double>(brightMip0[0]) == doctest::Approx(1.0).epsilon(0.02));

        auto darkMip0 = readTexelHalf4(*fx, result->prefilteredCubemap, 0, 0, kDim, kDim / 2, kDim / 2);
        INFO("mip0 dark face center rgb=(", darkMip0[0], ",", darkMip0[1], ",", darkMip0[2], ")");
        CHECK(static_cast<double>(darkMip0[0]) == doctest::Approx(0.05).epsilon(0.1).scale(0.02));
    }

    // --- monotonicity: sample the BRIGHT face's own center direction at
    // every mip -- mip 0 sees the bright face alone (~1.0); as roughness
    // grows, the widening GGX lobe increasingly blends in the 5 dark
    // neighbors, so brightness must be monotonically NON-increasing
    // across mips, with a REAL (not rounding-level) decrease from mip 0
    // to the last mip. -----------------------------------------------
    std::vector<double> brightAtMip;
    for (uint32_t mip = 0; mip < params.prefilteredMipCount; ++mip) {
        const uint32_t mipDim = std::max(1U, params.prefilteredBaseFaceSize >> mip);
        auto texel = readTexelHalf4(*fx, result->prefilteredCubemap, mip, /*face=*/4, mipDim, mipDim / 2, mipDim / 2);
        INFO("mip=", mip, " dim=", mipDim, " bright-face-center r=", texel[0]);
        brightAtMip.push_back(static_cast<double>(texel[0]));
    }
    for (size_t i = 1; i < brightAtMip.size(); ++i) {
        INFO("mip ", i - 1, " -> ", i, ": ", brightAtMip[i - 1], " -> ", brightAtMip[i]);
        // Small numerical slack (0.01) absorbs fp16/Monte-Carlo noise
        // without hiding a real ordering violation.
        CHECK(brightAtMip[i] <= brightAtMip[i - 1] + 0.01);
    }
    INFO("mip0=", brightAtMip.front(), " last mip=", brightAtMip.back());
    CHECK(brightAtMip.front() - brightAtMip.back() > 0.1);  // a REAL decrease, not noise

    CHECK_FALSE(fx->context.hasValidationErrors());
}
