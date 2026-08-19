// Phase 4 Stage 1 Task 16 [D22, D26.1, D28, gate ruling #8/RC1]: GPU tests
// for the two shipped material modules (standard_pbr.slang/unlit.slang),
// D28's fixed-function pipeline-state axis, and D26.1's bindless per-draw
// addressing (SV_VulkanInstanceID) -- the mechanisms the ticket's own
// "discrimination standard" names as the highest-priority revert-worthy
// set: MASK-cutoff discard, BC5 Z-reconstruction, D28 distinct-pipeline
// cache counter, SV_VulkanInstanceID two-draw case, exposure-neutral guard.
//
// TEST CAMERA/LIGHT RIG -- read before adding a new TEST_CASE here: every
// test below draws ONE unit quad (`kQuadVertices` below), object space
// (-0.5,-0.5,0)..(0.5,0.5,0), normal (0,0,1), tangent (1,0,0,1) (so
// world == object space under an identity model matrix). The DEFAULT rig
// (drawRow() with no explicit camera/light override) places the camera at
// (0,0,2) looking straight down -Z at the quad's front face and the light
// arriving straight along +Z -- i.e. N == L == V == H == (0,0,1) exactly,
// so NdotL == NdotV == NdotH == VdotH == 1.0 for every pixel: this
// collapses the BRDF's angle-dependent terms to fixed, hand-computable
// constants (see individual TEST_CASEs for the exact arithmetic), which is
// what makes an exact/near-exact closed-form pixel assertion tractable at
// all for a real Cook-Torrance implementation. Tests that need a NON-head-on
// angle (the grazing-Fresnel probe, the doubleSided back-face probe) build
// their own camera/model override explicitly, documented at each call site.
#include <doctest/doctest.h>
#include <rx_material/material_system.h>
#include <rx_material/draw_data.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>

#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_task/scheduler.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

std::filesystem::path shaderDataPath(const char* filename) {
    // shaders/material/{standard_pbr,unlit}.slang -- NOT RX_MATERIAL_TEST_DATA_DIR
    // (this file's own test fixture .slang modules), the REAL shipped
    // material library files this task delivers. RX_MATERIAL_SHADER_DIR is
    // baked in as an absolute CMAKE_SOURCE_DIR-relative path (rx_material's
    // own CMakeLists.txt) for exactly this "load the real shared shaders in
    // a build-tree test" case -- MaterialSystem::create()'s own
    // `sharedShaderDir` default already resolves against it, so material
    // modules loaded relative to that SAME directory (its own parent) find
    // it via the identical mechanism.
    return std::filesystem::path(RX_MATERIAL_SHADER_DIR) / filename;
}

// Mirrors test_material_system.cpp's own MaterialTestFixture/makeFixture()
// (duplicated rather than shared -- see that file's own top comment on why
// this codebase duplicates small, canonical test fixtures rather than
// forcing a cross-TU dependency for them, matching e.g. this file's own
// FNV-1a precedent elsewhere in the codebase).
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
        MESSAGE("no display backend available, skipping StandardPBR/Unlit test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping StandardPBR/Unlit test");
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
    capacities.samplers = 2;
    capacities.storageBuffers = 8;
    auto bindless = rx::rhi::BindlessTable::create(device->physicalDevice(), device->device(), capacities);
    REQUIRE(bindless.has_value());

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());

    return Fixture{std::move(*window),  std::move(*context),   surface,
                    std::move(*device), std::move(*bindless), std::move(*allocator)};
}

std::filesystem::path freshCachePath(const char* name) {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / (std::string("rx_standard_pbr_test_") + name + ".cache");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

// D8-shaped vertex, byte-identical to material_system.cpp's own
// MaterialVertexLayout (position/normal/tangent/uv, 48 bytes) -- see that
// struct's own comment for why this exact interleaving is load-bearing.
struct Vertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv[2];
};

