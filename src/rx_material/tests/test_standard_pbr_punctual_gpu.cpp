// src/rx_material/tests/test_standard_pbr_punctual_gpu.cpp -- Phase 5
// Stage 2 Task 13 [#49, gate matrix-p5t13-physical-lights.md]: value-
// asserted GPU tests for standard_pbr.slang's generalized punctual-light
// term (Directional/Point/Spot, previously directional-only) -- the
// ticket's own named acceptance criteria: "Single-light analytic falloff
// probe: rendered intensity at distance d matches inverse-square
// expectation within tolerance" plus the gate matrix's own per-row
// discrimination requirements (range-window conditional-on-presence,
// spot cone squared-saturate curve, directional regression).
//
// TEST RIG -- a TRIMMED duplicate of test_standard_pbr_ibl_gpu.cpp's own
// rig (Fixture/QuadMesh/DrawDataBuffer/renderQuad), scoped to exactly
// what punctual-light falloff probing needs (one quad, one draw,
// StandardPBR only, NO environment) -- matching this test suite's own
// established per-file-duplicated-fixture idiom (see that file's own
// header comment for why).
//
// GEOMETRY: the SAME unit quad every rig in this suite uses (object-space
// (-0.5,-0.5,0)..(0.5,0.5,0), normal (0,0,1)), camera fixed at (0,0,2)
// looking straight down -Z. Every Point/Spot TEST_CASE below places its
// light ALONG THE SAME +Z AXIS the quad's own normal points along
// (`lightPositionWorld = (0,0,d)`), so `NdotL == 1.0` identically
// regardless of `d` -- this ISOLATES the pure distance-attenuation term
// from the angle-dependent BRDF terms, which is what makes a RATIO
// assertion (`pixel(d1)/pixel(d2) == attenuation(d1)/attenuation(d2)`)
// exact rather than merely plausible: with `viewDir == lightDir ==
// halfVec == N == (0,0,1)` for every distance, `(diffuse+specular)` is a
// FIXED distance-independent constant, so it cancels exactly in the
// ratio -- the SAME "matched-pose" methodology test_standard_pbr_ibl_gpu.
// cpp's own mirror-metal TEST_CASE already established for this rig
// shape, and the SAME ratio-not-absolute-value methodology Task 4's own
// pre-exposure TEST_CASE (test_standard_pbr_unlit.cpp) established for
// exposure. The spot cone sweep instead keeps the light's own POSITION
// fixed (so distance attenuation stays constant) and tilts the light's
// own FACING direction (`lightSpotDirWorld`) by a known angle `theta`
// around the X axis -- `dot((0,0,-1), (0,sin(theta),-cos(theta))) ==
// cos(theta)` exactly, isolating the cone term the identical way.
#include <doctest/doctest.h>
#include <rx_material/draw_data.h>
#include <rx_material/material_system.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_scene/camera.h>
#include <rx_scene/light_math.h>

#include "ibl_environment_test_fixture.h"

#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_task/scheduler.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
        MESSAGE("no display backend available, skipping StandardPBR punctual-light test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping StandardPBR punctual-light test");
        return std::nullopt;
    }
    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());
    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);
    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    rx::rhi::BindlessTable::Capacities capacities;
    capacities.sampledImages = 16;
    capacities.samplers = 8;
    capacities.storageBuffers = 8;
    // [Phase 4 Stage 2 Task 22 / Phase 5 Task 10 precedent] material.slang
    // unconditionally declares `gShadowCompareSamplers`(binding 3)/
    // `gTexturesCube`(binding 4) now -- EVERY BindlessTable feeding a
    // MaterialSystem needs these two capacities nonzero or pipeline
    // creation fails validation (`VUID-VkGraphicsPipelineCreateInfo-
    // layout-00756`, "Shader uses descriptor slot 0.3/0.4... but not
    // declared in pipeline layout") -- reproduced directly during this
    // file's own development (this rig never registers a real shadow map
    // or cube image; both capacities exist purely so the pipeline LAYOUT
    // itself is well-formed).
    capacities.comparisonSamplers = 1;
    // [Review fix round 1, LOW finding 3] Bumped 1 -> 4: the new
    // env-vs-punctual coherence TEST_CASE registers a REAL synthetic
    // environment (`ibl_environment_test_fixture.h`'s own
    // `makeUniformTestEnvironment()`, 2 real cube registrations --
    // irradiance + prefiltered) on top of this rig's own pre-existing
    // headroom requirement.
    capacities.cubeImages = 4;
    // [Phase 5 Stage 2 Task 15, #51] Same requirement as `cubeImages` above:
    // standard_pbr.slang now unconditionally declares `gClusterBuffers`/
    // `gClusterLights` at bindings 5/6 via cluster_lighting.slang. This
    // file's own cluster-shading TEST_CASEs (below) register REAL buffers
    // into both -- generous headroom for several independent per-scenario
    // registrations across one fixture's lifetime.
    capacities.genericStorageBuffers = 8;
    capacities.clusterLightBuffers = 4;
    auto bindless = rx::rhi::BindlessTable::create(device->physicalDevice(), device->device(), capacities);
    REQUIRE(bindless.has_value());

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());

    return Fixture{std::move(*window),  std::move(*context),   surface,
                    std::move(*device), std::move(*bindless), std::move(*allocator)};
}

