// Phase 4 Stage 2 Task 22 fix round [F1, spec D21, gate ruling #23]:
// comparison-sampler 3x3 PCF exercised through the REAL production lit
// path -- MaterialSystem::getPipeline() against the real, shipped
// shaders/material/standard_pbr.slang -- not a standalone probe rig
// (rx_shadow's own test_shadow_caster_gpu.cpp keeps that rig as
// supporting evidence for the shadow-caster pipeline mechanism itself;
// THIS file is the production-path proof the independent review's F1
// finding requires).
//
// SCENE -- deliberately the SAME numeric scene rx_shadow's own probe rig
// uses (ground plane Y=0 X,Z in [-10,10]; box caster X,Z in [3,3.5],
// Y in [0,0.5]; light dir normalize(0.7,-0.12,0.1), ~80.4 degrees from
// the ground normal) so the SAME analytic probe-point derivations apply
// -- only the RENDERING PATH differs (a real StandardPBR material +
// MaterialSystem::getPipeline(), not a bespoke receiver shader).
#include <doctest/doctest.h>
#include <rx_material/material_system.h>
#include <rx_material/draw_data.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_shadow/shadow_caster_pipeline.h>
#include <rx_shadow/shadow_frustum.h>
#include <rx_task/scheduler.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace rx::shadow;

namespace {

std::filesystem::path shaderDataPath(const char* filename) {
    return std::filesystem::path(RX_MATERIAL_SHADER_DIR) / filename;
}

struct Fixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    rx::rhi::Device device;
    rx::rhi::BindlessTable bindless;
    rx::rhi::Allocator allocator;
};

std::optional<Fixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping StandardPBR shadow test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping StandardPBR shadow test");
        return std::nullopt;
    }
    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());
    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);
    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    // Generous headroom, not tight accounting: each `renderSceneWithShadow()`
    // call registers a handful of NEW bindless resources (2 fallback
    // textures + shadow map + material sampler + comparison sampler + 2
    // storage buffers, PLUS a fresh MaterialSystem::create() call's own
    // default draw-data storage buffer) and this fixture's own tests call
    // it multiple times (A/B bias comparisons) without releasing handles
    // in between -- a real per-call resource lifecycle is this test's own
    // simplification, not a production pattern (compare samples 08's own
    // real deferred-release-via-DeletionQueue discipline).
    rx::rhi::BindlessTable::Capacities capacities;
    capacities.sampledImages = 32;
    capacities.samplers = 16;
    capacities.storageBuffers = 16;
    capacities.comparisonSamplers = 8;
    // [Phase 5 Task 10, #46] material.slang now unconditionally declares
    // `gTexturesCube` at binding 4 -- every BindlessTable feeding
    // MaterialSystem needs a nonzero cube-image capacity, same reasoning as
    // `comparisonSamplers` above (this fixture's own tests never actually
    // register a cube texture, but the pipeline LAYOUT still needs a
    // matching binding for the shader's static declaration).
    capacities.cubeImages = 4;
    auto bindless = rx::rhi::BindlessTable::create(device->physicalDevice(), device->device(), capacities);
    REQUIRE(bindless.has_value());

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());

    return Fixture{std::move(*window),  std::move(*context),   surface,
                    std::move(*device), std::move(*bindless), std::move(*allocator)};
}

std::filesystem::path freshCachePath(const char* name) {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / (std::string("rx_standard_pbr_shadow_") + name + ".cache");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

// D8-shaped vertex -- byte-identical to material_system.cpp's own
// MaterialVertexLayout (position/normal/tangent/uv, 48 bytes).
struct Vertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv[2];
};

constexpr float kGroundHalf = 10.0F;
constexpr float kBoxMinX = 3.0F;
constexpr float kBoxMaxX = 3.5F;
constexpr float kBoxMinZ = 3.0F;
constexpr float kBoxMaxZ = 3.5F;
constexpr float kBoxHeight = 0.5F;

struct SceneMesh {
    rx::rhi::Buffer vertexBuffer;
    rx::rhi::Buffer indexBuffer;
    uint32_t indexCount = 0;
};

Vertex makeVertex(glm::vec3 pos, glm::vec3 normal, glm::vec3 tangent, glm::vec2 uv) {
    Vertex v{};
    v.position[0] = pos.x;
    v.position[1] = pos.y;
    v.position[2] = pos.z;
    v.normal[0] = normal.x;
    v.normal[1] = normal.y;
    v.normal[2] = normal.z;
    v.tangent[0] = tangent.x;
    v.tangent[1] = tangent.y;
    v.tangent[2] = tangent.z;
    v.tangent[3] = 1.0F;
    v.uv[0] = uv.x;
    v.uv[1] = uv.y;
    return v;
}

