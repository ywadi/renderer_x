#include <doctest/doctest.h>
#include <rx_asset/geometry_pool.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_rhi_vk/upload.h>
#include <rx_platform/window.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// geometry_pool_test.cpp -- coverage for rx::asset::GeometryPool [Phase 4
// Stage 1 Task 12, spec D8/D9, gate matrix-issue21 as amended by
// rulings-2026-08-18.md]. GPU tests only -- device-free is impossible for
// this class's real behavior (every upload() call allocates a real
// device-local VkBuffer pair and records a real GPU copy through
// rx::rhi::Uploader). BDA enablement gets its own file (bda_test.cpp);
// the #27 accounting-integration criterion gets its own file
// (accounting_test.cpp) -- both share this file's fixture shape but are
// kept separate, matching the existing rx_rhi_vk_tests convention of one
// file per feature-set (memory_budget_test.cpp/oom_handling_test.cpp
// alongside buffer_test.cpp).

namespace {

struct GpTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::Allocator allocator;
    rx::rhi::Uploader uploader;
};

std::optional<GpTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping GeometryPool test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping GeometryPool test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());

    auto uploader = rx::rhi::Uploader::create(*allocator, *device);
    REQUIRE(uploader.has_value());

    return GpTestFixture{std::move(*window), std::move(*context), std::move(*device), std::move(*allocator),
                          std::move(*uploader)};
}

// A `count`-vertex mesh with content that uniquely identifies both which
// mesh it is (`meshId`) and each vertex's index within it -- so a byte-
// exact readback comparison can prove BOTH "this data landed at the
// offset GeometryPool reported" AND "no other mesh's bytes bled into this
// range". Positions are also placed in NDC so the SAME data doubles as
// real drawable geometry for the exhaustion/readback test below: `xSpan`
// selects which half of clip space (e.g. {-1,0} or {0,1}) a 4-vertex quad
// occupies. Non-quad vertex counts (used by the fragmentation/reuse tests,
// which never draw) just get an arbitrary NDC-adjacent position -- content
// correctness is all those tests check.
std::vector<rx::asset::PoolVertex> makeVerts(uint32_t count, uint32_t meshId) {
    std::vector<rx::asset::PoolVertex> verts(count);
    for (uint32_t i = 0; i < count; ++i) {
        rx::asset::PoolVertex& v = verts[i];
        v.px = static_cast<float>(meshId) * 1000.0F + static_cast<float>(i);
        v.py = static_cast<float>(i) * 0.25F;
        v.pz = 0.0F;
        v.nx = 1.0F;
        v.ny = static_cast<float>(meshId);
        v.nz = static_cast<float>(i);
        v.tx = 2.0F;
        v.ty = static_cast<float>(meshId);
        v.tz = static_cast<float>(i);
        v.tw = 1.0F;
        v.u = static_cast<float>(meshId);
        v.v = static_cast<float>(i);
    }
    return verts;
}

std::vector<uint32_t> makeIndices(uint32_t count, uint32_t meshId) {
    std::vector<uint32_t> indices(count);
    for (uint32_t i = 0; i < count; ++i) {
        // Encodes meshId too (not just 0..count-1) so a content-readback
        // comparison on the index side is just as discriminating as the
        // vertex side -- a real index buffer's values are usually small
        // vertex-index references, but nothing in GeometryPool interprets
        // these bytes, so using them as an identity pattern is safe and
        // strictly stronger than an ordinary ascending sequence shared by
        // every mesh.
        indices[i] = meshId * 100 + i;
    }
    return indices;
}

