#pragma once
// src/rx_material/tests/ibl_environment_test_fixture.h -- Phase 5 Stage 1
// Task 10 [#46, gate matrix-p5t10-ibl-runtime-skybox.md]: builds a small,
// synthetic, BINDLESS-REGISTERED environment (irradiance cube + prefiltered
// cube + DFG LUT, per-face colors supplied by the caller) for GPU tests
// that need a REAL `rx::material::DrawDataGpu::env*`-shaped bundle without
// running the full `rx::ibl::bakeEnvironment()` bake chain (T9's own job,
// src/rx_ibl) -- this builds ALREADY-BAKED-SHAPED test doubles with known,
// hand-picked values so a test can assert closed-form expected pixel
// colors against them directly. Shared by test_standard_pbr_unlit.cpp
// (the occlusion-scales-IBL rewrite) and test_standard_pbr_ibl_gpu.cpp
// (this task's own new IBL lobe/skybox value tests) -- both compile into
// the SAME `rx_material_gpu_tests` binary, so an ordinary shared header
// (not a second per-file duplicate) is the right shape here, unlike this
// test suite's usual per-BINARY-duplicated-fixture idiom (which exists to
// avoid a cross-BINARY dependency, not a cross-file one within one binary).
//
// Mirrors src/rx_ibl/tests/ibl_gpu_fixture.h's own upload technique
// (staging buffer + one-shot command, not the full async Uploader path --
// a real load-time caller, samples/08_gltf_viewer, uses
// rx::ibl::bakeEnvironment() instead, which owns its own persistent-texture
// creation) and texture_cache.cpp's own `uploadEnvironmentFallback()`
// precedent for encoding fp16 texel data via `glm::packHalf4x16`/
// `packHalf2x16` (CLAUDE.md's "don't reinvent the wheel" -- GLM already
// ships a correct half-float packer, used elsewhere in this exact
// codebase for the identical purpose).
//
// FORMAT CHOICE: R16G16B16A16_SFLOAT for both cube textures, R16G16_SFLOAT
// for the DFG LUT -- EXACTLY rx::ibl::BakeResult's own production formats
// (bake.h's own header comment), not this suite's usual R32G32B32A32_SFLOAT
// test-convenience format (ibl_gpu_fixture.h's own header comment on why
// THAT file uses fp32: avoiding a packing step for its OWN synthetic
// sources). Matching production formats here matters because
// R16G16B16A16_SFLOAT's LINEAR-filter support is Vulkan-1.0-MANDATORY
// (unlike R32G32B32A32_SFLOAT's, which is not) -- these test cubes are
// read with a LINEAR-filtering sampler (mip selection via `SampleLevel`),
// so this is a real correctness requirement, not just fidelity-to-
// production for its own sake.

#include <rx_material/draw_data.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/texture.h>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace rx::material_test {

constexpr VkFormat kEnvCubeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kEnvDfgFormat = VK_FORMAT_R16G16_SFLOAT;

// One fully-built, bindless-registered synthetic environment. Every field
// is owned here (RAII via Texture2D/Buffer's own move-only ownership) --
// keep this alive for as long as any DrawDataGpu row populated via
// `applyTo()` might still be rendered.
struct TestEnvironment {
    rx::rhi::Texture2D irradianceCube;
    rx::rhi::Texture2D prefilteredCube;
    rx::rhi::Texture2D dfgLut;
    VkSampler cubeSampler = VK_NULL_HANDLE;
    VkSampler dfgSampler = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle irradianceHandle;
    rx::rhi::BindlessHandle prefilteredHandle;
    rx::rhi::BindlessHandle dfgLutHandle;
    rx::rhi::BindlessHandle cubeSamplerHandle;
    rx::rhi::BindlessHandle dfgSamplerHandle;

    // Populates a DrawDataGpu row's own `env*` fields from this environment
    // -- `intensity` is the PRE-EXPOSED scalar (see material.slang's own
    // RxDrawData::envIntensity header comment); every test using this
    // fixture passes intensity==1.0 (neutral) unless explicitly testing
    // exposure/intensity scaling itself.
    void applyTo(rx::material::DrawDataGpu& row, float intensity = 1.0F) const {
        row.envIrradianceCubeIndex = irradianceHandle.index();
        row.envPrefilteredCubeIndex = prefilteredHandle.index();
        row.envDfgLutIndex = dfgLutHandle.index();
        row.envCubeSamplerIndex = cubeSamplerHandle.index();
        row.envDfgSamplerIndex = dfgSamplerHandle.index();
        row.envMaxPrefilteredLod = 0.0F;  // single-mip test cubes -- every SampleLevel() call resolves to mip 0.
        row.envIntensity = intensity;
    }