std::filesystem::path freshCachePath(const char* name) {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / (std::string("rx_standard_pbr_punctual_test_") + name + ".cache");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

struct Vertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv[2];
};

constexpr std::array<Vertex, 4> kQuadVertices{{
    {{-0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}},
    {{0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}},
    {{0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},
    {{-0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},
}};
constexpr std::array<uint32_t, 6> kQuadIndices{0, 1, 2, 0, 2, 3};

struct QuadMesh {
    rx::rhi::Buffer vertexBuffer;
    rx::rhi::Buffer indexBuffer;
};

std::optional<QuadMesh> createQuadMesh(rx::rhi::Allocator& allocator) {
    auto vb = allocator.createHostVisibleBuffer(sizeof(kQuadVertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                 rx::rhi::MemoryCategory::Internal);
    auto ib = allocator.createHostVisibleBuffer(sizeof(kQuadIndices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                                 rx::rhi::MemoryCategory::Internal);
    if (!vb.has_value() || !ib.has_value()) {
        return std::nullopt;
    }
    std::memcpy(vb->mappedData(), kQuadVertices.data(), sizeof(kQuadVertices));
    vb->flush();
    std::memcpy(ib->mappedData(), kQuadIndices.data(), sizeof(kQuadIndices));
    ib->flush();
    return QuadMesh{std::move(*vb), std::move(*ib)};
}

// Base row: camera at (0,0,2) looking at the origin, orthographic
// projection matched to the quad's own extent, EVERY light field zeroed
// (a TEST_CASE populates exactly the punctual fields it probes).
// `modelTranslation` optionally moves the QUAD (not the camera/light) --
// used by the directional regression TEST_CASE only.
rx::material::DrawDataGpu makeBaseRow(glm::vec3 modelTranslation = glm::vec3(0.0F)) {
    rx::material::DrawDataGpu row;
    glm::mat4 model = glm::translate(glm::mat4(1.0F), modelTranslation);
    glm::mat4 view = glm::lookAt(glm::vec3(0.0F, 0.0F, 2.0F), glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    glm::mat4 proj = glm::orthoZO(-0.5F, 0.5F, -0.5F, 0.5F, 0.1F, 10.0F);
    proj[1][1] *= -1.0F;  // Vulkan Y-flip -- see test_standard_pbr_unlit.cpp's own makeHeadOnRow() for the full account.
    for (int col = 0; col < 4; ++col) {  // reversed-Z affine remap z' = 1-z.
        const float row3 = (col == 3) ? 1.0F : 0.0F;
        proj[col][2] = row3 - proj[col][2];
    }
    glm::mat4 viewProj = proj * view;
    glm::mat3 normalMat3 = glm::transpose(glm::inverse(glm::mat3(model)));

    row.model = glm::transpose(model);
    row.normalMatrix = glm::transpose(glm::mat4(normalMat3));
    row.viewProj = glm::transpose(viewProj);
    row.lightColor = glm::vec4(0.0F);  // OFF by default -- each TEST_CASE turns on exactly what it probes.
    row.ambientColor = glm::vec4(0.0F);
    row.cameraPosWorld = glm::vec4(0.0F, 0.0F, 2.0F, 0.0F);
    row.materialIndex = 0;
    return row;
}

struct DrawDataBuffer {
    rx::rhi::Buffer buffer;
    rx::rhi::BindlessHandle handle;
};

std::optional<DrawDataBuffer> createDrawDataBuffer(rx::rhi::Allocator& allocator, rx::rhi::BindlessTable& bindless,
                                                     const rx::material::DrawDataGpu& row) {
    auto buf = allocator.createHostVisibleBuffer(sizeof(row), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                  rx::rhi::MemoryCategory::Internal);
    if (!buf.has_value()) {
        return std::nullopt;
    }
    std::memcpy(buf->mappedData(), &row, sizeof(row));
    buf->flush();
    rx::rhi::BindlessHandle handle = bindless.registerStorageBuffer(buf->handle(), sizeof(row));
    if (!handle.isValid()) {
        return std::nullopt;
    }
    return DrawDataBuffer{std::move(*buf), handle};
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

struct Rgba8 {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};

uint32_t makeWhiteTexture(rx::material::MaterialSystem& system) {
    rx::material::TextureCreateInfo texInfo;
    texInfo.width = 1;
    texInfo.height = 1;
    texInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    std::array<uint8_t, 4> whitePixel{255, 255, 255, 255};
    texInfo.pixels = whitePixel.data();
    texInfo.pixelBytes = whitePixel.size();
    rx::material::TextureHandle tex = system.createTexture2D(texInfo);
    REQUIRE(tex.isValid());
    return system.textureBindlessIndex(tex);
}

uint32_t makeFlatNormalTexture(rx::material::MaterialSystem& system) {
    rx::material::TextureCreateInfo texInfo;
    texInfo.width = 1;
    texInfo.height = 1;
    texInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    std::array<uint8_t, 4> flatPixel{128, 128, 255, 255};
    texInfo.pixels = flatPixel.data();
    texInfo.pixelBytes = flatPixel.size();
    rx::material::TextureHandle tex = system.createTexture2D(texInfo);
    REQUIRE(tex.isValid());
    return system.textureBindlessIndex(tex);
}

// Neutral StandardPbrParams blob -- white diffuse, non-metal, fully
// rough (isolates a clean, distance/angle-independent-once-normalized
// BRDF constant -- see this file's own header comment).
std::vector<uint8_t> makePunctualBlob(rx::material::MaterialSystem& system, rx::material::MaterialHandle handle,
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
    setParam(blob, params, "baseColorUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});
    setParam(blob, params, "metallicRoughnessUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});
    setParam(blob, params, "normalUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});
    setParam(blob, params, "occlusionUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});
    setParam(blob, params, "emissiveUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});
    return blob;
}

// One tiny 8x8 offscreen render -- ONE quad draw against a real
// StandardPBR pipeline, reversed-Z depth. Returns the CENTER texel.
// Mirrors test_standard_pbr_ibl_gpu.cpp's own renderQuad() shape verbatim
// (this file's own established per-file rig-duplication convention).
Rgba8 renderQuad(rx::rhi::Device& device, rx::rhi::Allocator& allocator, rx::rhi::BindlessTable& bindless,
                  rx::rhi::BindlessHandle drawDataBufferHandle, uint32_t defaultSamplerIndex, const QuadMesh& mesh,
                  rx::material::MaterialSystem& system, rx::material::MaterialHandle handle,
                  const std::vector<uint8_t>& paramBlob) {
    constexpr VkExtent2D kExtent{8, 8};
    constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto executor = rx::graph::Executor::create(device, *scheduler);
    REQUIRE(executor != nullptr);

    rx::graph::RenderGraph graph;
    rx::graph::Pass& pass = graph.addPass("standard_pbr_punctual_test");
    rx::graph::AttachmentDesc colorDesc;
    colorDesc.format = kColorFormat;
    pass.addColorOutput("color", colorDesc);
    rx::graph::AttachmentDesc depthDesc;
    depthDesc.format = kDepthFormat;
    depthDesc.depthConvention = rx::graph::DepthConvention::Reversed;
    pass.setDepthStencilOutput("depth", depthDesc);
    graph.setBackbufferSource("color");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorPool(device.device(), &poolInfo, nullptr, &descriptorPool) == VK_SUCCESS);

    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &uboBinding;
    VkDescriptorSetLayout paramSetLayout = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorSetLayout(device.device(), &setLayoutInfo, nullptr, &paramSetLayout) == VK_SUCCESS);

    rx::graph::PassSignature sig;
    sig.colorCount = 1;
    sig.colorFormats[0] = kColorFormat;
    sig.depthFormat = kDepthFormat;
    sig.samples = VK_SAMPLE_COUNT_1_BIT;
    VkPipeline pipeline = system.getPipeline({handle, sig, 0});
    REQUIRE(pipeline != VK_NULL_HANDLE);
    VkPipelineLayout layout = system.pipelineLayout(handle);
    uint32_t pushSize = system.layoutInfo(handle).pushRanges[0].size;

    std::optional<rx::rhi::Buffer> paramBuffer;
    VkDescriptorSet paramSet = VK_NULL_HANDLE;

    pass.setExecute([&](rx::graph::PassContext& ctx) {
        VkCommandBuffer cmd = ctx.cmd;
        VkViewport viewport{0.0F, 0.0F, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height), 0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, kExtent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkDescriptorSet bindlessSet = bindless.descriptorSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &bindlessSet, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        rx::material::MaterialGlobalsPush push;
        push.defaultSamplerIndex = defaultSamplerIndex;
        push.drawDataBufferIndex = drawDataBufferHandle.index();
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, &push);

        auto buf = allocator.createHostVisibleBuffer(paramBlob.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                       rx::rhi::MemoryCategory::Internal);
        REQUIRE(buf.has_value());
        std::memcpy(buf->mappedData(), paramBlob.data(), paramBlob.size());
        buf->flush();

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &paramSetLayout;
        REQUIRE(vkAllocateDescriptorSets(device.device(), &allocInfo, &paramSet) == VK_SUCCESS);

        VkDescriptorBufferInfo bufferInfo{buf->handle(), 0, paramBlob.size()};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = paramSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device.device(), 1, &write, 0, nullptr);
        paramBuffer = std::move(buf);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &paramSet, 0, nullptr);

        VkDeviceSize zeroOffset = 0;
        VkBuffer vbHandle = mesh.vertexBuffer.handle();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbHandle, &zeroOffset);
        vkCmdBindIndexBuffer(cmd, mesh.indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, static_cast<uint32_t>(kQuadIndices.size()), 1, 0, 0, /*firstInstance=*/0);
    });

    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = kExtent.width;
    compileInfo.swapchainHeight = kExtent.height;
    compileInfo.swapchainFormat = kColorFormat;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);
    executor->realize(graph);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kColorFormat;
    imageInfo.extent = {kExtent.width, kExtent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    REQUIRE(vkCreateImage(device.device(), &imageInfo, nullptr, &image) == VK_SUCCESS);

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device.device(), image, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(device.physicalDevice(), &memProps);
    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    REQUIRE(memoryTypeIndex != UINT32_MAX);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(device.device(), &allocInfo, nullptr, &imageMemory) == VK_SUCCESS);
    REQUIRE(vkBindImageMemory(device.device(), image, imageMemory, 0) == VK_SUCCESS);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kColorFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    REQUIRE(vkCreateImageView(device.device(), &viewInfo, nullptr, &view) == VK_SUCCESS);

    VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(kExtent.width) * kExtent.height * 4;
    auto readback =
        allocator.createHostVisibleBuffer(pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rx::rhi::MemoryCategory::Internal);
    REQUIRE(readback.has_value());

    {
        auto cmdCtx = rx::rhi::CommandContext::create(device.device(), device.graphicsQueue(), device.graphicsQueueFamily());
        REQUIRE(cmdCtx.has_value());
        cmdCtx->runOnce([&](VkCommandBuffer cmd) { executor->execute(graph, cmd, image, view, kExtent); });
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {kExtent.width, kExtent.height, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
        });
    }
    readback->invalidate();
    const auto* rawPixels = static_cast<const uint8_t*>(readback->mappedData());
    const size_t centerIndex = (static_cast<size_t>(kExtent.height / 2) * kExtent.width + kExtent.width / 2) * 4;
    Rgba8 result{rawPixels[centerIndex], rawPixels[centerIndex + 1], rawPixels[centerIndex + 2], rawPixels[centerIndex + 3]};

    vkDestroyImageView(device.device(), view, nullptr);
    vkDestroyImage(device.device(), image, nullptr);
    vkFreeMemory(device.device(), imageMemory, nullptr);
    vkDestroyDescriptorSetLayout(device.device(), paramSetLayout, nullptr);
    vkDestroyDescriptorPool(device.device(), descriptorPool, nullptr);
    return result;
}

// End-to-end rig bundle -- built once per TEST_CASE, reused across every
// render() call inside it (multiple distances/angles per probe).
struct Rig {
    std::unique_ptr<Fixture> fixture;
    std::optional<QuadMesh> mesh;
    std::unique_ptr<rx::material::MaterialSystem> system;
    rx::material::MaterialHandle material;
    uint32_t whiteTex = 0;
    uint32_t flatNormalTex = 0;
    uint32_t samplerIndex = 0;
    VkSampler rawSampler = VK_NULL_HANDLE;
    std::vector<uint8_t> blob;

    Rgba8 render(const rx::material::DrawDataGpu& row) {
        auto drawDataBuffer = createDrawDataBuffer(fixture->allocator, fixture->bindless, row);
        REQUIRE(drawDataBuffer.has_value());
        return renderQuad(fixture->device, fixture->allocator, fixture->bindless, drawDataBuffer->handle,
                           samplerIndex, *mesh, *system, material, blob);
    }
};

// Explicit teardown for the ONE raw Vulkan object this rig creates
// outside any RAII wrapper (the sampler) -- called at the END of every
// TEST_CASE using a rig, before its own device/fixture teardown, exactly
// matching test_standard_pbr_unlit.cpp's own `destroyRig()` precedent (a
// leaked sampler otherwise fires `VUID-vkDestroyDevice-device-00378` at
// implicit `Fixture` teardown -- reproduced directly during this file's
// own development before this function existed).
void destroyRig(Rig& rig) { vkDestroySampler(rig.fixture->device.device(), rig.rawSampler, nullptr); }

std::unique_ptr<Rig> makeRig(const char* name) {
    auto fixtureOpt = makeFixture(name);
    if (!fixtureOpt.has_value()) {
        return nullptr;
    }
    auto fixture = std::make_unique<Fixture>(std::move(*fixtureOpt));
    auto mesh = createQuadMesh(fixture->allocator);
    REQUIRE(mesh.has_value());
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath(name));
    REQUIRE(system != nullptr);
    rx::material::MaterialHandle material =
        system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});

    auto rig = std::make_unique<Rig>();
    rig->whiteTex = makeWhiteTexture(*system);
    rig->flatNormalTex = makeFlatNormalTexture(*system);
    rig->samplerIndex = 0;
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    REQUIRE(vkCreateSampler(fixture->device.device(), &samplerInfo, nullptr, &sampler) == VK_SUCCESS);
    rx::rhi::BindlessHandle samplerHandle = fixture->bindless.registerSampler(sampler);
    REQUIRE(samplerHandle.isValid());
    rig->samplerIndex = samplerHandle.index();
    rig->rawSampler = sampler;
    rig->blob = makePunctualBlob(*system, material, rig->whiteTex, rig->flatNormalTex, rig->samplerIndex);
    rig->material = material;
    rig->system = std::move(system);
    rig->mesh.emplace(std::move(*mesh));
    rig->fixture = std::move(fixture);
    return rig;
}