// Ground plane (Y-up normal) + a box with REAL per-face normals/tangents
// (unlike rx_shadow's own depth-only test rig, which never needed a real
// normal -- this scene is actually LIT, so NdotL must be correct per
// face).
std::optional<SceneMesh> buildSceneMesh(rx::rhi::Allocator& allocator) {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Ground: one quad, normal +Y.
    vertices.push_back(makeVertex({-kGroundHalf, 0.0F, -kGroundHalf}, {0, 1, 0}, {1, 0, 0}, {0, 1}));
    vertices.push_back(makeVertex({kGroundHalf, 0.0F, -kGroundHalf}, {0, 1, 0}, {1, 0, 0}, {1, 1}));
    vertices.push_back(makeVertex({kGroundHalf, 0.0F, kGroundHalf}, {0, 1, 0}, {1, 0, 0}, {1, 0}));
    vertices.push_back(makeVertex({-kGroundHalf, 0.0F, kGroundHalf}, {0, 1, 0}, {1, 0, 0}, {0, 0}));
    indices.insert(indices.end(), {0, 1, 2, 0, 2, 3});

    // Box: 6 faces, 4 verts each (hard normals), 24 verts total.
    auto addFace = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 normal) {
        const auto base = static_cast<uint32_t>(vertices.size());
        const glm::vec3 tangent = glm::normalize(b - a);
        vertices.push_back(makeVertex(a, normal, tangent, {0, 1}));
        vertices.push_back(makeVertex(b, normal, tangent, {1, 1}));
        vertices.push_back(makeVertex(c, normal, tangent, {1, 0}));
        vertices.push_back(makeVertex(d, normal, tangent, {0, 0}));
        indices.insert(indices.end(), {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
    };
    const glm::vec3 p000{kBoxMinX, 0.0F, kBoxMinZ};
    const glm::vec3 p100{kBoxMaxX, 0.0F, kBoxMinZ};
    const glm::vec3 p110{kBoxMaxX, 0.0F, kBoxMaxZ};
    const glm::vec3 p010{kBoxMinX, 0.0F, kBoxMaxZ};
    const glm::vec3 p001{kBoxMinX, kBoxHeight, kBoxMinZ};
    const glm::vec3 p101{kBoxMaxX, kBoxHeight, kBoxMinZ};
    const glm::vec3 p111{kBoxMaxX, kBoxHeight, kBoxMaxZ};
    const glm::vec3 p011{kBoxMinX, kBoxHeight, kBoxMaxZ};
    addFace(p000, p100, p101, p001, {0, 0, -1});  // -Z
    addFace(p100, p110, p111, p101, {1, 0, 0});   // +X
    addFace(p110, p010, p011, p111, {0, 0, 1});   // +Z
    addFace(p010, p000, p001, p011, {-1, 0, 0});  // -X
    addFace(p001, p101, p111, p011, {0, 1, 0});   // top
    addFace(p010, p110, p100, p000, {0, -1, 0});  // bottom

    auto vb = allocator.createHostVisibleBuffer(vertices.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                 rx::rhi::MemoryCategory::Internal);
    auto ib = allocator.createHostVisibleBuffer(indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                                 rx::rhi::MemoryCategory::Internal);
    if (!vb.has_value() || !ib.has_value()) {
        return std::nullopt;
    }
    std::memcpy(vb->mappedData(), vertices.data(), vertices.size() * sizeof(Vertex));
    vb->flush();
    std::memcpy(ib->mappedData(), indices.data(), indices.size() * sizeof(uint32_t));
    ib->flush();
    return SceneMesh{std::move(*vb), std::move(*ib), static_cast<uint32_t>(indices.size())};
}

const glm::vec3 kLightDir = glm::normalize(glm::vec3(0.7F, -0.12F, 0.1F));
constexpr uint32_t kExtent = 256;
constexpr float kCameraHalfExtent = 12.0F;
constexpr uint32_t kShadowMapResolution = 256;

// [D13, gate ruling RC3] Reversed-Z main camera -- MaterialSystem::
// getPipeline() hardcodes GREATER_OR_EQUAL; a top-down ORTHOGRAPHIC eye
// (for exact analytic world<->pixel correspondence, mirroring rx_shadow's
// own test rig) needs the SAME affine `z' = 1-z` remap
// samples/06_materials' reverseOrthoZ()/this project's own established
// precedent uses (an orthographic projection has no perspective
// nonlinearity to restate for reversed-Z's precision benefit).
glm::mat4 topDownReversedZCameraViewProj() {
    const glm::mat4 view =
        glm::lookAt(glm::vec3(0.0F, 20.0F, 0.0F), glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, -1.0F));
    glm::mat4 proj = glm::orthoZO(-kCameraHalfExtent, kCameraHalfExtent, -kCameraHalfExtent, kCameraHalfExtent, 0.1F, 30.0F);
    proj[1][1] *= -1.0F;
    for (int col = 0; col < 4; ++col) {
        const float row3 = (col == 3) ? 1.0F : 0.0F;
        proj[col][2] = row3 - proj[col][2];
    }
    return proj * view;
}

struct PixelCoord {
    int x = 0;
    int y = 0;
};

PixelCoord worldToPixel(const glm::mat4& viewProj, const glm::vec3& world, uint32_t extent) {
    const glm::vec4 clip = viewProj * glm::vec4(world, 1.0F);
    const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
    const glm::vec2 uv = ndc * 0.5F + 0.5F;
    return PixelCoord{static_cast<int>(uv.x * static_cast<float>(extent)), static_cast<int>(uv.y * static_cast<float>(extent))};
}

struct Rgba8 {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};

Rgba8 pixelAt(const std::vector<uint8_t>& pixels, uint32_t extent, int x, int y) {
    const size_t idx = (static_cast<size_t>(y) * extent + static_cast<size_t>(x)) * 4;
    return Rgba8{pixels[idx], pixels[idx + 1], pixels[idx + 2], pixels[idx + 3]};
}

