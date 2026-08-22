// src/rx_material/tests/test_standard_pbr_ibl_gpu.cpp -- Phase 5 Stage 1
// Task 10 [#46, gate matrix-p5t10-ibl-runtime-skybox.md, FG1 closure]:
// value-asserted GPU tests for standard_pbr.slang's new IBL diffuse/
// specular lobes and shaders/ibl/skybox.slang's own background pass --
// the matrix's own named acceptance criteria: "Lambertian... under a
// KNOWN synthetic environment reproduces the closed-form... irradiance
// value", "Mirror-metal sphere under a known environment reproduces the
// environment (matched-pose value probes)", "rough-metal energy sane via
// the furnace test on the full path", "Skybox pass gated (reference +
// provenance)".
//
// Every environment here is a SYNTHETIC test double built by
// ibl_environment_test_fixture.h (this directory) -- NOT a real
// rx::ibl::bakeEnvironment() bake (that chain's own correctness is T9's
// job, src/rx_ibl/tests) -- known, hand-picked per-face colors and a
// directly-supplied DFG-LUT value let every assertion below be a real
// closed-form arithmetic check, not a "looks plausible" comparison.
//
// TEST RIG -- read before adding a new TEST_CASE here: TEST_CASEs 1-3
// duplicate a TRIMMED version of test_standard_pbr_unlit.cpp's own quad-
// render rig (Fixture/QuadMesh/DrawDataBuffer/renderQuadPixels), scoped to
// exactly what IBL-lobe probing needs (one quad, one draw, StandardPBR
// only) -- this file's own per-TU anonymous-namespace helpers cannot be
// shared with that file's (matching this test suite's own established
// per-file-duplicated-fixture idiom, restated in ibl_environment_test_
// fixture.h's own header comment for why THAT header is the one exception,
// shared cross-file). The default rig places the camera at (0,0,2) looking
// straight down -Z at the quad's front face (object-space (-0.5,-0.5,0)..
// (0.5,0.5,0), normal (0,0,1)) -- N==V==(0,0,1) exactly, so NdotV==1 and
// the reflection vector R==(0,0,1) (world +Z) exactly, which is what makes
// the mirror-metal TEST_CASE's own "matched-pose" probe tractable: the
// probed pixel's own reflection ray always points at cube FACE INDEX 4
// (+Z, Vulkan's own standard cubemap layer-order convention -- see
// ibl_environment_test_fixture.h's own `makeTestEnvironment()` header
// comment for the same convention cited on the upload side).
//
// TEST_CASE 4 (skybox) uses a SEPARATE, standalone rig (no material system,
// no quad) -- shaders/ibl/skybox.slang compiled+reflected directly via
// rx::shader::Compiler, matching samples/08_gltf_viewer's own
// buildSkyboxPipeline() shape (duplicated here at a smaller scale, not
// imported -- this library has no dependency on that sample's own .cpp).
#include <doctest/doctest.h>
#include <rx_material/draw_data.h>
#include <rx_material/material_system.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_scene/camera.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>

#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_task/scheduler.h>

#include "ibl_environment_test_fixture.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using rx::material_test::makeTestEnvironment;
using rx::material_test::makeUniformTestEnvironment;
using rx::material_test::TestEnvironment;

namespace {

std::filesystem::path shaderDataPath(const char* filename) {
    return std::filesystem::path(RX_MATERIAL_SHADER_DIR) / filename;
}

// [Phase 5 Task 10, #46] shaders/ibl/skybox.slang is deployed alongside
// standard_pbr.slang/etc. for THIS binary's own tests via a DEDICATED
// compile definition -- see this file's own CMakeLists.txt entry.
std::filesystem::path iblShaderDataPath(const char* filename) {
    return std::filesystem::path(RX_IBL_SHADER_DIR) / filename;
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
        MESSAGE("no display backend available, skipping StandardPBR IBL test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping StandardPBR IBL test");
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
    capacities.comparisonSamplers = 1;
    // [Phase 5 Task 10, #46] Real cube-image registrations -- this file's
    // own TEST_CASEs each build their own TestEnvironment (3 cube images:
    // irradiance/prefiltered share one upload, but the fixture registers
    // base/irradiance/prefiltered as up to 3 independent handles across a
    // TEST_CASE's own probes) -- 8 gives comfortable headroom.
    capacities.cubeImages = 8;
    // [Phase 5 Stage 2 Task 15, #51] Same requirement as `cubeImages` above:
    // standard_pbr.slang now unconditionally declares `gClusterBuffers`/
    // `gClusterLights` at bindings 5/6 via cluster_lighting.slang.
    capacities.genericStorageBuffers = 1;
    capacities.clusterLightBuffers = 1;
    auto bindless = rx::rhi::BindlessTable::create(device->physicalDevice(), device->device(), capacities);
    REQUIRE(bindless.has_value());

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());

    return Fixture{std::move(*window),  std::move(*context),   surface,
                    std::move(*device), std::move(*bindless), std::move(*allocator)};
}