// Builds a Point-light row -- light on the +Z axis at world distance `d`
// from the origin (SAME axis as the quad's own normal -- see this file's
// own header comment for why this isolates the attenuation term). `range
// <= 0` == infinite range (no windowing).
rx::material::DrawDataGpu makePointRow(float d, float range, glm::vec3 colorCandela) {
    rx::material::DrawDataGpu row = makeBaseRow();
    row.lightType = 1;  // rx::scene::LightType::Point
    row.lightColor = glm::vec4(colorCandela, 0.0F);
    row.lightPositionWorld = glm::vec4(0.0F, 0.0F, d, 0.0F);
    row.lightRange = range;
    return row;
}

bool near(float actual, float expected, float relTolerance) {
    return std::abs(actual - expected) <= relTolerance * std::max(std::abs(expected), 1.0F);
}

}  // namespace

TEST_CASE("StandardPBR punctual: Directional-light regression -- measured intensity is IDENTICAL at two "
          "world positions at different distances from the scene origin [gate matrix 'Directional light: "
          "literally unattenuated' row: 'a directional light's measured intensity is IDENTICAL at two "
          "probe points at different distances'] -- proves the generalized punctual-light term did NOT "
          "accidentally introduce a distance term into the Directional (lightType==0) path.") {
    auto rig = makeRig("directional_regression");
    if (!rig) {
        return;
    }

    rx::material::DrawDataGpu nearRow = makeBaseRow(glm::vec3(0.0F, 0.0F, 0.0F));
    nearRow.lightType = 0;  // Directional (also the default).
    nearRow.lightDirWorld = glm::vec4(0.0F, 0.0F, 1.0F, 0.0F);
    nearRow.lightColor = glm::vec4(2.0F, 2.0F, 2.0F, 0.0F);

    rx::material::DrawDataGpu farRow = nearRow;
    // Move the QUAD (model translation) to a different world position --
    // the light stays a constant per-pass direction/color either way
    // (that IS the "not attenuated" claim under test); only the SHADED
    // WORLD POSITION changes. Translated along the camera's own view
    // axis (-Z, staying within the rig's [0.1,10] near/far range and its
    // [-0.5,0.5] ortho X/Y extent so the quad remains visible/centered --
    // an EARLIER version of this TEST_CASE translated the quad far enough
    // to fall outside the view frustum entirely, which silently read back
    // the graph's own black clear color instead of a real shaded pixel;
    // caught empirically by this TEST_CASE's own `nearPixel.r > 0`
    // non-degenerate assertion below failing against a black `farPixel`).
    farRow.model = glm::transpose(glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -3.0F)));

    Rgba8 nearPixel = rig->render(nearRow);
    Rgba8 farPixel = rig->render(farRow);
    CHECK(nearPixel.r == farPixel.r);
    CHECK(nearPixel.g == farPixel.g);
    CHECK(nearPixel.b == farPixel.b);
    // Non-degenerate: both pixels are actually lit (not both coincidentally black).
    CHECK(nearPixel.r > 0);
    destroyRig(*rig);
}