template <typename T>
void setParam(std::vector<uint8_t>& blob, const std::vector<rx::material::MaterialParamInfo>& params, const char* name,
              const T& value) {
    for (const auto& p : params) {
        if (p.name == name) {
            REQUIRE(p.size == sizeof(T));
            std::memcpy(blob.data() + p.offset, &value, sizeof(T));
            return;
        }
    }
    FAIL("param not found: ", name);
}

// Neutral StandardPbrParams: white base color, roughness=1 (diffuse-
// dominant, easier to reason about for the probes below), metallic=0
// (dielectric F0=0.04, small but nonzero specular -- deliberately not
// zeroed further; the probes below use variance/threshold checks
// tolerant of a small specular contribution, not exact closed-form pixel
// values).
std::vector<uint8_t> makeParamBlob(rx::material::MaterialSystem& system, rx::material::MaterialHandle handle,
                                    uint32_t whiteTex, uint32_t flatNormalTex, uint32_t samplerIndex) {
    uint32_t blockSize = system.paramBlockSize(handle);
    const auto params = system.materialParams(handle);
    std::vector<uint8_t> blob(blockSize, 0);
    setParam(blob, params, "baseColorFactor", std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F});
    setParam(blob, params, "metallicFactor", 0.0F);
    setParam(blob, params, "roughnessFactor", 1.0F);
    setParam(blob, params, "normalScale", 1.0F);
    setParam(blob, params, "occlusionStrength", 1.0F);
    setParam(blob, params, "alphaCutoff", 0.0F);
    // [Phase 5 Stage 1 Task 8, #44] KHR_materials_ior/_specular's own
    // glTF-spec-default neutral values -- see standard_pbr.slang's own
    // computeDielectricF0F90() header comment (test_standard_pbr_unlit.cpp's
    // own makeDefaultStandardPbrBlob() carries the same three calls with
    // the full regression-guard rationale). A zero-filled `ior` would trip
    // the ior<=0 special case (Fresnel forced to 1.0 at every angle),
    // silently breaking this file's own shadow-attenuation pixel checks.
    setParam(blob, params, "ior", 1.5F);
    setParam(blob, params, "specularFactor", 1.0F);
    setParam(blob, params, "specularColorFactorAndPad", std::array<float, 4>{1.0F, 1.0F, 1.0F, 0.0F});
    setParam(blob, params, "dfgY", 1.0F);
    setParam(blob, params, "emissiveFactorAndPad", std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F});
    setParam(blob, params, "baseColorTexture", whiteTex);
    setParam(blob, params, "metallicRoughnessTexture", whiteTex);
    setParam(blob, params, "normalTexture", flatNormalTex);
    setParam(blob, params, "occlusionTexture", whiteTex);
    setParam(blob, params, "emissiveTexture", whiteTex);
    setParam(blob, params, "baseColorSampler", samplerIndex);
    setParam(blob, params, "metallicRoughnessSampler", samplerIndex);
    setParam(blob, params, "normalSampler", samplerIndex);
    setParam(blob, params, "occlusionSampler", samplerIndex);
    setParam(blob, params, "emissiveSampler", samplerIndex);
    std::array<float, 4> neutralUv{0.0F, 0.0F, 1.0F, 1.0F};
    setParam(blob, params, "baseColorUvOffsetScale", neutralUv);
    setParam(blob, params, "metallicRoughnessUvOffsetScale", neutralUv);
    setParam(blob, params, "normalUvOffsetScale", neutralUv);
    setParam(blob, params, "occlusionUvOffsetScale", neutralUv);
    setParam(blob, params, "emissiveUvOffsetScale", neutralUv);
    return blob;
}


struct RenderResult {
    std::vector<uint8_t> pixels;
    float worldTexelSize = 0.0F;
};

// Built ONCE per TEST_CASE and reused across every bias variant that
// TEST_CASE renders (the A/B zero-bias-vs-slope-scaled-bias probes below)
// -- NOT rebuilt per call. Destroying and recreating a whole
// MaterialSystem (hence its own pipeline layout) on every call, while the
// SAME BindlessTable/descriptor set stays alive across that destruction,
// was reproduced directly during this file's own development to corrupt
// the validation layer's own per-descriptor-set bookkeeping: a real
// SIGSEGV inside libVkLayer_khronos_validation.so's own vkCmdDrawIndexed
// validation on the SECOND such call, not a hypothetical or a bug in this
// engine's own code (gdb backtrace showed the crash entirely inside the
// vendored validation layer, called from an otherwise-ordinary
// vkCmdDrawIndexed). A single long-lived MaterialSystem instance across
// repeated renders is also the REALISTIC usage shape (a real renderer
// never tears down and rebuilds its MaterialSystem between frames just
// because a light's bias tuning changed) -- this is not merely a test
// workaround.
struct SceneRig {
    std::unique_ptr<rx::material::MaterialSystem> system;
    rx::material::MaterialHandle material;
    std::optional<SceneMesh> mesh;
    std::unique_ptr<ShadowCasterPipeline> caster;
    uint32_t whiteTex = 0;
    uint32_t flatNormalTex = 0;
    uint32_t samplerIndex = 0;
    VkSampler rawSampler = VK_NULL_HANDLE;
    ShadowFrustumFit fit;
    std::optional<rx::rhi::Buffer> casterDataBuffer;
    rx::rhi::BindlessHandle casterDataHandle;
    VkImage shadowImage = VK_NULL_HANDLE;
    VkDeviceMemory shadowMemory = VK_NULL_HANDLE;
    VkImageView shadowView = VK_NULL_HANDLE;
    VkSampler compareSampler = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle compareSamplerHandle;
    rx::rhi::BindlessHandle shadowMapHandle;
    std::optional<rx::rhi::Buffer> drawDataBuffer;
    rx::rhi::BindlessHandle drawDataHandle;
    std::vector<uint8_t> paramBlob;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::optional<rx::rhi::CommandContext> cmdCtx;
};

