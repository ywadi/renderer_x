// src/rx_material/tests/test_cluster_shading_gpu.cpp -- Phase 5 Stage 2
// Task 15 [#51, gate matrix-p5t15-clustered-shading.md]: value-asserted
// GPU proof of the SHADING-SIDE per-pixel froxel lookup
// (shaders/material/cluster_lighting.slang's `rx_evaluateClusteredLights()`
// / `clusterFroxelIndex()`), exercised directly (not through a full
// rasterized StandardPbr triangle -- test_standard_pbr_punctual_gpu.cpp's
// own job is the BRDF/single-slot-punctual math) via a tiny dedicated
// compute probe (shaders/material/test_cluster_shading_probe.slang, TEST-
// ONLY, see that file's own header comment).
//
// SCENARIO -- mirrors T14's own "behind-camera exclusion" methodology
// (test_cluster_membership_gpu.cpp) from the SHADING side instead of the
// culling side: a camera at the origin looking down -Z; ONE probe
// fragment at world (0,0,-20) (comfortably inside a middle Z-slice,
// avoiding slice-0's own documented looseness -- task-14-report.md's
// "Honest design note"); TWO synthetic Point lights, run through the
// REAL T14 compute chain (rx::cluster::ClusterPipelines::addClusterPasses()):
//   - Light A ("in-froxel"): world (0,0,-19), 1 unit from the probe along
//     the SAME axis as its normal (NdotL==1 exactly, isolating distance
//     attenuation -- test_standard_pbr_punctual_gpu.cpp's own established
//     "matched-pose" methodology), RED-only color (10,0,0) candela.
//   - Light B ("excluded"): world (0,0,+50) -- BEHIND the camera, entirely
//     outside every froxel in the grid by construction (T14's own proven
//     exclusion case) -- GREEN-only color (0,10,0) candela, deliberately
//     bright so any leakage would be obviously visible.
//
// ACCEPTANCE [matrix-p5t15's own row]: "a fragment's accumulated lighting
// includes ONLY the lights in ITS OWN froxel's record range... a light
// entirely outside a fragment's froxel must contribute exactly zero, not
// merely 'small'". The probe's own output `.g` channel (Light B's sole
// contribution channel) must be EXACTLY 0.0 -- not merely small -- while
// `.r` (Light A's channel) matches the closed-form Lambertian value
// exactly (Fd_Lambert() == 1/pi, NdotL==1, distSq==1, rangeWindow==1).
#include <doctest/doctest.h>
#include <rx_cluster/cluster_lighting.h>
#include <rx_core/log.h>
#include <rx_material/draw_data.h>
#include <rx_material/material_system.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_scene/camera.h>
#include <rx_scene/froxel_grid.h>
#include <rx_scene/light_math.h>
#include <rx_scene/scene.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>

#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_task/scheduler.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef RX_MATERIAL_SHADER_DIR
#error "RX_MATERIAL_SHADER_DIR must be defined by this test binary's own CMakeLists.txt"
#endif
#ifndef RX_CLUSTER_SHADER_DIR
#error "RX_CLUSTER_SHADER_DIR must be defined by this test binary's own CMakeLists.txt"
#endif

namespace {

struct Fixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
    rx::rhi::BindlessTable bindless;
    rx::rhi::Allocator allocator;
};

std::optional<Fixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping rx_cluster shading GPU test");
        return std::nullopt;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions, skipping rx_cluster shading GPU test");
        return std::nullopt;
    }
    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());
    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);
    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    // [Phase 5 Task 15, #51] genericStorageBuffers/clusterLightBuffers --
    // this test registers T14's own three named uint[] outputs plus the
    // hand-built ClusterLightGpu[] array, mirroring how a real material
    // pipeline's own compiled program (standard_pbr.slang -> cluster_
    // lighting.slang) unconditionally references bindings 5/6 -- this
    // probe shader does too (`import cluster_lighting;`).
    rx::rhi::BindlessTable::Capacities capacities{/*sampledImages=*/1, /*samplers=*/1, /*storageBuffers=*/1};
    capacities.comparisonSamplers = 1;
    capacities.cubeImages = 1;
    capacities.genericStorageBuffers = 4;
    capacities.clusterLightBuffers = 1;
    auto bindless = rx::rhi::BindlessTable::create(device->physicalDevice(), device->device(), capacities);
    REQUIRE(bindless.has_value());

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    REQUIRE(allocator.has_value());

    return Fixture{std::move(*window), std::move(*context), std::move(*device), std::move(*bindless),
                    std::move(*allocator)};
}

std::filesystem::path freshCachePath(const char* name) {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / (std::string("rx_cluster_shading_test_") + name + ".cache");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

// --- The probe compute pipeline [test-only, see the .slang file's own
// header comment] -- built via plain rx::shader::Compiler + rx_rhi_vk::
// PipelineLayoutBuilder, mirroring rx_cluster::cluster_lighting.cpp's own
// buildKernel() shape exactly (the SAME "compile -> reflect -> external-
// set-0-substituted layout -> vkCreateComputePipelines" sequence). -----
struct ProbePipeline {
    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    rx::shader::ShaderLayoutInfo layoutInfo;

    ~ProbePipeline() {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, module, nullptr);
        }
    }
    ProbePipeline() = default;
    ProbePipeline(const ProbePipeline&) = delete;
    ProbePipeline& operator=(const ProbePipeline&) = delete;
    ProbePipeline(ProbePipeline&&) = delete;
    ProbePipeline& operator=(ProbePipeline&&) = delete;
};