    // Explicit teardown -- VkSampler is not an RAII type in this codebase's
    // own test idiom (matches test_standard_pbr_unlit.cpp's own
    // `rawWrapSamplers`/explicit-vkDestroySampler precedent). Call before
    // the owning VkDevice is destroyed.
    void destroySamplers(VkDevice device) {
        if (cubeSampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, cubeSampler, nullptr);
            cubeSampler = VK_NULL_HANDLE;
        }
        if (dfgSampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, dfgSampler, nullptr);
            dfgSampler = VK_NULL_HANDLE;
        }
    }
};

namespace detail {

// Packs one RGBA texel into 8 bytes (4x uint16 fp16), matching
// kEnvCubeFormat's own R16G16B16A16_SFLOAT byte layout exactly.
inline void packRgba16f(std::vector<uint8_t>& out, glm::vec4 color) {
    uint64_t packed = glm::packHalf4x16(color);
    size_t offset = out.size();
    out.resize(offset + sizeof(packed));
    std::memcpy(out.data() + offset, &packed, sizeof(packed));
}

// Packs one RG texel into 4 bytes (2x uint16 fp16), matching kEnvDfgFormat's
// own R16G16_SFLOAT byte layout exactly.
inline void packRg16f(std::vector<uint8_t>& out, glm::vec2 value) {
    uint32_t packed = glm::packHalf2x16(value);
    size_t offset = out.size();
    out.resize(offset + sizeof(packed));
    std::memcpy(out.data() + offset, &packed, sizeof(packed));
}

// One-shot staging-buffer upload into `dstImage` (already created,
// VK_IMAGE_LAYOUT_UNDEFINED), leaving it SHADER_READ_ONLY_OPTIMAL --
// mirrors src/rx_ibl/tests/ibl_gpu_fixture.h's own uploadTestSource()
// technique (this file's own header comment).
inline bool uploadTexels(rx::rhi::Device& device, rx::rhi::Allocator& allocator, VkImage dstImage,
                          uint32_t width, uint32_t height, uint32_t layerCount, const std::vector<uint8_t>& bytes) {
    auto staging = allocator.createHostVisibleBuffer(bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!staging.has_value()) {
        return false;
    }
    std::memcpy(staging->mappedData(), bytes.data(), bytes.size());
    staging->flush();

    auto cmdCtx = rx::rhi::CommandContext::create(device.device(), device.graphicsQueue(), device.graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        return false;
    }
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toDst{};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toDst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toDst.srcAccessMask = VK_ACCESS_2_NONE;
        toDst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = dstImage;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layerCount};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toDst;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layerCount};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging->handle(), dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier2 toRead = toDst;
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &dep2);
    });
    return true;
}

inline VkSampler createLinearClampSampler(VkDevice device) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.minLod = 0.0F;
    info.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return sampler;
}

}  // namespace detail