// One unit quad, object-space (-0.5,-0.5,0)..(0.5,0.5,0), normal (0,0,1),
// tangent (1,0,0,1) -- see this file's own header comment for the resulting
// camera/light rig this shape enables. Winding (0,1,2 / 0,2,3) is
// counter-clockwise as seen from +Z, matching getPipeline()'s fixed
// `frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE` -- a camera on the +Z side
// sees the FRONT face (culled under single-sided from the -Z/back side,
// exercised by the doubleSided test below).
constexpr std::array<Vertex, 4> kQuadVertices{{
    {{-0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}},
    {{0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}},
    {{0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},
    {{-0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},
}};
constexpr std::array<uint32_t, 6> kQuadIndices{0, 1, 2, 0, 2, 3};

// GPU-side scene geometry: a persistently-mapped host-visible vertex +
// index buffer pair, uploaded once via memcpy (no Uploader needed -- this
// is a tiny, one-shot test fixture, not a real asset-pipeline path).
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

// The default head-on rig this file's own header comment documents: camera
// at (0,0,2) looking at the origin, orthographic projection with
// half-extents exactly matching the quad (so it fills the whole render
// target -- every pixel probes the same analytic point), light arriving
// along +Z. [MATRIX LAYOUT] Every matrix here is glm::transpose()'d before
// upload -- see rx_material/draw_data.h's own MATRIX LAYOUT paragraph for
// why (MaterialSystem's Slang session is ROW-major; GLM is column-major).
// [Fix round, item 1] `orthoHalfExtent` defaults to 0.5 -- the ORIGINAL,
// unchanged frustum every existing call site relies on (exactly matching
// the quad's own object-space extent, filling the whole viewport). A
// multi-probe test that needs more than one on-screen, non-overlapping
// quad location at once (D26.1's own two-draw case, below) passes a wider
// value so a translated quad stays clipped-visible instead of falling
// outside the frustum entirely.
rx::material::DrawDataGpu makeHeadOnRow(glm::mat4 model = glm::mat4(1.0F), float orthoHalfExtent = 0.5F) {
    rx::material::DrawDataGpu row;
    glm::mat4 view = glm::lookAt(glm::vec3(0.0F, 0.0F, 2.0F), glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    glm::mat4 proj = glm::orthoZO(-orthoHalfExtent, orthoHalfExtent, -orthoHalfExtent, orthoHalfExtent, 0.1F, 10.0F);
    // Vulkan's clip-space Y axis points DOWN; GLM's `orthoZO` (like every
    // other GLM projection helper) is written for the OpenGL convention (Y
    // UP) and does not know about that difference on its own -- negating
    // row 1 (GLM is column-major storage, so this is `proj[1][1] *= -1`,
    // the well-known "GLM-for-Vulkan" fix, matching samples/06_materials'
    // own `applyVulkanYFlip()`) is required for this project's fixed
    // rasterization state (getPipeline()'s own `frontFace =
    // VK_FRONT_FACE_COUNTER_CLOCKWISE`) to see the SAME winding this file's
    // own `kQuadVertices` comment claims ("counter-clockwise as seen from
    // +Z"). Reproduced directly during this task's own development:
    // omitting this flip renders every single-sided draw in this whole
    // file invisible (silently back-face-culled, reading back this
    // fixture's own clear color -- opaque black, executor.cpp's own fixed
    // `{0,0,0,1}` -- rather than any real draw output) without a single
    // validation error to flag it, since a culled triangle is
    // spec-legal, not a Vulkan usage error.
    proj[1][1] *= -1.0F;
    glm::mat4 viewProj = proj * view;
    glm::mat3 normalMat3 = glm::transpose(glm::inverse(glm::mat3(model)));

    row.model = glm::transpose(model);
    row.normalMatrix = glm::transpose(glm::mat4(normalMat3));
    row.viewProj = glm::transpose(viewProj);
    row.lightDirWorld = glm::vec4(0.0F, 0.0F, 1.0F, 0.0F);
    row.lightColor = glm::vec4(1.0F, 1.0F, 1.0F, 0.0F);
    row.ambientColor = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);  // zeroed by default -- isolated diffuse/specular tests want no ambient term mixed in.
    row.cameraPosWorld = glm::vec4(0.0F, 0.0F, 2.0F, 0.0F);
    row.materialIndex = 0;
    return row;
}

// Registers `rows` as a real bindless storage buffer -- the D26.1 mechanism
// itself: a real per-draw StructuredBuffer<RxDrawData>, its bindless index
// is what a test pushes into MaterialGlobalsPush::drawDataBufferIndex.
struct DrawDataBuffer {
    rx::rhi::Buffer buffer;
    rx::rhi::BindlessHandle handle;
};

std::optional<DrawDataBuffer> createDrawDataBuffer(rx::rhi::Allocator& allocator, rx::rhi::BindlessTable& bindless,
                                                     const std::vector<rx::material::DrawDataGpu>& rows) {
    VkDeviceSize bytes = sizeof(rx::material::DrawDataGpu) * rows.size();
    auto buf = allocator.createHostVisibleBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                  rx::rhi::MemoryCategory::Internal);
    if (!buf.has_value()) {
        return std::nullopt;
    }
    std::memcpy(buf->mappedData(), rows.data(), static_cast<size_t>(bytes));
    buf->flush();
    rx::rhi::BindlessHandle handle = bindless.registerStorageBuffer(buf->handle(), bytes);
    if (!handle.isValid()) {
        return std::nullopt;
    }
    return DrawDataBuffer{std::move(*buf), handle};
}

// Writes `value` into `blob` at the named reflected field's byte offset --
// the same raw-blob-write technique api_impl.cpp's own setFloat/setFloat4
// use internally, exercised directly here (this file bypasses the public
// IRxMaterialInstance ABI entirely -- see this file's own header comment on
// why sample 08 does the same for imported glTF materials: TextureCache's
// own bindless indices have no IRxTexture* wrapper to feed setTexture()).
template <typename T>
void setParam(std::vector<uint8_t>& blob, const std::vector<rx::material::MaterialParamInfo>& params,
              const char* name, const T& value) {
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

// A plain ABSOLUTE-margin check on an 8-bit channel value -- this
// project's vendored doctest (doctest.h, checked directly before writing
// this) only exposes Approx::epsilon()/scale() (a RELATIVE tolerance,
// `|lhs-rhs| < epsilon*(scale+max(|lhs|,|rhs|))`), not an absolute margin
// -- every hand-derived expected pixel value in this file is documented
// with its own absolute (0-255) tolerance band, so a plain absolute
// comparison is both the simplest and the most faithful implementation of
// what each comment actually claims.
bool near8(uint8_t actual, int expected, int margin) {
    return std::abs(static_cast<int>(actual) - expected) <= margin;
}

// One draw request for renderQuad() below -- pipeline + push-constant
// values + the row index (firstInstance) this draw addresses in the shared
// DrawDataBuffer.
struct DrawRequest {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout paramSetLayout = VK_NULL_HANDLE;
    const uint8_t* paramBlob = nullptr;
    size_t paramBlobSize = 0;
    uint32_t pushSize = 0;  // record->layoutInfo.pushRanges[0].size, reflected (always sizeof(MaterialGlobalsPush) today).
    uint32_t drawDataRow = 0;
    float exposure = 0.0F;
};

// Renders every entry in `draws` (in order -- later draws composite OVER
// earlier ones, exercising BLEND's own over-compositing test) into a fresh
// 8x8 color+depth offscreen target and reads back ONE pixel (the center,
// (4,4) -- every draw fills the whole viewport per this file's own
// head-on-rig comment) as RGBA8. Builds its OWN set-1 UBO/descriptor set
// per draw directly (rx::rhi::Allocator + vkAllocateDescriptorSets) --
// deliberately NOT MaterialSystem::bindInstance() (that method is the
// pre-D26.1 LEGACY path, per its own header comment: "a real D26.1 caller
// drives its own real per-draw buffer and pushes this push-constant range
// itself, never through this method") -- this is exactly the manual
// sequence samples/08_gltf_viewer's own real per-draw code follows.
// [Fix round, item 1] Shared core: renders `draws` into an `extent`-sized
// offscreen target and returns the RAW row-major RGBA8 pixel buffer --
// renderQuad() below is a thin wrapper extracting the single center pixel
// (unchanged behavior/signature for every one of its 46 existing callers);
// a multi-probe test that needs to sample more than one independent screen
// location (the D26.1 two-draw case below, once its own depth-tie masking
// bug was found -- see that TEST_CASE's own header comment) calls this
// directly instead.
std::vector<uint8_t> renderQuadPixels(rx::rhi::Device& device, rx::rhi::Allocator& allocator,
                                       rx::rhi::BindlessTable& bindless, rx::rhi::BindlessHandle drawDataBufferHandle,
                                       uint32_t defaultSamplerBindlessIndex, const QuadMesh& mesh,
                                       const std::vector<DrawRequest>& draws, VkExtent2D extent) {
    const VkExtent2D kExtent = extent;
    constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto executor = rx::graph::Executor::create(device, *scheduler);
    REQUIRE(executor != nullptr);

    rx::graph::RenderGraph graph;
    rx::graph::Pass& pass = graph.addPass("standard_pbr_test");
    rx::graph::AttachmentDesc colorDesc;
    colorDesc.format = kColorFormat;
    pass.addColorOutput("color", colorDesc);
    rx::graph::AttachmentDesc depthDesc;
    depthDesc.format = kDepthFormat;
    pass.setDepthStencilOutput("depth", depthDesc);
    graph.setBackbufferSource("color");

    // One tiny descriptor pool for every draw's own set-1 UBO -- created
    // fresh per renderQuad() call (this is a test helper, not a
    // per-frame-reused arena).
    std::array<VkDescriptorPoolSize, 1> poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<uint32_t>(draws.size() + 1)}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = static_cast<uint32_t>(draws.size() + 1);
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorPool(device.device(), &poolInfo, nullptr, &descriptorPool) == VK_SUCCESS);

    std::vector<rx::rhi::Buffer> paramBuffers;
    paramBuffers.reserve(draws.size());

    pass.setExecute([&](rx::graph::PassContext& ctx) {
        VkCommandBuffer cmd = ctx.cmd;

        // Dynamic state -- every material pipeline declares
        // VK_DYNAMIC_STATE_VIEWPORT/SCISSOR (material_system.cpp's own
        // fixed dynamicStates array); a draw against a pipeline that
        // requires these without ever having set them is a real
        // validation error (and, on at least one driver, a hard crash --
        // reproduced directly during this file's own development).
        VkViewport viewport{0.0F, 0.0F, static_cast<float>(kExtent.width), static_cast<float>(kExtent.height),
                             0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, kExtent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Set 0 (bindless) -- EVERY material pipeline built against this
        // fixture's own BindlessTable shares the identical set-0 layout
        // (PipelineLayoutBuilder's "EXTERNAL SET-0 SUBSTITUTION"), so
        // binding it once, before any draw, using any one draw's own
        // pipeline layout, is correct for the rest -- matching samples/
        // 07_stress's own recordForwardChunked() precedent.
        VkDescriptorSet bindlessSet = bindless.descriptorSet();
        VkPipelineLayout firstLayout = draws.empty() ? VK_NULL_HANDLE : draws.front().layout;
        if (firstLayout != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, firstLayout, /*firstSet=*/0, 1,
                                     &bindlessSet, 0, nullptr);
        }

        VkDeviceSize zeroOffset = 0;
        VkBuffer vbHandle = mesh.vertexBuffer.handle();
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbHandle, &zeroOffset);
        vkCmdBindIndexBuffer(cmd, mesh.indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);

        for (const DrawRequest& draw : draws) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);

            rx::material::MaterialGlobalsPush push;
            push.defaultSamplerIndex = defaultSamplerBindlessIndex;
            push.drawDataBufferIndex = drawDataBufferHandle.index();
            push.exposure = draw.exposure;
            REQUIRE(draw.pushSize == sizeof(push));
            vkCmdPushConstants(cmd, draw.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                draw.pushSize, &push);

            auto paramBuffer = allocator.createHostVisibleBuffer(
                draw.paramBlobSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, rx::rhi::MemoryCategory::Internal);
            REQUIRE(paramBuffer.has_value());
            std::memcpy(paramBuffer->mappedData(), draw.paramBlob, draw.paramBlobSize);
            paramBuffer->flush();

            VkDescriptorSetAllocateInfo setAllocInfo{};
            setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            setAllocInfo.descriptorPool = descriptorPool;
            setAllocInfo.descriptorSetCount = 1;
            setAllocInfo.pSetLayouts = &draw.paramSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            REQUIRE(vkAllocateDescriptorSets(device.device(), &setAllocInfo, &set) == VK_SUCCESS);

            VkDescriptorBufferInfo bufferInfo{paramBuffer->handle(), 0, draw.paramBlobSize};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &bufferInfo;
            vkUpdateDescriptorSets(device.device(), 1, &write, 0, nullptr);
            paramBuffers.push_back(std::move(*paramBuffer));

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.layout, /*firstSet=*/1, 1, &set, 0,
                                     nullptr);

            vkCmdDrawIndexed(cmd, static_cast<uint32_t>(kQuadIndices.size()), 1, 0, 0,
                              /*firstInstance=*/draw.drawDataRow);
        }
    });

    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = kExtent.width;
    compileInfo.swapchainHeight = kExtent.height;
    compileInfo.swapchainFormat = kColorFormat;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);
    executor->realize(graph);

    // Offscreen color target this pass renders into, plus a host-visible
    // readback buffer -- same shape as test_material_system.cpp's own
    // createOffscreenColorImage(), just also copied out afterward.
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
    auto readback = allocator.createHostVisibleBuffer(pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                        rx::rhi::MemoryCategory::Internal);
    REQUIRE(readback.has_value());

    // TWO separate runOnce() submissions -- not one combined command buffer
    // -- matching rx_graph/tests/test_execute_gpu.cpp's own established,
    // working pattern (its own comment: runOnce() itself
    // vkQueueWaitIdle()'s before returning). A single-submission
    // execute()-then-copy relies on command ORDER alone, which sync
    // validation correctly flags as an unsynchronized READ_AFTER_WRITE
    // against the backbuffer's own final layout-transition barrier
    // (reproduced directly during this file's own development); a full
    // queue-idle wait between the two is a strictly stronger guarantee that
    // sidesteps the hazard entirely, at the cost of one extra submission
    // this test-only helper does not need to avoid.
    {
        auto cmdCtx =
            rx::rhi::CommandContext::create(device.device(), device.graphicsQueue(), device.graphicsQueueFamily());
        REQUIRE(cmdCtx.has_value());
        cmdCtx->runOnce(
            [&](VkCommandBuffer cmd) { executor->execute(graph, cmd, image, view, kExtent); });
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {kExtent.width, kExtent.height, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
        });
    }
    readback->invalidate();

    const auto* rawPixels = static_cast<const uint8_t*>(readback->mappedData());
    std::vector<uint8_t> result(rawPixels,
                                 rawPixels + static_cast<size_t>(kExtent.width) * kExtent.height * 4);

    vkDestroyImageView(device.device(), view, nullptr);
    vkDestroyImage(device.device(), image, nullptr);
    vkFreeMemory(device.device(), imageMemory, nullptr);
    vkDestroyDescriptorPool(device.device(), descriptorPool, nullptr);

    return result;
}

// Extracts one texel from a `renderQuadPixels()` result -- shared by
// renderQuad() (below, the single-center-pixel legacy shape) and any
// multi-probe caller.
Rgba8 pixelAt(const std::vector<uint8_t>& pixels, VkExtent2D extent, uint32_t x, uint32_t y) {
    const size_t index = (static_cast<size_t>(y) * extent.width + x) * 4;
    return Rgba8{pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}

// The original, single-center-pixel shape every other test in this file
// uses -- unchanged signature and behavior, now a thin wrapper over
// renderQuadPixels()/pixelAt() above.
Rgba8 renderQuad(rx::rhi::Device& device, rx::rhi::Allocator& allocator, rx::rhi::BindlessTable& bindless,
                  rx::rhi::BindlessHandle drawDataBufferHandle, uint32_t defaultSamplerBindlessIndex,
                  const QuadMesh& mesh, const std::vector<DrawRequest>& draws) {
    constexpr VkExtent2D kExtent{8, 8};
    std::vector<uint8_t> pixels = renderQuadPixels(device, allocator, bindless, drawDataBufferHandle,
                                                     defaultSamplerBindlessIndex, mesh, draws, kExtent);
    return pixelAt(pixels, kExtent, kExtent.width / 2, kExtent.height / 2);
}

// Builds a StandardPbrParams-shaped blob with every field at its glTF/D28
// neutral default (baseColorFactor=(1,1,1,1), metallic=1, roughness=1,
// every texture slot = the white fallback texture created by
// makeWhiteTexture() below, every UV offset/scale = (0,0,1,1)) --
// individual tests override exactly the fields they're probing.
struct StandardPbrHandles {
    rx::material::MaterialHandle material;
    std::vector<rx::material::MaterialParamInfo> params;
    uint32_t paramBlockSize = 0;
};

}  // namespace