void destroySceneRig(Fixture& fixture, SceneRig& rig) {
    const VkDevice device = fixture.device.device();
    if (device == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(device);
    if (rig.rawSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, rig.rawSampler, nullptr);
    }
    if (rig.compareSampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, rig.compareSampler, nullptr);
    }
    if (rig.shadowView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, rig.shadowView, nullptr);
    }
    if (rig.shadowImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, rig.shadowImage, nullptr);
    }
    if (rig.shadowMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, rig.shadowMemory, nullptr);
    }
}

std::optional<SceneRig> buildSceneRig(Fixture& fixture) {
    const VkDevice device = fixture.device.device();
    const VkPhysicalDevice physicalDevice = fixture.device.physicalDevice();

    SceneRig rig;
    rig.system = rx::material::MaterialSystem::create(fixture.device, fixture.bindless, freshCachePath("scene_probe"));
    if (!rig.system) {
        return std::nullopt;
    }
    // doubleSided=true: this scene's own hand-authored ground/box winding
    // is not independently verified against MaterialSystem's real
    // back-face culling (cullMode=BACK unless doubleSided) the way
    // rx_shadow's own depth-only probe rig sidesteps the question
    // entirely (cullMode=NONE there, since a shadow-caster pass casts
    // regardless of facing) -- disabling culling here is a deliberate,
    // low-risk test-scene simplification, not a claim about winding
    // correctness this test cares about proving.
    rx::material::MaterialFixedFunctionState fixedFunctionState;
    fixedFunctionState.doubleSided = true;
    rig.material = rig.system->loadMaterial(shaderDataPath("standard_pbr.slang"), fixedFunctionState);

    rig.mesh = buildSceneMesh(fixture.allocator);
    if (!rig.mesh.has_value()) {
        return std::nullopt;
    }

    rig.caster = ShadowCasterPipeline::create(fixture.device, fixture.bindless);
    if (!rig.caster) {
        return std::nullopt;
    }

    // --- White / flat-normal fallback textures -----------------------------
    {
        std::array<uint8_t, 4> whitePixel{255, 255, 255, 255};
        rx::material::TextureCreateInfo info;
        info.width = 1;
        info.height = 1;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.pixels = whitePixel.data();
        info.pixelBytes = whitePixel.size();
        rx::material::TextureHandle tex = rig.system->createTexture2D(info);
        if (!tex.isValid()) {
            return std::nullopt;
        }
        rig.whiteTex = rig.system->textureBindlessIndex(tex);

        std::array<uint8_t, 4> flatPixel{128, 128, 255, 255};
        info.pixels = flatPixel.data();
        rx::material::TextureHandle normalTex = rig.system->createTexture2D(info);
        if (!normalTex.isValid()) {
            return std::nullopt;
        }
        rig.flatNormalTex = rig.system->textureBindlessIndex(normalTex);
    }

    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &rig.rawSampler) != VK_SUCCESS) {
            return std::nullopt;
        }
        rx::rhi::BindlessHandle h = fixture.bindless.registerSampler(rig.rawSampler);
        if (!h.isValid()) {
            return std::nullopt;
        }
        rig.samplerIndex = h.index();
    }

    // --- Fitted, texel-snapped light frustum --------------------------------
    const glm::mat4 lightView = lightSpaceView(kLightDir);
    rig.fit = fitShadowFrustum(glm::vec3(-kGroundHalf, 0.0F, -kGroundHalf), glm::vec3(kGroundHalf, kBoxHeight, kGroundHalf),
                                 lightView, kShadowMapResolution, /*depthPaddingWorldUnits=*/2.0F);

    ShadowDrawData casterRow;
    casterRow.model = glm::transpose(glm::mat4(1.0F));
    casterRow.lightViewProj = glm::transpose(rig.fit.lightViewProj);
    rig.casterDataBuffer = fixture.allocator.createHostVisibleBuffer(sizeof(ShadowDrawData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                                        rx::rhi::MemoryCategory::Internal);
    if (!rig.casterDataBuffer.has_value()) {
        return std::nullopt;
    }
    std::memcpy(rig.casterDataBuffer->mappedData(), &casterRow, sizeof(casterRow));
    rig.casterDataBuffer->flush();
    rig.casterDataHandle = fixture.bindless.registerStorageBuffer(rig.casterDataBuffer->handle(), sizeof(ShadowDrawData));
    if (!rig.casterDataHandle.isValid()) {
        return std::nullopt;
    }

    // --- Shadow map image ---------------------------------------------------
    VkImageCreateInfo shadowImageInfo{};
    shadowImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    shadowImageInfo.imageType = VK_IMAGE_TYPE_2D;
    shadowImageInfo.format = VK_FORMAT_D32_SFLOAT;
    shadowImageInfo.extent = {kShadowMapResolution, kShadowMapResolution, 1};
    shadowImageInfo.mipLevels = 1;
    shadowImageInfo.arrayLayers = 1;
    shadowImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    shadowImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    shadowImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    shadowImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &shadowImageInfo, nullptr, &rig.shadowImage) != VK_SUCCESS) {
        return std::nullopt;
    }
    VkMemoryRequirements shadowMemReq{};
    vkGetImageMemoryRequirements(device, rig.shadowImage, &shadowMemReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    uint32_t shadowMemType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((shadowMemReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            shadowMemType = i;
            break;
        }
    }
    if (shadowMemType == UINT32_MAX) {
        return std::nullopt;
    }
    VkMemoryAllocateInfo shadowAllocInfo{};
    shadowAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    shadowAllocInfo.allocationSize = shadowMemReq.size;
    shadowAllocInfo.memoryTypeIndex = shadowMemType;
    if (vkAllocateMemory(device, &shadowAllocInfo, nullptr, &rig.shadowMemory) != VK_SUCCESS) {
        return std::nullopt;
    }
    vkBindImageMemory(device, rig.shadowImage, rig.shadowMemory, 0);
    VkImageViewCreateInfo shadowViewInfo{};
    shadowViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    shadowViewInfo.image = rig.shadowImage;
    shadowViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    shadowViewInfo.format = VK_FORMAT_D32_SFLOAT;
    shadowViewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &shadowViewInfo, nullptr, &rig.shadowView) != VK_SUCCESS) {
        return std::nullopt;
    }

    rig.cmdCtx = rx::rhi::CommandContext::create(device, fixture.device.graphicsQueue(), fixture.device.graphicsQueueFamily());
    if (!rig.cmdCtx.has_value()) {
        return std::nullopt;
    }

    // --- Comparison sampler, registered into the REAL BindlessTable's ------
    // new binding 3 (kComparisonSamplerBinding) -----------------------------
    VkSamplerCreateInfo compareSamplerInfo{};
    compareSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    compareSamplerInfo.magFilter = VK_FILTER_LINEAR;
    compareSamplerInfo.minFilter = VK_FILTER_LINEAR;
    compareSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    compareSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    compareSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    compareSamplerInfo.compareEnable = VK_TRUE;
    compareSamplerInfo.compareOp = VK_COMPARE_OP_LESS;
    compareSamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &compareSamplerInfo, nullptr, &rig.compareSampler) != VK_SUCCESS) {
        return std::nullopt;
    }
    rig.compareSamplerHandle = fixture.bindless.registerComparisonSampler(rig.compareSampler);
    if (!rig.compareSamplerHandle.isValid()) {
        return std::nullopt;
    }
    rig.shadowMapHandle = fixture.bindless.registerSampledImage(rig.shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!rig.shadowMapHandle.isValid()) {
        return std::nullopt;
    }

    // --- Real DrawDataGpu row: camera + light + shadow fields --------------
    // Identical across every bias variant this rig ever renders (bias only
    // affects the STORED shadow-map depth values, never this row's own
    // content) -- built once, here.
    const glm::mat4 cameraViewProj = topDownReversedZCameraViewProj();
    rx::material::DrawDataGpu row;
    row.model = glm::transpose(glm::mat4(1.0F));
    row.normalMatrix = glm::transpose(glm::mat4(1.0F));
    row.viewProj = glm::transpose(cameraViewProj);
    // RxDrawData::lightDirWorld convention: unit vector FROM the surface
    // TOWARD the light -- the negation of kLightDir's own "light travel
    // direction" convention (matches LightRecord::direction's own
    // documented meaning, scene.h).
    row.lightDirWorld = glm::vec4(-kLightDir, 0.0F);
    row.lightColor = glm::vec4(3.0F, 3.0F, 3.0F, 0.0F);
    row.ambientColor = glm::vec4(0.08F, 0.08F, 0.08F, 0.0F);
    row.cameraPosWorld = glm::vec4(0.0F, 20.0F, 0.0F, 0.0F);
    row.materialIndex = 0;
    row.lightViewProj = glm::transpose(rig.fit.lightViewProj);
    row.shadowMapTextureIndex = rig.shadowMapHandle.index();
    row.shadowCompareSamplerIndex = rig.compareSamplerHandle.index();
    row.shadowTexelSize = 1.0F / static_cast<float>(kShadowMapResolution);

    rig.drawDataBuffer =
        fixture.allocator.createHostVisibleBuffer(sizeof(row), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rx::rhi::MemoryCategory::Internal);
    if (!rig.drawDataBuffer.has_value()) {
        return std::nullopt;
    }
    std::memcpy(rig.drawDataBuffer->mappedData(), &row, sizeof(row));
    rig.drawDataBuffer->flush();
    rig.drawDataHandle = fixture.bindless.registerStorageBuffer(rig.drawDataBuffer->handle(), sizeof(row));
    if (!rig.drawDataHandle.isValid()) {
        return std::nullopt;
    }

    rig.paramBlob = makeParamBlob(*rig.system, rig.material, rig.whiteTex, rig.flatNormalTex, rig.samplerIndex);

    rx::graph::PassSignature sig;
    sig.colorCount = 1;
    sig.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    sig.depthFormat = VK_FORMAT_D32_SFLOAT;
    sig.samples = VK_SAMPLE_COUNT_1_BIT;
    rig.pipeline = rig.system->getPipeline({rig.material, sig, 0});
    if (rig.pipeline == VK_NULL_HANDLE) {
        return std::nullopt;
    }

    return rig;
}

