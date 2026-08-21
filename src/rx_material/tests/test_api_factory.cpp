// Task 6, spec Phase 3 design D5: rxCreateMaterialSystem() +
// IRxMaterialSystem::loadMaterial()'s happy path against a REAL VkDevice-
// backed internal rx::material::MaterialSystem, plus
// IRxMaterialInstance::setFloat/setFloat4/setTexture's validation
// contract (name exists in the material's reflected parameters, type
// matches) exercised against real reflected data from test_unlit.slang
// (a `float4 tint`) and test_textured.slang (a `uint albedoIndex` --
// this engine's D8 bindless-index convention). The device-free half of
// this ABI's contract (queryInterface identity, refcount round-trips,
// error codes on malformed input) lives in test_api_contract.cpp
// (rx_material_tests) instead -- see that file's own header comment.
#include <doctest/doctest.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_material/material_system.h>
#include <rx_material/rx_api.h>
#include <rx_material/rx_api_detail.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_task/scheduler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// RX_MATERIAL_TEST_DATA_DIR -- see test_material_system.cpp's own
// comment; identical convention, this file's own copy of the same tiny
// helper (matching that file's own precedent of not sharing private
// per-test-binary helpers across translation units).

namespace {

std::filesystem::path testDataPath(const char* filename) {
    return std::filesystem::path(RX_MATERIAL_TEST_DATA_DIR) / filename;
}

// Identical fixture shape to test_material_system.cpp's own
// MaterialTestFixture/makeFixture() -- duplicated, not shared, matching
// that file's own precedent (see its comment) rather than reaching into
// another test binary's private helpers.
struct ApiTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    rx::rhi::Device device;
    rx::rhi::BindlessTable bindless;
};

std::optional<ApiTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping rx_api test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping rx_api test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    rx::rhi::BindlessTable::Capacities capacities;
    capacities.sampledImages = 4;
    capacities.samplers = 2;
    capacities.storageBuffers = 1;
    // [Phase 4 Stage 2 Task 22 fix round, F1] Same requirement as
    // test_material_system.cpp's own identical comment.
    capacities.comparisonSamplers = 1;
    // [Phase 5 Task 10, #46] material.slang now unconditionally declares
    // `gTexturesCube` at binding 4 -- same requirement as
    // `comparisonSamplers` above.
    capacities.cubeImages = 1;
    auto bindless = rx::rhi::BindlessTable::create(device->physicalDevice(), device->device(), capacities);
    REQUIRE(bindless.has_value());

    return ApiTestFixture{std::move(*window), std::move(*context), surface, std::move(*device),
                            std::move(*bindless)};
}

std::filesystem::path freshCachePath(const char* name) {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / (std::string("rx_material_api_test_") + name + ".cache");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

// Minimal IRxTexture test double [Task 6] -- proves
// IRxMaterialInstance::setTexture()'s addRef()/release() lifetime
// handling (it stores the pointer with one added reference, releases
// any previously stored one on overwrite, and releases whatever is
// still stored when the instance itself is destroyed) without needing a
// real renderer-owned texture: nothing in Task 6's own surface creates a
// real IRxTexture yet (no rxCreateTexture factory -- see rx_api.h's own
// comment on IRxTexture), so a hand-rolled double is the only way to
// exercise this path at all before that exists. This is a TEST-ONLY
// implementation of a COM-lite interface from outside api_impl.cpp --
// legal per the ABI shape itself (any binary can implement a pure-
// virtual interface), but not a claim that third-party IRxTexture
// implementations are a supported Task 6 use case.
class FakeTexture final : public IRxTexture {
public:
    RxResult queryInterface(const RxGuid& iid, void** outObject) override {
        if (outObject == nullptr) {
            return RX_E_INVALIDARG;
        }
        if (guidEquals(iid, kIID_IRxUnknown) || guidEquals(iid, kIID_IRxTexture)) {
            addRef();
            *outObject = static_cast<IRxTexture*>(this);
            return RX_OK;
        }
        *outObject = nullptr;
        return RX_E_NOINTERFACE;
    }
    uint32_t addRef() override { return ++refCount_; }
    uint32_t release() override {
        uint32_t remaining = --refCount_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    [[nodiscard]] uint32_t refCount() const { return refCount_; }

private:
    static bool guidEquals(const RxGuid& a, const RxGuid& b) {
        return a.data1 == b.data1 && a.data2 == b.data2 && a.data3 == b.data3 &&
               std::equal(std::begin(a.data4), std::end(a.data4), std::begin(b.data4));
    }

    uint32_t refCount_ = 1;
};

// [Fix round 1, task-6-review.md F1] Same cheap structural guard as
// test_api_contract.cpp's own identically-named helper (duplicated, not
// shared -- matching this file's own established precedent of not
// reaching into another test binary's private helpers). See that file's
// comment on its own copy for exactly what this does and doesn't prove.
bool isDocumentedResult(RxResult result) {
    switch (result) {
        case RX_OK:
        case RX_E_FAIL:
        case RX_E_INVALIDARG:
        case RX_E_NOTFOUND:
        case RX_E_COMPILE:
        case RX_E_NOINTERFACE:
            return true;
        default:
            return false;
    }
}

// ===== Task 4 [seed-10 carried] ===========================================
// Render+readback helper for the "the bound texture actually changes the
// rendered image" GPU tests below. Draws a full-screen quad through
// `internal`'s real MaterialSystem::bindInstance() -- the internal bridge
// sample 06_materials established (public rx_api.h surface for material/
// instance/texture management, this one internal call for the draw-time
// bind rx_api.h deliberately does not expose this phase -- see rx_api.h's
// own comment) -- exactly like test_material_system.cpp's own bindInstance
// GPU tests already do, then reads back one pixel from each screen
// quadrant.
//
// QUADRANT-PROBE DERIVATION (documented once here, not re-derived per call
// site): the quad's 4 corners map screen space (0,0)/(W,0)/(W,H)/(0,H) to
// UV (0,0)/(1,0)/(1,1)/(0,1) respectively (bilinear across both triangles --
// see kVertices below), so a screen pixel at continuous position
// (x+0.5, y+0.5) samples UV ((x+0.5)/W, (y+0.5)/H). A LINEAR-filtered sample
// against a 2x2 texture is EXACTLY (zero blend) the corresponding texel
// whenever a UV component stays within [0, 0.25] or [0.75, 1]: standard
// bilinear tap math puts both sample taps on the SAME clamped texel index
// throughout that whole range, not just exactly at the texel center (worked
// by hand in task-4-report.md). Probing at 1/8 and 7/8 fractions of each
// axis sits comfortably inside those exact-match bands, so every readback
// below is an exact, zero-tolerance byte match, never a blended
// approximation.
constexpr uint32_t kQuadrantProbeExtent = 64;
constexpr VkFormat kQuadrantProbeColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

// [Phase 4 Task 16, D8] Grew by one field (`tangent`, float4, w =
// handedness) to match forward_entry.slang's own updated vertexMain
// parameter list / material_system.cpp's own updated MaterialVertexLayout
// (48 bytes, position+normal+tangent+uv) -- a stride mismatch here would
// make the GPU read garbage position/normal/uv data (this test's own
// original 32-byte layout, uploaded against the new 48-byte-stride pipeline
// vertex-input state, IS exactly what broke this test when the D8 tangent
// field first landed: bytes past the third vertex's `uv` silently became
// the fourth vertex's `position`, etc.). The tangent value itself is inert
// for this test (a flat-shaded checkerboard-sampling quad, no normal
// mapping) -- an arbitrary, valid unit tangent (+X, handedness +1).
struct QuadTestVertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv[2];
};

struct QuadrantPixels {
    std::array<uint8_t, 4> topLeft;
    std::array<uint8_t, 4> topRight;
    std::array<uint8_t, 4> bottomLeft;
    std::array<uint8_t, 4> bottomRight;
};

// Identical shape to test_material_system.cpp's own helper of the same
// name -- duplicated, not shared, matching this file's own established
// precedent (see its header comment) of not reaching into another test
// binary's private helpers.
struct OffscreenColorImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

std::optional<OffscreenColorImage> createOffscreenColorImage(VkDevice device, VkPhysicalDevice physicalDevice,
                                                               VkFormat format, VkExtent2D extent) {
    OffscreenColorImage result;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &result.image) != VK_SUCCESS) {
        return std::nullopt;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, result.image, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == UINT32_MAX) {
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &result.memory) != VK_SUCCESS) {
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }
    if (vkBindImageMemory(device, result.image, result.memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, result.memory, nullptr);
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &result.view) != VK_SUCCESS) {
        vkFreeMemory(device, result.memory, nullptr);
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    return result;
}