// A real drawable quad in NDC (4 vertices, 6 indices -- two triangles),
// occupying [xMin, xMax] x [-1, 1]. Used by the exhaustion/readback test
// to actually render from two different GeometryPool blocks.
std::pair<std::vector<rx::asset::PoolVertex>, std::vector<uint32_t>> makeQuad(uint32_t meshId, float xMin,
                                                                                float xMax) {
    std::vector<rx::asset::PoolVertex> verts(4);
    const std::array<std::array<float, 2>, 4> corners{{{xMin, -1.0F}, {xMax, -1.0F}, {xMax, 1.0F}, {xMin, 1.0F}}};
    for (size_t i = 0; i < 4; ++i) {
        verts[i] = rx::asset::PoolVertex{};
        verts[i].px = corners[i][0];
        verts[i].py = corners[i][1];
        verts[i].pz = 0.0F;
    }
    (void)meshId;
    std::vector<uint32_t> indices{0, 1, 2, 0, 2, 3};
    return {verts, indices};
}

// Copies `size` bytes out of `src` (assumed device-local, TRANSFER_SRC_BIT)
// into a freshly allocated, invalidated, host-visible buffer -- the same
// pattern rx_rhi_vk/tests/upload_test.cpp's own readBackBuffer() uses.
std::vector<uint8_t> readBackBuffer(rx::rhi::Allocator& allocator, VkDevice device, VkQueue queue,
                                     uint32_t queueFamily, VkBuffer src, VkDeviceSize offset, VkDeviceSize size) {
    auto readback = allocator.createHostVisibleBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    REQUIRE(readback.has_value());

    auto cmdCtx = rx::rhi::CommandContext::create(device, queue, queueFamily);
    REQUIRE(cmdCtx.has_value());
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferCopy region{offset, 0, size};
        vkCmdCopyBuffer(cmd, src, readback->handle(), 1, &region);
    });

    readback->invalidate();

    std::vector<uint8_t> result(static_cast<size_t>(size));
    std::memcpy(result.data(), readback->mappedData(), result.size());
    return result;
}

}  // namespace