TEST_CASE("StandardPBR punctual: Point light near-field inverse-square falloff -- rendered intensity at "
          "distance d matches the inverse-square expectation within tolerance [ticket #49's own named "
          "acceptance criterion: 'Single-light analytic falloff probe']. d1=1, d2=2, infinite range (both "
          "well within the near-field where the range-window term is moot) -- expected ratio "
          "pixel(d1)/pixel(d2) == (d2/d1)^2 == 4.0 exactly (the fixed BRDF constant this rig's own "
          "matched-pose geometry produces cancels in the ratio -- see this file's own header comment).") {
    auto rig = makeRig("point_inverse_square");
    if (!rig) {
        return;
    }
    Rgba8 near1 = rig->render(makePointRow(1.0F, 0.0F, glm::vec3(2.0F)));
    Rgba8 near2 = rig->render(makePointRow(2.0F, 0.0F, glm::vec3(2.0F)));
    REQUIRE(near2.r > 0);  // non-degenerate (would silently pass a 0/0 ratio check otherwise).
    float measuredRatio = static_cast<float>(near1.r) / static_cast<float>(near2.r);
    MESSAGE("point inverse-square: pixel(d=1)=", static_cast<int>(near1.r), " pixel(d=2)=", static_cast<int>(near2.r),
            " measured ratio=", measuredRatio, " expected=4.0");
    CHECK(near(measuredRatio, 4.0F, 0.15F));  // 8-bit quantization tolerance, matching this suite's own ratio-test idiom.
    destroyRig(*rig);
}