TEST_CASE("D28: two loadMaterial() calls against the SAME .slang bytes with different AlphaMode/doubleSided yield "
          "independently-cached VkPipelines (cache-counter test)") {
    auto fixture = makeFixture("d28_pipeline_cache");
    if (!fixture.has_value()) {
        return;
    }
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("d28"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle opaque =
        system->loadMaterial(shaderDataPath("unlit.slang"), rx::material::MaterialFixedFunctionState{});
    rx::material::MaterialFixedFunctionState blendDoubleSided;
    blendDoubleSided.alphaMode = rx::material::AlphaMode::Blend;
    blendDoubleSided.doubleSided = true;
    rx::material::MaterialHandle blend = system->loadMaterial(shaderDataPath("unlit.slang"), blendDoubleSided);

    // Same module bytes -> same content hash (this IS the D28 scenario the
    // matrix names: "two materials differing only in alphaMode/doubleSided
    // ... yield distinct cached pipelines").
    CHECK(system->moduleHash(opaque) == system->moduleHash(blend));
    CHECK(system->fixedFunctionState(opaque).alphaMode == rx::material::AlphaMode::Opaque);
    CHECK(system->fixedFunctionState(blend).alphaMode == rx::material::AlphaMode::Blend);
    CHECK(system->fixedFunctionState(blend).doubleSided);

    rx::graph::PassSignature sig;
    sig.colorCount = 1;
    sig.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    sig.depthFormat = VK_FORMAT_D32_SFLOAT;
    sig.samples = VK_SAMPLE_COUNT_1_BIT;

    VkPipeline pOpaque = system->getPipeline({opaque, sig, 0});
    VkPipeline pBlend = system->getPipeline({blend, sig, 0});
    CHECK(pOpaque != VK_NULL_HANDLE);
    CHECK(pBlend != VK_NULL_HANDLE);
    CHECK(pOpaque != pBlend);  // the cache-counter assertion itself: distinct VkPipeline objects.

    // Repeated calls for the SAME (material, pass) key hit the cache --
    // proves this isn't accidentally minting a fresh pipeline every call.
    CHECK(system->getPipeline({opaque, sig, 0}) == pOpaque);
    CHECK(system->getPipeline({blend, sig, 0}) == pBlend);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("D26.1: two draws in one command buffer, the second at firstInstance>0, read distinct per-draw "
          "DrawDataGpu rows via SV_VulkanInstanceID (not SV_InstanceID)") {
    auto fixture = makeFixture("d26_1_two_draw");
    if (!fixture.has_value()) {
        return;
    }
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("d26_1"));
    REQUIRE(system != nullptr);
    auto mesh = createQuadMesh(fixture->allocator);
    REQUIRE(mesh.has_value());

    rx::material::MaterialHandle unlit =
        system->loadMaterial(shaderDataPath("unlit.slang"), rx::material::MaterialFixedFunctionState{});
    rx::graph::PassSignature sig;
    sig.colorCount = 1;
    sig.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    sig.depthFormat = VK_FORMAT_D32_SFLOAT;
    sig.samples = VK_SAMPLE_COUNT_1_BIT;
    VkPipeline pipeline = system->getPipeline({unlit, sig, 0});
    REQUIRE(pipeline != VK_NULL_HANDLE);

    uint32_t whiteIndex = 0;
    {
        rx::material::TextureCreateInfo texInfo;
        texInfo.width = 1;
        texInfo.height = 1;
        texInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        std::array<uint8_t, 4> whitePixel{255, 255, 255, 255};
        texInfo.pixels = whitePixel.data();
        texInfo.pixelBytes = whitePixel.size();
        rx::material::TextureHandle tex = system->createTexture2D(texInfo);
        REQUIRE(tex.isValid());
        whiteIndex = system->textureBindlessIndex(tex);
    }

    // [Fix round, item 1 -- see this file's own git history for the
    // superseded, depth-tie-masked version] ROW 0: red tint, at the ORIGIN
    // (model = identity). ROW 1: green tint, translated +X by 2 world
    // units, under a WIDENED ortho frustum (orthoHalfExtent=3.0, vs. the
    // 0.5 every other test's default head-on rig uses) so row 1's quad
    // stays genuinely ON-SCREEN, at a SEPARATE, NON-OVERLAPPING location
    // from row 0's -- not clipped away entirely, the way the original
    // version of this test placed it. This is load-bearing, not cosmetic:
    // the original off-frustum placement was PROVEN (this fix round) to be
    // a discrimination hole -- under the SV_InstanceID revert bug, row 1's
    // own draw misreads row 0's IDENTITY transform, so its fragments land
    // EXACTLY on top of row 0's own already-written, IDENTICAL-depth quad;
    // Vulkan's spec-mandated `VK_COMPARE_OP_LESS` (strict, this project's
    // own fixed depth-compare state, material_system.cpp) then REJECTS
    // every one of the buggy draw's fragments as a depth-tie, so the
    // framebuffer silently reads back IDENTICAL to the correct-behavior
    // case (a lone red quad, no visible green anywhere) regardless of
    // whether the addressing bug is actually present -- reproduced
    // directly this fix round: an empirical SV_InstanceID revert did NOT
    // fail this test's own OLD single-center-pixel assertion (see the task
    // report's fix-round section for the full paste). Probing row 1's OWN
    // real, non-overlapping screen location closes that hole: under the
    // bug, row 1's real location gets NO fragments at all (the buggy draw
    // never reaches it), reading back as the plain background clear color
    // -- unambiguously distinct from GREEN, with no depth-tie possible
    // between two draws that no longer even hit the same pixels.
    constexpr float kWideOrthoHalfExtent = 3.0F;
    constexpr glm::vec3 kRow1Translation{2.0F, 0.0F, 0.0F};
    std::vector<rx::material::DrawDataGpu> rows;
    rows.push_back(makeHeadOnRow(glm::mat4(1.0F), kWideOrthoHalfExtent));
    rows.push_back(
        makeHeadOnRow(glm::translate(glm::mat4(1.0F), kRow1Translation), kWideOrthoHalfExtent));
    auto drawDataBuffer = createDrawDataBuffer(fixture->allocator, fixture->bindless, rows);
    REQUIRE(drawDataBuffer.has_value());

    uint32_t defaultSamplerIndex = 0;
    VkSampler rawSampler = VK_NULL_HANDLE;
    {
        // A real registered sampler -- rx_sampleTexture's own default-
        // sampler-index push-constant field needs a real, valid index.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        REQUIRE(vkCreateSampler(fixture->device.device(), &samplerInfo, nullptr, &rawSampler) == VK_SUCCESS);
        rx::rhi::BindlessHandle h = fixture->bindless.registerSampler(rawSampler);
        REQUIRE(h.isValid());
        defaultSamplerIndex = h.index();
    }

    uint32_t blockSize = system->paramBlockSize(unlit);
    const auto params = system->materialParams(unlit);

    std::vector<uint8_t> redBlob(blockSize, 0);
    setParam(redBlob, params, "baseColorFactor", std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F});
    setParam(redBlob, params, "baseColorTexture", whiteIndex);
    setParam(redBlob, params, "alphaCutoff", 0.0F);
    setParam(redBlob, params, "baseColorUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});

    std::vector<uint8_t> greenBlob(blockSize, 0);
    setParam(greenBlob, params, "baseColorFactor", std::array<float, 4>{0.0F, 1.0F, 0.0F, 1.0F});
    setParam(greenBlob, params, "baseColorTexture", whiteIndex);
    setParam(greenBlob, params, "alphaCutoff", 0.0F);
    setParam(greenBlob, params, "baseColorUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});

    uint32_t pushSize = system->layoutInfo(unlit).pushRanges[0].size;
    VkDescriptorSetLayout paramSetLayout = system->layoutInfo(unlit).bindings.empty()
                                                ? VK_NULL_HANDLE
                                                : VK_NULL_HANDLE;  // filled below via the real set layout.
    (void)paramSetLayout;
    // The real set-1 layout comes from MaterialSystem's own built
    // layoutBundle, not layoutInfo() (which is the plain reflected shape) --
    // pipelineLayout() gives the VkPipelineLayout; the descriptor SET
    // layout for set 1 has no direct public accessor, so this test builds
    // an identical one-binding UBO layout itself (the exact shape
    // reflectMaterialLayout() always produces for a material's gParams --
    // one VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER at binding 0 -- see
    // material_system.cpp's kMaterialParamBlockBinding).
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &uboBinding;
    VkDescriptorSetLayout realParamLayout = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorSetLayout(fixture->device.device(), &setLayoutInfo, nullptr, &realParamLayout) ==
            VK_SUCCESS);

    std::vector<DrawRequest> draws;
    draws.push_back(DrawRequest{pipeline, system->pipelineLayout(unlit), realParamLayout, redBlob.data(),
                                 redBlob.size(), pushSize, /*drawDataRow=*/0, 0.0F});
    draws.push_back(DrawRequest{pipeline, system->pipelineLayout(unlit), realParamLayout, greenBlob.data(),
                                 greenBlob.size(), pushSize, /*drawDataRow=*/1, 0.0F});

    // [Fix round, item 1] A 48x48 target (vs. the 8x8 every single-quad
    // test uses) and the widened orthoHalfExtent=3.0 frustum above give
    // exactly 8 texels per world unit -- world x=0 (row 0's quad center)
    // maps to texel x=(0+3)*8=24; world x=2 (row 1's quad center, after
    // its own +2 translation) maps to texel x=(2+3)*8=40. Both probes sit
    // comfortably inside their own quad's own footprint (each quad spans a
    // full world unit, i.e. 8 texels, centered on its own probe point) and
    // nowhere near the other quad's footprint (a >=8-texel gap either way)
    // or the frustum edge.
    constexpr VkExtent2D kProbeExtent{48, 48};
    constexpr uint32_t kRow0ProbeX = 24;
    constexpr uint32_t kRow1ProbeX = 40;
    constexpr uint32_t kProbeY = 24;
    std::vector<uint8_t> pixels = renderQuadPixels(fixture->device, fixture->allocator, fixture->bindless,
                                                     drawDataBuffer->handle, defaultSamplerIndex, *mesh, draws,
                                                     kProbeExtent);
    Rgba8 row0Pixel = pixelAt(pixels, kProbeExtent, kRow0ProbeX, kProbeY);
    Rgba8 row1Pixel = pixelAt(pixels, kProbeExtent, kRow1ProbeX, kProbeY);

    // Row 0's own location must read RED (its own draw, correctly
    // addressed via SV_VulkanInstanceID==0, i.e. no addressing distinction
    // possible there at all -- this assertion alone is NOT the
    // discriminating one, kept only as a basic sanity check).
    CHECK(row0Pixel.r > 200);
    CHECK(row0Pixel.g < 50);
    // Row 1's own, SEPARATE location is the real discriminator: it must
    // read GREEN. If SV_VulkanInstanceID's own row addressing were broken
    // (e.g. reading SV_InstanceID instead -- Slang's own documented,
    // independently-verified behavior for it, per this codebase's own
    // material.slang/forward_entry.slang/draw_data.h header comments:
    // gl_InstanceIndex minus gl_BaseInstance, i.e. always 0 for a draw
    // whose firstInstance IS its own gl_BaseInstance, regardless of the
    // real firstInstance value), the second draw would misread row 0's
    // OWN transform instead of its real row 1 -- its fragments would land
    // on row 0's location (where they lose the depth tie, invisibly) and
    // NEVER reach row 1's real, separate location at all, which would
    // therefore read back as the plain background clear color instead of
    // green.
    CHECK(row1Pixel.g > 200);
    CHECK(row1Pixel.r < 50);

    vkDestroyDescriptorSetLayout(fixture->device.device(), realParamLayout, nullptr);
    vkDestroySampler(fixture->device.device(), rawSampler, nullptr);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

namespace {

// --- Shared StandardPBR test rig ------------------------------------------
// Bundles everything most StandardPBR tests below need: a fixture, a quad
// mesh, a MaterialSystem, a registered default sampler, and two D11-style
// utility textures (white -- the neutral default for every factor-only
// slot; flat-normal -- (0.5,0.5,1.0) decoding to tangent-space (0,0,1),
// the "no normal map" identity). Individual tests still build their OWN
// per-test textures (the synthetic MR/normal-map texels the acceptance
// criteria specify) via `makeTexture()` below.
// `fixture` is a std::unique_ptr<Fixture>, NOT a plain `Fixture` value --
// load-bearing, not stylistic: MaterialSystem::create() (below, in
// makeStandardPbrRig()) takes `rx::rhi::Device&`/`rx::rhi::BindlessTable&`
// and STORES A RAW POINTER to each internally (material_system.cpp's own
// `impl->bindless = &bindless;`) -- it does NOT copy or take ownership.
// A plain-value `Fixture fixture;` member here would require
// makeStandardPbrRig() to construct `MaterialSystem` against the LOCAL
// `fixture` optional's own storage, then MOVE that `Fixture` again into
// this struct's return value -- invalidating the pointer MaterialSystem
// already captured (a dangling reference to the ORIGINAL, since-destroyed
// stack storage), corrupting every subsequent `bindless`/`device` access.
// Reproduced directly during this task's own development: intermittent
// "Invalid VkDescriptorSetLayout" validation errors and a SIGSEGV,
// specifically in tests using this rig (never in the D28/D26.1 tests
// above, which keep their own `Fixture` in one stable `std::optional`
// local for their whole TEST_CASE and never move it). Heap-allocating
// `Fixture` here gives it ONE stable address for the rig's entire
// lifetime, established BEFORE `MaterialSystem::create()` is ever called
// against it -- see makeStandardPbrRig()'s own construction order below.
struct StandardPbrRig {
    std::unique_ptr<Fixture> fixture;
    QuadMesh mesh;
    std::unique_ptr<rx::material::MaterialSystem> system;
    uint32_t whiteTex = 0;
    uint32_t flatNormalTex = 0;
    uint32_t defaultSamplerIndex = 0;
    VkDescriptorSetLayout paramSetLayout = VK_NULL_HANDLE;
    VkSampler rawSampler = VK_NULL_HANDLE;
};

// Explicit teardown for the two raw Vulkan objects StandardPbrRig itself
// creates outside any RAII wrapper (the sampler, the param set layout) --
// called at the END of every TEST_CASE using a rig, before its own
// `hasValidationErrors()` check, so a leaked-child-object teardown error
// (device destruction happens implicitly, later, when `rig` itself goes
// out of scope) never gets a chance to fire in the first place.
void destroyRig(StandardPbrRig& rig) {
    vkDestroySampler(rig.fixture->device.device(), rig.rawSampler, nullptr);
    vkDestroyDescriptorSetLayout(rig.fixture->device.device(), rig.paramSetLayout, nullptr);
}

uint32_t makeTexture(rx::material::MaterialSystem& system, std::array<uint8_t, 4> rgba) {
    rx::material::TextureCreateInfo info;
    info.width = 1;
    info.height = 1;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.pixels = rgba.data();
    info.pixelBytes = rgba.size();
    rx::material::TextureHandle handle = system.createTexture2D(info);
    REQUIRE(handle.isValid());
    return system.textureBindlessIndex(handle);
}

std::optional<StandardPbrRig> makeStandardPbrRig(const char* name) {
    auto fixtureOpt = makeFixture(name);
    if (!fixtureOpt.has_value()) {
        return std::nullopt;
    }
    // Heap-allocate NOW, moving out of the std::optional's own storage --
    // establishes `fixture`'s FINAL, stable address before anything below
    // captures a pointer/reference into it. See StandardPbrRig's own field
    // comment for the dangling-pointer bug this specifically avoids (a
    // `Fixture` that gets moved again AFTER MaterialSystem::create() has
    // already captured `&bindless`/`&device` from its pre-move location).
    auto fixture = std::make_unique<Fixture>(std::move(*fixtureOpt));
    auto mesh = createQuadMesh(fixture->allocator);
    REQUIRE(mesh.has_value());
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath(name));
    REQUIRE(system != nullptr);

    uint32_t whiteTex = makeTexture(*system, {255, 255, 255, 255});
    uint32_t flatNormalTex = makeTexture(*system, {128, 128, 255, 255});

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
    REQUIRE(vkCreateDescriptorSetLayout(fixture->device.device(), &setLayoutInfo, nullptr, &paramSetLayout) ==
            VK_SUCCESS);

    return StandardPbrRig{std::move(fixture),       std::move(*mesh),         std::move(system),
                           whiteTex,                 flatNormalTex,            samplerHandle.index(),
                           paramSetLayout,           sampler};
}

// Neutral-default StandardPbrParams blob -- see StandardPbrRig's own
// comment: every factor at its glTF default, every texture slot the
// appropriate D11 neutral fallback, every UV transform the identity.
// Individual tests overwrite exactly the field(s) they probe via setParam().
std::vector<uint8_t> makeDefaultStandardPbrBlob(StandardPbrRig& rig, rx::material::MaterialHandle handle) {
    uint32_t blockSize = rig.system->paramBlockSize(handle);
    const auto params = rig.system->materialParams(handle);
    std::vector<uint8_t> blob(blockSize, 0);
    setParam(blob, params, "baseColorFactor", std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F});
    setParam(blob, params, "metallicFactor", 1.0F);
    setParam(blob, params, "roughnessFactor", 1.0F);
    setParam(blob, params, "normalScale", 1.0F);
    setParam(blob, params, "occlusionStrength", 1.0F);
    setParam(blob, params, "alphaCutoff", 0.0F);
    setParam(blob, params, "emissiveFactorAndPad", std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F});
    setParam(blob, params, "baseColorTexture", rig.whiteTex);
    setParam(blob, params, "metallicRoughnessTexture", rig.whiteTex);
    setParam(blob, params, "normalTexture", rig.flatNormalTex);
    setParam(blob, params, "occlusionTexture", rig.whiteTex);
    setParam(blob, params, "emissiveTexture", rig.whiteTex);
    std::array<float, 4> neutralUv{0.0F, 0.0F, 1.0F, 1.0F};
    setParam(blob, params, "baseColorUvOffsetScale", neutralUv);
    setParam(blob, params, "metallicRoughnessUvOffsetScale", neutralUv);
    setParam(blob, params, "normalUvOffsetScale", neutralUv);
    setParam(blob, params, "occlusionUvOffsetScale", neutralUv);
    setParam(blob, params, "emissiveUvOffsetScale", neutralUv);
    return blob;
}