TEST_CASE("GeometryPool::upload two meshes produces distinct, non-overlapping, byte-exact ranges") {
    auto fixture = makeFixture("rx_asset_gp_distinct_ranges");
    if (!fixture.has_value()) {
        return;
    }

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader);
    REQUIRE(pool != nullptr);

    // Deliberately ODD, non-power-of-two-friendly vertex/index counts (5
    // and 7) -- if the 48-byte/4-byte VMA suballocation alignment were
    // ever dropped, an odd element count is exactly what would produce a
    // misaligned byte offset for the SECOND allocation (the first, at
    // offset 0, is trivially aligned regardless).
    auto vertsA = makeVerts(5, /*meshId=*/1);
    auto indicesA = makeIndices(7, /*meshId=*/1);
    auto vertsB = makeVerts(3, /*meshId=*/2);
    auto indicesB = makeIndices(9, /*meshId=*/2);

    rx::asset::MeshRange rangeA = pool->upload(vertsA, indicesA);
    rx::asset::MeshRange rangeB = pool->upload(vertsB, indicesB);

    CHECK(rangeA.blockId == 0);
    CHECK(rangeB.blockId == 0);
    CHECK(rangeA.indexCount == 7);
    CHECK(rangeB.indexCount == 9);

    // Non-overlap: B's vertex/index ranges start no earlier than A's end.
    CHECK(rangeB.vertexOffset >= rangeA.vertexOffset + static_cast<int32_t>(vertsA.size()));
    CHECK(rangeB.firstIndex >= rangeA.firstIndex + static_cast<uint32_t>(indicesA.size()));

    // Byte-exact readback proof: this is the discriminating check for the
    // alignment/offset math (matrix-issue21's "structural invariant"
    // row) -- MeshRange's vertexOffset/firstIndex are already ELEMENT
    // counts, so reconstructing `vertexOffset * sizeof(PoolVertex)` and
    // reading back exactly that many bytes only matches the ORIGINAL
    // pattern if GeometryPool's internal byte-offset -> element-offset
    // conversion was exact (a silently truncated/misaligned offset would
    // read back garbage or a neighboring mesh's bytes instead).
    // vertexBufferHandle()/indexBufferHandle() are test/diagnostic
    // accessors (geometry_pool.h) added specifically so a test can read
    // back a block's raw buffer content directly, without GeometryPool
    // exposing raw handles as part of its production API (bind() is the
    // sanctioned production path).
    VkBuffer vertexBuffer = pool->vertexBufferHandle(rangeA.blockId);
    VkBuffer indexBuffer = pool->indexBufferHandle(rangeA.blockId);
    REQUIRE(vertexBuffer != VK_NULL_HANDLE);
    REQUIRE(indexBuffer != VK_NULL_HANDLE);

    VkDeviceSize vertexOffsetBytesA = static_cast<VkDeviceSize>(rangeA.vertexOffset) * sizeof(rx::asset::PoolVertex);
    VkDeviceSize vertexOffsetBytesB = static_cast<VkDeviceSize>(rangeB.vertexOffset) * sizeof(rx::asset::PoolVertex);
    VkDeviceSize indexOffsetBytesA = static_cast<VkDeviceSize>(rangeA.firstIndex) * sizeof(uint32_t);
    VkDeviceSize indexOffsetBytesB = static_cast<VkDeviceSize>(rangeB.firstIndex) * sizeof(uint32_t);

    std::vector<uint8_t> readVertsA =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                       fixture->device.graphicsQueueFamily(), vertexBuffer, vertexOffsetBytesA,
                       vertsA.size() * sizeof(rx::asset::PoolVertex));
    std::vector<uint8_t> readVertsB =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                       fixture->device.graphicsQueueFamily(), vertexBuffer, vertexOffsetBytesB,
                       vertsB.size() * sizeof(rx::asset::PoolVertex));
    std::vector<uint8_t> readIndicesA =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                       fixture->device.graphicsQueueFamily(), indexBuffer, indexOffsetBytesA,
                       indicesA.size() * sizeof(uint32_t));
    std::vector<uint8_t> readIndicesB =
        readBackBuffer(fixture->allocator, fixture->device.device(), fixture->device.graphicsQueue(),
                       fixture->device.graphicsQueueFamily(), indexBuffer, indexOffsetBytesB,
                       indicesB.size() * sizeof(uint32_t));

    CHECK(std::memcmp(readVertsA.data(), vertsA.data(), readVertsA.size()) == 0);
    CHECK(std::memcmp(readVertsB.data(), vertsB.data(), readVertsB.size()) == 0);
    CHECK(std::memcmp(readIndicesA.data(), indicesA.data(), readIndicesA.size()) == 0);
    CHECK(std::memcmp(readIndicesB.data(), indicesB.data(), readIndicesB.size()) == 0);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("GeometryPool::free releases space that a same-size re-upload reuses without growing a new block") {
    auto fixture = makeFixture("rx_asset_gp_free_reuse");
    if (!fixture.has_value()) {
        return;
    }

    // Chunk sized to hold EXACTLY two 4-vertex/4-index meshes -- this is
    // what makes the discrimination clean: if free() were a no-op (the
    // exact defect this test guards against), the third upload below
    // would NOT fit in block 0 and would force a second block, which the
    // final CHECK(pool->blockCount() == 1) directly catches.
    rx::asset::PoolConfig config;
    config.vertexChunkBytes = 2 * 4 * sizeof(rx::asset::PoolVertex);
    config.indexChunkBytes = 2 * 4 * sizeof(uint32_t);

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader, config);
    REQUIRE(pool != nullptr);

    auto vertsA = makeVerts(4, 1);
    auto indicesA = makeIndices(4, 1);
    auto vertsB = makeVerts(4, 2);
    auto indicesB = makeIndices(4, 2);

    rx::asset::MeshRange rangeA = pool->upload(vertsA, indicesA);
    rx::asset::MeshRange rangeB = pool->upload(vertsB, indicesB);
    REQUIRE(pool->blockCount() == 1);
    CHECK(rangeA.blockId == 0);
    CHECK(rangeB.blockId == 0);

    const rx::asset::PoolStats statsFull = pool->stats();
    CHECK(statsFull.vertexBytesUsed == 8 * sizeof(rx::asset::PoolVertex));
    CHECK(statsFull.indexBytesUsed == 8 * sizeof(uint32_t));

    pool->free(rangeA);
    const rx::asset::PoolStats statsAfterFree = pool->stats();
    CHECK(statsAfterFree.vertexBytesUsed == 4 * sizeof(rx::asset::PoolVertex));
    CHECK(statsAfterFree.indexBytesUsed == 4 * sizeof(uint32_t));
    CHECK(statsAfterFree.blockCount == 1);

    auto vertsC = makeVerts(4, 3);
    auto indicesC = makeIndices(4, 3);
    rx::asset::MeshRange rangeC = pool->upload(vertsC, indicesC);

    // The load-bearing assertion: block 0's chunk was sized to hold
    // EXACTLY two meshes, B is still live, so C can only land in block 0
    // if A's freed space was genuinely reused -- a broken/no-op free()
    // forces a second block here instead.
    CHECK(rangeC.blockId == 0);
    CHECK(pool->blockCount() == 1);

    const rx::asset::PoolStats statsAfterReuse = pool->stats();
    CHECK(statsAfterReuse.vertexBytesUsed == 8 * sizeof(rx::asset::PoolVertex));
    CHECK(statsAfterReuse.indexBytesUsed == 8 * sizeof(uint32_t));

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("GeometryPool checkerboard fragmentation forces the exhaustion/new-block path -- proves no hidden "
          "compaction (D9)") {
    auto fixture = makeFixture("rx_asset_gp_checkerboard");
    if (!fixture.has_value()) {
        return;
    }

    // Chunk sized to hold EXACTLY four 4-vertex/4-index meshes.
    rx::asset::PoolConfig config;
    config.vertexChunkBytes = 4 * 4 * sizeof(rx::asset::PoolVertex);
    config.indexChunkBytes = 4 * 4 * sizeof(uint32_t);

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader, config);
    REQUIRE(pool != nullptr);

    std::array<rx::asset::MeshRange, 4> ranges{};
    for (uint32_t i = 0; i < 4; ++i) {
        auto verts = makeVerts(4, i);
        auto indices = makeIndices(4, i);
        ranges[i] = pool->upload(verts, indices);
        REQUIRE(ranges[i].blockId == 0);
    }
    REQUIRE(pool->blockCount() == 1);

    // Checkerboard: free slots 1 and 3, leaving two ISOLATED 4-vertex/
    // 4-index holes with a live allocation on at least one side of each
    // (never adjacent to each other), so they can never coalesce into one
    // larger contiguous span.
    pool->free(ranges[1]);
    pool->free(ranges[3]);
    CHECK(pool->blockCount() == 1);

    const rx::asset::PoolStats statsAfterFree = pool->stats();
    // Two 4-vertex/4-index holes freed out of four -- half the chunk is
    // free in TOTAL, but never in one contiguous piece.
    CHECK(statsAfterFree.vertexBytesUsed == 2 * 4 * sizeof(rx::asset::PoolVertex));
    CHECK(statsAfterFree.indexBytesUsed == 2 * 4 * sizeof(uint32_t));

    // Request 6 vertices/6 indices: LARGER than any single hole (4
    // elements) but SMALLER than total free bytes across both holes (8
    // elements). A defragmenting/compacting allocator could satisfy this
    // by moving live data to consolidate the two holes; GeometryPool must
    // NOT do that (D9) -- it must take the exhaustion/new-block path
    // instead, exactly like running out of space for real.
    auto vertsNew = makeVerts(6, 99);
    auto indicesNew = makeIndices(6, 99);
    rx::asset::MeshRange rangeNew = pool->upload(vertsNew, indicesNew);

    CHECK(rangeNew.blockId == 1);
    CHECK(pool->blockCount() == 2);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