TEST_CASE("StandardPBR punctual: Point light range-window DISCRIMINATION -- a ranged light's falloff near "
          "its own cutoff diverges sharply from a pure inverse-square (unranged) light at the SAME two "
          "distances, proving range-windowing is actually CONDITIONAL on range's presence, not always-on "
          "or always-off [gate matrix 'Range absent = infinite range, pure inverse-square law' row's own "
          "named acceptance criterion]. range=10, d1=5 (window still close to 1.0), d2=9 (close to the "
          "cutoff, window~0.34 for the RANGED light, still 1.0 for the UNRANGED light at the same "
          "distance) -- hand-computed (python3, double precision): rangeAttenuation(5,10)=0.0375, "
          "rangeAttenuation(9,10)=0.0042457, ratio=8.8325; the UNRANGED equivalent ratio at the same two "
          "distances is exactly (9/5)^2=3.24 -- a ~2.7x difference. (d1/d2 deliberately NOT 1 and 9: an "
          "earlier version of this TEST_CASE used d1=1, whose near-field attenuation is close enough to "
          "1.0 that the resulting >80x dynamic range clipped the near pixel to 255 at any color intensity "
          "large enough to keep the far/windowed pixel above 8-bit noise -- caught empirically via this "
          "TEST_CASE's own logged raw pixel values, not asserted; d1=5 keeps both endpoints in a clean, "
          "non-clipping, non-zero 8-bit range.)") {
    auto rig = makeRig("point_range_window");
    if (!rig) {
        return;
    }
    const glm::vec3 color(20.0F);
    Rgba8 rangedNear = rig->render(makePointRow(5.0F, 10.0F, color));
    Rgba8 rangedFar = rig->render(makePointRow(9.0F, 10.0F, color));
    Rgba8 unrangedNear = rig->render(makePointRow(5.0F, 0.0F, color));
    Rgba8 unrangedFar = rig->render(makePointRow(9.0F, 0.0F, color));

    MESSAGE("range-window discrimination raw pixels: rangedNear=", static_cast<int>(rangedNear.r), " rangedFar=",
            static_cast<int>(rangedFar.r), " unrangedNear=", static_cast<int>(unrangedNear.r), " unrangedFar=",
            static_cast<int>(unrangedFar.r));
    REQUIRE(rangedFar.r > 0);
    REQUIRE(unrangedFar.r > 0);
    REQUIRE(rangedNear.r < 250);   // headroom check -- not saturated against the 8-bit ceiling.
    REQUIRE(unrangedNear.r < 250);
    float rangedRatio = static_cast<float>(rangedNear.r) / static_cast<float>(rangedFar.r);
    float unrangedRatio = static_cast<float>(unrangedNear.r) / static_cast<float>(unrangedFar.r);
    MESSAGE("range-window discrimination: ranged ratio=", rangedRatio, " (expect ~8.83), unranged ratio=",
            unrangedRatio, " (expect ~3.24)");

    CHECK(near(unrangedRatio, 3.24F, 0.15F));
    CHECK(near(rangedRatio, 8.8325F, 0.15F));
    // The RANGED ratio must be markedly larger than the unranged one --
    // the windowing term's own extra darkening near the cutoff. This is
    // the actual discrimination: a shader that silently ignored
    // `lightRange` entirely would produce rangedRatio == unrangedRatio.
    CHECK(rangedRatio > unrangedRatio * 2.0F);
    destroyRig(*rig);
}