// Re-records the shadow-caster pass into `rig`'s own (shared, persistent)
// shadow map with `depthBiasConstantFactor`/`depthBiasSlopeFactor`, then
// renders the REAL StandardPBR receiver pass into a FRESH offscreen color
// target and reads it back. Every OTHER resource (MaterialSystem,
// pipeline, textures, comparison sampler, shadow map image, draw-data
// buffer) is `rig`'s own, built once by `buildSceneRig()` and reused
// across every call -- see `SceneRig`'s own header comment for why.
std::optional<RenderResult> renderWithBias(Fixture& fixture, SceneRig& rig, float depthBiasConstantFactor,
                                            float depthBiasSlopeFactor) {
    const VkDevice device = fixture.device.device();
    const VkPhysicalDevice physicalDevice = fixture.device.physicalDevice();

    // --- Pass A: re-render the shadow map with this call's own bias --------
    rig.cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toDepth{};
        toDepth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toDepth.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toDepth.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toDepth.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        toDepth.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // UNDEFINED is still correct even on a re-render (this attachment's
        // LOAD_OP_CLEAR below discards whatever was there regardless of
        // its true prior layout -- SHADER_READ_ONLY_OPTIMAL after the
        // first call).
        toDepth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        toDepth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDepth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDepth.image = rig.shadowImage;
        toDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toDepth;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = rig.shadowView;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil = {1.0F, 0};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = {{0, 0}, {kShadowMapResolution, kShadowMapResolution}};
        renderingInfo.layerCount = 1;
        renderingInfo.pDepthAttachment = &depthAttachment;
        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport viewport{0.0F, 0.0F, static_cast<float>(kShadowMapResolution), static_cast<float>(kShadowMapResolution), 0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, {kShadowMapResolution, kShadowMapResolution}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        rig.caster->bindAndSetDepthBias(cmd, depthBiasConstantFactor, depthBiasSlopeFactor);
        VkDescriptorSet set = fixture.bindless.descriptorSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rig.caster->pipelineLayout(), 0, 1, &set, 0, nullptr);
        ShadowCasterPushConstants push{rig.casterDataHandle.index()};
        vkCmdPushConstants(cmd, rig.caster->pipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

        VkDeviceSize offset = 0;
        VkBuffer vb = rig.mesh->vertexBuffer.handle();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cmd, rig.mesh->indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, rig.mesh->indexCount, 1, 0, 0, 0);

        vkCmdEndRendering(cmd);

        VkImageMemoryBarrier2 toRead{};
        toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = rig.shadowImage;
        toRead.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep2{};
        dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &dep2);
    });

    // --- Pass B: receiver -- the REAL StandardPBR pipeline, into a FRESH ---
    // offscreen color target (the one thing genuinely per-call: this
    // call's own captured image) -----------------------------------------
    rx::graph::RenderGraph graph;
    rx::graph::Pass& pass = graph.addPass("scene_probe");
    rx::graph::AttachmentDesc colorDesc;
    colorDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
    pass.addColorOutput("color", colorDesc);
    rx::graph::AttachmentDesc depthDesc;
    depthDesc.format = VK_FORMAT_D32_SFLOAT;
    // [D29/RC3] Reversed -- matches MaterialSystem::getPipeline()'s own
    // GREATER_OR_EQUAL flip and this file's own topDownReversedZCameraViewProj().
    depthDesc.depthConvention = rx::graph::DepthConvention::Reversed;
    pass.setDepthStencilOutput("depth", depthDesc);
    graph.setBackbufferSource("color");

    std::array<VkDescriptorPoolSize, 1> poolSizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
        return std::nullopt;
    }

    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo paramSetLayoutInfo{};
    paramSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    paramSetLayoutInfo.bindingCount = 1;
    paramSetLayoutInfo.pBindings = &uboBinding;
    VkDescriptorSetLayout paramSetLayout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &paramSetLayoutInfo, nullptr, &paramSetLayout) != VK_SUCCESS) {
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        return std::nullopt;
    }

    std::optional<rx::rhi::Buffer> paramBuffer;
    pass.setExecute([&](rx::graph::PassContext& ctx) {
        VkCommandBuffer cmd = ctx.cmd;
        VkViewport viewport{0.0F, 0.0F, static_cast<float>(kExtent), static_cast<float>(kExtent), 0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, {kExtent, kExtent}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkDescriptorSet bindlessSet = fixture.bindless.descriptorSet();
        VkPipelineLayout layout = rig.system->pipelineLayout(rig.material);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &bindlessSet, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rig.pipeline);

        rx::material::MaterialGlobalsPush push;
        push.defaultSamplerIndex = rig.samplerIndex;
        push.drawDataBufferIndex = rig.drawDataHandle.index();
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

        paramBuffer = fixture.allocator.createHostVisibleBuffer(rig.paramBlob.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                                  rx::rhi::MemoryCategory::Internal);
        REQUIRE(paramBuffer.has_value());
        std::memcpy(paramBuffer->mappedData(), rig.paramBlob.data(), rig.paramBlob.size());
        paramBuffer->flush();

        VkDescriptorSetAllocateInfo setAllocInfo{};
        setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAllocInfo.descriptorPool = descriptorPool;
        setAllocInfo.descriptorSetCount = 1;
        setAllocInfo.pSetLayouts = &paramSetLayout;
        VkDescriptorSet paramSet = VK_NULL_HANDLE;
        REQUIRE(vkAllocateDescriptorSets(device, &setAllocInfo, &paramSet) == VK_SUCCESS);

        VkDescriptorBufferInfo bufferInfo{paramBuffer->handle(), 0, rig.paramBlob.size()};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = paramSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &paramSet, 0, nullptr);

        VkDeviceSize offset = 0;
        VkBuffer vb = rig.mesh->vertexBuffer.handle();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdBindIndexBuffer(cmd, rig.mesh->indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, rig.mesh->indexCount, 1, 0, 0, 0);
    });

    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = kExtent;
    compileInfo.swapchainHeight = kExtent;
    compileInfo.swapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);

    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto realExecutor = rx::graph::Executor::create(fixture.device, *scheduler);
    if (!realExecutor) {
        vkDestroyDescriptorSetLayout(device, paramSetLayout, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        return std::nullopt;
    }
    realExecutor->realize(graph);

    VkImageCreateInfo colorImageInfo{};
    colorImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    colorImageInfo.imageType = VK_IMAGE_TYPE_2D;
    colorImageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    colorImageInfo.extent = {kExtent, kExtent, 1};
    colorImageInfo.mipLevels = 1;
    colorImageInfo.arrayLayers = 1;
    colorImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    colorImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    colorImageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    colorImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage colorImage = VK_NULL_HANDLE;
    if (vkCreateImage(device, &colorImageInfo, nullptr, &colorImage) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(device, paramSetLayout, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        return std::nullopt;
    }
    VkMemoryRequirements colorMemReq{};
    vkGetImageMemoryRequirements(device, colorImage, &colorMemReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    uint32_t colorMemType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((colorMemReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            colorMemType = i;
            break;
        }
    }
    REQUIRE(colorMemType != UINT32_MAX);
    VkMemoryAllocateInfo colorAllocInfo{};
    colorAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    colorAllocInfo.allocationSize = colorMemReq.size;
    colorAllocInfo.memoryTypeIndex = colorMemType;
    VkDeviceMemory colorMemory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(device, &colorAllocInfo, nullptr, &colorMemory) == VK_SUCCESS);
    vkBindImageMemory(device, colorImage, colorMemory, 0);
    VkImageViewCreateInfo colorViewInfo{};
    colorViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    colorViewInfo.image = colorImage;
    colorViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    colorViewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    colorViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView colorView = VK_NULL_HANDLE;
    REQUIRE(vkCreateImageView(device, &colorViewInfo, nullptr, &colorView) == VK_SUCCESS);

    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(kExtent) * kExtent * 4;
    auto readback = fixture.allocator.createHostVisibleBuffer(pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    rig.cmdCtx->runOnce([&](VkCommandBuffer cmd) { realExecutor->execute(graph, cmd, colorImage, colorView, VkExtent2D{kExtent, kExtent}); });
    rig.cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kExtent, kExtent, 1};
        vkCmdCopyImageToBuffer(cmd, colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
    });
    readback->invalidate();

    RenderResult result;
    result.pixels.resize(static_cast<size_t>(pixelBytes));
    std::memcpy(result.pixels.data(), readback->mappedData(), result.pixels.size());
    result.worldTexelSize = rig.fit.worldTexelSize;

    vkDeviceWaitIdle(device);
    vkDestroyImageView(device, colorView, nullptr);
    vkDestroyImage(device, colorImage, nullptr);
    vkFreeMemory(device, colorMemory, nullptr);
    vkDestroyDescriptorSetLayout(device, paramSetLayout, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);

    return result;
}

}  // namespace