std::filesystem::path freshCachePath(const char* name) {
    std::filesystem::path path = std::filesystem::temp_directory_path() / (std::string("rx_standard_pbr_ibl_test_") + name + ".cache");
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

// One unit quad, object-space (-0.5,-0.5,0)..(0.5,0.5,0), normal (0,0,1) --
// SAME shape as test_standard_pbr_unlit.cpp's own kQuadVertices, see this
// file's own header comment ("TEST RIG") for the resulting camera/
// reflection-direction rig this enables.
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

// The default head-on rig -- camera at (0,0,2) looking at the origin,
// orthographic projection matched to the quad's own extent, light
// DISABLED (lightColor zeroed -- every TEST_CASE here isolates IBL alone
// unless it explicitly re-enables direct light). [MATRIX LAYOUT] every
// matrix is glm::transpose()'d before upload -- see rx_material/draw_data.h.
rx::material::DrawDataGpu makeHeadOnIblRow() {
    rx::material::DrawDataGpu row;
    glm::mat4 view = glm::lookAt(glm::vec3(0.0F, 0.0F, 2.0F), glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    glm::mat4 proj = glm::orthoZO(-0.5F, 0.5F, -0.5F, 0.5F, 0.1F, 10.0F);
    proj[1][1] *= -1.0F;  // Vulkan Y-flip -- see test_standard_pbr_unlit.cpp's own makeHeadOnRow() for the full account.
    for (int col = 0; col < 4; ++col) {  // reversed-Z affine remap z' = 1-z.
        const float row3 = (col == 3) ? 1.0F : 0.0F;
        proj[col][2] = row3 - proj[col][2];
    }
    glm::mat4 viewProj = proj * view;

    row.model = glm::mat4(1.0F);
    row.normalMatrix = glm::mat4(1.0F);
    row.viewProj = glm::transpose(viewProj);
    row.lightDirWorld = glm::vec4(0.0F, 0.0F, 1.0F, 0.0F);
    row.lightColor = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);  // direct light OFF -- isolates IBL.
    row.ambientColor = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);  // retired field, inert.
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

bool near8(uint8_t actual, int expected, int margin) { return std::abs(static_cast<int>(actual) - expected) <= margin; }

// One tiny 8x8 offscreen render: ONE quad draw against a REAL StandardPBR
// pipeline (real bindless set-0, real set-1 UBO), reversed-Z depth. Returns
// the CENTER texel as RGBA8. Mirrors test_standard_pbr_unlit.cpp's own
// renderQuad() shape, trimmed to a single fixed draw (no multi-draw
// generality this file's own TEST_CASEs never need).
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
    rx::graph::Pass& pass = graph.addPass("standard_pbr_ibl_test");
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

// Builds a StandardPbrParams-shaped blob at glTF-neutral defaults (white
// base color, real ior/specular/dfgY neutrals -- see
// test_standard_pbr_unlit.cpp's own makeDefaultStandardPbrBlob() for the
// identical baseline this mirrors) plus this environment's OWN env* fields.
// `whiteTex` is a 1x1 white RGBA8 texture's bindless index (every texture
// slot in these tests is the neutral white/flat-normal fallback -- no
// TEST_CASE here probes texture sampling, only the lighting math).
std::vector<uint8_t> makeIblBlob(rx::material::MaterialSystem& system, rx::material::MaterialHandle handle,
                                  uint32_t whiteTex, uint32_t flatNormalTex, uint32_t samplerIndex, float metallic,
                                  float roughness, glm::vec4 baseColor) {
    uint32_t blockSize = system.paramBlockSize(handle);
    const auto params = system.materialParams(handle);
    std::vector<uint8_t> blob(blockSize, 0);
    setParam(blob, params, "baseColorFactor", std::array<float, 4>{baseColor.x, baseColor.y, baseColor.z, baseColor.w});
    setParam(blob, params, "metallicFactor", metallic);
    setParam(blob, params, "roughnessFactor", roughness);
    setParam(blob, params, "normalScale", 1.0F);
    setParam(blob, params, "occlusionStrength", 1.0F);
    setParam(blob, params, "alphaCutoff", 0.0F);
    setParam(blob, params, "ior", 1.5F);
    setParam(blob, params, "specularFactor", 1.0F);
    setParam(blob, params, "specularColorFactorAndPad", std::array<float, 4>{1.0F, 1.0F, 1.0F, 0.0F});
    setParam(blob, params, "dfgY", 1.0F);  // CPU-bound neutral -- irrelevant whenever hasEnvironment (this file's whole point).
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
    std::array<uint8_t, 4> flatPixel{128, 128, 255, 255};  // (0,0,1) tangent-space normal.
    texInfo.pixels = flatPixel.data();
    texInfo.pixelBytes = flatPixel.size();
    rx::material::TextureHandle tex = system.createTexture2D(texInfo);
    REQUIRE(tex.isValid());
    return system.textureBindlessIndex(tex);
}

}  // namespace