// `entryPoint` -- [matrix-p5t15's own equivalence row] `"csProbe"` (the
// clustered path) or `"csProbeBruteForce"` (the row's own required
// PERMANENT brute-force reference permutation, test_cluster_shading_
// probe.slang's own sibling entry point) -- both compiled from the SAME
// .slang file, selected here rather than via a runtime branch inside one
// shader, per that row's own "reachable via a build/runtime flag" text.
std::unique_ptr<ProbePipeline> buildProbePipeline(VkDevice device, rx::rhi::BindlessTable& bindless,
                                                    const char* entryPoint = "csProbe") {
    // [Slang runtime-compiles this file EVERY test-binary run -- no C++
    // rebuild needed for a source edit, exactly like every other Slang-
    // backed GPU test in this codebase -- this is what makes the revert-
    // discrimination proof below (sabotage cluster_lighting.slang, re-run,
    // revert) work without a rebuild.]
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
    const std::filesystem::path path =
        std::filesystem::path(RX_MATERIAL_SHADER_DIR) / "test_cluster_shading_probe.slang";
    rx::shader::CompileResult compileResult = compiler->compileFromFile(path.string(), {entryPoint});
    if (!compileResult.ok) {
        RX_LOG_ERROR("rx_cluster shading test: probe shader compile failed: {}", compileResult.diagnostics);
    }
    REQUIRE(compileResult.ok);
    REQUIRE(compileResult.entryPointCode.size() == 1);

    auto layoutInfo = rx::shader::reflect(compileResult);
    REQUIRE(layoutInfo.has_value());

    auto probe = std::make_unique<ProbePipeline>();
    probe->device = device;
    probe->layoutInfo = *layoutInfo;

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = compileResult.entryPointCode[0].code.size() * sizeof(uint32_t);
    moduleInfo.pCode = compileResult.entryPointCode[0].code.data();
    REQUIRE(vkCreateShaderModule(device, &moduleInfo, nullptr, &probe->module) == VK_SUCCESS);

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, *layoutInfo, bindless.descriptorSetLayout());
    REQUIRE(layoutBundle.has_value());
    probe->layoutBundle = std::move(*layoutBundle);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = probe->module;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = probe->layoutBundle.layout;
    REQUIRE(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &probe->pipeline) ==
            VK_SUCCESS);
    return probe;
}

struct ProbeParamsGpu {
    uint32_t clusterCountX;
    uint32_t clusterCountY;
    uint32_t clusterCountZ;
    float clusterTanHalfFovY;
    float clusterAspectRatio;
    float clusterZLightFar;
    float clusterInvLinearizer;
    uint32_t clusterOffsetsBufferIndex;
    uint32_t clusterWriteCountsBufferIndex;
    uint32_t clusterLightIndicesBufferIndex;
    uint32_t clusterLightsBufferIndex;
    // [matrix-p5t15's own equivalence row] Only read by `csProbeBruteForce`
    // -- see test_cluster_shading_probe.slang's own ProbeParams::
    // totalLightCount comment.
    uint32_t totalLightCount;
};

struct ProbeResult {
    std::array<glm::vec3, 1> diffuse{};
    std::array<glm::vec3, 1> specular{};
};