TEST_CASE("Real lit-path shadow: acne probe -- a grazing-angle ground patch is uniformly lit through StandardPBR "
          "with slope-scaled bias, and shows acne (high variance) at zero bias") {
    auto fixture = makeFixture("rx_material_shadow_acne_probe");
    if (!fixture.has_value()) {
        return;
    }
    auto rig = buildSceneRig(*fixture);
    REQUIRE(rig.has_value());

    const glm::mat4 cameraViewProj = topDownReversedZCameraViewProj();
    const PixelCoord center = worldToPixel(cameraViewProj, glm::vec3(-6.0F, 0.0F, -6.0F), kExtent);
    REQUIRE(center.x >= 3);
    REQUIRE(center.y >= 3);
    REQUIRE(center.x < static_cast<int>(kExtent) - 3);
    REQUIRE(center.y < static_cast<int>(kExtent) - 3);

    auto neighborhoodVariance = [&](const std::vector<uint8_t>& pixels) {
        double sum = 0.0;
        double sumSq = 0.0;
        int count = 0;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const double value = static_cast<double>(pixelAt(pixels, kExtent, center.x + dx, center.y + dy).r);
                sum += value;
                sumSq += value * value;
                ++count;
            }
        }
        const double mean = sum / count;
        return sumSq / count - mean * mean;
    };

    auto zeroBias = renderWithBias(*fixture, *rig, 0.0F, 0.0F);
    REQUIRE(zeroBias.has_value());
    const double acneVariance = neighborhoodVariance(zeroBias->pixels);

    auto biased = renderWithBias(*fixture, *rig, 4.0F, 3.0F);
    REQUIRE(biased.has_value());
    const double fixedVariance = neighborhoodVariance(biased->pixels);

    CAPTURE(acneVariance);
    CAPTURE(fixedVariance);
    CHECK(fixedVariance < acneVariance * 0.5);

    CHECK_FALSE(fixture->context.hasValidationErrors());
    destroySceneRig(*fixture, *rig);
}