void destroyOffscreenColorImage(VkDevice device, OffscreenColorImage& img) {
    if (img.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, img.view, nullptr);
    }
    if (img.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, img.image, nullptr);
    }
    if (img.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, img.memory, nullptr);
    }
    img = OffscreenColorImage{};
}

std::optional<QuadrantPixels> renderQuadAndReadbackQuadrants(rx::rhi::Device& device, rx::rhi::BindlessTable& bindless,
                                                              rx::material::MaterialSystem& internal,
                                                              rx::material::MaterialHandle materialHandle,
                                                              const void* paramBlob, size_t paramBlobSize) {
    auto allocator = rx::rhi::Allocator::createRaw(device.physicalDevice(), device.device(), device.instance());
    if (!allocator.has_value()) {
        return std::nullopt;
    }

    // Full-screen quad, clip-space corners fed directly -- forward_entry.
    // slang's own "PHASE 3 SCOPE NOTE": `position` IS clip space, no
    // transform exists. Winding: EMPIRICALLY VERIFIED against this exact
    // pipeline (a real GPU run, not a hand-derived formula taken on faith --
    // an initial hand derivation using Vulkan's own documented signed-area
    // facing formula got this backwards and was caught by exactly this
    // task's own GPU test rendering nothing at all; see task-4-report.md for
    // the full account) -- (A,C,B)/(A,D,C) is the winding that survives the
    // fixed VK_CULL_MODE_BACK_BIT/VK_FRONT_FACE_COUNTER_CLOCKWISE
    // rasterization state getPipeline() (material_system.cpp) always
    // builds, against a standard (non-flipped) VkViewport. NOT the same
    // winding as sample 06_materials' own addQuad(), which is correct for
    // THAT sample's own Y-flipped-projection setup, not this transform-free
    // one.
    constexpr std::array<QuadTestVertex, 4> kVertices{{
        {{-1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},  // A: top-left
        {{1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},   // B: top-right
        {{1.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F}},    // C: bottom-right
        {{-1.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 1.0F}},   // D: bottom-left
    }};
    constexpr std::array<uint32_t, 6> kIndices{0, 2, 1, 0, 3, 2};

    auto vertexBuffer = allocator->createHostVisibleBuffer(sizeof(kVertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto indexBuffer = allocator->createHostVisibleBuffer(sizeof(kIndices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!vertexBuffer.has_value() || !indexBuffer.has_value()) {
        return std::nullopt;
    }
    std::memcpy(vertexBuffer->mappedData(), kVertices.data(), sizeof(kVertices));
    vertexBuffer->flush();
    std::memcpy(indexBuffer->mappedData(), kIndices.data(), sizeof(kIndices));
    indexBuffer->flush();

    // Phase 4 Task 7 [spec D4 amendment]: Executor::create() now requires a
    // real rx::task::Scheduler& -- this function's own local one, kept alive
    // for the rest of this function's (synchronous) execution.
    auto scheduler = rx::task::Scheduler::create();
    if (scheduler == nullptr) {
        return std::nullopt;
    }
    auto executor = rx::graph::Executor::create(device, *scheduler);
    if (executor == nullptr) {
        return std::nullopt;
    }

    constexpr VkExtent2D kExtent{kQuadrantProbeExtent, kQuadrantProbeExtent};

    rx::graph::RenderGraph graph;
    rx::graph::Pass& pass = graph.addPass("forward");
    rx::graph::AttachmentDesc colorDesc;
    colorDesc.format = kQuadrantProbeColorFormat;
    pass.addColorOutput("color", colorDesc);
    graph.setBackbufferSource("color");

    pass.setExecute([&](rx::graph::PassContext& ctx) {
        internal.beginFrame(/*frameInFlightIndex=*/0, /*frameNumber=*/0);
        rx::material::InstanceBinding binding{materialHandle, paramBlob, paramBlobSize};
        internal.bindInstance(ctx.cmd, ctx, binding);

        // Descriptor SET 0 (the real bindless table) -- bindInstance()
        // itself only ever binds set 1 (this material's own gParams UBO),
        // exactly like sample 06_materials' own recordDraws() comment
        // documents ("Binds descriptor SET 0 ... exactly once").
        VkDescriptorSet bindlessSet = bindless.descriptorSet();
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, internal.pipelineLayout(materialHandle),
                                 /*firstSet=*/0, 1, &bindlessSet, 0, nullptr);

        VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width),
                             static_cast<float>(ctx.renderArea.height), 0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, ctx.renderArea};
        vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
        vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

        VkBuffer vb = vertexBuffer->handle();
        VkDeviceSize vbOffset = 0;
        vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vb, &vbOffset);
        vkCmdBindIndexBuffer(ctx.cmd, indexBuffer->handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(ctx.cmd, static_cast<uint32_t>(kIndices.size()), 1, 0, 0, 0);
    });

    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = kExtent.width;
    compileInfo.swapchainHeight = kExtent.height;
    compileInfo.swapchainFormat = kQuadrantProbeColorFormat;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);
    executor->realize(graph);

    auto offscreen =
        createOffscreenColorImage(device.device(), device.physicalDevice(), kQuadrantProbeColorFormat, kExtent);
    if (!offscreen.has_value()) {
        return std::nullopt;
    }

    auto readback = allocator->createHostVisibleBuffer(static_cast<VkDeviceSize>(kExtent.width) * kExtent.height * 4,
                                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!readback.has_value()) {
        destroyOffscreenColorImage(device.device(), *offscreen);
        return std::nullopt;
    }

    auto cmdCtx =
        rx::rhi::CommandContext::create(device.device(), device.graphicsQueue(), device.graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        destroyOffscreenColorImage(device.device(), *offscreen);
        return std::nullopt;
    }

    cmdCtx->runOnce(
        [&](VkCommandBuffer cmd) { executor->execute(graph, cmd, offscreen->image, offscreen->view, kExtent); });

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {kExtent.width, kExtent.height, 1};
        vkCmdCopyImageToBuffer(cmd, offscreen->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                                &region);
    });

    readback->invalidate();
    const auto* pixels = static_cast<const uint8_t*>(readback->mappedData());

    auto pixelAt = [&](uint32_t x, uint32_t y) -> std::array<uint8_t, 4> {
        size_t offset = (static_cast<size_t>(y) * kExtent.width + x) * 4;
        return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
    };

    // 1/8 and 7/8 fractions -- see this function's own header comment.
    const uint32_t nearEdge = kExtent.width / 8;
    const uint32_t farEdge = kExtent.width - nearEdge - 1;

    QuadrantPixels result;
    result.topLeft = pixelAt(nearEdge, nearEdge);
    result.topRight = pixelAt(farEdge, nearEdge);
    result.bottomLeft = pixelAt(nearEdge, farEdge);
    result.bottomRight = pixelAt(farEdge, farEdge);

    destroyOffscreenColorImage(device.device(), *offscreen);
    return result;
}

}  // namespace