// Runs the FULL T14 compute chain (rx::cluster::ClusterPipelines) plus
// this file's own probe pass, in ONE graph / ONE runOnce() -- mirrors
// rx_cluster's own cluster_gpu_fixture.h::runCluster() shape, extended
// with the probe. `lights` is the hand-built (not Scene-derived, for
// exact test control) ClusterLightGpu row array.
ProbeResult runProbe(Fixture& fixture, rx::cluster::ClusterPipelines& pipelines, ProbePipeline& probe,
                     const std::vector<rx::cluster::ClusterLightGpu>& lights, const rx::scene::Camera& camera,
                     uint32_t viewportWidth, uint32_t viewportHeight, const rx::cluster::ClusterParams& params,
                     glm::vec3 probeWorldPos) {
    using rx::graph::PassContext;
    using rx::graph::QueueClass;
    using rx::graph::RenderGraph;

    VkDevice device = fixture.device.device();

    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto executor = rx::graph::Executor::create(fixture.device, *scheduler);
    REQUIRE(executor != nullptr);

    auto lightsBuffer = fixture.allocator.createHostVisibleBuffer(
        std::max<VkDeviceSize>(sizeof(rx::cluster::ClusterLightGpu),
                                static_cast<VkDeviceSize>(lights.size()) * sizeof(rx::cluster::ClusterLightGpu)),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(lightsBuffer.has_value());
    if (!lights.empty()) {
        std::memcpy(lightsBuffer->mappedData(), lights.data(), lights.size() * sizeof(rx::cluster::ClusterLightGpu));
        lightsBuffer->flush();
    }
    rx::rhi::BindlessHandle lightsHandle =
        fixture.bindless.registerClusterLightBuffer(lightsBuffer->handle(), VK_WHOLE_SIZE);
    REQUIRE(lightsHandle.isValid());

    const rx::cluster::ClusterFrameInputs frameInputs{lightsBuffer->handle(), static_cast<uint32_t>(lights.size()),
                                                        /*frameSlot=*/0};

    RenderGraph graph;
    const rx::scene::froxel::FroxelGridParams grid =
        pipelines.addClusterPasses(graph, frameInputs, camera, viewportWidth, viewportHeight, params);

    auto testPositions = fixture.allocator.createHostVisibleBuffer(sizeof(glm::vec3), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(testPositions.has_value());
    std::memcpy(testPositions->mappedData(), &probeWorldPos, sizeof(glm::vec3));
    testPositions->flush();

    const glm::mat4 viewTransposed = glm::transpose(camera.view());
    auto viewBuffer = fixture.allocator.createHostVisibleBuffer(sizeof(glm::mat4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(viewBuffer.has_value());
    std::memcpy(viewBuffer->mappedData(), &viewTransposed, sizeof(glm::mat4));
    viewBuffer->flush();

    auto outDiffuse = fixture.allocator.createHostVisibleBuffer(
        sizeof(glm::vec3), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(outDiffuse.has_value());
    auto outSpecular = fixture.allocator.createHostVisibleBuffer(
        sizeof(glm::vec3), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(outSpecular.has_value());
    // [Slang forbids a second `[[vk::push_constant]]` block alongside
    // material.slang's own `gMaterialGlobals` -- see the .slang file's own
    // header comment -- so ProbeParams travels as an ordinary bound
    // uniform buffer (set 1, binding 0) instead.]
    auto probeParamsBuffer = fixture.allocator.createHostVisibleBuffer(sizeof(ProbeParamsGpu), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    REQUIRE(probeParamsBuffer.has_value());

    VkDescriptorPoolSize poolSizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VkDescriptorPool probeSetPool = VK_NULL_HANDLE;
    REQUIRE(vkCreateDescriptorPool(device, &poolInfo, nullptr, &probeSetPool) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = probeSetPool;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &probe.layoutBundle.setLayouts[1];
    VkDescriptorSet probeSet = VK_NULL_HANDLE;
    REQUIRE(vkAllocateDescriptorSets(device, &setAllocInfo, &probeSet) == VK_SUCCESS);

    // [rx_graph's own RenderGraph::compile() requires a backbuffer source
    // even for a compute-only graph -- mirrors tools/rx_cluster_bench's
    // own "present" dummy color pass precedent.]
    rx::graph::AttachmentDesc bbDesc;
    bbDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
    graph.addPass("present").addColorOutput("bb", bbDesc);
    graph.setBackbufferSource("bb");

    // [Fix round] Declared OUTSIDE the lambda (not local to setExecute())
    // so this function's own caller can release() them after runOnce()
    // completes -- this function is called MULTIPLE times per TEST_CASE
    // (the positive scenario, the sabotaged re-run, the restored re-run),
    // and this fixture's own BindlessTable genericStorageBuffers capacity
    // is finite; never releasing these three registrations between calls
    // exhausts it by the second call, reproduced directly while writing
    // this test.
    rx::rhi::BindlessHandle offsetsHandle;
    rx::rhi::BindlessHandle writeCountsHandle;
    rx::rhi::BindlessHandle lightIndicesHandle;
    graph.addPass("cluster_shading_probe", QueueClass::AsyncCompute)
        .addStorageBufferInput("clusterOffsets")
        .addStorageBufferInput("clusterWriteCounts")
        .addStorageBufferInput("clusterLightIndices")
        .setSideEffect()
        .setExecute([&](PassContext& ctx) {
            offsetsHandle = fixture.bindless.registerGenericStorageBuffer(ctx.buffer("clusterOffsets"), VK_WHOLE_SIZE);
            writeCountsHandle =
                fixture.bindless.registerGenericStorageBuffer(ctx.buffer("clusterWriteCounts"), VK_WHOLE_SIZE);
            lightIndicesHandle =
                fixture.bindless.registerGenericStorageBuffer(ctx.buffer("clusterLightIndices"), VK_WHOLE_SIZE);
            REQUIRE(offsetsHandle.isValid());
            REQUIRE(writeCountsHandle.isValid());
            REQUIRE(lightIndicesHandle.isValid());

            ProbeParamsGpu probeParams{grid.countX,
                                        grid.countY,
                                        grid.countZ,
                                        grid.tanHalfFovY,
                                        grid.aspectRatio,
                                        grid.zLightFar,
                                        grid.invLinearizer,
                                        offsetsHandle.index(),
                                        writeCountsHandle.index(),
                                        lightIndicesHandle.index(),
                                        lightsHandle.index(),
                                        static_cast<uint32_t>(lights.size())};
            std::memcpy(probeParamsBuffer->mappedData(), &probeParams, sizeof(probeParams));
            probeParamsBuffer->flush();

            std::array<VkDescriptorBufferInfo, 5> bufferInfos{
                VkDescriptorBufferInfo{probeParamsBuffer->handle(), 0, VK_WHOLE_SIZE},
                VkDescriptorBufferInfo{testPositions->handle(), 0, VK_WHOLE_SIZE},
                VkDescriptorBufferInfo{viewBuffer->handle(), 0, VK_WHOLE_SIZE},
                VkDescriptorBufferInfo{outDiffuse->handle(), 0, VK_WHOLE_SIZE},
                VkDescriptorBufferInfo{outSpecular->handle(), 0, VK_WHOLE_SIZE},
            };
            std::array<VkWriteDescriptorSet, 5> writes{};
            for (uint32_t i = 0; i < writes.size(); ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = probeSet;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &bufferInfos[i];
            }
            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

            VkDescriptorSet bindlessSet = fixture.bindless.descriptorSet();
            std::array<VkDescriptorSet, 2> sets{bindlessSet, probeSet};
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probe.pipeline);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probe.layoutBundle.layout, 0,
                                      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
            vkCmdDispatch(ctx.cmd, 1, 1, 1);
        });

    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = viewportWidth;
    compileInfo.swapchainHeight = viewportHeight;
    compileInfo.swapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);
    executor->realize(graph);

    // A real (if unused) backbuffer image -- "present"'s own addColorOutput
    // needs a real target to write into.
    VkImageCreateInfo bbImageInfo{};
    bbImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    bbImageInfo.imageType = VK_IMAGE_TYPE_2D;
    bbImageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    bbImageInfo.extent = {viewportWidth, viewportHeight, 1};
    bbImageInfo.mipLevels = 1;
    bbImageInfo.arrayLayers = 1;
    bbImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    bbImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    bbImageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    bbImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage bbImage = VK_NULL_HANDLE;
    REQUIRE(vkCreateImage(device, &bbImageInfo, nullptr, &bbImage) == VK_SUCCESS);
    VkMemoryRequirements bbMemReq{};
    vkGetImageMemoryRequirements(device, bbImage, &bbMemReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(fixture.device.physicalDevice(), &memProps);
    uint32_t bbMemType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((bbMemReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            bbMemType = i;
            break;
        }
    }
    REQUIRE(bbMemType != UINT32_MAX);
    VkMemoryAllocateInfo bbAllocInfo{};
    bbAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bbAllocInfo.allocationSize = bbMemReq.size;
    bbAllocInfo.memoryTypeIndex = bbMemType;
    VkDeviceMemory bbMemory = VK_NULL_HANDLE;
    REQUIRE(vkAllocateMemory(device, &bbAllocInfo, nullptr, &bbMemory) == VK_SUCCESS);
    REQUIRE(vkBindImageMemory(device, bbImage, bbMemory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo bbViewInfo{};
    bbViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    bbViewInfo.image = bbImage;
    bbViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    bbViewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    bbViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView bbView = VK_NULL_HANDLE;
    REQUIRE(vkCreateImageView(device, &bbViewInfo, nullptr, &bbView) == VK_SUCCESS);

    auto cmdCtx = rx::rhi::CommandContext::create(device, fixture.device.graphicsQueue(), fixture.device.graphicsQueueFamily());
    REQUIRE(cmdCtx.has_value());
    cmdCtx->runOnce(
        [&](VkCommandBuffer cmd) { executor->execute(graph, cmd, bbImage, bbView, {viewportWidth, viewportHeight}); });

    vkDestroyImageView(device, bbView, nullptr);
    vkDestroyImage(device, bbImage, nullptr);
    vkFreeMemory(device, bbMemory, nullptr);

    outDiffuse->invalidate();
    outSpecular->invalidate();
    ProbeResult result;
    std::memcpy(result.diffuse.data(), outDiffuse->mappedData(), sizeof(glm::vec3));
    std::memcpy(result.specular.data(), outSpecular->mappedData(), sizeof(glm::vec3));

    vkDestroyDescriptorPool(device, probeSetPool, nullptr);
    fixture.bindless.release(lightsHandle);
    fixture.bindless.release(offsetsHandle);
    fixture.bindless.release(writeCountsHandle);
    fixture.bindless.release(lightIndicesHandle);
    return result;
}

bool near(float actual, float expected, float absTolerance) { return std::fabs(actual - expected) <= absTolerance; }

// [Fix round 1, task-15-review.md Finding 1/Medium] RAII guard for the
// sabotage/restore sequence below: captures the ORIGINAL file bytes up
// front (constructor argument, not re-read later) and unconditionally
// rewrites them to `path` in the destructor UNLESS `dismiss()` was
// already called. This makes the restore crash-safe against ANY exit
// path from the risk window between the sabotage-write and the
// explicit restore-write -- a `REQUIRE()` failure inside `runProbe()`
// (over a dozen of them: scheduler/executor creation, buffer
// allocation, bindless registration, descriptor pool/set allocation),
// an unrelated exception, or an early return all unwind the C++ stack
// through this guard's destructor exactly like any other RAII type.
//
// This closes a REAL, previously-triggered incident class, not a
// hypothetical one: the implementer's own task-15-report.md ("Bugs
// found and fixed") discloses that a bindless-capacity-exhaustion
// `REQUIRE()` failure mid-`TEST_CASE` once left the PRODUCTION
// `cluster_lighting.slang` sabotaged on disk until caught and fixed by
// hand -- that specific trigger was fixed (releasing bindless handles
// at the end of `runProbe()`), but the general structural risk (ANY
// other `REQUIRE()` failure in the same window) was not, per the
// review's own Finding 1. This guard closes the general class instead
// of the one specific trigger.
//
// The destructor is intentionally best-effort (no exception thrown from
// it, even on a write failure) -- it may run DURING stack unwinding from
// a fatal doctest assertion; throwing again from a destructor already
// unwinding from an exception calls `std::terminate()`, which would
// destroy the test binary's own ability to report the original failure
// AND skip every later TEST_CASE in the same binary. "Restore what we
// can, never make the crash worse" is the correct destructor contract
// here, matching this project's own "no deferred fixes" standing rule
// applied to a destructor's own failure mode, not just the happy path.
class SourceFileRestoreGuard {
public:
    SourceFileRestoreGuard(std::filesystem::path path, std::string originalContent)
        : path_(std::move(path)), original_(std::move(originalContent)) {}

    ~SourceFileRestoreGuard() {
        if (dismissed_) {
            return;
        }
        std::ofstream outFile(path_, std::ios::binary | std::ios::trunc);
        if (outFile.is_open()) {
            outFile << original_;
        }
        // No REQUIRE/CHECK/throw here -- see this class's own header
        // comment on why a destructor must never throw while already
        // unwinding.
    }

    // Called only after the sequence's own explicit restore-write AND
    // its own REQUIRE()-verified byte-identical readback both succeed
    // (see the TEST_CASE below) -- once dismissed, the destructor is a
    // no-op, since the file is already provably correct on disk at that
    // point and a second unconditional rewrite would be redundant, not
    // unsafe, but pointless.
    void dismiss() { dismissed_ = true; }

    SourceFileRestoreGuard(const SourceFileRestoreGuard&) = delete;
    SourceFileRestoreGuard& operator=(const SourceFileRestoreGuard&) = delete;
    SourceFileRestoreGuard(SourceFileRestoreGuard&&) = delete;
    SourceFileRestoreGuard& operator=(SourceFileRestoreGuard&&) = delete;

private:
    std::filesystem::path path_;
    std::string original_;
    bool dismissed_ = false;
};

}  // namespace

TEST_CASE("Clustered shading: exact per-froxel membership from the SHADING side -- an in-froxel light "
          "contributes its own closed-form value, a light entirely outside the probe's froxel (behind the "
          "camera, T14's own proven exclusion case) contributes EXACTLY zero") {
    auto fixtureOpt = makeFixture("rx_cluster_shading_membership");
    if (!fixtureOpt.has_value()) {
        return;
    }
    Fixture fixture = std::move(*fixtureOpt);

    auto computeCache = rx::rhi::ComputePipelineCache::create(fixture.device.device(), freshCachePath("chain"));
    REQUIRE(computeCache.has_value());
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
    auto pipelinesOpt = rx::cluster::ClusterPipelines::create(fixture.device.device(), *computeCache, *compiler,
                                                                RX_CLUSTER_SHADER_DIR, /*framesInFlight=*/1);
    REQUIRE(pipelinesOpt.has_value());
    rx::cluster::ClusterPipelines pipelines = std::move(*pipelinesOpt);

    auto probe = buildProbePipeline(fixture.device.device(), fixture.bindless);

    rx::scene::Camera camera;  // default: position (0,0,0), looking down -Z, 60deg vfov.
    camera.aspectRatio = 1.0F;
    constexpr uint32_t kViewport = 256;
    rx::cluster::ClusterParams params;  // defaults: kDefaultZLightNear=5, kDefaultZLightFar=100.

    // Probe fragment: 20 units down -Z -- comfortably inside a middle
    // Z-slice (see this file's own header comment for why NOT near
    // zLightNear==5, slice 0's own documented looseness).
    const glm::vec3 probeWorldPos(0.0F, 0.0F, -20.0F);

    // Light A ("in-froxel"): 1 unit closer to the camera than the probe,
    // along the SAME axis the probe's own fixed normal (0,0,1) points --
    // NdotL==1 exactly (test_standard_pbr_punctual_gpu.cpp's own
    // established matched-pose methodology).
    // [Fix round] Cull radius comes from the SAME production derivation
    // buildClusterLightList() uses (rx::scene::froxel::pointLightCullRadius(),
    // an illuminance-cutoff-based radius) -- NOT an arbitrary large
    // constant. A hardcoded 1000-unit radius (this test's own original,
    // wrong draft) makes EVERY light's culling "sphere" reach every froxel
    // in the grid regardless of real position, silently defeating the
    // whole "behind-camera exclusion" scenario this TEST_CASE exists to
    // prove -- reproduced directly while writing this test (Light B leaked
    // through at exactly its own closed-form inverse-square value, proving
    // it was genuinely, wrongly INCLUDED, not a rounding artifact).
    const float kCullCutoffLux = 1.0F;
    rx::cluster::ClusterLightGpu lightA{};
    const glm::vec3 lightAWorldPos(0.0F, 0.0F, -19.0F);
    const float lightACullRadius = rx::scene::froxel::pointLightCullRadius(glm::vec3(10.0F, 0.0F, 0.0F), 0.0F, kCullCutoffLux);
    lightA.viewPositionRadius =
        glm::vec4(glm::vec3(camera.view() * glm::vec4(lightAWorldPos, 1.0F)), lightACullRadius);
    lightA.viewAxisSinInverse = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);
    lightA.cosSquaredFlags = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);  // isSpot=0 -> Point.
    lightA.shadingTypeRangeAngle = glm::vec4(1.0F, 0.0F, 0.0F, 0.0F);  // Point, range=0 (infinite).
    lightA.shadingPositionWorld = glm::vec4(lightAWorldPos, 0.0F);
    lightA.shadingColorCandela = glm::vec4(10.0F, 0.0F, 0.0F, 0.0F);  // RED-only.
    lightA.shadingSpotDirWorld = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);

    // Light B ("excluded"): BEHIND the camera (positive view-space Z) --
    // T14's own proven exclusion case (test_cluster_membership_gpu.cpp's
    // "Behind-camera" scenario), reused here from the shading side.
    rx::cluster::ClusterLightGpu lightB{};
    const glm::vec3 lightBWorldPos(0.0F, 0.0F, 50.0F);
    const float lightBCullRadius = rx::scene::froxel::pointLightCullRadius(glm::vec3(0.0F, 10.0F, 0.0F), 0.0F, kCullCutoffLux);
    lightB.viewPositionRadius =
        glm::vec4(glm::vec3(camera.view() * glm::vec4(lightBWorldPos, 1.0F)), lightBCullRadius);
    lightB.viewAxisSinInverse = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);
    lightB.cosSquaredFlags = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);
    lightB.shadingTypeRangeAngle = glm::vec4(1.0F, 0.0F, 0.0F, 0.0F);
    lightB.shadingPositionWorld = glm::vec4(lightBWorldPos, 0.0F);
    lightB.shadingColorCandela = glm::vec4(0.0F, 10.0F, 0.0F, 0.0F);  // GREEN-only, deliberately bright.
    lightB.shadingSpotDirWorld = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);

    const std::vector<rx::cluster::ClusterLightGpu> lights{lightA, lightB};
    ProbeResult result = runProbe(fixture, pipelines, *probe, lights, camera, kViewport, kViewport, params, probeWorldPos);

    // Closed form: Fd_Lambert()=1/pi, NdotL=1, distSq=1 (Light A is
    // exactly 1 unit away), rangeWindow=1 (range=0 -> no window).
    // outDiffuse.r = diffuseColor.r(1) * (1/pi) * colorCandela.r(10) * attenuation(1) * NdotL(1) = 10/pi.
    const float expectedDiffuseR = 10.0F / static_cast<float>(M_PI);
    CAPTURE(result.diffuse[0].r);
    CAPTURE(result.diffuse[0].g);
    CAPTURE(result.diffuse[0].b);
    CHECK(near(result.diffuse[0].r, expectedDiffuseR, 0.02F));
    // EXACT zero, not merely small -- matrix-p5t15's own acceptance text.
    CHECK(result.diffuse[0].g == 0.0F);
    CHECK(result.specular[0].g == 0.0F);

    // --- Revert-discrimination proof [empirical, this task's own standing
    // rule] -- sabotage cluster_lighting.slang's own NdotL early-out
    // (invert the comparison, which makes EVERY light -- Light A included
    // -- get skipped), re-run (no C++ rebuild needed -- Slang compiles at
    // test-binary runtime), confirm the diffuse.r contribution collapses
    // to EXACTLY 0, then restore and re-confirm green. Proves the
    // membership/accumulation mechanism this TEST_CASE's own positive
    // assertion relies on is load-bearing, not vacuously true. ---------
    const std::filesystem::path clusterLightingPath =
        std::filesystem::path(RX_MATERIAL_SHADER_DIR) / "cluster_lighting.slang";
    std::ifstream inFile(clusterLightingPath, std::ios::binary);
    REQUIRE(inFile.is_open());
    std::ostringstream originalStream;
    originalStream << inFile.rdbuf();
    inFile.close();
    const std::string original = originalStream.str();

    const std::string needle = "if (NdotL <= 0.0) {";
    const std::string sabotaged_needle = "if (NdotL > 0.0) {";
    const size_t pos = original.find(needle);
    REQUIRE(pos != std::string::npos);
    std::string sabotaged = original;
    sabotaged.replace(pos, needle.size(), sabotaged_needle);

    // [Fix round 1, review Finding 1/Medium] Constructed BEFORE the
    // sabotage-write below, capturing `original` (already read above) up
    // front -- from this point until `dismiss()` is called (after the
    // restore-write's own byte-identical verification succeeds, below),
    // ANY exit from this scope (a REQUIRE() failure inside runProbe(),
    // an exception, an early return) restores the production shader
    // source in this guard's own destructor. See its class comment for
    // the full incident this closes.
    SourceFileRestoreGuard restoreGuard(clusterLightingPath, original);
    {
        std::ofstream outFile(clusterLightingPath, std::ios::binary | std::ios::trunc);
        REQUIRE(outFile.is_open());
        outFile << sabotaged;
    }
    // [Fix round] `probe` (the compiled VkPipeline) was already built
    // (buildProbePipeline(), above) from the PRE-sabotage source -- Slang
    // compiles once, at pipeline-BUILD time, not per-dispatch, so simply
    // re-running the SAME already-linked VkPipeline against the new file
    // contents is a no-op (reproduced directly while writing this test:
    // the "sabotaged" run below returned the UNCHANGED, correct value
    // until this rebuild was added). Rebuilding here is what actually
    // picks up the on-disk edit, exactly mirroring every other sabotage-
    // discrimination proof in this codebase's own established convention
    // ("no C++ rebuild needed" refers to this TEST BINARY not needing a
    // recompile -- the SHADER pipeline itself still must be rebuilt from
    // the edited source, same as reloadChanged()'s own hot-reload path
    // would do in production).
    probe = buildProbePipeline(fixture.device.device(), fixture.bindless);

    ProbeResult sabotagedResult =
        runProbe(fixture, pipelines, *probe, lights, camera, kViewport, kViewport, params, probeWorldPos);
    CAPTURE(sabotagedResult.diffuse[0].r);
    CHECK(sabotagedResult.diffuse[0].r == 0.0F);

    {
        std::ofstream outFile(clusterLightingPath, std::ios::binary | std::ios::trunc);
        REQUIRE(outFile.is_open());
        outFile << original;
    }
    std::ifstream verifyFile(clusterLightingPath, std::ios::binary);
    std::ostringstream verifyStream;
    verifyStream << verifyFile.rdbuf();
    REQUIRE(verifyStream.str() == original);
    // [Fix round 1, review Finding 1/Medium] The file is now provably
    // correct on disk (the REQUIRE() immediately above just verified a
    // byte-identical readback) -- dismiss the guard so its destructor
    // becomes a no-op. Anything that fails AFTER this point (e.g. the
    // restoredResult probe run below) no longer needs the guard's own
    // safety net, since there is nothing left to restore.
    restoreGuard.dismiss();
    probe = buildProbePipeline(fixture.device.device(), fixture.bindless);

    ProbeResult restoredResult =
        runProbe(fixture, pipelines, *probe, lights, camera, kViewport, kViewport, params, probeWorldPos);
    CHECK(near(restoredResult.diffuse[0].r, expectedDiffuseR, 0.02F));
    CHECK(restoredResult.diffuse[0].g == 0.0F);
}

// [matrix-p5t15's own "Clustered-vs-unclustered equivalence" row, plan:
// 519-520] Runs the SAME 6-light scene / 5 probe positions through BOTH
// `rx_evaluateClusteredLights()` (froxel-indexed, real T14 compute chain)
// and `rx_evaluateBruteForceClusteredLights()` (the row's own required
// PERMANENT reference permutation, cluster_lighting.slang -- loops every
// light directly, no froxel indirection) and asserts the two paths agree
// within a float-accumulation-order-only tolerance -- matrix row 42's own
// acceptance text: "Tolerance derives from float-accumulation-order
// differences ONLY... any discrepancy exceeding that floor is a real
// assignment bug."
//
// All 6 lights sit at generous, EXPLICIT cull radii (300 -- comfortably
// larger than this grid's own zLightFar=100) so T14's froxelizer assigns
// every light to every froxel its real illuminance reaches, deliberately
// avoiding the OTHER test's own concern (cull-radius-driven exclusion,
// test_cluster_membership_gpu.cpp/the TEST_CASE above already cover
// that) -- this test isolates "given the SAME light set, does the
// froxel-indexed subset sum to the SAME value as summing every light
// directly", which is exactly what clustering's own correctness
// obligation is (a pure performance optimization over brute force).
//
// The probe's fixed world-space normal (0,0,1) [see this file's own
// probe-shader-mirroring kN comment] makes NdotL's SIGN depend only on
// each light's Z relative to the probe (world -Z is "into the scene",
// so a light with a LESS-NEGATIVE z than the probe sits nearer the
// camera along the probe's own normal and contributes positively) --
// probes are placed at increasing depth specifically so each one sees a
// DIFFERENT-SIZED, genuinely varied subset of the 6 lights (0, 1, 3, 5,
// then all 6), not a fixed count, exercising the membership-selection
// mechanism across a real range of per-froxel occupancy.
TEST_CASE("Clustered shading: clustered path matches the PERMANENT brute-force reference path "
          "(rx_evaluateBruteForceClusteredLights) within float-accumulation tolerance, across probes seeing "
          "0/1/3/5/6 of the same 6 lights") {
    auto fixtureOpt = makeFixture("rx_cluster_shading_equivalence");
    if (!fixtureOpt.has_value()) {
        return;
    }
    Fixture fixture = std::move(*fixtureOpt);

    auto computeCache = rx::rhi::ComputePipelineCache::create(fixture.device.device(), freshCachePath("equiv_chain"));
    REQUIRE(computeCache.has_value());
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
    auto pipelinesOpt = rx::cluster::ClusterPipelines::create(fixture.device.device(), *computeCache, *compiler,
                                                                RX_CLUSTER_SHADER_DIR, /*framesInFlight=*/1);
    REQUIRE(pipelinesOpt.has_value());
    rx::cluster::ClusterPipelines pipelines = std::move(*pipelinesOpt);

    auto clusteredProbe = buildProbePipeline(fixture.device.device(), fixture.bindless, "csProbe");
    auto bruteForceProbe = buildProbePipeline(fixture.device.device(), fixture.bindless, "csProbeBruteForce");

    rx::scene::Camera camera;  // default: position (0,0,0), looking down -Z, 60deg vfov.
    camera.aspectRatio = 1.0F;
    constexpr uint32_t kViewport = 256;
    rx::cluster::ClusterParams params;  // defaults: kDefaultZLightNear=5, kDefaultZLightFar=100.
    constexpr float kGenerousCullRadius = 300.0F;  // see this TEST_CASE's own header comment.

    auto makePointLight = [&](glm::vec3 worldPos, glm::vec3 colorCandela) {
        rx::cluster::ClusterLightGpu light{};
        light.viewPositionRadius = glm::vec4(glm::vec3(camera.view() * glm::vec4(worldPos, 1.0F)), kGenerousCullRadius);
        light.viewAxisSinInverse = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);
        light.cosSquaredFlags = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);
        light.shadingTypeRangeAngle = glm::vec4(1.0F, 0.0F, 0.0F, 0.0F);  // Point, range=0 (unwindowed).
        light.shadingPositionWorld = glm::vec4(worldPos, 0.0F);
        light.shadingColorCandela = glm::vec4(colorCandela, 0.0F);
        light.shadingSpotDirWorld = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);
        return light;
    };

    // L0..L5: increasing depth, generous cull radius, one finite-range
    // Point (L3) and one Spot (L2) so both attenuation branches (range
    // window, cone) are exercised identically by both paths.
    std::vector<rx::cluster::ClusterLightGpu> lights;
    lights.push_back(makePointLight(glm::vec3(2.0F, 0.0F, -8.0F), glm::vec3(15.0F, 14.0F, 12.0F)));   // L0
    lights.push_back(makePointLight(glm::vec3(-3.0F, 0.0F, -14.0F), glm::vec3(12.0F, 10.0F, 18.0F)));  // L1
    {
        // L2: Spot for SHADING purposes (`shadingTypeRangeAngle.x=2.0`
        // below, the sole Point-vs-Spot discriminator
        // clusterAccumulateSingleLight() reads). `cosSquaredFlags`/
        // `viewAxisSinInverse` (the head, CULLING-only float4s
        // froxel_common.slang's own `spotIntersectsFroxel()` consults via
        // `cosSquaredFlags.y > 0.5`) are DELIBERATELY left at
        // `makePointLight()`'s own all-point defaults, so T14's real
        // compute-side culling pass treats this light as a plain
        // sphere-vs-froxel test (same generous 300-unit radius as every
        // other light here) rather than a narrower cone-vs-froxel test --
        // guaranteeing CULLING-side inclusion at every probe below
        // regardless of the cone's own real geometry, so this test isolates
        // the SHADING-side cone-attenuation arithmetic (does
        // clustered-vs-brute-force agree on the ATTENUATED value) from the
        // CULLING-side cone-vs-froxel intersection test (a different,
        // already-covered T14 concern, matrix-p5t14's own scope). A real
        // spot light in production DOES set these fields for tighter
        // culling (cluster_lighting.cpp:153-154) -- this test's own
        // shading-focused scope is why it deliberately does not replicate
        // that here.
        rx::cluster::ClusterLightGpu spot = makePointLight(glm::vec3(4.0F, 0.0F, -20.0F), glm::vec3(25.0F, 20.0F, 15.0F));
        const rx::scene::lightmath::SpotAngleScaleOffset angles =
            rx::scene::lightmath::spotAngleScaleOffset(glm::radians(20.0F), glm::radians(60.0F));
        spot.shadingTypeRangeAngle = glm::vec4(2.0F, 0.0F, angles.scale, angles.offset);  // Spot (2.0), range=0.
        lights.push_back(spot);  // L2
    }
    {
        rx::cluster::ClusterLightGpu ranged = makePointLight(glm::vec3(-6.0F, 0.0F, -28.0F), glm::vec3(10.0F, 16.0F, 10.0F));
        ranged.shadingTypeRangeAngle = glm::vec4(1.0F, 40.0F, 0.0F, 0.0F);  // Point, finite range=40 -- exercises the window branch.
        lights.push_back(ranged);  // L3
    }
    lights.push_back(makePointLight(glm::vec3(5.0F, 0.0F, -38.0F), glm::vec3(18.0F, 8.0F, 14.0F)));   // L4
    lights.push_back(makePointLight(glm::vec3(-2.0F, 0.0F, -48.0F), glm::vec3(14.0F, 14.0F, 20.0F)));  // L5

    // Probe depths chosen (see this TEST_CASE's own header comment) so
    // each sees a different-sized subset: P0 sees none (shallower than
    // every light); P1 sees {L0}; P2 sees {L0,L1,L2}; P3 sees
    // {L0,L1,L2,L3,L4}; P4 sees all six.
    const std::array<glm::vec3, 5> probes{
        glm::vec3(0.0F, 0.0F, -6.0F),
        glm::vec3(0.0F, 0.0F, -12.0F),
        glm::vec3(1.0F, 0.0F, -25.0F),
        glm::vec3(0.0F, 0.0F, -42.0F),
        glm::vec3(-3.0F, 0.0F, -70.0F),
    };
    const std::array<uint32_t, 5> expectedMemberCount{0, 1, 3, 5, 6};

    constexpr float kTolerance = 0.01F;  // float-accumulation-order-only floor -- see this TEST_CASE's own header comment.
    for (size_t i = 0; i < probes.size(); ++i) {
        ProbeResult clustered =
            runProbe(fixture, pipelines, *clusteredProbe, lights, camera, kViewport, kViewport, params, probes[i]);
        ProbeResult bruteForce =
            runProbe(fixture, pipelines, *bruteForceProbe, lights, camera, kViewport, kViewport, params, probes[i]);

        CAPTURE(i);
        CAPTURE(expectedMemberCount[i]);
        CAPTURE(clustered.diffuse[0].r);
        CAPTURE(clustered.diffuse[0].g);
        CAPTURE(clustered.diffuse[0].b);
        CAPTURE(bruteForce.diffuse[0].r);
        CAPTURE(bruteForce.diffuse[0].g);
        CAPTURE(bruteForce.diffuse[0].b);
        CHECK(near(clustered.diffuse[0].r, bruteForce.diffuse[0].r, kTolerance));
        CHECK(near(clustered.diffuse[0].g, bruteForce.diffuse[0].g, kTolerance));
        CHECK(near(clustered.diffuse[0].b, bruteForce.diffuse[0].b, kTolerance));
        CHECK(near(clustered.specular[0].r, bruteForce.specular[0].r, kTolerance));
        CHECK(near(clustered.specular[0].g, bruteForce.specular[0].g, kTolerance));
        CHECK(near(clustered.specular[0].b, bruteForce.specular[0].b, kTolerance));

        if (expectedMemberCount[i] == 0) {
            // Not vacuously true -- P0 sees no light in EITHER path
            // (every light is deeper than the probe, NdotL<=0 for all six),
            // so both sums must be EXACTLY zero, not merely equal to each
            // other (two paths could theoretically agree on a wrong
            // nonzero value; this pins the zero case to its own known
            // ground truth).
            CHECK(clustered.diffuse[0].r == 0.0F);
            CHECK(bruteForce.diffuse[0].r == 0.0F);
        } else {
            // Rules out a vacuous pass (both paths returning zero by
            // coincidence, e.g. from a broken bindless index) for every
            // probe that DOES have real contributors.
            CHECK(bruteForce.diffuse[0].r + bruteForce.diffuse[0].g + bruteForce.diffuse[0].b > 0.05F);
        }
    }
}