TEST_CASE("Real lit-path shadow: peter-panning probe -- the caster's own base silhouette is continuous with its "
          "cast shadow through StandardPBR (no visible gap)") {
    auto fixture = makeFixture("rx_material_shadow_peter_panning_probe");
    if (!fixture.has_value()) {
        return;
    }
    auto rig = buildSceneRig(*fixture);
    REQUIRE(rig.has_value());
    auto biased = renderWithBias(*fixture, *rig, 4.0F, 3.0F);
    REQUIRE(biased.has_value());

    const glm::mat4 cameraViewProj = topDownReversedZCameraViewProj();
    const PixelCoord contact = worldToPixel(cameraViewProj, glm::vec3(3.55F, 0.0F, 3.25F), kExtent);
    REQUIRE(contact.x >= 0);
    REQUIRE(contact.x < static_cast<int>(kExtent));
    REQUIRE(contact.y >= 0);
    REQUIRE(contact.y < static_cast<int>(kExtent));

    // A far, definitely-unshadowed reference patch to compare against --
    // the "shadowed" claim is relative brightness, not an absolute
    // threshold (StandardPBR's own ambient+ direct terms aren't as easy
    // to hand-derive as the standalone probe rig's raw 0-255 visibility
    // value).
    const PixelCoord reference = worldToPixel(cameraViewProj, glm::vec3(-6.0F, 0.0F, -6.0F), kExtent);
    const Rgba8 contactPixel = pixelAt(biased->pixels, kExtent, contact.x, contact.y);
    const Rgba8 referencePixel = pixelAt(biased->pixels, kExtent, reference.x, reference.y);

    CAPTURE(static_cast<int>(contactPixel.r));
    CAPTURE(static_cast<int>(referencePixel.r));
    CHECK(static_cast<int>(contactPixel.r) < static_cast<int>(referencePixel.r) / 2);

    CHECK_FALSE(fixture->context.hasValidationErrors());
    destroySceneRig(*fixture, *rig);
}