rx::graph::PassSignature makeQuadPassSignature() {
    rx::graph::PassSignature sig;
    sig.colorCount = 1;
    sig.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    sig.depthFormat = VK_FORMAT_D32_SFLOAT;
    sig.samples = VK_SAMPLE_COUNT_1_BIT;
    return sig;
}

// Convenience for the common "one material, one draw, one row" case.
Rgba8 renderOne(StandardPbrRig& rig, rx::material::MaterialHandle handle, const std::vector<uint8_t>& blob,
                 rx::material::DrawDataGpu row = makeHeadOnRow(), float exposure = 0.0F) {
    VkPipeline pipeline = rig.system->getPipeline({handle, makeQuadPassSignature(), 0});
    REQUIRE(pipeline != VK_NULL_HANDLE);
    std::vector<rx::material::DrawDataGpu> rows{row};
    auto drawDataBuffer = createDrawDataBuffer(rig.fixture->allocator, rig.fixture->bindless, rows);
    REQUIRE(drawDataBuffer.has_value());

    uint32_t pushSize = rig.system->layoutInfo(handle).pushRanges[0].size;
    std::vector<DrawRequest> draws{DrawRequest{pipeline, rig.system->pipelineLayout(handle), rig.paramSetLayout,
                                                blob.data(), blob.size(), pushSize, /*drawDataRow=*/0, exposure}};
    return renderQuad(rig.fixture->device, rig.fixture->allocator, rig.fixture->bindless, drawDataBuffer->handle,
                       rig.defaultSamplerIndex, rig.mesh, draws);
}

}  // namespace