namespace {

// Minimal push-constant-color fill pipeline over a POSITION-only vertex
// input bound at GeometryPool's own PoolVertex stride -- exactly the
// no-descriptor-set shape rx_graph/tests/test_execute_gpu.cpp's own
// "Fill" pipeline uses (buildFillPipeline()), adapted to read POSITION
// from a real vertex buffer (GeometryPool's) instead of hardcoding NDC
// positions in the shader. No bindless table, no material system --
// GeometryPool's own tests must not depend on either.
const char* kDrawShaderSource = R"(
struct PushConstants { float4 color; };
[[vk::push_constant]] ConstantBuffer<PushConstants> gPush;

struct VSIn { float3 position : POSITION; };
struct VSOut { float4 position : SV_Position; };

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    VSOut o;
    o.position = float4(input.position, 1.0);
    return o;
}

[shader("fragment")]
float4 fsMain() : SV_Target
{
    return gPush.color;
}
)";

const std::vector<std::string> kDrawEntryPoints = {"vsMain", "fsMain"};

struct DrawPipeline {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    // Reflected push-constant stage mask (rx_shader's reflect()) -- even
    // though only fsMain reads gPush.color, Slang/reflect() report the
    // push-constant range as visible to EVERY stage that can see the
    // `[[vk::push_constant]]` global's declaration in the linked module
    // (VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT here), and
    // Vulkan requires vkCmdPushConstants()'s own stageFlags to cover the
    // WHOLE overlapping VkPushConstantRange, not just the stage that
    // happens to read the value (VUID-vkCmdPushConstants-offset-01796) --
    // this is exactly what rx_graph/tests/test_execute_gpu.cpp's own
    // "Fill"/"Invert" pipelines already do (never hardcode a single stage
    // flag), reproduced here.
    VkShaderStageFlags pushConstantStages = 0;
};