TEST_CASE("StandardPBR punctual: Spot cone angular attenuation -- dead-center (full brightness), inside "
          "the inner/outer window (partial, matching the squared-saturate closed form), and outside the "
          "outer cone (fully dark) [gate matrix 'Spot cone attenuation (inner/outer angle)' row: 'GPU "
          "falloff probe asserts a spot light's measured intensity at a sampled point inside/at/outside "
          "the outer cone matches the saturate(...)^2 curve's predicted value']. Light POSITION is fixed "
          "at (0,0,4) (constant distance attenuation across all three renders); only the light's own "
          "FACING direction (`lightSpotDirWorld`) is tilted by `theta` around the X axis -- see this "
          "file's own header comment for the `dot((0,0,-1),(0,sin(theta),-cos(theta)))==cos(theta)` "
          "derivation.") {
    auto rig = makeRig("spot_cone");
    if (!rig) {
        return;
    }
    const float innerConeAngle = 0.2F;
    const float outerConeAngle = 0.6F;
    const rx::scene::lightmath::SpotAngleScaleOffset scaleOffset =
        rx::scene::lightmath::spotAngleScaleOffset(innerConeAngle, outerConeAngle);

    auto makeSpotRow = [&](float theta) {
        rx::material::DrawDataGpu row = makePointRow(4.0F, 0.0F, glm::vec3(4.0F));
        row.lightType = 2;  // rx::scene::LightType::Spot
        row.lightAngleScale = scaleOffset.scale;
        row.lightAngleOffset = scaleOffset.offset;
        row.lightSpotDirWorld = glm::vec4(0.0F, std::sin(theta), -std::cos(theta), 0.0F);
        return row;
    };

    Rgba8 center = rig->render(makeSpotRow(0.0F));    // dead center: cd=1, full brightness.
    Rgba8 partial = rig->render(makeSpotRow(0.4F));   // strictly inside (inner=0.2, outer=0.6): partial.
    Rgba8 outside = rig->render(makeSpotRow(0.8F));   // beyond outerConeAngle: fully dark.

    MESSAGE("spot cone: center=", static_cast<int>(center.r), " partial=", static_cast<int>(partial.r),
            " outside=", static_cast<int>(outside.r));

    REQUIRE(center.r > 0);
    // DISCRIMINATION: fully outside the cone must read EXACT black, not
    // merely dim -- a broken/missing cone term would still light this
    // pixel (same brightness as `center`, since distance/NdotL are held
    // constant across all three renders).
    CHECK(outside.r == 0);
    CHECK(outside.g == 0);
    CHECK(outside.b == 0);
    // Partial must sit STRICTLY between outside (0) and center (full) --
    // proving the squared-saturate curve is a real, continuous falloff,
    // not a binary in/out step function.
    CHECK(partial.r > 0);
    CHECK(partial.r < center.r);
    // Matches the closed-form squared-saturate curve's own predicted
    // ratio (angleAttenuation(theta=0.4) == 0.38274, hand-computed
    // independently, python3): partial/center should track that ratio
    // (both share the identical distance/NdotL/BRDF-constant factors,
    // which cancel exactly, same methodology as the point-light ratio
    // tests above).
    float measuredRatio = static_cast<float>(partial.r) / static_cast<float>(center.r);
    CHECK(near(measuredRatio, 0.38274F, 0.2F));
    destroyRig(*rig);
}