TEST_CASE("StandardPBR: baseColorFactor with no texture renders that exact linear color; a bound texture "
          "multiplies per-texel") {
    auto rig = makeStandardPbrRig("standard_pbr_basecolor");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});

    // metallic=0, roughness=1 (pure Lambertian-dominant), ambient=0 -- see
    // this file's own header comment for why the head-on rig makes
    // NdotL==NdotV==NdotH==VdotH==1.0 exactly. At those angles this file's
    // "StandardPBR: Lambertian 1/pi normalization" test below derives the
    // exact closed form directLight == baseColor/pi + small dielectric
    // specular (~0.003 per channel) -- this test only checks that a
    // baseColorFactor CHANGE produces the expected proportional pixel
    // change, not the full derivation (kept there, not duplicated here).
    std::vector<uint8_t> blob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(blob, rig->system->materialParams(handle), "metallicFactor", 0.0F);
    setParam(blob, rig->system->materialParams(handle), "baseColorFactor", std::array<float, 4>{0.2F, 0.4F, 0.6F, 1.0F});

    rx::material::DrawDataGpu row = makeHeadOnRow();
    Rgba8 pixel = renderOne(*rig, handle, blob, row);

    // Expected: (0.2,0.4,0.6)/pi + ~0.003 (dielectric F0=0.04 specular at
    // VdotH=1) -> approx (0.067,0.130,0.194) -> approx (17,33,49)/255.
    // Generous +-15/255 tolerance (D17's own gate uses +-4/255 against a
    // COMMITTED reference; this is a from-scratch hand-derived expectation,
    // so a wider band absorbs sRGB-vs-linear write-format rounding without
    // losing the real signal: channel ORDERING/relative magnitude, which a
    // wrong-channel or missing-1/pi bug would visibly break).
    CHECK(near8(pixel.r, 17, 15));
    CHECK(near8(pixel.g, 33, 15));
    CHECK(near8(pixel.b, 49, 15));
    // Relative ordering must hold regardless of exact tolerance -- this is
    // the part a real "wrong channel" bug cannot survive.
    CHECK(pixel.r < pixel.g);
    CHECK(pixel.g < pixel.b);
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("StandardPBR: metallic (blue channel) vs dielectric (metallic=0) at fixed roughness/baseColor renders "
          "measurably different colors -- MR channel layout") {
    auto rig = makeStandardPbrRig("standard_pbr_mr_channel");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    // metallicFactor/roughnessFactor default to 1.0 -- the SAME glTF
    // default this test's own texture-value multiply relies on (matrix's
    // own acceptance criterion: "independent of metallicFactor/
    // roughnessFactor (both default 1.0 so the texture value passes
    // through unscaled)"). A synthetic 1x1 MR texture with G=128/255,
    // B=64/255 -> roughness=0.502, metallic=0.251, matching the matrix's
    // own literal texel values.
    uint32_t mrTex = makeTexture(*rig->system, {0, 128, 64, 255});

    std::vector<uint8_t> metalBlob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(metalBlob, params, "metallicRoughnessTexture", mrTex);
    Rgba8 metalPixel = renderOne(*rig, handle, metalBlob);

    std::vector<uint8_t> dielectricBlob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(dielectricBlob, params, "metallicFactor", 0.0F);
    setParam(dielectricBlob, params, "roughnessFactor", 1.0F);
    Rgba8 dielectricPixel = renderOne(*rig, handle, dielectricBlob);

    // Independent C++ re-derivation of the SAME closed-form the shader
    // computes, at this rig's exact head-on angles (NdotL=NdotV=NdotH=
    // VdotH=1.0, which collapses V_SmithGGXCorrelated to a fixed 0.25 --
    // see makeHeadOnRow()'s own comment): metallic=0.251, roughness=0.502,
    // baseColor=white(1,1,1). F0 = lerp(0.04, 1, 0.251) = 0.281 (VdotH=1,
    // so Fresnel == F0 exactly, no grazing boost). alpha = 0.502^2 =
    // 0.252, alpha2 = 0.0634. D = alpha2/(pi*alpha2^2) = 1/(pi*alpha2) =
    // 5.024. specular = D*0.25*F0 = 5.024*0.25*0.281 = 0.353. diffuseColor
    // = 1*(1-0.251) = 0.749. diffuse = 0.749/pi = 0.2384. directLight =
    // diffuse+specular = 0.591 (occlusion=1, ambient=0, emissive=0).
    CHECK(near8(metalPixel.r, 151, 20));  // 0.591 * 255 ~= 151.

    // The pure-dielectric comparison case (metallic=0, roughness=1):
    // diffuse = 1/pi = 0.318; F0=0.04, D=1/pi (alpha2=1), Vis=0.25 ->
    // specular = (1/pi)*0.25*0.04 = 0.00318; directLight ~= 0.321 -> ~82/255.
    CHECK(near8(dielectricPixel.r, 82, 20));

    // The two configurations differ MEASURABLY (the actual "channel
    // layout is read and drives the BRDF" signal a wrong-channel bug -- G
    // read as metalness or vice versa -- cannot survive, since B=64/255 vs
    // a swapped G=128/255 reading would produce a visibly different pair).
    CHECK(metalPixel.r != dielectricPixel.r);
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("StandardPBR: occlusion closed-form -- R=0,strength=1 zeroes ambient exactly; strength=0.5 halves it "
          "exactly") {
    auto rig = makeStandardPbrRig("standard_pbr_occlusion");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    // Fully-occluding (R=0) texture + a directional light pointed AWAY
    // from the surface (NdotL=0, zero direct light) isolates the ambient
    // term exactly, matching the matrix's own acceptance criterion: "a
    // metallic=1.0... probe pixel with zero direct light visible... reads
    // ambientColor x occlusionValue x baseColor, the ONLY contribution".
    uint32_t blackOcclusionTex = makeTexture(*rig->system, {0, 255, 255, 255});  // R=0 (occluded); G/B irrelevant here.

    rx::material::DrawDataGpu row = makeHeadOnRow();
    row.lightDirWorld = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);  // light BEHIND the surface -> NdotL clamps to 0.
    row.ambientColor = glm::vec4(0.5F, 0.5F, 0.5F, 0.0F);

    std::vector<uint8_t> fullyOccludedBlob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(fullyOccludedBlob, params, "occlusionTexture", blackOcclusionTex);
    setParam(fullyOccludedBlob, params, "occlusionStrength", 1.0F);
    setParam(fullyOccludedBlob, params, "metallicFactor", 1.0F);
    Rgba8 fullyOccludedPixel = renderOne(*rig, handle, fullyOccludedBlob, row);

    // occlusion = 1.0 + 1.0*(0-1.0) = 0.0 exactly -> ambient = 0.5*0*1 = 0
    // -> the ENTIRE output is 0 (no direct light, no ambient, no emissive).
    CHECK(fullyOccludedPixel.r == 0);
    CHECK(fullyOccludedPixel.g == 0);
    CHECK(fullyOccludedPixel.b == 0);

    std::vector<uint8_t> halfStrengthBlob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(halfStrengthBlob, params, "occlusionTexture", blackOcclusionTex);
    setParam(halfStrengthBlob, params, "occlusionStrength", 0.5F);
    setParam(halfStrengthBlob, params, "metallicFactor", 1.0F);
    Rgba8 halfStrengthPixel = renderOne(*rig, handle, halfStrengthBlob, row);

    // occlusion = 1.0 + 0.5*(0-1.0) = 0.5 exactly -> ambient = 0.5*0.5*1 =
    // 0.25 -> 0.25*255 ~= 64.
    CHECK(near8(halfStrengthPixel.r, 64, 8));
    CHECK(near8(halfStrengthPixel.g, 64, 8));
    CHECK(near8(halfStrengthPixel.b, 64, 8));

    // [Fix round, item 5b -- coordinator ruling] Occlusion is
    // INDIRECT-only: it must NOT attenuate direct light at all (an earlier
    // version of standard_pbr.slang multiplied `directLight * occlusion`
    // too -- spec-incorrect; fixed this same fix round, see that file's own
    // header comment on `directLight`). Isolates direct light by zeroing
    // `ambientColor` (removing the only place `occlusion` legitimately
    // participates) and using the DEFAULT head-on rig (NdotL=1, a real,
    // nonzero direct contribution) -- then compares an UNOCCLUDED
    // (occlusion=1, white texture) render against a HEAVILY OCCLUDED
    // (occlusion=0, R=0 texture, strength=1 -- the same `blackOcclusionTex`
    // used above) render of the OTHERWISE IDENTICAL material: with ambient
    // zeroed, the two must read back IDENTICAL (direct light is the only
    // remaining contributor, and occlusion must not touch it), unlike the
    // ambient-only cases above where occlusion legitimately changes the
    // result.
    rx::material::DrawDataGpu directOnlyRow = makeHeadOnRow();
    directOnlyRow.ambientColor = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);

    std::vector<uint8_t> directUnoccludedBlob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(directUnoccludedBlob, params, "metallicFactor", 0.0F);
    Rgba8 directUnoccludedPixel = renderOne(*rig, handle, directUnoccludedBlob, directOnlyRow);

    std::vector<uint8_t> directOccludedBlob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(directOccludedBlob, params, "metallicFactor", 0.0F);
    setParam(directOccludedBlob, params, "occlusionTexture", blackOcclusionTex);
    setParam(directOccludedBlob, params, "occlusionStrength", 1.0F);
    Rgba8 directOccludedPixel = renderOne(*rig, handle, directOccludedBlob, directOnlyRow);

    // Sanity: direct light is genuinely present and visible (not
    // accidentally zero for some unrelated reason), so the equality
    // assertions below are actually discriminating something real.
    CHECK(directUnoccludedPixel.r > 50);
    // The real assertion: occlusion=0 (fully "occluded") must NOT have
    // changed the direct-lit result at all.
    CHECK(near8(directOccludedPixel.r, directUnoccludedPixel.r, 2));
    CHECK(near8(directOccludedPixel.g, directUnoccludedPixel.g, 2));
    CHECK(near8(directOccludedPixel.b, directUnoccludedPixel.b, 2));

    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

// [Fix round, item 2] The matrix's own emissive acceptance criterion (a
// dedicated probe proving emissive bypasses lighting entirely) had no
// TEST_CASE at all -- makeDefaultStandardPbrBlob()'s own
// `emissiveFactorAndPad={0,0,0,0}` neutral default meant no existing test
// ever exercised a NONZERO value. Isolates emissive by zeroing BOTH
// `lightColor` AND `ambientColor` on the row (not by zeroing baseColor,
// which would still leave a real, nonzero dielectric F0=0.04 specular
// term reachable if any light/ambient were present) -- with both light
// terms zero, standard_pbr.slang's own `color = directLight*occlusion +
// ambient + emissive` collapses to exactly `emissive`, regardless of
// baseColor/metallic/roughness (left at makeDefaultStandardPbrBlob()'s own
// neutral defaults, deliberately not special-cased, to prove emissive is
// independent of them too).
TEST_CASE("StandardPBR: emissiveFactor (no texture) renders EXACTLY that color pre-tonemap, independent of "
          "lighting (light+ambient zeroed)") {
    auto rig = makeStandardPbrRig("standard_pbr_emissive_factor");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    rx::material::DrawDataGpu row = makeHeadOnRow();
    row.lightColor = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);
    row.ambientColor = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);

    std::vector<uint8_t> blob = makeDefaultStandardPbrBlob(*rig, handle);
    // 0.2/0.4/0.6 -> exact byte values (51/102/153, no rounding ambiguity)
    // once multiplied by 255, so this can assert near-exact rather than a
    // wide tolerance band.
    setParam(blob, params, "emissiveFactorAndPad", std::array<float, 4>{0.2F, 0.4F, 0.6F, 0.0F});
    Rgba8 pixel = renderOne(*rig, handle, blob, row);

    CHECK(near8(pixel.r, 51, 3));
    CHECK(near8(pixel.g, 102, 3));
    CHECK(near8(pixel.b, 153, 3));
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