TEST_CASE("StandardPBR IBL: Lambertian diffuse under a uniform environment reproduces the closed-form "
          "albedo x radiance value exactly [gate matrix 'Diffuse IBL feeding the diffuse lobe' row] -- "
          "[T10 fix round, task-10-review.md Finding 1/2] dfgValue=(0.05,0.4) satisfies the dfg.x<=dfg.y "
          "invariant every real T9 bake output satisfies (the retired (0,0) constant was degenerate: E==0 "
          "identically under EITHER the buggy or the corrected specular-DFG formula, which is exactly why "
          "this TEST_CASE alone could never have caught Finding 1). Even with a non-degenerate dfg, this "
          "specific probe is STILL formula-invariant, but for an exact, provable reason, not a test gap: "
          "energy compensation is OFF for this pipeline variant (specializationBits=0, renderQuad()'s own "
          "getPipeline() call), so `ibl = diffuseColor*irradianceSample*(1-E) + prefilteredSample*E`; with "
          "metallic=0 and a white baseColorFactor, diffuseColor==(1,1,1) and (uniform environment) "
          "irradianceSample==prefilteredSample==L, so `ibl = L*(1-E) + L*E == L` EXACTLY for ANY E in "
          "[0,1] -- the split-sum method's own energy-conservation identity, not something a different dfg "
          "choice can un-cancel. Discriminating the specular E-formula therefore requires killing the "
          "diffuse lobe (metallic=1), which is exactly what the mirror-metal and furnace TEST_CASEs below "
          "do; THIS TEST_CASE's own job is validating the diffuse lobe's own machinery (irradiance "
          "sampling, Lambertian evaluation, the `(1-E)` wiring) independently of what E's value is -- "
          "bake.h's own pre-divided irradiance convention means the uploaded uniform-radiance texel L IS "
          "the closed-form Lambertian-under-uniform-environment answer directly (irradiance = L*pi; "
          "outgoing Lambertian radiance = albedo/pi * irradiance == albedo*L for a white albedo == L "
          "exactly)") {
    auto fixture = makeFixture("standard_pbr_ibl_lambertian");
    if (!fixture.has_value()) {
        return;
    }
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("lambertian"));
    REQUIRE(system != nullptr);
    auto mesh = createQuadMesh(fixture->allocator);
    REQUIRE(mesh.has_value());

    rx::material::MaterialHandle handle =
        system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});

    uint32_t whiteTex = makeWhiteTexture(*system);
    uint32_t flatNormalTex = makeFlatNormalTexture(*system);
    uint32_t samplerIndex = 0;
    VkSampler rawSampler = VK_NULL_HANDLE;
    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = VK_LOD_CLAMP_NONE;
        REQUIRE(vkCreateSampler(fixture->device.device(), &info, nullptr, &rawSampler) == VK_SUCCESS);
        rx::rhi::BindlessHandle h = fixture->bindless.registerSampler(rawSampler);
        REQUIRE(h.isValid());
        samplerIndex = h.index();
    }

    // radiance L=0.5 -- comfortably below the UNORM store's 1.0 clamp
    // ceiling. dfgValue=(0.05,0.4): [T10 fix round] a realistic,
    // invariant-respecting (dfg.x < dfg.y) pair for a fully-rough
    // (roughness=1.0, see the blob below) dielectric probe -- see this
    // TEST_CASE's own header comment for why the specific value no longer
    // matters to the final assertion (energy conservation, energy comp
    // OFF).
    constexpr float kRadiance = 0.5F;
    constexpr float kDfgX = 0.05F;
    constexpr float kDfgY = 0.4F;
    auto env = makeUniformTestEnvironment(fixture->device, fixture->allocator, fixture->bindless,
                                           glm::vec4(kRadiance, kRadiance, kRadiance, 1.0F),
                                           glm::vec2(kDfgX, kDfgY));
    REQUIRE(env.has_value());

    rx::material::DrawDataGpu row = makeHeadOnIblRow();
    env->applyTo(row);
    auto drawDataBuffer = createDrawDataBuffer(fixture->allocator, fixture->bindless, row);
    REQUIRE(drawDataBuffer.has_value());

    std::vector<uint8_t> blob =
        makeIblBlob(*system, handle, whiteTex, flatNormalTex, samplerIndex, /*metallic=*/0.0F, /*roughness=*/1.0F,
                    glm::vec4(1.0F, 1.0F, 1.0F, 1.0F));

    Rgba8 pixel = renderQuad(fixture->device, fixture->allocator, fixture->bindless, drawDataBuffer->handle,
                              samplerIndex, *mesh, *system, handle, blob);

    // Expected: L*255 == 0.5*255 == 127.5 -> 127 or 128 depending on
    // rounding; a generous +-6 tolerance absorbs lavapipe's own fp16
    // storage rounding through the irradiance cubemap's own R16G16B16A16_
    // SFLOAT texel (kRadiance=0.5 is EXACTLY representable in fp16, so
    // this tolerance is about the shading math's own rounding, not the
    // storage format). This closed form is EXACTLY L regardless of which
    // specular-DFG formula is in effect (see header comment) -- included
    // here as INFO, not a second CHECK, precisely to make that invariance
    // visible rather than silently assumed.
    const int expected = static_cast<int>(std::lround(kRadiance * 255.0));
    INFO("pixel r=", static_cast<int>(pixel.r), " g=", static_cast<int>(pixel.g), " b=", static_cast<int>(pixel.b),
         " expected=", expected, " (formula-invariant: both the corrected and the retired-buggy E give ibl==L here)");
    CHECK(near8(pixel.r, expected, 6));
    CHECK(near8(pixel.g, expected, 6));
    CHECK(near8(pixel.b, expected, 6));

    env->destroySamplers(fixture->device.device());
    vkDestroySampler(fixture->device.device(), rawSampler, nullptr);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("StandardPBR IBL: mirror-metal sphere under a known environment reproduces the environment -- a "
          "matched-pose value probe [ticket #46's own acceptance line, quoted verbatim] -- [T10 fix round, "
          "task-10-review.md Finding 1/2] metallic=1, roughness clamped to kMinRoughness (near-mirror), white "
          "F0 (baseColorFactor white x metallic=1 -> F0=(1,1,1) exactly). dfgValue=(0.12,0.80): the retired "
          "(1,0) constant both violated the dfg.x<=dfg.y invariant every real T9 bake output satisfies AND "
          "had its own 'expected' derived from the SAME (buggy) formula the shader shipped -- a tautology, "
          "not independent ground truth. The CORRECTED formula [brdf.slang iblSpecularReflectance(), "
          "mix(dfg.x,dfg.y,f0)] has a robust endpoint identity at f0==(1,1,1) exactly (this probe's own "
          "F0): mix(dfg.x,dfg.y,1)==dfg.y for ANY dfg.x, so E==dfg.y==0.80 independently of the retired "
          "formula's own dfg.x term -- a perfect, lossless-in-dfg.y mirror, not exactly f0 anymore (that "
          "pedagogical shape only held for the OLD formula's own math). iblSpecular = prefilteredSample * E, "
          "iblDiffuse == 0 (metallic=1 kills diffuseColor before E is even applied), energy compensation OFF "
          "for this pipeline variant (specializationBits=0) -- the head-on rig's own reflection vector "
          "R==(0,0,1) (world +Z) always samples cube FACE INDEX 4 (Vulkan's own standard layer-order "
          "convention, this file's own header comment) -- a DISTINCT, recognizable color on that ONE face, "
          "black everywhere else, proves the probe reads the CORRECT face, not merely 'some' face. This "
          "TEST_CASE also asserts an explicit DISCRIMINATION check: the retired formula's own prediction "
          "(E==dfg.x+dfg.y==0.92 at f0=1) differs from the corrected E==0.80 by exactly dfg.x==0.12, a "
          "pixel delta comfortably outside this TEST_CASE's own +-8/255 tolerance") {
    auto fixture = makeFixture("standard_pbr_ibl_mirror");
    if (!fixture.has_value()) {
        return;
    }
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("mirror"));
    REQUIRE(system != nullptr);
    auto mesh = createQuadMesh(fixture->allocator);
    REQUIRE(mesh.has_value());

    rx::material::MaterialHandle handle =
        system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});

    uint32_t whiteTex = makeWhiteTexture(*system);
    uint32_t flatNormalTex = makeFlatNormalTexture(*system);
    uint32_t samplerIndex = 0;
    VkSampler rawSampler = VK_NULL_HANDLE;
    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = VK_LOD_CLAMP_NONE;
        REQUIRE(vkCreateSampler(fixture->device.device(), &info, nullptr, &rawSampler) == VK_SUCCESS);
        rx::rhi::BindlessHandle h = fixture->bindless.registerSampler(rawSampler);
        REQUIRE(h.isValid());
        samplerIndex = h.index();
    }

    // +X,-X,+Y,-Y,+Z,-Z order -- index 4 (+Z) gets a distinctive orange;
    // every other face is black, so a wrong-face read is unmistakable
    // (reads near-black instead of orange), not just "a different shade
    // of some plausible color".
    constexpr glm::vec4 kFaceColor{0.9F, 0.45F, 0.05F, 1.0F};
    std::array<glm::vec4, 6> faces{glm::vec4(0.0F), glm::vec4(0.0F), glm::vec4(0.0F),
                                     glm::vec4(0.0F), kFaceColor,      glm::vec4(0.0F)};
    // [T10 fix round] dfg.x=0.12 < dfg.y=0.80 -- satisfies the real-bake
    // invariant (was (1,0), inverted) and is large enough in dfg.x that the
    // retired formula's own prediction (dfg.x+dfg.y at f0=1, see header
    // comment) diverges from the corrected one (dfg.y alone) by more than
    // this TEST_CASE's own tolerance -- a genuine discriminator, not merely
    // invariant-compliant.
    constexpr float kDfgX = 0.12F;
    constexpr float kDfgY = 0.80F;
    auto env =
        makeTestEnvironment(fixture->device, fixture->allocator, fixture->bindless, faces, glm::vec2(kDfgX, kDfgY));
    REQUIRE(env.has_value());

    rx::material::DrawDataGpu row = makeHeadOnIblRow();
    env->applyTo(row);
    auto drawDataBuffer = createDrawDataBuffer(fixture->allocator, fixture->bindless, row);
    REQUIRE(drawDataBuffer.has_value());

    std::vector<uint8_t> blob =
        makeIblBlob(*system, handle, whiteTex, flatNormalTex, samplerIndex, /*metallic=*/1.0F, /*roughness=*/0.0F,
                    glm::vec4(1.0F, 1.0F, 1.0F, 1.0F));

    Rgba8 pixel = renderQuad(fixture->device, fixture->allocator, fixture->bindless, drawDataBuffer->handle,
                              samplerIndex, *mesh, *system, handle, blob);

    // Independently-derived (not read from the shader) closed-form
    // expected, using the CORRECTED split-sum formula's own endpoint
    // identity at f0==(1,1,1) exactly: mix(dfg.x,dfg.y,1)==dfg.y for ANY
    // dfg.x -- see this TEST_CASE's own header comment.
    constexpr float kExpectedE = kDfgY;
    const int expectedR = static_cast<int>(std::lround(kFaceColor.r * kExpectedE * 255.0F));
    const int expectedG = static_cast<int>(std::lround(kFaceColor.g * kExpectedE * 255.0F));
    const int expectedB = static_cast<int>(std::lround(kFaceColor.b * kExpectedE * 255.0F));
    INFO("pixel r=", static_cast<int>(pixel.r), " g=", static_cast<int>(pixel.g), " b=", static_cast<int>(pixel.b),
         " expected=(", expectedR, ",", expectedG, ",", expectedB, ")");
    CHECK(near8(pixel.r, expectedR, 8));
    CHECK(near8(pixel.g, expectedG, 8));
    CHECK(near8(pixel.b, expectedB, 8));
    // Discrimination: NOT black -- proves this isn't accidentally reading
    // one of the 5 black faces instead.
    CHECK(pixel.r > 100);

    // [T10 fix round, task-10-review.md Finding 1] Explicit discrimination
    // against the RETIRED (buggy) formula's own prediction -- at f0=1 the
    // retired `f0*dfg.x+dfg.y` reduces to `dfg.x+dfg.y`, strictly greater
    // than the corrected `dfg.y` by exactly `dfg.x`. Assert the two
    // predictions are themselves far enough apart to matter (i.e. this
    // TEST_CASE's own choice of dfg.x is not accidentally too small to
    // discriminate), independent of what the actual rendered pixel reads.
    constexpr float kOldFormulaE = kDfgX + kDfgY;  // f0*dfg.x+dfg.y at f0=1.
    const int oldPredictedR = static_cast<int>(std::lround(kFaceColor.r * kOldFormulaE * 255.0F));
    const int deltaR = oldPredictedR - expectedR;
    INFO("retired-formula prediction r=", oldPredictedR, " corrected expected r=", expectedR, " delta=", deltaR);
    CHECK(deltaR > 8);  // exceeds this TEST_CASE's own +-8/255 tolerance -- a real gate-flip, not noise.

    env->destroySamplers(fixture->device.device());
    vkDestroySampler(fixture->device.device(), rawSampler, nullptr);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("StandardPBR IBL: rough-metal furnace sanity on the FULL runtime path -- under a uniform environment "
          "(radiance L, every face/mip identical by construction) a rough metal's own IBL specular response "
          "reproduces the closed form L*E exactly -- [T10 fix round, task-10-review.md Finding 1/2] "
          "E=mix(dfgX,dfgY,f0)==(1-f0)*dfgX+f0*dfgY (Filament's own `specularDFG()`, "
          "shaders/src/surface_light_indirect.fs v1.75.0:135, matching brdf.slang's own corrected "
          "`iblSpecularReflectance()`), computed HERE independently in C++ from first principles -- NOT by "
          "calling the shader's own helper -- so this is a real ground truth, not a tautology (the RETIRED "
          "version of this TEST_CASE computed `expectedE = kF0*kDfgX+kDfgY`, the EXACT SAME formula the "
          "shader shipped, which is why it could never have caught Finding 1 no matter what the constants "
          "were). dfgX=0.08 < dfgY=0.55 satisfies the dfg.x<=dfg.y invariant every real T9 bake output "
          "satisfies (the retired dfgX=0.9 > dfgY=0.05 inverted it). Also asserts the retired formula's own "
          "prediction diverges from the corrected one by well outside this TEST_CASE's own +-8/255 "
          "tolerance -- a real, quantified gate-flip, and stays WITHIN [0, L] -- energy-sane, never "
          "exceeding the environment's own radiance (the classic furnace-test invariant, applied end to end "
          "through the real production shader rather than brdf.slang's own standalone Monte-Carlo harness)") {
    auto fixture = makeFixture("standard_pbr_ibl_furnace");
    if (!fixture.has_value()) {
        return;
    }
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("furnace"));
    REQUIRE(system != nullptr);
    auto mesh = createQuadMesh(fixture->allocator);
    REQUIRE(mesh.has_value());

    rx::material::MaterialHandle handle =
        system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});

    uint32_t whiteTex = makeWhiteTexture(*system);
    uint32_t flatNormalTex = makeFlatNormalTexture(*system);
    uint32_t samplerIndex = 0;
    VkSampler rawSampler = VK_NULL_HANDLE;
    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = VK_LOD_CLAMP_NONE;
        REQUIRE(vkCreateSampler(fixture->device.device(), &info, nullptr, &rawSampler) == VK_SUCCESS);
        rx::rhi::BindlessHandle h = fixture->bindless.registerSampler(rawSampler);
        REQUIRE(h.isValid());
        samplerIndex = h.index();
    }

    constexpr float kRadiance = 0.6F;
    // [T10 fix round] dfgX is the Fresnel-scaling (F0) channel -- small at
    // this probe's own NdotV==1 head-on geometry, per dfg_lut.slang's own
    // accumulation (`r.x += term*fc`, `fc=(1-VoH)^5` small near VoH==1).
    // dfgY is the base energy-return channel -- larger, but not the
    // near-1.0 a near-mirror texel would carry, since this probe's own
    // roughness=0.6 (fairly rough) reduces total specular energy return.
    // dfgX < dfgY strictly, satisfying every real T9 bake's own invariant
    // (the retired dfgX=0.9/dfgY=0.05 pair had this backwards).
    constexpr float kDfgX = 0.08F;
    constexpr float kDfgY = 0.55F;
    constexpr float kF0 = 0.7F;     // grey rough metal (baseColorFactor x metallic=1).
    auto env = makeUniformTestEnvironment(fixture->device, fixture->allocator, fixture->bindless,
                                           glm::vec4(kRadiance, kRadiance, kRadiance, 1.0F), glm::vec2(kDfgX, kDfgY));
    REQUIRE(env.has_value());

    rx::material::DrawDataGpu row = makeHeadOnIblRow();
    env->applyTo(row);
    auto drawDataBuffer = createDrawDataBuffer(fixture->allocator, fixture->bindless, row);
    REQUIRE(drawDataBuffer.has_value());

    std::vector<uint8_t> blob =
        makeIblBlob(*system, handle, whiteTex, flatNormalTex, samplerIndex, /*metallic=*/1.0F, /*roughness=*/0.6F,
                    glm::vec4(kF0, kF0, kF0, 1.0F));

    Rgba8 pixel = renderQuad(fixture->device, fixture->allocator, fixture->bindless, drawDataBuffer->handle,
                              samplerIndex, *mesh, *system, handle, blob);

    // [T10 fix round, task-10-review.md Finding 2] Independent ground
    // truth: Filament's own `specularDFG()` reconstruction (v1.75.0
    // surface_light_indirect.fs:135, CubemapIBL.cpp:790-833's own
    // DFV_Multiscatter() comment "Er() = (1-f0)*DFV.x + f0*DFV.y"),
    // transcribed here directly in C++ from the cited source -- NOT
    // derived by calling brdf.slang's own iblSpecularReflectance() or any
    // shader code -- so a bug in that shader function cannot silently
    // match this expectation.
    const float expectedE = (1.0F - kF0) * kDfgX + kF0 * kDfgY;
    REQUIRE(expectedE >= 0.0F);
    REQUIRE(expectedE <= 1.0F);  // sanity on this TEST_CASE's own chosen constants -- a real dfg texel never exceeds 1.
    const float expectedRadiance = kRadiance * expectedE;
    const int expected = static_cast<int>(std::lround(expectedRadiance * 255.0));
    INFO("pixel r=", static_cast<int>(pixel.r), " expectedE=", expectedE, " expected=", expected);
    CHECK(near8(pixel.r, expected, 8));
    CHECK(near8(pixel.g, expected, 8));
    CHECK(near8(pixel.b, expected, 8));

    // [T10 fix round, task-10-review.md Finding 1] Explicit discrimination
    // against the RETIRED (buggy) formula's own prediction
    // (`f0*dfgX+dfgY`, byte-identical to the pre-fix
    // iblSpecularReflectance()) -- assert the two independently-computed
    // predictions are themselves far enough apart that this TEST_CASE's
    // own choice of constants is a real discriminator, not an accident of
    // rounding.
    const float oldFormulaE = kF0 * kDfgX + kDfgY;
    const float oldFormulaRadiance = kRadiance * oldFormulaE;
    const int oldPredicted = static_cast<int>(std::lround(oldFormulaRadiance * 255.0));
    const int deltaFromOldFormula = oldPredicted - expected;
    INFO("retired-formula E=", oldFormulaE, " prediction=", oldPredicted, " corrected expected=", expected,
         " delta=", deltaFromOldFormula);
    CHECK(deltaFromOldFormula > 8);  // exceeds this TEST_CASE's own +-8/255 tolerance -- a real gate-flip.

    // The furnace invariant itself: never brighter than the environment's
    // own radiance (a real energy-conservation bug -- e.g. E applied
    // twice, or an accidental +1 -- would blow this past kRadiance*255).
    const int radianceCeiling = static_cast<int>(std::lround(kRadiance * 255.0)) + 4;  // +4 == D17's own per-channel slack.
    CHECK(pixel.r <= radianceCeiling);
    CHECK(pixel.g <= radianceCeiling);
    CHECK(pixel.b <= radianceCeiling);

    env->destroySamplers(fixture->device.device());
    vkDestroySampler(fixture->device.device(), rawSampler, nullptr);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