TEST_CASE("rxCreateMaterialSystem + loadMaterial: happy path against a real device, IRxMaterialInstance validates "
          "reflected float4 params") {
    auto fixture = makeFixture("rx_api_factory_happy_path");
    if (!fixture.has_value()) {
        return;
    }

    auto internal =
        rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("api_happy_path"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);
    REQUIRE(system != nullptr);

    IRxMaterial* material = nullptr;
    std::string unlitPath = testDataPath("test_unlit.slang").string();
    REQUIRE(system->loadMaterial(unlitPath.c_str(), &material) == RX_OK);
    REQUIRE(material != nullptr);
    CHECK(std::string(material->name()) == "test_unlit");

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);
    REQUIRE(instance != nullptr);

    // Real reflected field: `tint` is a `float4` in test_unlit.slang.
    float tint[4] = {1.0F, 0.5F, 0.25F, 1.0F};
    CHECK(instance->setFloat4("tint", tint) == RX_OK);

    // Same real field, wrong setter -- type mismatch, not "unknown".
    CHECK(instance->setFloat("tint", 1.0F) == RX_E_INVALIDARG);
    FakeTexture* wrongTypeTexture = new FakeTexture();
    CHECK(instance->setTexture("tint", wrongTypeTexture) == RX_E_INVALIDARG);
    wrongTypeTexture->release();

    // No such parameter on this material at all.
    CHECK(instance->setFloat("nonexistent", 1.0F) == RX_E_NOTFOUND);
    float bogus[4] = {0, 0, 0, 0};
    CHECK(instance->setFloat4("nonexistent", bogus) == RX_E_NOTFOUND);
    FakeTexture* notFoundTexture = new FakeTexture();
    CHECK(instance->setTexture("nonexistent", notFoundTexture) == RX_E_NOTFOUND);
    notFoundTexture->release();

    // Null-argument validation.
    CHECK(instance->setFloat(nullptr, 1.0F) == RX_E_INVALIDARG);
    CHECK(instance->setFloat4("tint", nullptr) == RX_E_INVALIDARG);
    CHECK(instance->setTexture("tint", nullptr) == RX_E_INVALIDARG);

    // queryInterface identity, one level down the object graph too.
    void* instanceAsUnknown = nullptr;
    REQUIRE(instance->queryInterface(kIID_IRxUnknown, &instanceAsUnknown) == RX_OK);
    void* instanceAsInstance = nullptr;
    REQUIRE(instance->queryInterface(kIID_IRxMaterialInstance, &instanceAsInstance) == RX_OK);
    CHECK(instanceAsUnknown == instanceAsInstance);
    static_cast<IRxUnknown*>(instanceAsUnknown)->release();
    static_cast<IRxUnknown*>(instanceAsInstance)->release();

    instance->release();
    material->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Fix round 1, task-7-review.md F1] `FakeTexture` is a hand-rolled,