void destroyDrawPipeline(VkDevice device, DrawPipeline& p) {
    if (p.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, p.pipeline, nullptr);
    }
    if (p.fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, p.fragModule, nullptr);
    }
    if (p.vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, p.vertModule, nullptr);
    }
    p.pipeline = VK_NULL_HANDLE;
    p.fragModule = VK_NULL_HANDLE;
    p.vertModule = VK_NULL_HANDLE;
    p.layoutBundle = rx::rhi::PipelineLayoutBundle{};
}

std::optional<DrawPipeline> buildDrawPipeline(VkDevice device, VkFormat colorFormat) {
    DrawPipeline result;

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        return std::nullopt;
    }

    rx::shader::CompileResult compileResult =
        compiler->compileFromSource("RxAssetDrawModule", kDrawShaderSource, kDrawEntryPoints);
    if (!compileResult.ok) {
        MESSAGE("rx_asset draw shader compile failed: ", compileResult.diagnostics);
        return std::nullopt;
    }

    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value() || layoutInfo->pushRanges.size() != 1) {
        return std::nullopt;
    }

    // No externalSet0 -- this shader declares no descriptor sets at all.
    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, *layoutInfo);
    if (!layoutBundle.has_value()) {
        return std::nullopt;
    }
    result.pushConstantStages = layoutInfo->pushRanges[0].stages;
    result.layoutBundle = std::move(*layoutBundle);

    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = blob.code.size() * sizeof(uint32_t);
        moduleInfo.pCode = blob.code.data();

        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            destroyDrawPipeline(device, result);
            return std::nullopt;
        }
        if (blob.entryPointName == "vsMain") {
            result.vertModule = module;
        } else if (blob.entryPointName == "fsMain") {
            result.fragModule = module;
        } else {
            vkDestroyShaderModule(device, module, nullptr);
            destroyDrawPipeline(device, result);
            return std::nullopt;
        }
    }
    if (result.vertModule == VK_NULL_HANDLE || result.fragModule == VK_NULL_HANDLE) {
        destroyDrawPipeline(device, result);
        return std::nullopt;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = result.vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = result.fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(rx::asset::PoolVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attribute{};
    attribute.location = 0;
    attribute.binding = 0;
    attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
    attribute.offset = offsetof(rx::asset::PoolVertex, px);

    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount = 1;
    vertexInputState.pVertexBindingDescriptions = &binding;
    vertexInputState.vertexAttributeDescriptionCount = 1;
    vertexInputState.pVertexAttributeDescriptions = &attribute;

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

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                      VK_COLOR_COMPONENT_A_BIT;

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
    pipelineInfo.pColorBlendState = &colorBlendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = result.layoutBundle.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;  // dynamic rendering
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &result.pipeline) !=
        VK_SUCCESS) {
        destroyDrawPipeline(device, result);
        return std::nullopt;
    }

    return result;
}

}  // namespace