namespace {

// --- Skybox pass value test [Phase 5 Task 10, #46] -- standalone rig,
// no MaterialSystem: shaders/ibl/skybox.slang compiled+reflected directly,
// matching samples/08_gltf_viewer's own buildSkyboxPipeline() shape at a
// smaller scale (duplicated, not shared -- this library has no dependency
// on that sample's own .cpp, and this is exactly this test suite's own
// established per-binary-duplicated-fixture idiom).
struct SkyboxCompiledPass {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t pushOffset = 0;
    uint32_t pushSize = 0;
    VkShaderStageFlags pushStages = 0;
};

std::optional<std::string> readFileToString(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool buildSkyboxTestPipeline(VkDevice device, rx::shader::Compiler& compiler, VkDescriptorSetLayout bindlessSetLayout,
                              VkFormat colorFormat, VkFormat depthFormat, const std::filesystem::path& skyboxSlangPath,
                              SkyboxCompiledPass& pass) {
    auto source = readFileToString(skyboxSlangPath);
    REQUIRE(source.has_value());
    rx::shader::CompileResult compileResult =
        compiler.compileFromSource("SkyboxTestModule", *source, {"vsMain", "fsMain"});
    REQUIRE_MESSAGE(compileResult.ok, compileResult.diagnostics);
    auto layoutInfo = rx::shader::reflect(compileResult);
    REQUIRE(layoutInfo.has_value());
    REQUIRE(layoutInfo->pushRanges.size() == 1);
    REQUIRE(layoutInfo->pushRanges[0].size == sizeof(rx::material::SkyboxPush));

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, *layoutInfo, bindlessSetLayout);
    REQUIRE(layoutBundle.has_value());
    pass.layoutBundle = std::move(*layoutBundle);
    pass.pushOffset = layoutInfo->pushRanges[0].offset;
    pass.pushSize = layoutInfo->pushRanges[0].size;
    pass.pushStages = layoutInfo->pushRanges[0].stages;

    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = blob.code.size() * sizeof(uint32_t);
        info.pCode = blob.code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        REQUIRE(vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS);
        if (blob.entryPointName == "vsMain") {
            pass.vertModule = module;
        } else if (blob.entryPointName == "fsMain") {
            pass.fragModule = module;
        }
    }
    REQUIRE(pass.vertModule != VK_NULL_HANDLE);
    REQUIRE(pass.fragModule != VK_NULL_HANDLE);

    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
    inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = VK_CULL_MODE_NONE;
    rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationState.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencilState{};
    depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState.depthTestEnable = VK_TRUE;
    depthStencilState.depthWriteEnable = VK_FALSE;
    depthStencilState.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlendState{};
    colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = &blendAttachment;
    std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    VkPipelineRenderingCreateInfo renderingCreateInfo{};
    renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCreateInfo.colorAttachmentCount = 1;
    renderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    renderingCreateInfo.depthAttachmentFormat = depthFormat;
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = pass.vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pass.fragModule;
    stages[1].pName = "main";
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingCreateInfo;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInputState;
    pipelineInfo.pInputAssemblyState = &inputAssemblyState;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizationState;
    pipelineInfo.pMultisampleState = &multisampleState;
    pipelineInfo.pDepthStencilState = &depthStencilState;
    pipelineInfo.pColorBlendState = &colorBlendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pass.layoutBundle.layout;
    pipelineInfo.basePipelineIndex = -1;
    return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pass.pipeline) == VK_SUCCESS;
}

}  // namespace