// non-engine `IRxTexture` implementation -- it fails the internal
// `kIID_InternalTextureBridge` queryInterface check (anonymous-namespace-
// private to api_impl.cpp; no test double could ever answer it), so
// `setTexture()` must REJECT it with RX_E_INVALIDARG rather than silently
// binding bindless index 0. This test previously (Task 6/7's original
// submission) asserted RX_OK here and used `FakeTexture` to exercise
// setTexture()'s addRef()/release() lifecycle management -- that
// lifecycle coverage now lives in the REAL-texture test right below this
// one (a rejected call never reaches the refcount-mutating code path at
// all, so `FakeTexture` genuinely cannot exercise it anymore).
TEST_CASE("IRxMaterialInstance::setTexture rejects a non-engine-created IRxTexture (e.g. a hand-rolled test "
          "double) with RX_E_INVALIDARG") {
    auto fixture = makeFixture("rx_api_factory_texture_param");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_texture_param"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    IRxMaterial* material = nullptr;
    std::string texturedPath = testDataPath("test_textured.slang").string();
    REQUIRE(system->loadMaterial(texturedPath.c_str(), &material) == RX_OK);
    REQUIRE(material != nullptr);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);

    // Wrong setter for a real uint (bindless-index) field -- type
    // mismatch, checked (and unaffected by F1's fix) before the
    // engine-texture check ever runs.
    CHECK(instance->setFloat("albedoIndex", 1.0F) == RX_E_INVALIDARG);

    auto* fake = new FakeTexture();
    CHECK(fake->refCount() == 1);
    CHECK(instance->setTexture("albedoIndex", fake) == RX_E_INVALIDARG);
    // Rejection happens before any refcount mutation at all -- `fake`'s
    // count is completely untouched by the rejected call.
    CHECK(fake->refCount() == 1);
    fake->release();

    instance->release();
    material->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// The addRef()/release() lifecycle coverage the old FakeTexture-based test
// used to provide, preserved here against REAL engine-created textures
// (the only kind setTexture() accepts as of F1's fix) -- probeRefCount()
// below reads a live IRxTexture's current refcount side-effect-free (one
// addRef() immediately undone by one release(), net zero) since, unlike
// the test-only FakeTexture double, the real TextureImpl exposes no
// refCount() accessor of its own.
namespace {
uint32_t probeRefCount(IRxTexture* texture) {
    uint32_t afterAddRef = texture->addRef();
    uint32_t afterRelease = texture->release();
    (void)afterRelease;
    return afterAddRef - 1;
}
}  // namespace

TEST_CASE("IRxMaterialInstance::setTexture manages the IRxTexture reference it stores across overwrite and "
          "instance destruction (real engine-created textures)") {
    auto fixture = makeFixture("rx_api_factory_texture_refcount");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_texture_refcount"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    IRxMaterial* material = nullptr;
    std::string texturedPath = testDataPath("test_textured.slang").string();
    REQUIRE(system->loadMaterial(texturedPath.c_str(), &material) == RX_OK);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);

    std::vector<uint8_t> pixels(static_cast<size_t>(2) * 2 * 4, 0x40);
    RxTextureDesc td{};
    td.pixels = pixels.data();
    td.pixelBytes = pixels.size();
    td.width = 2;
    td.height = 2;
    td.format = RX_FORMAT_RGBA8_UNORM;
    td.generateMips = 0;

    IRxTexture* first = nullptr;
    REQUIRE(system->createTexture2D(&td, &first) == RX_OK);
    CHECK(probeRefCount(first) == 1);

    REQUIRE(instance->setTexture("albedoIndex", first) == RX_OK);
    CHECK(probeRefCount(first) == 2);  // instance now holds its own reference too

    // Overwriting the same parameter releases the previously stored
    // texture and addRef()s the new one.
    IRxTexture* second = nullptr;
    REQUIRE(system->createTexture2D(&td, &second) == RX_OK);
    REQUIRE(instance->setTexture("albedoIndex", second) == RX_OK);
    CHECK(probeRefCount(first) == 1);   // instance's reference on `first` was released
    CHECK(probeRefCount(second) == 2);  // instance now holds a reference on `second`

    first->release();  // drop the caller's own ref -- `first` is fully destroyed now.

    // Destroying the instance releases whatever it still holds
    // (`second`) exactly once.
    instance->release();
    CHECK(probeRefCount(second) == 1);  // only the caller's own ref remains
    second->release();

    material->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("loadMaterial maps a real Slang compile failure to RX_E_COMPILE, never throws across the boundary, and "
          "leaves no orphaned live API object behind") {
    auto fixture = makeFixture("rx_api_factory_bad_module");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_bad_module"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    // [Fix round 1, task-6-review.md F2] Recorded AFTER the system object
    // itself exists (so this count only reflects what `loadMaterial`
    // below does, not construction of `system`), immediately before the
    // call that is expected to fail. A failed loadMaterial() must never
    // leave behind a live MaterialImpl API object -- this is the
    // detail::debugLiveApiObjectCount() seam's exact purpose (see
    // rx_api_detail.h). Note what this DOES and DOES NOT prove: it
    // observes only IRxUnknown-rooted API wrapper objects
    // (MaterialSystemImpl/MaterialImpl/MaterialInstanceImpl), not
    // rx::material::MaterialSystem's own internal MaterialRecord/GPU
    // resources (Task 5's MaterialSystem exposes no count accessor for
    // those) -- so it cannot directly observe F2's specific internal-
    // record-orphaning failure mode. It DOES prove the API layer itself
    // never constructs-then-abandons a MaterialImpl on this failure
    // path, which was already true before the F2 reorder (loadMaterial
    // only ever calls `new MaterialImpl(...)` after every prior step has
    // already succeeded) and remains true after it -- a real, if
    // narrower, regression guard.
    uint64_t liveBefore = rx::material::detail::debugLiveApiObjectCount();

    IRxMaterial* material = reinterpret_cast<IRxMaterial*>(static_cast<uintptr_t>(0x1));  // poisoned
    std::string badPath = testDataPath("test_bad_syntax.slang").string();
    CHECK(system->loadMaterial(badPath.c_str(), &material) == RX_E_COMPILE);
    CHECK(material == nullptr);
    CHECK(rx::material::detail::debugLiveApiObjectCount() == liveBefore);

    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Stage 0 audit F3 regression, GPU-backed half] api_impl.cpp:534 used to