TEST_CASE("StandardPBR punctual: pre-exposure coherence -- a Point light's rendered intensity scales by "
          "EXACTLY rx::scene::Camera::exposure()'s own ratio when the camera's exposure setting changes "
          "[Task 13's own 'reconciliation' requirement: punctual lights, the environment (already proven "
          "by Task 4/Task 10's own tests), and directional lights all go through the SAME single "
          "pre-exposure multiplication point -- this TEST_CASE is the Point-light half of that proof, "
          "mirroring test_standard_pbr_unlit.cpp's own directional-light pre-exposure TEST_CASE exactly]. "
          "The RATIO assertion (not an absolute value) is the same double-application-revert "
          "discriminator Task 4's own report names: a stray leftover second exposure multiply would "
          "measure the SQUARE of the expected ratio, not the ratio itself.") {
    auto rig = makeRig("point_pre_exposure");
    if (!rig) {
        return;
    }
    rx::scene::Camera neutralCamera;  // exposure() == 1.0 exactly (Task 4's own engaged-neutral default).
    rx::scene::Camera brighterCamera;
    brighterCamera.setExposure(-1.0F);  // exposure() == 5/3 exactly [task-4-report.md's own cited value].

    const glm::vec3 baseColorCandela(3.0F);
    auto makeExposedRow = [&](float exposure) {
        return makePointRow(2.0F, 0.0F, baseColorCandela * exposure);
    };

    Rgba8 neutralPixel = rig->render(makeExposedRow(neutralCamera.exposure()));
    Rgba8 brighterPixel = rig->render(makeExposedRow(brighterCamera.exposure()));
    REQUIRE(neutralPixel.r > 0);
    float measuredRatio = static_cast<float>(brighterPixel.r) / static_cast<float>(neutralPixel.r);
    float expectedRatio = brighterCamera.exposure() / neutralCamera.exposure();
    MESSAGE("point pre-exposure: neutral=", static_cast<int>(neutralPixel.r), " brighter=",
            static_cast<int>(brighterPixel.r), " measured ratio=", measuredRatio, " expected=", expectedRatio);
    CHECK(near(measuredRatio, expectedRatio, 0.1F));
    // Explicit double-application discriminator: the SQUARE of the
    // expected ratio would be a materially different (larger) number --
    // if this failed here (measured close to expectedRatio^2 instead),
    // that would mean exposure is being applied twice somewhere.
    CHECK_FALSE(near(measuredRatio, expectedRatio * expectedRatio, 0.1F));
    destroyRig(*rig);
}