// Builds a full TestEnvironment: a 1x1-per-face, single-mip irradiance
// cube and a 1x1-per-face, single-mip prefiltered cube (six independently
// colored faces, `faceColors` in the SAME +X,-X,+Y,-Y,+Z,-Z order
// `rx::rhi::Texture2D::createCubeForPresuppliedMips()`'s own layer-index
// convention expects), plus a 1x1 DFG LUT holding exactly `dfgValue`
// everywhere -- every real sample this fixture's own callers issue reads
// the SAME single texel per face/LUT regardless of direction/UV, by
// construction, which is exactly what makes a hand-computed closed-form
// expected pixel value tractable for a test. Registers every resource into
// `bindless` (a REAL `registerCubeSampledImage()`/`registerSampledImage()`/
// `registerSampler()` call each -- this is genuine bindless production
// consumption, not a readback-only test double). Returns std::nullopt
// (never asserts) on any failure so a calling TEST_CASE can REQUIRE() with
// its own message.
inline std::optional<TestEnvironment> makeTestEnvironment(rx::rhi::Device& device, rx::rhi::Allocator& allocator,
                                                            rx::rhi::BindlessTable& bindless,
                                                            const std::array<glm::vec4, 6>& faceColors,
                                                            glm::vec2 dfgValue) {
    // `rx::rhi::Texture2D` has no PUBLIC default constructor (only its own
    // factory methods, returning std::optional<Texture2D>, can produce one
    // -- see rx_ibl/src/bake.cpp's own identical "built as N separate
    // locals rather than default-constructing... and assigning into
    // fields" precedent) -- every Texture2D-typed field is built as its
    // own local FIRST, then `TestEnvironment` is aggregate-list-
    // initialized from them (needs only the public MOVE constructor, never
    // the private default one) below, not default-constructed-then-
    // assigned-into.
    auto irradiance = rx::rhi::Texture2D::createCubeForPresuppliedMips(
        device.physicalDevice(), device.device(), allocator, VkExtent2D{1, 1}, kEnvCubeFormat,
        VK_IMAGE_USAGE_SAMPLED_BIT, /*mipLevels=*/1);
    auto prefiltered = rx::rhi::Texture2D::createCubeForPresuppliedMips(
        device.physicalDevice(), device.device(), allocator, VkExtent2D{1, 1}, kEnvCubeFormat,
        VK_IMAGE_USAGE_SAMPLED_BIT, /*mipLevels=*/1);
    auto dfgLut = rx::rhi::Texture2D::create(device.physicalDevice(), device.device(), allocator, VkExtent2D{1, 1},
                                              kEnvDfgFormat, VK_IMAGE_USAGE_SAMPLED_BIT, /*requestedMipLevels=*/1);
    if (!irradiance.has_value() || !prefiltered.has_value() || !dfgLut.has_value()) {
        return std::nullopt;
    }
    TestEnvironment env{std::move(*irradiance), std::move(*prefiltered), std::move(*dfgLut)};

    std::vector<uint8_t> cubeBytes;
    cubeBytes.reserve(6 * 8);
    for (const glm::vec4& c : faceColors) {
        detail::packRgba16f(cubeBytes, c);
    }
    if (!detail::uploadTexels(device, allocator, env.irradianceCube.image(), 1, 1, 6, cubeBytes) ||
        !detail::uploadTexels(device, allocator, env.prefilteredCube.image(), 1, 1, 6, cubeBytes)) {
        return std::nullopt;
    }

    std::vector<uint8_t> dfgBytes;
    detail::packRg16f(dfgBytes, dfgValue);
    if (!detail::uploadTexels(device, allocator, env.dfgLut.image(), 1, 1, 1, dfgBytes)) {
        return std::nullopt;
    }

    env.cubeSampler = detail::createLinearClampSampler(device.device());
    env.dfgSampler = detail::createLinearClampSampler(device.device());
    if (env.cubeSampler == VK_NULL_HANDLE || env.dfgSampler == VK_NULL_HANDLE) {
        return std::nullopt;
    }

    env.irradianceHandle = bindless.registerCubeSampledImage(env.irradianceCube.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    env.prefilteredHandle = bindless.registerCubeSampledImage(env.prefilteredCube.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    env.dfgLutHandle = bindless.registerSampledImage(env.dfgLut.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    env.cubeSamplerHandle = bindless.registerSampler(env.cubeSampler);
    env.dfgSamplerHandle = bindless.registerSampler(env.dfgSampler);
    if (!env.irradianceHandle.isValid() || !env.prefilteredHandle.isValid() || !env.dfgLutHandle.isValid() ||
        !env.cubeSamplerHandle.isValid() || !env.dfgSamplerHandle.isValid()) {
        return std::nullopt;
    }

    return env;
}

// Convenience overload -- all 6 faces the SAME uniform color (the common
// case: a Lambertian-under-uniform-environment probe, a furnace-test
// environment, an occlusion-scaling probe -- none of these need
// per-face variation).
inline std::optional<TestEnvironment> makeUniformTestEnvironment(rx::rhi::Device& device, rx::rhi::Allocator& allocator,
                                                                    rx::rhi::BindlessTable& bindless, glm::vec4 radiance,
                                                                    glm::vec2 dfgValue) {
    std::array<glm::vec4, 6> faces{radiance, radiance, radiance, radiance, radiance, radiance};
    return makeTestEnvironment(device, allocator, bindless, faces, dfgValue);
}

}  // namespace rx::material_test