// construct std::filesystem::path OUTSIDE loadMaterial()'s try block
// (api_impl.cpp:548 also called path.string() inside the catch, which
// would itself have been unreachable if construction is what threw). On
// the windows-cross-zig target (path::value_type == wchar_t there), a
// byte sequence invalid as UTF-8 makes that narrow->wide construction
// throw std::system_error, unwinding straight across this ABI boundary.
// The fix moved the construction inside the try and switched the
// catch-side log to the raw `slangModulePath` const char*.
//
// This test calls the REAL, production loadMaterial() against a real
// internal_ MaterialSystem (a real VkDevice) -- the device-free case in
// test_api_contract.cpp cannot reach this construction line at all
// (internal_ == nullptr short-circuits first there; see that test's own
// comment), so THIS is the case that genuinely exercises the fixed code,
// not merely a structural "returned a documented RxResult" check.
//
// Honesty note (stated once here, not re-derived elsewhere): on Linux/
// libstdc++ (every POSIX libc++, in fact), std::filesystem::path::
// value_type is `char`, so constructing a path from a `const char*` is a
// straight byte copy with no narrow->wide conversion at all -- this exact
// input can never actually throw std::system_error on this platform, on
// either side of the fix. What this test DOES prove on Linux: the moved
// construction still lets malformed byte content flow safely all the way
// through to the real internal MaterialSystem's own file-open failure
// (there is no file at this path), surfacing as the same documented
// RX_E_COMPILE a genuine Slang compile failure produces (see the test
// immediately above), never a crash or an uncaught exception. The
// std::system_error-specific half of F3 is windows-cross-only and
// verified here only by inspection of the fix itself -- this repo's CI
// has no windows-cross GPU job to run a device-backed test like this one
// against on that target (Stage 0 audit's own F8 observation).
TEST_CASE("loadMaterial against a real internal MaterialSystem rejects a byte sequence invalid as UTF-8 as the "
          "module path with a documented error code, never throws or terminates") {
    auto fixture = makeFixture("rx_api_factory_malformed_path");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_malformed_path"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    uint64_t liveBefore = rx::material::detail::debugLiveApiObjectCount();

    // Same malformed byte sequence as test_api_contract.cpp's device-free
    // case -- see that test's own comment for why each byte is invalid
    // UTF-8 and why the NUL sits only at the true end.
    const char kMalformedUtf8Path[] = {'b', 'a', 'd', '_', static_cast<char>(0xFF), static_cast<char>(0xFE),
                                        static_cast<char>(0x80), '.', 's', 'l', 'a', 'n', 'g', '\0'};

    IRxMaterial* material = reinterpret_cast<IRxMaterial*>(static_cast<uintptr_t>(0x1));  // poisoned
    RxResult result = system->loadMaterial(kMalformedUtf8Path, &material);
    CHECK(result == RX_E_COMPILE);
    CHECK(material == nullptr);
    CHECK(rx::material::detail::debugLiveApiObjectCount() == liveBefore);

    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("entry-point audit: IRxMaterialInstance's three setters return a documented RxResult across "
          "normal/edge/malformed inputs against a real instance, never crash") {
    auto fixture = makeFixture("rx_api_factory_setter_audit");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_setter_audit"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    IRxMaterial* material = nullptr;
    std::string unlitPath = testDataPath("test_unlit.slang").string();
    REQUIRE(system->loadMaterial(unlitPath.c_str(), &material) == RX_OK);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);

    auto* texture = new FakeTexture();
    float value4[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    // A long name deliberately pokes at std::string's allocation path
    // (the exact code F1 found without a try/catch) -- not an OOM
    // injection, just a normal-sized-heap-allocation input rather than a
    // short-string-optimized one.
    std::string longName(4096, 'x');

    const char* names[] = {"tint", "nonexistent", "", longName.c_str()};
    for (const char* name : names) {
        CHECK(isDocumentedResult(instance->setFloat(name, 1.0F)));
        CHECK(isDocumentedResult(instance->setFloat4(name, value4)));
        CHECK(isDocumentedResult(instance->setTexture(name, texture)));
    }

    // Null-argument edges on all three setters.
    CHECK(isDocumentedResult(instance->setFloat(nullptr, 1.0F)));
    CHECK(isDocumentedResult(instance->setFloat4("tint", nullptr)));
    CHECK(isDocumentedResult(instance->setFloat4(nullptr, value4)));
    CHECK(isDocumentedResult(instance->setTexture("tint", nullptr)));
    CHECK(isDocumentedResult(instance->setTexture(nullptr, texture)));

    texture->release();
    instance->release();
    material->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Task 7 =============================================================
// [coordinator addition 2] The setters below no longer validate against a
// second, throwaway Slang reflection session (Task 6's own
// reflectMaterialParams(), now deleted) -- they read straight from
// rx::material::MaterialSystem::materialParams()/paramBlockSize(), and
// write directly into MaterialInstanceImpl's own real CPU-side blob at
// each field's reflected byte offset. These tests observe that blob
// directly through rx_api_detail.h's bridge accessors (the same accessors
// a future sample's draw-time bindInstance() call would use) -- this is
// the mandatory "instance param write->readback of arena blob at
// reflected offsets (exact bytes)" case from the Task 7 brief's test list,
// exercised at the ABI layer (test_material_system.cpp covers the
// identical claim at the internal MaterialSystem layer already).
TEST_CASE("IRxMaterialInstance's setters write byte-exact values into the internal blob at the material's own "
          "reflected offsets") {
    auto fixture = makeFixture("rx_api_factory_blob_bytes");
    if (!fixture.has_value()) {
        return;
    }

    auto internal =
        rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("blob_bytes"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    IRxMaterial* material = nullptr;
    std::string unlitPath = testDataPath("test_unlit.slang").string();
    REQUIRE(system->loadMaterial(unlitPath.c_str(), &material) == RX_OK);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);

    rx::material::MaterialHandle handle = rx::material::detail::materialHandle(material);
    const std::vector<rx::material::MaterialParamInfo>& params = internal->materialParams(handle);
    REQUIRE(params.size() == 1);
    CHECK(params[0].name == "tint");
    CHECK(params[0].offset == 0);

    // Fresh instance: blob is zero-initialized at every reflected byte.
    const void* blobBefore = rx::material::detail::materialInstanceBlobData(instance);
    size_t blobSize = rx::material::detail::materialInstanceBlobSize(instance);
    REQUIRE(blobSize == internal->paramBlockSize(handle));
    REQUIRE(blobSize >= params[0].offset + params[0].size);
    std::vector<uint8_t> zeros(blobSize, 0);
    CHECK(std::memcmp(blobBefore, zeros.data(), blobSize) == 0);

    float tint[4] = {0.125F, 0.25F, 0.5F, 1.0F};
    REQUIRE(instance->setFloat4("tint", tint) == RX_OK);

    const void* blobAfter = rx::material::detail::materialInstanceBlobData(instance);
    CHECK(std::memcmp(static_cast<const uint8_t*>(blobAfter) + params[0].offset, tint, sizeof(tint)) == 0);

    instance->release();
    material->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [coordinator addition 4] createTexture2D() through the public ABI, and
// setTexture()'s real (not FakeTexture-double) bindless-index write path.
TEST_CASE("IRxMaterialSystem::createTexture2D creates a real texture; setTexture writes its REAL bindless index "
          "into the instance blob") {
    auto fixture = makeFixture("rx_api_factory_create_texture");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_create_texture"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    constexpr uint32_t kWidth = 2;
    constexpr uint32_t kHeight = 2;
    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4, 0x7F);

    RxTextureDesc textureDesc{};
    textureDesc.pixels = pixels.data();
    textureDesc.pixelBytes = pixels.size();
    textureDesc.width = kWidth;
    textureDesc.height = kHeight;
    textureDesc.format = RX_FORMAT_RGBA8_UNORM;
    textureDesc.generateMips = 0;

    IRxTexture* texture = nullptr;
    REQUIRE(system->createTexture2D(&textureDesc, &texture) == RX_OK);
    REQUIRE(texture != nullptr);

    IRxMaterial* material = nullptr;
    std::string texturedPath = testDataPath("test_textured.slang").string();
    REQUIRE(system->loadMaterial(texturedPath.c_str(), &material) == RX_OK);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);

    rx::material::MaterialHandle materialHandle = rx::material::detail::materialHandle(material);
    const std::vector<rx::material::MaterialParamInfo>& params = internal->materialParams(materialHandle);
    REQUIRE(params.size() == 1);
    CHECK(params[0].name == "albedoIndex");

    REQUIRE(instance->setTexture("albedoIndex", texture) == RX_OK);

    // The blob must contain the texture's REAL bindless index -- this
    // fixture builds a fresh rx::rhi::BindlessTable per test, and this is
    // the very first sampled-image registration against it, so the real
    // index is deterministically 0 (bindless.cpp registers sequentially
    // from index 0).
    const void* blob = rx::material::detail::materialInstanceBlobData(instance);
    uint32_t writtenIndex = 0;
    std::memcpy(&writtenIndex, static_cast<const uint8_t*>(blob) + params[0].offset, sizeof(uint32_t));
    CHECK(writtenIndex == 0);

    instance->release();
    material->release();
    texture->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("IRxMaterialSystem::createTexture2D validates desc/outTexture and rejects malformed input") {
    auto fixture = makeFixture("rx_api_factory_create_texture_invalid");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_create_texture_invalid"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    IRxTexture* texture = reinterpret_cast<IRxTexture*>(static_cast<uintptr_t>(0x1));  // poisoned
    CHECK(system->createTexture2D(nullptr, &texture) == RX_E_INVALIDARG);
    CHECK(texture == nullptr);

    std::vector<uint8_t> pixels(16, 0);
    RxTextureDesc validDesc{};
    validDesc.pixels = pixels.data();
    validDesc.pixelBytes = pixels.size();
    validDesc.width = 2;
    validDesc.height = 2;
    validDesc.format = RX_FORMAT_RGBA8_UNORM;
    CHECK(system->createTexture2D(&validDesc, nullptr) == RX_E_INVALIDARG);

    RxTextureDesc zeroWidth = validDesc;
    zeroWidth.width = 0;
    texture = nullptr;
    CHECK(system->createTexture2D(&zeroWidth, &texture) == RX_E_INVALIDARG);
    CHECK(texture == nullptr);

    RxTextureDesc noPixels = validDesc;
    noPixels.pixels = nullptr;
    noPixels.pixelBytes = 0;
    CHECK(system->createTexture2D(&noPixels, &texture) == RX_E_INVALIDARG);

    RxTextureDesc badFormat = validDesc;
    badFormat.format = 999;
    CHECK(system->createTexture2D(&badFormat, &texture) == RX_E_INVALIDARG);

    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [coordinator addition 1] reloadChanged() forwards to the real internal
// MaterialSystem::reloadChanged() (D9) rather than the Task 6 documented
// no-op -- verified by actually editing the loaded module on disk and
// confirming (via the SAME internal rx::material::MaterialSystem* this
// test already holds, per RxMaterialSystemDesc's own bridge contract) that
// the pipeline this material builds genuinely changes.
TEST_CASE("IRxMaterialSystem::reloadChanged forwards to the real internal MaterialSystem and actually reloads a "
          "changed module") {
    auto fixture = makeFixture("rx_api_factory_reload");
    if (!fixture.has_value()) {
        return;
    }

    auto internal =
        rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("api_reload"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    std::filesystem::path modulePath = std::filesystem::temp_directory_path() / "rx_api_factory_reload_test.slang";
    auto baseTime = std::filesystem::file_time_type::clock::now();
    {
        std::ofstream out(modulePath, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out << R"(
import material;
struct UnlitParams { float4 tint; };
[[vk::binding(0, 1)]] ParameterBlock<UnlitParams> gParams;
struct Unlit : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        float4 tint = gParams.tint;
        tint.x += v.worldPos.x * 1e-4;
        tint.y += v.normal.y * 1e-4;
        tint.z += v.uv.x * 1e-4;
        tint.w += v.tangent.x * 1e-4;
        tint.x += v.lightDirWorld.x * 1e-4;
        tint.y += v.lightColor.x * 1e-4;
        tint.z += v.ambientColor.x * 1e-4;
        tint.w += v.cameraPosWorld.x * 1e-4;
        return tint;
    }
};
export struct MaterialImpl : IMaterialShader = Unlit;
)";
    }
    std::error_code ec;
    std::filesystem::last_write_time(modulePath, baseTime, ec);
    REQUIRE_FALSE(ec);

    IRxMaterial* material = nullptr;
    std::string modulePathStr = modulePath.string();
    REQUIRE(system->loadMaterial(modulePathStr.c_str(), &material) == RX_OK);
    rx::material::MaterialHandle handle = rx::material::detail::materialHandle(material);
    uint64_t hashBefore = internal->moduleHash(handle);

    // A reloadChanged() call before any edit must return RX_OK and change
    // nothing.
    CHECK(system->reloadChanged() == RX_OK);
    CHECK(internal->moduleHash(handle) == hashBefore);

    {
        std::ofstream out(modulePath, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out << R"(
import material;
struct UnlitParams { float4 tint; };
[[vk::binding(0, 1)]] ParameterBlock<UnlitParams> gParams;
struct Unlit : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        float4 tint = gParams.tint * 0.5;
        tint.x += v.worldPos.x * 1e-4;
        tint.y += v.normal.y * 1e-4;
        tint.z += v.uv.x * 1e-4;
        tint.w += v.tangent.x * 1e-4;
        tint.x += v.lightDirWorld.x * 1e-4;
        tint.y += v.lightColor.x * 1e-4;
        tint.z += v.ambientColor.x * 1e-4;
        tint.w += v.cameraPosWorld.x * 1e-4;
        return tint;
    }
};
export struct MaterialImpl : IMaterialShader = Unlit;
)";
    }
    std::filesystem::last_write_time(modulePath, baseTime + std::chrono::seconds(1), ec);
    REQUIRE_FALSE(ec);

    CHECK(system->reloadChanged() == RX_OK);
    CHECK(internal->moduleHash(handle) != hashBefore);

    material->release();
    system->release();
    std::filesystem::remove(modulePath, ec);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// reloadChanged() on a device-free (internal_ == nullptr) instance stays a
// safe, documented no-op -- Task 6's own contract, unchanged: this method
// never touches `internal_` when null.
TEST_CASE("IRxMaterialSystem::reloadChanged is a safe no-op on a device-free instance") {
    RxMaterialSystemDesc desc{nullptr};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);
    CHECK(system->reloadChanged() == RX_OK);
    system->release();
}