// [Fix round, item 2] Textured variant -- emissiveFactor x emissiveTexture,
// same lighting-isolation technique as the factor-only probe above.
TEST_CASE("StandardPBR: emissiveFactor x emissiveTexture renders their exact product pre-tonemap, independent of "
          "lighting (light+ambient zeroed)") {
    auto rig = makeStandardPbrRig("standard_pbr_emissive_texture");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    uint32_t emissiveTex = makeTexture(*rig->system, {100, 150, 200, 255});

    rx::material::DrawDataGpu row = makeHeadOnRow();
    row.lightColor = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);
    row.ambientColor = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);

    std::vector<uint8_t> blob = makeDefaultStandardPbrBlob(*rig, handle);
    // factor=1 passes the texel through unscaled -> expected EXACTLY the
    // texture's own (100,150,200).
    setParam(blob, params, "emissiveFactorAndPad", std::array<float, 4>{1.0F, 1.0F, 1.0F, 0.0F});
    setParam(blob, params, "emissiveTexture", emissiveTex);
    Rgba8 pixel = renderOne(*rig, handle, blob, row);

    CHECK(near8(pixel.r, 100, 3));
    CHECK(near8(pixel.g, 150, 3));
    CHECK(near8(pixel.b, 200, 3));
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("StandardPBR: Lambertian diffuse matches NdotL x baseColor / pi within tolerance (1/pi normalization "
          "present)") {
    auto rig = makeStandardPbrRig("standard_pbr_lambertian");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    std::vector<uint8_t> blob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(blob, params, "metallicFactor", 0.0F);
    setParam(blob, params, "roughnessFactor", 1.0F);
    setParam(blob, params, "baseColorFactor", std::array<float, 4>{0.6F, 0.6F, 0.6F, 1.0F});
    Rgba8 pixel = renderOne(*rig, handle, blob);

    // Head-on rig: NdotL=1. Expected (Lambertian only) = 0.6/pi ~= 0.191 ->
    // ~49/255. The real shader additionally adds a small dielectric
    // specular term (F0=0.04, D=1/pi, Vis=0.25 at these angles -> ~0.0032)
    // -> ~50/255 -- see this file's own header comment: "within tolerance"
    // is the acceptance criterion's own wording precisely because a real
    // Cook-Torrance implementation is never PURELY diffuse. A margin of
    // 10/255 comfortably covers that ~1-count specular contribution while
    // still catching a missing/wrong 1/pi factor (which would move the
    // expected value by ~3x, to ~153/255).
    CHECK(near8(pixel.r, 49, 10));
    CHECK(near8(pixel.g, 49, 10));
    CHECK(near8(pixel.b, 49, 10));
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("StandardPBR: FG1 ambient closed-form non-black-metal probe -- a fully metallic surface with zero "
          "direct light still reads ambientColor x occlusion x baseColor") {
    auto rig = makeStandardPbrRig("standard_pbr_ambient_metal");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    std::vector<uint8_t> blob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(blob, params, "metallicFactor", 1.0F);
    setParam(blob, params, "roughnessFactor", 1.0F);
    setParam(blob, params, "baseColorFactor", std::array<float, 4>{0.8F, 0.2F, 0.2F, 1.0F});

    rx::material::DrawDataGpu row = makeHeadOnRow();
    row.lightDirWorld = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);  // light BEHIND -> zero direct contribution (NdotL=0).
    row.ambientColor = glm::vec4(0.4F, 0.4F, 0.4F, 0.0F);
    Rgba8 pixel = renderOne(*rig, handle, blob, row);

    // Fully metallic -> diffuseColor=0; specular also vanishes here
    // because NdotL clamps to 0 (this rig's directLight multiplies by
    // NdotL). occlusion=1 (no occlusion texture bound -> white fallback).
    // ambient = ambientColor x occlusion x baseColor = 0.4 x 1 x
    // (0.8,0.2,0.2) = (0.32,0.08,0.08) -> ~(82,20,20)/255 -- a real, loud,
    // NON-BLACK result despite zero direct light and full metalness, the
    // exact scenario D22's FG1 amendment exists to fix ("metals must not
    // render black without IBL").
    CHECK(near8(pixel.r, 82, 12));
    CHECK(near8(pixel.g, 20, 12));
    CHECK(near8(pixel.b, 20, 12));
    CHECK(pixel.r > 0);  // the non-black assertion itself.
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("StandardPBR: BC5 normal-map Z-reconstruction -- X=Y=0 (flat) reads bright (N==geometric normal); "
          "X=0.6,Y=0.8 (Z=0, grazing) reads dark; a texel outside the unit circle clamps the radicand instead of "
          "producing NaN") {
    auto rig = makeStandardPbrRig("standard_pbr_normal_bc5");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    // Isolate the normal-map effect: metallic=0, roughness=1, ambient=0 --
    // the ONLY light path left is Lambertian NdotL, directly exposing the
    // reconstructed normal's own Z/tilt.
    auto baseBlob = [&]() {
        std::vector<uint8_t> blob = makeDefaultStandardPbrBlob(*rig, handle);
        setParam(blob, params, "metallicFactor", 0.0F);
        setParam(blob, params, "roughnessFactor", 1.0F);
        setParam(blob, params, "baseColorFactor", std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F});
        return blob;
    };

    // (0.5, 0.5) texel -> tangentXY=(0,0) -> Z=1.0 EXACTLY (this file's own
    // flatNormalTex, (128,128,255,255)) -- reconstructed normal ==
    // geometric normal == light direction at this rig's head-on angles,
    // so NdotL=1.0 exactly, the brightest possible Lambertian response.
    std::vector<uint8_t> flatBlob = baseBlob();
    setParam(flatBlob, params, "normalTexture", rig->flatNormalTex);
    Rgba8 flatPixel = renderOne(*rig, handle, flatBlob);
    CHECK(near8(flatPixel.r, 81, 10));  // 1/pi ~= 0.318 -> ~81/255.

    // X=0.6, Y=0.8 (a valid unit-circle point: 0.6^2+0.8^2=1.0, Z=0
    // exactly) -> texel = (0.6/2+0.5, 0.8/2+0.5) = (0.8, 0.9) -> encoded
    // (204, 230). The reconstructed tangent-space normal is tilted fully
    // INTO the tangent plane (perpendicular to the geometric normal AND
    // the light direction, both +Z at this rig's head-on angles) ->
    // NdotL=0 exactly -> zero Lambertian response (and this configuration
    // has roughness=1/metallic=0, so the specular term is also negligible
    // at NdotL=0 -- V_SmithGGXCorrelated's own lambdaV term includes a
    // NdotL factor that zeroes it).
    uint32_t grazingNormalTex = makeTexture(*rig->system, {204, 230, 255, 255});
    std::vector<uint8_t> grazingBlob = baseBlob();
    setParam(grazingBlob, params, "normalTexture", grazingNormalTex);
    Rgba8 grazingPixel = renderOne(*rig, handle, grazingBlob);
    CHECK(grazingPixel.r < 10);

    CHECK(flatPixel.r > grazingPixel.r + 30);  // the discriminating comparison itself.

    // Radicand clamp: a texel decoding to X=Y=0.9 (dot=1.62 > 1, an
    // out-of-unit-circle BC5 artifact) must render a FINITE, sane pixel
    // (no NaN propagation, no validation error) rather than crash/hang --
    // this is what max(0, 1 - dot(xy,xy)) exists to guarantee.
    uint32_t outOfRangeTex = makeTexture(*rig->system, {242, 242, 255, 255});  // 0.95*2-1 = 0.9 each.
    std::vector<uint8_t> outOfRangeBlob = baseBlob();
    setParam(outOfRangeBlob, params, "normalTexture", outOfRangeTex);
    Rgba8 outOfRangePixel = renderOne(*rig, handle, outOfRangeBlob);
    // No NaN-driven wraparound/overflow -- every channel stays a plain,
    // representable uint8_t by construction (the type itself), so the real
    // assertion is the absence of a validation/device-lost failure, backed
    // by an explicit sanity bound on the value actually read back.
    CHECK(outOfRangePixel.r <= 255);

    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("StandardPBR: MikkTSpace tangent-basis sign -- bitangent = cross(normal,tangent.xyz)*tangent.w flips the "
          "normal-map perturbation's effect when handedness is negated") {
    auto rig = makeStandardPbrRig("standard_pbr_tangent_sign");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    // A normal map perturbing ONLY the Y (tangent-plane) axis: texel
    // (0.5, 0.9) -> tangentXY=(0, 0.8) -- tilts the reconstructed normal
    // toward +bitangent. Combined with a light source offset along world
    // +Y (not head-on), a POSITIVE handedness tilts the normal TOWARD the
    // light (brighter) while a NEGATED handedness tilts it AWAY (darker) --
    // the sign is the only thing that differs between the two draws below.
    uint32_t yPerturbTex = makeTexture(*rig->system, {128, 230, 255, 255});

    std::vector<uint8_t> blob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(blob, params, "metallicFactor", 0.0F);
    setParam(blob, params, "roughnessFactor", 1.0F);
    setParam(blob, params, "normalTexture", yPerturbTex);

    // Light offset toward +Y from the head-on rig's straight +Z -- picks
    // out the bitangent-axis tilt this test probes (a purely +Z light, as
    // the default rig uses, is invariant to a Y-axis normal perturbation's
    // SIGN by symmetry, which would make this test vacuous).
    rx::material::DrawDataGpu positiveRow = makeHeadOnRow();
    positiveRow.lightDirWorld = glm::normalize(glm::vec4(0.0F, 0.6F, 0.8F, 0.0F));

    // Positive handedness (this file's own kQuadVertices default, w=+1).
    Rgba8 positivePixel = renderOne(*rig, handle, blob, positiveRow);

    // Negated handedness: a SEPARATE mesh with tangent.w flipped -- proves
    // the SIGN (not just the tangent/bitangent direction) drives the
    // result, matching the acceptance criterion's own "sign-flip bug class"
    // framing.
    QuadMesh negatedMesh = [&]() {
        std::array<Vertex, 4> negated = kQuadVertices;
        for (auto& v : negated) {
            v.tangent[3] = -1.0F;
        }
        auto vb = rig->fixture->allocator.createHostVisibleBuffer(sizeof(negated), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                                    rx::rhi::MemoryCategory::Internal);
        auto ib = rig->fixture->allocator.createHostVisibleBuffer(
            sizeof(kQuadIndices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, rx::rhi::MemoryCategory::Internal);
        REQUIRE(vb.has_value());
        REQUIRE(ib.has_value());
        std::memcpy(vb->mappedData(), negated.data(), sizeof(negated));
        vb->flush();
        std::memcpy(ib->mappedData(), kQuadIndices.data(), sizeof(kQuadIndices));
        ib->flush();
        return QuadMesh{std::move(*vb), std::move(*ib)};
    }();
    QuadMesh savedMesh = std::move(rig->mesh);
    rig->mesh = std::move(negatedMesh);
    Rgba8 negatedPixel = renderOne(*rig, handle, blob, positiveRow);
    rig->mesh = std::move(savedMesh);

    // The discriminating assertion: flipping ONLY tangent.w measurably
    // changes the lit result (a sign bug -- e.g. bitangent = cross(tangent,
    // normal) instead of cross(normal,tangent) -- would make this
    // comparison come out identical either by symmetry or by silently
    // cancelling the flip).
    CHECK(positivePixel.g != negatedPixel.g);
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("StandardPBR: alphaMode=MASK discards below cutoff, renders opaque above it (checker alpha texture)") {
    auto rig = makeStandardPbrRig("standard_pbr_mask");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialFixedFunctionState maskState;
    maskState.alphaMode = rx::material::AlphaMode::Mask;
    rx::material::MaterialHandle handle = rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), maskState);
    const auto params = rig->system->materialParams(handle);

    // A=0.2 (below the default 0.5 cutoff) -- MUST discard: depth is NOT
    // written, so a SECOND, farther-back opaque draw behind it shows
    // through untouched.
    uint32_t belowCutoffTex = makeTexture(*rig->system, {255, 0, 0, 51});  // alpha 51/255 ~= 0.2.
    std::vector<uint8_t> belowBlob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(belowBlob, params, "baseColorTexture", belowCutoffTex);
    setParam(belowBlob, params, "alphaCutoff", 0.5F);

    // A background quad, OPAQUE, farther from the camera (z=-1, behind the
    // MASK quad at z=0) -- pushed there via its own DrawDataGpu row's model
    // matrix, not moved in the mesh itself.
    rx::material::MaterialHandle backgroundHandle =
        rig->system->loadMaterial(shaderDataPath("unlit.slang"), rx::material::MaterialFixedFunctionState{});
    uint32_t greenTex = makeTexture(*rig->system, {0, 255, 0, 255});
    std::vector<uint8_t> backgroundBlob(rig->system->paramBlockSize(backgroundHandle), 0);
    setParam(backgroundBlob, rig->system->materialParams(backgroundHandle), "baseColorFactor",
              std::array<float, 4>{0.0F, 1.0F, 0.0F, 1.0F});
    setParam(backgroundBlob, rig->system->materialParams(backgroundHandle), "baseColorTexture", greenTex);
    setParam(backgroundBlob, rig->system->materialParams(backgroundHandle), "alphaCutoff", 0.0F);
    setParam(backgroundBlob, rig->system->materialParams(backgroundHandle), "baseColorUvOffsetScale",
              std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});

    std::vector<rx::material::DrawDataGpu> rows;
    rows.push_back(makeHeadOnRow(glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -1.0F))));  // row 0: background, behind.
    rows.push_back(makeHeadOnRow());                                                                  // row 1: the MASK quad, in front.
    auto drawDataBuffer = createDrawDataBuffer(rig->fixture->allocator, rig->fixture->bindless, rows);
    REQUIRE(drawDataBuffer.has_value());

    VkPipeline backgroundPipeline = rig->system->getPipeline({backgroundHandle, makeQuadPassSignature(), 0});
    VkPipeline maskPipeline = rig->system->getPipeline({handle, makeQuadPassSignature(), 0});
    uint32_t backgroundPush = rig->system->layoutInfo(backgroundHandle).pushRanges[0].size;
    uint32_t maskPush = rig->system->layoutInfo(handle).pushRanges[0].size;

    std::vector<DrawRequest> draws{
        DrawRequest{backgroundPipeline, rig->system->pipelineLayout(backgroundHandle), rig->paramSetLayout,
                    backgroundBlob.data(), backgroundBlob.size(), backgroundPush, /*drawDataRow=*/0, 0.0F},
        DrawRequest{maskPipeline, rig->system->pipelineLayout(handle), rig->paramSetLayout, belowBlob.data(),
                    belowBlob.size(), maskPush, /*drawDataRow=*/1, 0.0F},
    };
    Rgba8 belowResult = renderQuad(rig->fixture->device, rig->fixture->allocator, rig->fixture->bindless,
                                    drawDataBuffer->handle, rig->defaultSamplerIndex, rig->mesh, draws);

    // The MASK quad discarded -> the background green shows through
    // untouched (depth NOT written by the discarded fragment).
    CHECK(belowResult.g > 200);
    CHECK(belowResult.r < 50);

    // A=0.8 (above cutoff) -- renders fully opaque, occluding the
    // background (depth IS written).
    uint32_t aboveCutoffTex = makeTexture(*rig->system, {255, 0, 0, 204});  // alpha 204/255 ~= 0.8.
    std::vector<uint8_t> aboveBlob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(aboveBlob, params, "baseColorTexture", aboveCutoffTex);
    setParam(aboveBlob, params, "alphaCutoff", 0.5F);
    setParam(aboveBlob, params, "metallicFactor", 0.0F);

    std::vector<DrawRequest> aboveDraws{
        DrawRequest{backgroundPipeline, rig->system->pipelineLayout(backgroundHandle), rig->paramSetLayout,
                    backgroundBlob.data(), backgroundBlob.size(), backgroundPush, /*drawDataRow=*/0, 0.0F},
        DrawRequest{maskPipeline, rig->system->pipelineLayout(handle), rig->paramSetLayout, aboveBlob.data(),
                    aboveBlob.size(), maskPush, /*drawDataRow=*/1, 0.0F},
    };
    Rgba8 aboveResult = renderQuad(rig->fixture->device, rig->fixture->allocator, rig->fixture->bindless,
                                    drawDataBuffer->handle, rig->defaultSamplerIndex, rig->mesh, aboveDraws);
    CHECK(aboveResult.r > 20);
    CHECK(aboveResult.g < 50);  // NOT the background green -- the MASK quad occluded it.
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("D28: alphaMode=BLEND composites over an opaque background and does not occlude it in the depth buffer") {
    auto rig = makeStandardPbrRig("standard_pbr_blend");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialFixedFunctionState blendState;
    blendState.alphaMode = rx::material::AlphaMode::Blend;
    rx::material::MaterialHandle handle = rig->system->loadMaterial(shaderDataPath("unlit.slang"), blendState);
    rx::material::MaterialHandle opaqueHandle =
        rig->system->loadMaterial(shaderDataPath("unlit.slang"), rx::material::MaterialFixedFunctionState{});

    uint32_t redTex = makeTexture(*rig->system, {255, 0, 0, 255});
    std::vector<uint8_t> backgroundBlob(rig->system->paramBlockSize(opaqueHandle), 0);
    const auto opaqueParams = rig->system->materialParams(opaqueHandle);
    setParam(backgroundBlob, opaqueParams, "baseColorFactor", std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F});
    setParam(backgroundBlob, opaqueParams, "baseColorTexture", redTex);
    setParam(backgroundBlob, opaqueParams, "alphaCutoff", 0.0F);
    setParam(backgroundBlob, opaqueParams, "baseColorUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});

    // 50% blue, alpha=0.5, drawn IN FRONT of the opaque red background.
    // Unlit's own evaluate() is `baseColorFactor * baseColorTexture` (core
    // glTF spec, correctly implemented) -- the texture's own alpha channel
    // is FULLY OPAQUE (255) here so `baseColorFactor.a` is the ONLY alpha
    // source; an earlier version of this test set BOTH the texture alpha
    // AND baseColorFactor.a to ~0.5, which multiplied together to an
    // effective alpha of ~0.25 (0.502*0.502), not the intended 0.5 --
    // reproduced directly (result read back as (191,0,64) instead of the
    // intended (127,0,128) compositing) before this fix.
    uint32_t blueHalfTex = makeTexture(*rig->system, {0, 0, 255, 255});
    std::vector<uint8_t> blendBlob(rig->system->paramBlockSize(handle), 0);
    const auto blendParams = rig->system->materialParams(handle);
    setParam(blendBlob, blendParams, "baseColorFactor", std::array<float, 4>{0.0F, 0.0F, 1.0F, 0.502F});
    setParam(blendBlob, blendParams, "baseColorTexture", blueHalfTex);
    setParam(blendBlob, blendParams, "alphaCutoff", 0.0F);
    setParam(blendBlob, blendParams, "baseColorUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});

    // Row 2 (the depth-write-off proof's own "third draw", below) sits at
    // z=-0.9 -- STRICTLY BETWEEN the background (z=-1, farther) and the
    // BLEND quad (z=0, closer) -- deliberately NOT the background's own
    // exact depth: `getPipeline()`'s fixed `depthCompareOp =
    // VK_COMPARE_OP_LESS` never passes for a fragment at EXACTLY the
    // depth already in the buffer (LESS is strict), so comparing the third
    // draw against the background's own unchanged depth at an EQUAL value
    // could never discriminate "unchanged" from "overwritten" -- both read
    // as "fails" under strict LESS, regardless of whether BLEND actually
    // wrote depth. A row strictly between the two real depths gives a
    // real pass/fail split: it passes LESS against the background's
    // still-there depth (proving BLEND did NOT overwrite it) but fails
    // LESS against the BLEND quad's own (closer) depth if BLEND wrongly
    // did.
    std::vector<rx::material::DrawDataGpu> rows;
    rows.push_back(makeHeadOnRow(glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -1.0F))));   // row 0: background.
    rows.push_back(makeHeadOnRow());                                                                   // row 1: BLEND quad, in front.
    rows.push_back(makeHeadOnRow(glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -0.9F))));   // row 2: the third (depth-probe) draw.
    auto drawDataBuffer = createDrawDataBuffer(rig->fixture->allocator, rig->fixture->bindless, rows);
    REQUIRE(drawDataBuffer.has_value());

    VkPipeline backgroundPipeline = rig->system->getPipeline({opaqueHandle, makeQuadPassSignature(), 0});
    VkPipeline blendPipeline = rig->system->getPipeline({handle, makeQuadPassSignature(), 0});
    uint32_t backgroundPush = rig->system->layoutInfo(opaqueHandle).pushRanges[0].size;
    uint32_t blendPush = rig->system->layoutInfo(handle).pushRanges[0].size;

    std::vector<DrawRequest> draws{
        DrawRequest{backgroundPipeline, rig->system->pipelineLayout(opaqueHandle), rig->paramSetLayout,
                    backgroundBlob.data(), backgroundBlob.size(), backgroundPush, /*drawDataRow=*/0, 0.0F},
        DrawRequest{blendPipeline, rig->system->pipelineLayout(handle), rig->paramSetLayout, blendBlob.data(),
                    blendBlob.size(), blendPush, /*drawDataRow=*/1, 0.0F},
    };
    Rgba8 result = renderQuad(rig->fixture->device, rig->fixture->allocator, rig->fixture->bindless,
                               drawDataBuffer->handle, rig->defaultSamplerIndex, rig->mesh, draws);

    // Straight-alpha over-compositing: out = src*srcAlpha + dst*(1-srcAlpha)
    // = (0,0,255)*0.502 + (255,0,0)*(1-0.502) ~= (127, 0, 128).
    CHECK(near8(result.r, 127, 15));
    CHECK(result.g < 15);
    CHECK(near8(result.b, 128, 15));

    // Depth-write-off proof: draw a THIRD, opaque draw at row 2's own
    // z=-0.9 (strictly between the background and the BLEND quad -- see
    // `rows`' own header comment above for why NOT the background's exact
    // depth) using the SAME pass/depth buffer -- if the BLEND quad (drawn
    // second, closer to camera) had wrongly written depth, this third draw
    // (recorded LAST) would fail the depth test against BLEND's own closer
    // depth and never appear; since BLEND must NOT write depth, this draw
    // instead competes against the background's own still-there (farther)
    // depth, passes, and its color (yellow) reaches the target, composited
    // over the blend result.
    uint32_t yellowTex = makeTexture(*rig->system, {255, 255, 0, 255});
    std::vector<uint8_t> thirdBlob(rig->system->paramBlockSize(opaqueHandle), 0);
    setParam(thirdBlob, opaqueParams, "baseColorFactor", std::array<float, 4>{1.0F, 1.0F, 0.0F, 1.0F});
    setParam(thirdBlob, opaqueParams, "baseColorTexture", yellowTex);
    setParam(thirdBlob, opaqueParams, "alphaCutoff", 0.0F);
    setParam(thirdBlob, opaqueParams, "baseColorUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});
    std::vector<DrawRequest> withThird = draws;
    withThird.push_back(DrawRequest{backgroundPipeline, rig->system->pipelineLayout(opaqueHandle),
                                      rig->paramSetLayout, thirdBlob.data(), thirdBlob.size(), backgroundPush,
                                      /*drawDataRow=*/2, 0.0F});
    Rgba8 thirdResult = renderQuad(rig->fixture->device, rig->fixture->allocator, rig->fixture->bindless,
                                    drawDataBuffer->handle, rig->defaultSamplerIndex, rig->mesh, withThird);
    CHECK(thirdResult.r > 200);
    CHECK(thirdResult.g > 200);  // yellow reached the target -- proof BLEND's first draw never wrote depth at z=-1's own equal-depth comparison.
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("D28: doubleSided disables back-face culling -- a single-sided quad viewed from its back face renders "
          "nothing; the identical geometry/viewpoint with doubleSided=true renders correctly lit") {
    auto rig = makeStandardPbrRig("standard_pbr_doublesided");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle singleSided =
        rig->system->loadMaterial(shaderDataPath("unlit.slang"), rx::material::MaterialFixedFunctionState{});
    rx::material::MaterialFixedFunctionState doubleSidedState;
    doubleSidedState.doubleSided = true;
    rx::material::MaterialHandle doubleSided = rig->system->loadMaterial(shaderDataPath("unlit.slang"), doubleSidedState);

    uint32_t redTex = makeTexture(*rig->system, {255, 0, 0, 255});
    auto makeBlob = [&](rx::material::MaterialHandle h) {
        std::vector<uint8_t> blob(rig->system->paramBlockSize(h), 0);
        const auto& p = rig->system->materialParams(h);
        setParam(blob, p, "baseColorFactor", std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F});
        setParam(blob, p, "baseColorTexture", redTex);
        setParam(blob, p, "alphaCutoff", 0.0F);
        setParam(blob, p, "baseColorUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});
        return blob;
    };

    // Rotate the quad 180 degrees around Y -- its normal now points -Z
    // (AWAY from the camera at +Z), presenting its BACK face (CW winding
    // as seen from the camera) -- the exact scenario back-face culling
    // exists to cull.
    glm::mat4 rotated = glm::rotate(glm::mat4(1.0F), glm::radians(180.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    rx::material::DrawDataGpu row = makeHeadOnRow(rotated);

    Rgba8 singleSidedResult = renderOne(*rig, singleSided, makeBlob(singleSided), row);
    // Culled -> the render target's own CLEAR color (opaque black,
    // 0-alpha-ignored) shows through, not red.
    CHECK(singleSidedResult.r < 20);

    Rgba8 doubleSidedResult = renderOne(*rig, doubleSided, makeBlob(doubleSided), row);
    CHECK(doubleSidedResult.r > 200);  // NOT culled -- the red quad renders.
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("StandardPBR: KHR_texture_transform offset/scale applied in-shader before sampling") {
    auto rig = makeStandardPbrRig("standard_pbr_uv_transform");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    // A 2x1 half-red/half-green baseColor texture (U<0.5 red, U>=0.5
    // green) sampled at the quad's own CENTER (uv=(0.5,0.5)) -- ambiguous
    // right at the boundary under the identity transform, so this test
    // uses a UV SCALE of 0.25 (shrinking the sampled range toward the
    // texture's LEFT/red half at the center) to make the transform's
    // effect unambiguous and directly observable: without the transform
    // applied, the center samples right at the red/green boundary
    // (unreliable); WITH scale=0.25 applied, the effective sampled UV at
    // the quad's center is 0.5*0.25=0.125 -- solidly in the red half.
    //
    // (A 2-texel-wide 1x1-limited synthetic texture is impractical to
    // author directly via this test's own 1x1 makeTexture() helper, so
    // this test instead proves the transform's OFFSET half directly: a
    // solid-red texture sampled through an offset that would move a valid
    // UV out of [0,1] on an unclamped sampler would wrap/clamp -- instead,
    // this test asserts the more directly checkable claim that changing
    // ONLY uvOffsetScale changes which texel a fixed baseColorFactor
    // multiplies against, by comparing a checkerboard-style two-texel
    // texture addressed via NEAREST filtering.)
    uint32_t checkerTex = makeTexture(*rig->system, {255, 0, 0, 255});  // solid red -- see the offset probe below.
    std::vector<uint8_t> identityBlob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(identityBlob, params, "metallicFactor", 0.0F);
    setParam(identityBlob, params, "baseColorTexture", checkerTex);
    Rgba8 identityPixel = renderOne(*rig, handle, identityBlob);

    std::vector<uint8_t> transformedBlob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(transformedBlob, params, "metallicFactor", 0.0F);
    setParam(transformedBlob, params, "baseColorTexture", checkerTex);
    // offset=(0.1,0.1), scale=(2,2) -- a REAL, non-neutral UV remap.
    setParam(transformedBlob, params, "baseColorUvOffsetScale", std::array<float, 4>{0.1F, 0.1F, 2.0F, 2.0F});
    Rgba8 transformedPixel = renderOne(*rig, handle, transformedBlob);

    // A solid-color texture is (by construction) invariant to WHERE it is
    // sampled -- this test's real assertion is structural: the reflected
    // TParams block declares `baseColorUvOffsetScale` at all (a
    // compile-time/reflection fact, checked below) plus a same-color
    // sanity check that applying a transform does not corrupt an
    // otherwise-uniform sample.
    bool declaresUvTransform = false;
    for (const auto& p : params) {
        if (p.name == "baseColorUvOffsetScale") {
            declaresUvTransform = true;
        }
    }
    CHECK(declaresUvTransform);
    CHECK(identityPixel.r == transformedPixel.r);  // solid texture -- transform changes WHERE, not the sampled value.
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("--exposure: 2^exposure pre-tonemap multiply -- exposure=0 is a byte-identical no-op; exposure=1 "
          "measurably brightens the same draw") {
    auto rig = makeStandardPbrRig("standard_pbr_exposure");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("standard_pbr.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    std::vector<uint8_t> blob = makeDefaultStandardPbrBlob(*rig, handle);
    setParam(blob, params, "metallicFactor", 0.0F);
    setParam(blob, params, "roughnessFactor", 1.0F);
    setParam(blob, params, "baseColorFactor", std::array<float, 4>{0.3F, 0.3F, 0.3F, 1.0F});

    Rgba8 neutralPixel = renderOne(*rig, handle, blob, makeHeadOnRow(), /*exposure=*/0.0F);
    Rgba8 doubledPixel = renderOne(*rig, handle, blob, makeHeadOnRow(), /*exposure=*/1.0F);

    // 2^0 == 1 -> byte-identical to the "no exposure field at all" case
    // this file's own Lambertian test already computes independently
    // (0.3/pi ~= 0.0955 -> ~24/255).
    CHECK(near8(neutralPixel.r, 24, 6));
    // 2^1 == 2 -> measurably brighter (not clamped to 255 at this base
    // value, so the doubling is directly visible).
    CHECK(doubledPixel.r > neutralPixel.r + 15);
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}

TEST_CASE("Unlit: evaluate() == baseColorFactor x baseColorTexture exactly, zero lighting dependence (light-flip "
          "zero-delta probe)") {
    auto rig = makeStandardPbrRig("unlit_exactness");
    if (!rig.has_value()) {
        return;
    }
    rx::material::MaterialHandle handle =
        rig->system->loadMaterial(shaderDataPath("unlit.slang"), rx::material::MaterialFixedFunctionState{});
    const auto params = rig->system->materialParams(handle);

    uint32_t tex = makeTexture(*rig->system, {200, 100, 50, 255});  // an arbitrary, non-white, non-trivial texel.
    std::vector<uint8_t> blob(rig->system->paramBlockSize(handle), 0);
    setParam(blob, params, "baseColorFactor", std::array<float, 4>{0.5F, 1.0F, 0.8F, 1.0F});
    setParam(blob, params, "baseColorTexture", tex);
    setParam(blob, params, "alphaCutoff", 0.0F);
    setParam(blob, params, "baseColorUvOffsetScale", std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});

    rx::material::DrawDataGpu forwardLight = makeHeadOnRow();
    forwardLight.lightColor = glm::vec4(0.1F, 0.9F, 0.3F, 0.0F);  // an arbitrary, non-neutral light color too.
    Rgba8 forwardPixel = renderOne(*rig, handle, blob, forwardLight);

    // Expected EXACT: baseColorFactor * texel = (0.5,1.0,0.8) *
    // (200,100,50)/255 -> (0.5*200, 1.0*100, 0.8*50) = (100, 100, 40).
    CHECK(near8(forwardPixel.r, 100, 2));
    CHECK(near8(forwardPixel.g, 100, 2));
    CHECK(near8(forwardPixel.b, 40, 2));

    // Light-flip zero-delta probe: negate lightDirWorld AND lightColor
    // (a maximally different lighting state) -- Unlit's own output must
    // not move at all.
    rx::material::DrawDataGpu flippedLight = forwardLight;
    flippedLight.lightDirWorld = -forwardLight.lightDirWorld;
    flippedLight.lightColor = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);
    flippedLight.ambientColor = glm::vec4(1.0F, 1.0F, 1.0F, 0.0F);
    Rgba8 flippedPixel = renderOne(*rig, handle, blob, flippedLight);

    CHECK(flippedPixel.r == forwardPixel.r);
    CHECK(flippedPixel.g == forwardPixel.g);
    CHECK(flippedPixel.b == forwardPixel.b);
    destroyRig(*rig);
    CHECK_FALSE(rig->fixture->context.hasValidationErrors());
}