TEST_CASE("Real lit-path shadow: PCF softness probe -- the shadow edge's brightness gradient through StandardPBR "
          "spans at least 2 shadow-map texels") {
    auto fixture = makeFixture("rx_material_shadow_pcf_softness_probe");
    if (!fixture.has_value()) {
        return;
    }
    auto rig = buildSceneRig(*fixture);
    REQUIRE(rig.has_value());
    auto biased = renderWithBias(*fixture, *rig, 4.0F, 3.0F);
    REQUIRE(biased.has_value());
    REQUIRE(biased->worldTexelSize > 0.0F);

    const glm::mat4 cameraViewProj = topDownReversedZCameraViewProj();
    constexpr float kScanStartX = 5.5F;
    constexpr float kScanEndX = 7.0F;
    constexpr int kScanSteps = 60;
    constexpr float kScanZ = 3.6F;

    std::vector<int> brightness(kScanSteps, -1);
    for (int i = 0; i < kScanSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kScanSteps - 1);
        const float x = kScanStartX + t * (kScanEndX - kScanStartX);
        const PixelCoord p = worldToPixel(cameraViewProj, glm::vec3(x, 0.0F, kScanZ), kExtent);
        if (p.x < 0 || p.x >= static_cast<int>(kExtent) || p.y < 0 || p.y >= static_cast<int>(kExtent)) {
            continue;
        }
        brightness[i] = pixelAt(biased->pixels, kExtent, p.x, p.y).r;
    }

    int minB = 256;
    int maxB = -1;
    for (int v : brightness) {
        if (v < 0) {
            continue;
        }
        minB = std::min(minB, v);
        maxB = std::max(maxB, v);
    }
    REQUIRE(maxB > minB);
    const int lowThreshold = minB + (maxB - minB) / 4;
    const int highThreshold = maxB - (maxB - minB) / 4;

    int firstLow = -1;
    int firstHigh = -1;
    for (int i = 0; i < kScanSteps; ++i) {
        if (brightness[i] < 0) {
            continue;
        }
        if (firstLow == -1 && brightness[i] <= lowThreshold) {
            firstLow = i;
        }
        if (firstHigh == -1 && brightness[i] >= highThreshold) {
            firstHigh = i;
        }
    }
    REQUIRE(firstLow >= 0);
    REQUIRE(firstHigh >= 0);

    const float worldPerStep = (kScanEndX - kScanStartX) / static_cast<float>(kScanSteps - 1);
    const float transitionWorldSpan = std::abs(static_cast<float>(firstHigh - firstLow)) * worldPerStep;
    const float transitionTexels = transitionWorldSpan / biased->worldTexelSize;

    CAPTURE(transitionWorldSpan);
    CAPTURE(biased->worldTexelSize);
    CAPTURE(transitionTexels);
    CHECK(transitionTexels >= 2.0F);

    CHECK_FALSE(fixture->context.hasValidationErrors());
    destroySceneRig(*fixture, *rig);
}