// ===== Task 4 [seed-10 carried] ===========================================
// The carried bar this task closes (Phase 3 Task 8's own review, "The
// central question: texture path vs. texture sampling"): a
// `createTexture2D`-created texture bound via `setTexture` must VISIBLY
// change the rendered image, not merely reach a GPU-visible blob numerically
// inert. `test_textured_sample.slang` (tests/data/) is the fixture --
// `evaluate()` returns `rx_sampleTexture(gParams.albedoIndex, uv)` directly,
// unlike test_textured.slang's own `albedoIndex` (still only read
// numerically, kept as the "real bindless-index PATH, not sampling" case
// this task's own review found and this fixture is deliberately NOT).
namespace {
constexpr std::array<uint8_t, 4> kQuadTopLeft{255, 0, 0, 255};
constexpr std::array<uint8_t, 4> kQuadTopRight{0, 255, 0, 255};
constexpr std::array<uint8_t, 4> kQuadBottomLeft{0, 0, 255, 255};
constexpr std::array<uint8_t, 4> kQuadBottomRight{255, 255, 0, 255};

// Row-major 2x2 RGBA8 buffer: row 0 = [topLeft, topRight], row 1 =
// [bottomLeft, bottomRight] -- matches renderQuadAndReadbackQuadrants()'s
// own UV<->quadrant derivation exactly.
std::vector<uint8_t> makeQuadTestPixels() {
    std::vector<uint8_t> pixels;
    pixels.reserve(16);
    pixels.insert(pixels.end(), kQuadTopLeft.begin(), kQuadTopLeft.end());
    pixels.insert(pixels.end(), kQuadTopRight.begin(), kQuadTopRight.end());
    pixels.insert(pixels.end(), kQuadBottomLeft.begin(), kQuadBottomLeft.end());
    pixels.insert(pixels.end(), kQuadBottomRight.begin(), kQuadBottomRight.end());
    return pixels;
}

void checkQuadrantPixels(const QuadrantPixels& readback) {
    CHECK(std::memcmp(readback.topLeft.data(), kQuadTopLeft.data(), 4) == 0);
    CHECK(std::memcmp(readback.topRight.data(), kQuadTopRight.data(), 4) == 0);
    CHECK(std::memcmp(readback.bottomLeft.data(), kQuadBottomLeft.data(), 4) == 0);
    CHECK(std::memcmp(readback.bottomRight.data(), kQuadBottomRight.data(), 4) == 0);
}
}  // namespace