// [Fix round 1, task-15-review.md Finding 2/Low] A dedicated GPU value-
// asserted test placed EXACTLY at a real computed froxel Z-slice
// boundary -- strengthens matrix row 3's own "shading-side lookup
// agrees bit-exactly with the build side" conclusion (previously argued
// by construction: identical formulas + a single shared
// `FroxelGridParams` object flowing to both the culling compute kernels
// AND the shading-side caller, `ClusterPipelines::addClusterPasses()`,
// `cluster_lighting.cpp:279-298`) with a direct empirical check AT the
// seam itself -- the classically bug-prone spot in any clustered-shading
// implementation, since the CPU (build-side culling, this test's own
// `findSliceZ()` C++ oracle call) and GPU (shading-side
// `clusterFindSliceZ()`, compiled Slang) evaluate the SAME textual
// formula on different hardware/compiler paths, and a value meant to
// land EXACTLY on an integer slice boundary is exactly where CPU/GPU
// floating-point evaluation could in principle diverge by a rounding
// unit and flip which side of the `int()` truncation it lands on.
//
// Construction: `rx::scene::froxel::buildFroxelGrid()` is called
// directly here with the SAME arguments (camera, viewport,
// `ClusterParams`) `ClusterPipelines::addClusterPasses()` itself passes
// to the SAME function internally (`cluster_lighting.cpp:284-286`) -- a
// second, independent call to a pure, deterministic function with
// identical inputs, not a duplicated/hand-derived grid, so this test's
// own `grid` is provably the SAME grid the real GPU compute chain below
// uses. `sliceZDistance(boundaryIndex, grid)` (the CPU oracle for a
// slice boundary's own view-space distance) gives the boundary Z;
// `findSliceZ(-boundaryDist, grid)` (the SAME CPU oracle `rx_cluster`'s
// own build-side culling code already trusts) gives "the build-side's
// own membership decision" for a point EXACTLY at that boundary --
// QUERIED, not hand-assumed, since `exp2`/`log2` need not round-trip to
// an exact integer.
//
// A light sits DELIBERATELY TIGHT (`cullRadius=0.5`, well under the
// `delta=1.0` unit gap to the boundary -- unlike this file's other
// tests' own DELIBERATELY GENEROUS radii, which exist to avoid
// entangling culling-sphere geometry with the property under test; here
// the opposite is deliberate: a small, explicit radius keeps the
// light's own real build-side membership UNAMBIGUOUSLY confined to slice
// `boundaryIndex-1` alone, verified below via a `REQUIRE()` on the
// neighbouring slice's own width rather than assumed), so the light's
// build-side slice is a sound deduction, not a guess. A probe sits
// EXACTLY at the boundary distance. The assertion branches on the CPU
// oracle's own real verdict: if the boundary resolves to slice
// `boundaryIndex-1` (the SAME slice as the light), the probe MUST see
// the light (closed-form nonzero value, the SAME NdotL==1/distSq==1
// matched-pose shape the exact-membership TEST_CASE above establishes);
// if it resolves to slice `boundaryIndex` (a different slice), the probe
// MUST NOT (exactly zero) -- either branch is a genuine, falsifiable
// prediction from the SAME formula the real GPU shading path runs, not a
// tautology; a genuine CPU/GPU divergence at this boundary would flip
// the GPU's real answer against this prediction and fail the assertion.
TEST_CASE("Clustered shading: fragment/light pair placed exactly AT a computed froxel Z-slice boundary matches "
          "the build-side's own real membership decision (rx::scene::froxel::findSliceZ oracle)") {
    auto fixtureOpt = makeFixture("rx_cluster_shading_boundary");
    if (!fixtureOpt.has_value()) {
        return;
    }
    Fixture fixture = std::move(*fixtureOpt);

    auto computeCache =
        rx::rhi::ComputePipelineCache::create(fixture.device.device(), freshCachePath("boundary_chain"));
    REQUIRE(computeCache.has_value());
    auto compiler = rx::shader::Compiler::create();
    REQUIRE(compiler.has_value());
    auto pipelinesOpt = rx::cluster::ClusterPipelines::create(fixture.device.device(), *computeCache, *compiler,
                                                                RX_CLUSTER_SHADER_DIR, /*framesInFlight=*/1);
    REQUIRE(pipelinesOpt.has_value());
    rx::cluster::ClusterPipelines pipelines = std::move(*pipelinesOpt);

    auto probe = buildProbePipeline(fixture.device.device(), fixture.bindless);

    rx::scene::Camera camera;  // default: position (0,0,0), looking down -Z, 60deg vfov.
    camera.aspectRatio = 1.0F;
    constexpr uint32_t kViewport = 256;
    rx::cluster::ClusterParams params;  // defaults: kDefaultZLightNear=5, kDefaultZLightFar=100, sliceCountZ=16.

    // Independent second call to the SAME pure/deterministic function
    // `ClusterPipelines::addClusterPasses()` itself calls internally
    // (`cluster_lighting.cpp:284-286`) with IDENTICAL arguments -- see
    // this TEST_CASE's own header comment.
    const rx::scene::froxel::FroxelGridParams grid =
        rx::scene::froxel::buildFroxelGrid(kViewport, kViewport, camera.verticalFovRadians, camera.aspectRatio,
                                             params.zLightNear, params.zLightFar, params.targetFroxelBudget,
                                             params.sliceCountZ);
    REQUIRE(grid.countZ >= 4);  // sanity -- the mid-slice index below needs real interior room on both sides.

    const uint32_t boundaryIndex = grid.countZ / 2;
    const float boundaryDist = rx::scene::froxel::sliceZDistance(boundaryIndex, grid);
    const float lowerBound = rx::scene::froxel::sliceZDistance(boundaryIndex - 1, grid);
    CAPTURE(boundaryIndex);
    CAPTURE(boundaryDist);
    CAPTURE(lowerBound);
    constexpr float kDelta = 1.0F;       // light sits this far inside the boundary, camera-ward -- matched-pose NdotL==1.
    constexpr float kCullRadius = 0.5F;  // deliberately tight -- see this TEST_CASE's own header comment.
    // Geometric precondition for "the light's cull sphere cannot reach
    // the neighbouring slice below" -- see this TEST_CASE's own header
    // comment's "sound deduction, not a guess" claim.
    REQUIRE(boundaryDist - lowerBound > kDelta + kCullRadius);

    // "The build-side's own membership decision" [review's own words] --
    // queried, not assumed: whichever slice `findSliceZ()` really
    // resolves this exact boundary distance to.
    const uint32_t probeSlice = rx::scene::froxel::findSliceZ(-boundaryDist, grid);
    const uint32_t lightSlice = boundaryIndex - 1;
    CAPTURE(probeSlice);
    CAPTURE(lightSlice);
    const bool expectContribution = (probeSlice == lightSlice);

    const glm::vec3 probeWorldPos(0.0F, 0.0F, -boundaryDist);
    const glm::vec3 lightWorldPos(0.0F, 0.0F, -(boundaryDist - kDelta));

    rx::cluster::ClusterLightGpu light{};
    light.viewPositionRadius = glm::vec4(glm::vec3(camera.view() * glm::vec4(lightWorldPos, 1.0F)), kCullRadius);
    light.viewAxisSinInverse = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);
    light.cosSquaredFlags = glm::vec4(0.0F, 0.0F, 0.0F, 0.0F);
    light.shadingTypeRangeAngle = glm::vec4(1.0F, 0.0F, 0.0F, 0.0F);  // Point, range=0 (unwindowed).
    light.shadingPositionWorld = glm::vec4(lightWorldPos, 0.0F);
    light.shadingColorCandela = glm::vec4(10.0F, 0.0F, 0.0F, 0.0F);  // RED-only, same convention as the membership TEST_CASE.
    light.shadingSpotDirWorld = glm::vec4(0.0F, 0.0F, -1.0F, 0.0F);

    const std::vector<rx::cluster::ClusterLightGpu> lights{light};
    ProbeResult result =
        runProbe(fixture, pipelines, *probe, lights, camera, kViewport, kViewport, params, probeWorldPos);

    CAPTURE(result.diffuse[0].r);
    CAPTURE(expectContribution);
    if (expectContribution) {
        // Closed form: Fd_Lambert()=1/pi, NdotL=1 (kDelta-unit matched
        // pose), distSq=1, rangeWindow=1 -- SAME shape as the
        // exact-membership TEST_CASE above.
        const float expectedDiffuseR = 10.0F / static_cast<float>(M_PI);
        CHECK(near(result.diffuse[0].r, expectedDiffuseR, 0.02F));
    } else {
        CHECK(result.diffuse[0].r == 0.0F);
    }
}