TEST_CASE("StandardPBR: environment-lux and punctual-light-lux compose in ONE coherent photometric frame "
          "[review fix round 1, LOW finding 3 -- Adjudication 1's own dimensional re-derivation "
          "(task-13-review.md), turned into a value assertion]. A uniform environment of radiance L "
          "(bake.h's own pre-divided-by-pi irradiance convention, isolated via dfg=(0,0) so "
          "iblDiffuse==diffuseColor*L exactly and iblSpecular==0) is asserted to produce the SAME rendered "
          "pixel as a directional light of colorLux C=L*envIntensity*PI -- the standard photometric "
          "identity a uniform-radiance-L hemisphere integrates to under Lambert's cosine law (irradiance == "
          "PI*L), the SAME identity test_standard_pbr_ibl_gpu.cpp's own Lambertian TEST_CASE already "
          "documents. The directional side is isolated to pure Lambertian diffuse via ior=1.0 (forces "
          "dielectric F0=(0,0,0) exactly, computeDielectricF0F90()); combined with this rig's own head-on "
          "geometry (VdotH==1 exactly), F_Schlick(f0=0,f90,VoH=1)==f0==0 identically (brdf.slang: "
          "`f0+(f90-f0)*pow(clamp(1-VoH,0,1),5)`, and `clamp(1-1,0,1)==0` so the whole p5 term vanishes) -- "
          "the direct specular lobe is EXACTLY zero, not merely negligible, leaving directLight == "
          "diffuseColor*Fd_Lambert()*C*NdotL == C/pi == L*envIntensity exactly. Verified at BOTH neutral "
          "(exposure==1.0) and a non-neutral Camera exposure (both producers scaled by the IDENTICAL "
          "CPU-side pre-exposure multiply, matching the single-application-point convention) -- this is "
          "what makes the test discriminate a UNIT BREAK rather than merely reproducing a coincidental "
          "match: a stray extra 4*PI anywhere in the candela/lux conversion path would make the two renders "
          "diverge by roughly 12.6x, trivially visible at 8-bit precision (empirically reproduced via a "
          "temporary sabotage-and-revert cycle, see task-13-report.md's own 'Fix round 1' section); a stray "
          "SQUARED exposure on either producer would still pass at exposure==1.0 (where x==x^2) but would "
          "visibly diverge at the non-neutral exposure used here.") {
    auto rig = makeRig("env_punctual_coherence");
    if (!rig) {
        return;
    }

    constexpr float kRadiance = 0.4F;      // uniform environment radiance L.
    constexpr float kEnvIntensity = 1.0F;  // matches EnvironmentDesc's own documented neutral default.
    constexpr float kPi = 3.14159265358979F;
    // Physical identity (Adjudication 1's own dimensional re-derivation,
    // task-13-review.md): a uniform-radiance-L hemisphere produces
    // irradiance == L*pi at a normally-incident surface -- the equivalent
    // directional-light illuminance (lux) is therefore L*envIntensity*pi.
    const float colorLux = kRadiance * kEnvIntensity * kPi;

    auto env = rx::material_test::makeUniformTestEnvironment(
        rig->fixture->device, rig->fixture->allocator, rig->fixture->bindless,
        glm::vec4(kRadiance, kRadiance, kRadiance, 1.0F),
        glm::vec2(0.0F, 0.0F));  // dfg=(0,0) -> E==0 -> iblSpecular==0, iblDiffuse==diffuseColor*L exactly.
    REQUIRE(env.has_value());

    // ior=1.0 override -- see this TEST_CASE's own header comment for why
    // this makes the direct specular lobe EXACTLY zero at this rig's
    // head-on geometry. makePunctualBlob()'s own default (1.5, the glTF
    // spec default) is overwritten via a second setParam() call, matching
    // this suite's own "build the neutral blob, override exactly the
    // field(s) this TEST_CASE probes" idiom.
    std::vector<uint8_t> blob =
        makePunctualBlob(*rig->system, rig->material, rig->whiteTex, rig->flatNormalTex, rig->samplerIndex);
    {
        const auto params = rig->system->materialParams(rig->material);
        setParam(blob, params, "ior", 1.0F);
    }

    auto renderPair = [&](float exposure) {
        rx::material::DrawDataGpu envRow = makeBaseRow();
        env->applyTo(envRow, kEnvIntensity * exposure);  // PRE-EXPOSED -- SAME single CPU-side multiply convention.
        auto envBuf = createDrawDataBuffer(rig->fixture->allocator, rig->fixture->bindless, envRow);
        REQUIRE(envBuf.has_value());
        Rgba8 envPixel = renderQuad(rig->fixture->device, rig->fixture->allocator, rig->fixture->bindless,
                                     envBuf->handle, rig->samplerIndex, *rig->mesh, *rig->system, rig->material, blob);

        rx::material::DrawDataGpu dirRow = makeBaseRow();
        dirRow.lightType = 0;  // Directional (unattenuated).
        dirRow.lightDirWorld = glm::vec4(0.0F, 0.0F, 1.0F, 0.0F);  // head-on -- matches envRow's own implicit N==(0,0,1).
        dirRow.lightColor = glm::vec4(glm::vec3(colorLux * exposure), 0.0F);  // SAME single pre-exposure multiply.
        auto dirBuf = createDrawDataBuffer(rig->fixture->allocator, rig->fixture->bindless, dirRow);
        REQUIRE(dirBuf.has_value());
        Rgba8 dirPixel = renderQuad(rig->fixture->device, rig->fixture->allocator, rig->fixture->bindless,
                                     dirBuf->handle, rig->samplerIndex, *rig->mesh, *rig->system, rig->material, blob);
        return std::pair<Rgba8, Rgba8>{envPixel, dirPixel};
    };

    // Neutral exposure (1.0).
    auto [envNeutral, dirNeutral] = renderPair(1.0F);
    MESSAGE("env-vs-punctual coherence @ exposure=1.0: env=", static_cast<int>(envNeutral.r), " directional=",
            static_cast<int>(dirNeutral.r), " (both should equal round(255*L*envIntensity)=",
            static_cast<int>(std::lround(255.0 * kRadiance * kEnvIntensity)), ")");
    REQUIRE(envNeutral.r > 0);
    REQUIRE(dirNeutral.r > 0);
    CHECK(near(static_cast<float>(dirNeutral.r), static_cast<float>(envNeutral.r), 0.06F));

    // Non-neutral exposure -- rx::scene::Camera::setExposure(-1.0F) ->
    // exposure()==5/3 exactly [task-4-report.md's own cited value, reused
    // by the pre-exposure-coherence TEST_CASE above].
    rx::scene::Camera brighterCamera;
    brighterCamera.setExposure(-1.0F);
    auto [envBright, dirBright] = renderPair(brighterCamera.exposure());
    MESSAGE("env-vs-punctual coherence @ exposure=", brighterCamera.exposure(), ": env=",
            static_cast<int>(envBright.r), " directional=", static_cast<int>(dirBright.r));
    REQUIRE(envBright.r > 0);
    REQUIRE(dirBright.r > 0);
    CHECK(near(static_cast<float>(dirBright.r), static_cast<float>(envBright.r), 0.06F));
    // Both sides must ALSO have genuinely brightened relative to neutral
    // (proves the non-neutral case is a real, live exposure change, not
    // an accidental no-op that would make the equality above vacuous).
    CHECK(envBright.r > envNeutral.r);
    CHECK(dirBright.r > dirNeutral.r);

    env->destroySamplers(rig->fixture->device.device());
    destroyRig(*rig);
}