TEST_CASE("IRxMaterialInstance::setTexture's bound texture actually changes the rendered image via "
          "rx_sampleTexture (public API up to the internal draw-time bridge)") {
    auto fixture = makeFixture("rx_api_factory_sample_texture");
    if (!fixture.has_value()) {
        return;
    }

    // [Fix round, sampler-wrap P0] CLAMP_TO_EDGE requested EXPLICITLY, not
    // inherited from MaterialSystem::create()'s own default (now REPEAT,
    // glTF's spec default -- see that method's own header comment and
    // material_system.cpp's `defaultSamplerInfo` comment for the full
    // account of the defect this closes). This test's own 2x2 test
    // texture (test_textured_sample.slang's `evaluate()`, sampled via the
    // TWO-argument rx_sampleTexture() overload -- this is the ORIGINAL
    // Task 4 seam-bleed regression: the quadrant probes sit right up to
    // UV 0/1, and a deliberately non-tiled texture like this one needs
    // CLAMP_TO_EDGE to avoid REPEAT's wrong-neighbor bleed (task-4-report.
    // md's own "What went wrong once" section) -- unchanged assertions,
    // now reached via an explicit request instead of an implicit default.
    auto internal =
        rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("api_sample_texture"),
                                              /*sharedShaderDir=*/{}, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    std::vector<uint8_t> pixels = makeQuadTestPixels();
    RxTextureDesc textureDesc{};
    textureDesc.pixels = pixels.data();
    textureDesc.pixelBytes = pixels.size();
    textureDesc.width = 2;
    textureDesc.height = 2;
    textureDesc.format = RX_FORMAT_RGBA8_UNORM;
    textureDesc.generateMips = 0;

    IRxTexture* texture = nullptr;
    REQUIRE(system->createTexture2D(&textureDesc, &texture) == RX_OK);
    REQUIRE(texture != nullptr);

    IRxMaterial* material = nullptr;
    std::string materialPath = testDataPath("test_textured_sample.slang").string();
    REQUIRE(system->loadMaterial(materialPath.c_str(), &material) == RX_OK);
    REQUIRE(material != nullptr);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);
    REQUIRE(instance->setTexture("albedoIndex", texture) == RX_OK);

    rx::material::MaterialHandle handle = rx::material::detail::materialHandle(material);
    const void* blob = rx::material::detail::materialInstanceBlobData(instance);
    size_t blobSize = rx::material::detail::materialInstanceBlobSize(instance);

    auto readback =
        renderQuadAndReadbackQuadrants(fixture->device, fixture->bindless, *internal, handle, blob, blobSize);
    REQUIRE(readback.has_value());
    checkQuadrantPixels(*readback);

    instance->release();
    material->release();
    texture->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Task 4] Hot-reload regression: a textured material keeps sampling