TEST_CASE("Skybox: a pixel not covered by any opaque geometry samples the environment cubemap directly at the "
          "camera ray's own direction -- a value match against the source cubemap texel [gate matrix 'Skybox "
          "pass' row, quoted: 'pixels NOT covered by any opaque geometry sample the environment cubemap "
          "directly at the camera ray's direction']. The camera looks straight down -Z from the origin; every "
          "ray through the viewport's own CENTER pixel is exactly the camera's forward vector (0,0,-1) "
          "regardless of FOV/aspect (a standard symmetric-frustum perspective property) -- world -Z is cube "
          "FACE INDEX 5 (Vulkan's own +X,-X,+Y,-Y,+Z,-Z layer order -- see this file's own body comment for the "
          "getDirectionForFace() citation). "
          "no opaque geometry is ever drawn in this test (an empty render pass, cleared depth only), so the "
          "WHOLE viewport is 'not covered', and the depth test (GREATER_OR_EQUAL against the clear value 0.0) "
          "passes everywhere -- this is a direct end-to-end proof of skybox.slang's own camera-ray "
          "reconstruction, not just its cubemap-sampling instruction in isolation") {
    auto fixture = makeFixture("skybox_ray_value");
    if (!fixture.has_value()) {
        return;
    }

    // World -Z is cube face index 5 (-Z), per getDirectionForFace()'s own
    // face5 branch `dir=(-cx,cy,-1)` (prefilter_specular.slang, at
    // cx=cy=0: dir=(0,0,-1)) -- the SAME Vulkan-standard convention this
    // file's own mirror-metal TEST_CASE already cites for face4 (+Z).
    constexpr glm::vec4 kFaceColor{0.2F, 0.7F, 0.9F, 1.0F};
    std::array<glm::vec4, 6> faces{glm::vec4(0.0F), glm::vec4(0.0F), glm::vec4(0.0F),
                                     glm::vec4(0.0F), glm::vec4(0.0F), kFaceColor};
    auto env = makeTestEnvironment(fixture->device, fixture->allocator, fixture->bindless, faces, glm::vec2(0.0F, 0.0F));
    REQUIRE(env.has_value());

    VkSampler cubeSampler = VK_NULL_HANDLE;
    {
        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = VK_LOD_CLAMP_NONE;
        REQUIRE(vkCreateSampler(fixture->device.device(), &info, nullptr, &cubeSampler) == VK_SUCCESS);
    }
    rx::rhi::BindlessHandle cubeSamplerHandle = fixture->bindless.registerSampler(cubeSampler);
    REQUIRE(cubeSamplerHandle.isValid());

    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
    constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    SkyboxCompiledPass pass;
    REQUIRE(buildSkyboxTestPipeline(fixture->device.device(), *compiler, fixture->bindless.descriptorSetLayout(),
                                     kColorFormat, kDepthFormat, iblShaderDataPath("skybox.slang"), pass));

    // Camera at the origin, looking straight down -Z -- SkyboxDataGpu's own
    // invViewProj built from a real rx::scene::Camera (reversed-Z, matching
    // skybox.slang's own depth-test convention).
    rx::scene::Camera camera;
    camera.verticalFovRadians = glm::radians(60.0F);
    camera.aspectRatio = 1.0F;
    camera.nearPlane = 0.1F;
    camera.cullingFarPlane = 100.0F;
    glm::mat4 viewMatrix =
        glm::lookAt(glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, -1.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    glm::mat4 viewProj = camera.cullingProj() * viewMatrix;
    glm::mat4 invViewProj = glm::inverse(viewProj);

    rx::material::SkyboxDataGpu skyboxRow;
    skyboxRow.invViewProj = glm::transpose(invViewProj);
    skyboxRow.cameraPosWorld = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);
    skyboxRow.baseCubeIndex = env->irradianceHandle.index();  // any of the 3 registered cube handles carries THIS test's own uploaded per-face colors identically (makeTestEnvironment() uploads the SAME faceColors into irradiance+prefiltered) -- base specifically is not separately built by this test-only fixture (irradiance/prefiltered ARE what it builds); using irradianceHandle here is exactly equivalent for this test's own purpose (sampling a plain cube by direction).
    skyboxRow.cubeSamplerIndex = cubeSamplerHandle.index();
    skyboxRow.intensity = 1.0F;

    auto skyboxBuf = fixture->allocator.createHostVisibleBuffer(sizeof(skyboxRow), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                                  rx::rhi::MemoryCategory::Internal);
    REQUIRE(skyboxBuf.has_value());
    std::memcpy(skyboxBuf->mappedData(), &skyboxRow, sizeof(skyboxRow));
    skyboxBuf->flush();
    rx::rhi::BindlessHandle skyboxBufHandle = fixture->bindless.registerStorageBuffer(skyboxBuf->handle(), sizeof(skyboxRow));
    REQUIRE(skyboxBufHandle.isValid());

    // [Bug found and fixed during this task's own development] ODD extent,
    // not the usual 8x8: this test's own probe reads the CENTER texel at
    // index (width/2, height/2) and needs that texel's own NDC coordinate
    // to land EXACTLY on (0,0) (the true optical-axis center this TEST_
    // CASE's own header comment relies on) -- for an EVEN-sized viewport
    // there IS no such texel (an 8-wide image's own index 4 texel center
    // sits at NDC (4.5/8)*2-1 == 0.125, not 0.0), which was found to
    // sample close enough to the +Z/-Z face BOUNDARY to blend a visible
    // amount of the adjacent (black) face in -- reproduced directly this
    // round (G/B channels reading ~13-17/255 low). A 9-wide image's own
    // index-4 texel sits at ((4+0.5)/9)*2-1 == 0.0 exactly.
    constexpr VkExtent2D kExtent{9, 9};
    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto executor = rx::graph::Executor::create(fixture->device, *scheduler);
    REQUIRE(executor != nullptr);

    rx::graph::RenderGraph graph;
    rx::graph::Pass& renderPass = graph.addPass("skybox_test");
    rx::graph::AttachmentDesc colorDesc;
    colorDesc.format = kColorFormat;
    renderPass.addColorOutput("color", colorDesc);
    rx::graph::AttachmentDesc depthDesc;
    depthDesc.format = kDepthFormat;
    depthDesc.depthConvention = rx::graph::DepthConvention::Reversed;
    renderPass.setDepthStencilOutput("depth", depthDesc);
    graph.setBackbufferSource("color");

    renderPass.setExecute([&](rx::graph::PassContext& ctx) {
        VkCommandBuffer cmd = ctx.cmd;
        VkViewport viewport{0.0F, 0.0F, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height), 0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, kExtent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkDescriptorSet bindlessSet = fixture->bindless.descriptorSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.layoutBundle.layout, 0, 1, &bindlessSet, 0,
                                 nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);

        rx::material::SkyboxPush push;
        push.skyboxDataBufferIndex = skyboxBufHandle.index();
        vkCmdPushConstants(cmd, pass.layoutBundle.layout, pass.pushStages, pass.pushOffset, pass.pushSize, &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
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
    REQUIRE(vkCreateImage(fixture->device.device(), &imageInfo, nullptr, &image) == VK_SUCCESS);
    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(fixture->device.device(), image, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(fixture->device.physicalDevice(), &memProps);
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
    REQUIRE(vkAllocateMemory(fixture->device.device(), &allocInfo, nullptr, &imageMemory) == VK_SUCCESS);
    REQUIRE(vkBindImageMemory(fixture->device.device(), image, imageMemory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kColorFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    REQUIRE(vkCreateImageView(fixture->device.device(), &viewInfo, nullptr, &view) == VK_SUCCESS);

    VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(kExtent.width) * kExtent.height * 4;
    auto readback = fixture->allocator.createHostVisibleBuffer(pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                                  rx::rhi::MemoryCategory::Internal);
    REQUIRE(readback.has_value());
    {
        auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                        fixture->device.graphicsQueueFamily());
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
    Rgba8 pixel{rawPixels[centerIndex], rawPixels[centerIndex + 1], rawPixels[centerIndex + 2], rawPixels[centerIndex + 3]};

    const int expectedR = static_cast<int>(std::lround(kFaceColor.r * 255.0F));
    const int expectedG = static_cast<int>(std::lround(kFaceColor.g * 255.0F));
    const int expectedB = static_cast<int>(std::lround(kFaceColor.b * 255.0F));
    INFO("pixel r=", static_cast<int>(pixel.r), " g=", static_cast<int>(pixel.g), " b=", static_cast<int>(pixel.b),
         " expected=(", expectedR, ",", expectedG, ",", expectedB, ")");
    CHECK(near8(pixel.r, expectedR, 8));
    CHECK(near8(pixel.g, expectedG, 8));
    CHECK(near8(pixel.b, expectedB, 8));
    CHECK(pixel.g > 100);  // discrimination: not one of the 5 black faces.

    vkDestroyImageView(fixture->device.device(), view, nullptr);
    vkDestroyImage(fixture->device.device(), image, nullptr);
    vkFreeMemory(fixture->device.device(), imageMemory, nullptr);
    if (pass.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(fixture->device.device(), pass.pipeline, nullptr);
    }
    if (pass.fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(fixture->device.device(), pass.fragModule, nullptr);
    }
    if (pass.vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(fixture->device.device(), pass.vertModule, nullptr);
    }
    env->destroySamplers(fixture->device.device());
    vkDestroySampler(fixture->device.device(), cubeSampler, nullptr);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