TEST_CASE("GeometryPool exhaustion creates a second block, and BOTH blocks are drawable with real indexed draws "
          "(readback probe)") {
    auto fixture = makeFixture("rx_asset_gp_exhaustion_draw");
    if (!fixture.has_value()) {
        return;
    }

    // Chunk sized to hold EXACTLY one quad (4 vertices / 6 indices) --
    // the second quad below cannot possibly fit in block 0, forcing
    // real exhaustion (not a contrived assertion).
    rx::asset::PoolConfig config;
    config.vertexChunkBytes = 4 * sizeof(rx::asset::PoolVertex);
    config.indexChunkBytes = 6 * sizeof(uint32_t);

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader, config);
    REQUIRE(pool != nullptr);

    auto [vertsLeft, indicesLeft] = makeQuad(1, -1.0F, 0.0F);
    auto [vertsRight, indicesRight] = makeQuad(2, 0.0F, 1.0F);

    rx::asset::MeshRange rangeLeft = pool->upload(vertsLeft, indicesLeft);
    rx::asset::MeshRange rangeRight = pool->upload(vertsRight, indicesRight);

    REQUIRE(rangeLeft.blockId == 0);
    REQUIRE(rangeRight.blockId == 1);
    REQUIRE(pool->blockCount() == 2);

    const VkDevice device = fixture->device.device();
    constexpr uint32_t kWidth = 8;
    constexpr uint32_t kHeight = 4;
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

    auto drawPipeline = buildDrawPipeline(device, kFormat);
    REQUIRE(drawPipeline.has_value());

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kFormat;
    imageInfo.extent = {kWidth, kHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    REQUIRE(vkCreateImage(device, &imageInfo, nullptr, &image) == VK_SUCCESS);

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, image, &memReq);
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

    VkMemoryAllocateInfo memAllocInfo{};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReq.size;
    memAllocInfo.memoryTypeIndex = memoryTypeIndex;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(device, &memAllocInfo, nullptr, &imageMemory) == VK_SUCCESS);
    REQUIRE(vkBindImageMemory(device, image, imageMemory, 0) == VK_SUCCESS);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView imageView = VK_NULL_HANDLE;
    REQUIRE(vkCreateImageView(device, &viewInfo, nullptr, &imageView) == VK_SUCCESS);

    constexpr VkDeviceSize kPixelBytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
    std::array<uint8_t, kPixelBytes> pixels{};

    {
        auto cmdCtx =
            rx::rhi::CommandContext::create(device, fixture->device.graphicsQueue(), fixture->device.graphicsQueueFamily());
        REQUIRE(cmdCtx.has_value());

        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = imageView;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue.color = VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}};

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = VkRect2D{{0, 0}, {kWidth, kHeight}};
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            vkCmdBeginRendering(cmd, &renderingInfo);

            VkViewport viewport{0.0F, 0.0F, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0F, 1.0F};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipeline->pipeline);

            // Block 0 (left quad), red.
            std::array<float, 4> red{1.0F, 0.0F, 0.0F, 1.0F};
            vkCmdPushConstants(cmd, drawPipeline->layoutBundle.layout, drawPipeline->pushConstantStages, 0,
                                sizeof(red), red.data());
            pool->bind(cmd, rangeLeft.blockId);
            vkCmdDrawIndexed(cmd, rangeLeft.indexCount, 1, rangeLeft.firstIndex, rangeLeft.vertexOffset, 0);

            // Block 1 (right quad), blue.
            std::array<float, 4> blue{0.0F, 0.0F, 1.0F, 1.0F};
            vkCmdPushConstants(cmd, drawPipeline->layoutBundle.layout, drawPipeline->pushConstantStages, 0,
                                sizeof(blue), blue.data());
            pool->bind(cmd, rangeRight.blockId);
            vkCmdDrawIndexed(cmd, rangeRight.indexCount, 1, rangeRight.firstIndex, rangeRight.vertexOffset, 0);

            vkCmdEndRendering(cmd);
            rx::rhi::transitionImage(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        });

        auto readback = fixture->allocator.createHostVisibleBuffer(kPixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        REQUIRE(readback.has_value());
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {kWidth, kHeight, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
        });
        readback->invalidate();
        std::memcpy(pixels.data(), readback->mappedData(), pixels.size());
    }

    vkDeviceWaitIdle(device);
    destroyDrawPipeline(device, *drawPipeline);
    vkDestroyImageView(device, imageView, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkFreeMemory(device, imageMemory, nullptr);

    // Sample one pixel well inside each half: (1,2) is in the left
    // (block 0, red) quad; (6,2) is in the right (block 1, blue) quad.
    auto pixelAt = [&](uint32_t x, uint32_t y) {
        size_t idx = (static_cast<size_t>(y) * kWidth + x) * 4;
        return std::array<uint8_t, 4>{pixels[idx], pixels[idx + 1], pixels[idx + 2], pixels[idx + 3]};
    };
    auto leftPixel = pixelAt(1, 2);
    auto rightPixel = pixelAt(6, 2);

    CHECK(leftPixel[0] == 255);
    CHECK(leftPixel[1] == 0);
    CHECK(leftPixel[2] == 0);
    CHECK(rightPixel[0] == 0);
    CHECK(rightPixel[1] == 0);
    CHECK(rightPixel[2] == 255);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("GeometryPool::stats reports block count and bytes used/capacity accurately across growth") {
    auto fixture = makeFixture("rx_asset_gp_stats");
    if (!fixture.has_value()) {
        return;
    }

    rx::asset::PoolConfig config;
    config.vertexChunkBytes = 4 * sizeof(rx::asset::PoolVertex);
    config.indexChunkBytes = 6 * sizeof(uint32_t);

    auto pool = rx::asset::GeometryPool::create(fixture->allocator, fixture->device, fixture->uploader, config);
    REQUIRE(pool != nullptr);

    CHECK(pool->stats().blockCount == 0);
    CHECK(pool->blockCount() == 0);

    auto [vertsA, indicesA] = makeQuad(1, -1.0F, 0.0F);
    pool->upload(vertsA, indicesA);

    rx::asset::PoolStats afterOne = pool->stats();
    CHECK(afterOne.blockCount == 1);
    CHECK(afterOne.blocks.size() == 1);
    CHECK(afterOne.vertexBytesCapacity == config.vertexChunkBytes);
    CHECK(afterOne.indexBytesCapacity == config.indexChunkBytes);
    CHECK(afterOne.vertexBytesUsed == 4 * sizeof(rx::asset::PoolVertex));
    CHECK(afterOne.indexBytesUsed == 6 * sizeof(uint32_t));

    auto [vertsB, indicesB] = makeQuad(2, 0.0F, 1.0F);
    pool->upload(vertsB, indicesB);

    rx::asset::PoolStats afterTwo = pool->stats();
    CHECK(afterTwo.blockCount == 2);
    CHECK(afterTwo.vertexBytesCapacity == 2 * config.vertexChunkBytes);
    CHECK(afterTwo.vertexBytesUsed == 8 * sizeof(rx::asset::PoolVertex));

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