// correctly across reloadChanged() (D9) -- not merely keeping SOME
// last-good pipeline (Task 7's own general reload machinery already
// covers that thoroughly), but specifically that the re-rendered image
// still shows the real texture content after a genuine recompile.
TEST_CASE("hot-reload of a textured material keeps sampling correctly (reloadChanged() then re-render)") {
    auto fixture = makeFixture("rx_api_factory_sample_texture_reload");
    if (!fixture.has_value()) {
        return;
    }

    // [Fix round, sampler-wrap P0] CLAMP_TO_EDGE requested EXPLICITLY -- see
    // the sibling "setTexture's bound texture..." TEST_CASE above for the
    // full account (same quadrant-probe/non-tiled-texture reasoning; this
    // test reuses the identical fixture shape across a reload).
    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_sample_texture_reload"),
                                                            /*sharedShaderDir=*/{},
                                                            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    std::vector<uint8_t> pixels = makeQuadTestPixels();
    RxTextureDesc textureDesc{};
    textureDesc.pixels = pixels.data();
    textureDesc.pixelBytes = pixels.size();
    textureDesc.width = 2;
    textureDesc.height = 2;
    textureDesc.format = RX_FORMAT_RGBA8_UNORM;
    textureDesc.generateMips = 0;

    IRxTexture* texture = nullptr;
    REQUIRE(system->createTexture2D(&textureDesc, &texture) == RX_OK);

    // Loaded from a TEMP COPY, not the committed tests/data/ fixture
    // directly -- reloadChanged() (D9) needs a real on-disk mtime/content
    // change to detect, and this test must never mutate a committed
    // fixture file.
    std::filesystem::path modulePath =
        std::filesystem::temp_directory_path() / "rx_api_factory_textured_sample_reload_test.slang";
    auto baseTime = std::filesystem::file_time_type::clock::now();
    auto writeModule = [&](const char* content, std::filesystem::file_time_type mtime) {
        std::ofstream out(modulePath, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out << content;
        out.close();
        std::error_code ec;
        std::filesystem::last_write_time(modulePath, mtime, ec);
        REQUIRE_FALSE(ec);
    };

    constexpr const char* kV1 = R"(
import material;
struct TexturedSampleParams { uint albedoIndex; };
[[vk::binding(0, 1)]] ParameterBlock<TexturedSampleParams> gParams;
struct TexturedSample : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        float4 sampled = rx_sampleTexture(gParams.albedoIndex, v.uv);
        sampled.x += v.worldPos.x * 1e-6;
        sampled.y += v.normal.y * 1e-6;
        sampled.z += v.tangent.x * 1e-6;
        sampled.w += v.lightDirWorld.x * 1e-6;
        sampled.x += v.lightColor.x * 1e-6;
        sampled.y += v.ambientColor.x * 1e-6;
        sampled.z += v.cameraPosWorld.x * 1e-6;
        return sampled;
    }
};
export struct MaterialImpl : IMaterialShader = TexturedSample;
)";
    // v2: textually different (one inert extra statement) but semantically
    // identical sampling behavior -- forces a real content-hash change and
    // a real recompile [D9]; the point of this regression is that the SAME
    // sampling result survives a genuine reload, not that the math changes.
    constexpr const char* kV2 = R"(
import material;
struct TexturedSampleParams { uint albedoIndex; };
[[vk::binding(0, 1)]] ParameterBlock<TexturedSampleParams> gParams;
struct TexturedSample : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        float4 sampled = rx_sampleTexture(gParams.albedoIndex, v.uv);
        sampled.x += v.worldPos.x * 1e-6;
        sampled.y += v.normal.y * 1e-6;
        sampled.z += v.tangent.x * 1e-6;
        sampled.w += v.lightDirWorld.x * 1e-6;
        sampled.x += v.lightColor.x * 1e-6;
        sampled.y += v.ambientColor.x * 1e-6;
        sampled.z += v.cameraPosWorld.x * 1e-6;
        sampled.z += 0.0;
        return sampled;
    }
};
export struct MaterialImpl : IMaterialShader = TexturedSample;
)";

    writeModule(kV1, baseTime);

    IRxMaterial* material = nullptr;
    std::string modulePathStr = modulePath.string();
    REQUIRE(system->loadMaterial(modulePathStr.c_str(), &material) == RX_OK);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);
    REQUIRE(instance->setTexture("albedoIndex", texture) == RX_OK);

    rx::material::MaterialHandle handle = rx::material::detail::materialHandle(material);
    uint64_t hashBefore = internal->moduleHash(handle);

    {
        const void* blob = rx::material::detail::materialInstanceBlobData(instance);
        size_t blobSize = rx::material::detail::materialInstanceBlobSize(instance);
        auto readback =
            renderQuadAndReadbackQuadrants(fixture->device, fixture->bindless, *internal, handle, blob, blobSize);
        REQUIRE(readback.has_value());
        checkQuadrantPixels(*readback);
    }

    writeModule(kV2, baseTime + std::chrono::seconds(1));
    REQUIRE(system->reloadChanged() == RX_OK);
    CHECK(internal->moduleHash(handle) != hashBefore);

    {
        const void* blob = rx::material::detail::materialInstanceBlobData(instance);
        size_t blobSize = rx::material::detail::materialInstanceBlobSize(instance);
        auto readback =
            renderQuadAndReadbackQuadrants(fixture->device, fixture->bindless, *internal, handle, blob, blobSize);
        REQUIRE(readback.has_value());
        checkQuadrantPixels(*readback);
    }

    instance->release();
    material->release();
    texture->release();
    system->release();
    std::error_code ec;
    std::filesystem::remove(modulePath, ec);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
