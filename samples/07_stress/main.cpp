// Sample 07: parallel-recording stress benchmark [Phase 4 Task 7, spec D4
// amendment -- Stage 0's exit sample].
//
// A procedural instanced field: --draws N (default 30000) separate objects
// (never real GPU-side instancing -- see the header comment on
// recordForwardChunked() below for exactly why each one is its own
// vkCmdDrawIndexed call), 4 mesh/pipeline-state variants so state changes
// are non-trivial, drawn through the SAME graph-driven forward+tonemap
// shape samples/05_multipass established, with the forward pass CHUNKED
// (Pass::setExecuteChunked()) -- the sample this whole task exists to prove
// the parallel recording path with real, adjustable measurement knobs.
//
// --threads N is a MEASUREMENT INSTRUMENT, not an engine-wide switch
// [docs/threading.md]: it overrides this sample's own Scheduler's worker
// count (Scheduler::create(N)), which the executor's chunk-count derivation
// (rx::graph::detail::chunkCountForWorkerCount(), executor.h) already
// self-scales to -- `--threads 1` collapses the forward pass to exactly one
// chunk, i.e. genuinely serial recording (the A/B baseline); the default
// (no --threads given, Scheduler::create(0) -- hardware_concurrency() - 1)
// is the "parallel" side of the same A/B comparison. Parallelism itself is
// never toggled off by this flag -- it configures how many workers THIS
// sample's own Scheduler has, exactly like every other standalone consumer
// (docs/threading.md's "Host-engine coexistence" section).
//
// INSTANCE DATA IS STATIC: every instance's world position/scale/color is
// computed once, procedurally, and uploaded once at scene setup -- never
// recomputed or re-uploaded per frame. This is a deliberate experimental-
// design choice, not a missed opportunity for animation: recomputing 30000
// rows of CPU-side transform math every frame would conflate that cost with
// the one thing this sample measures (the forward pass's own CPU RECORDING
// time), making the single-thread-vs-default-worker-count comparison this
// task's report requires dishonest. The camera is likewise fixed (no orbit)
// for the same reason -- see makeCameraPose()'s own comment.
//
// GRID LAYOUT: a flat, non-overlapping grid on the XZ plane (gridSize =
// ceil(sqrt(drawCount)) per side, spacing wide enough that no two
// instances' footprints ever touch), viewed from directly above through an
// orthographic camera. This is what makes the headless correctness gate's
// four analytic pixel probes tractable at all: every instance occupies a
// distinct, non-overlapping screen region with zero inter-instance
// occlusion, so a probe at instance i's exact world top-point projects to a
// pixel that unambiguously belongs to instance i alone, for any drawCount.
//
// HEADLESS GATE (ctest, ~16 draws -- see samples/07_stress/CMakeLists.txt's
// own comment for why the registered gate uses a small drawCount, not the
// 30000 default): fixed 3 frames, then asserts EXACT counters (draws
// submitted == --draws, chunk count == rx::graph::detail::
// chunkCountForWorkerCount(scheduler->workerCount()), pool allocations
// within the same documented budget src/rx_graph/tests/test_execute_gpu.cpp
// already asserts) plus four analytic pixel probes, one per mesh/pipeline
// variant -- DOMINANCE-style (a probed channel is clearly higher than the
// others by a margin), not exact-value, matching samples/05_multipass's own
// convention for exactly the reason that file documents: dominance survives
// both this pass's own lighting scale (a uniform per-pixel multiplier) and
// the tonemap pass's Reinhard curve (a monotonic per-channel function)
// without this test needing to reproduce either formula bit-for-bit -- see
// this file's own probe-derivation section for the short proof. Channel
// POSITION (which byte is R/G/B) is resolved from the real backbuffer
// format (channelIndicesForFormat()), not guessed or checked both ways --
// "channel-order-exact" per this task's own brief.
//
// --present: an interactive window with the same static field, printing
// per-second stats (fps, cpu-record ms, draws) to stdout -- the sample
// feature this task's brief calls for; the actual reported numbers
// (task-7-report.md) additionally cite a Tracy zone-stats capture of the
// same "graph_pass_chunk_fanout"/"graph_pass_chunk_record" zones
// executor.cpp already emits (spec D3), for single-thread vs default
// worker count.
//
// Registered as ctest sample_07_stress_headless.
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <rx_platform/window.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_rhi_vk/mesh_buffers.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_rhi_vk/upload.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <rx_task/scheduler.h>

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <SDL3/SDL.h>
#include <volk.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// --- Constants --------------------------------------------------------------

constexpr uint32_t kHeadlessWidth = 256;
constexpr uint32_t kHeadlessHeight = 256;
constexpr VkDeviceSize kHeadlessPixelBytes = static_cast<VkDeviceSize>(kHeadlessWidth) * kHeadlessHeight * 4;
constexpr uint32_t kHeadlessFrameCount = 3;

// The interactive/measurement default -- task-7-brief.md: "default --draws
// 30000". The ctest-registered headless gate overrides this to a much
// smaller value (see this sample's own CMakeLists.txt) -- chunk-count
// derivation is workerCount-only (executor.h), never drawCount-dependent,
// so a small gate drawCount exercises the identical parallel-recording
// machinery a real 30000-draw run does, just framed small enough for exact
// pixel probing.
constexpr uint32_t kDefaultDraws = 30000;

constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// Both procedural meshes are generated at unit size (cube half-extent 1,
// sphere radius 1 -- see generateCube()/generateSphere() below) so a single
// `positionAndScale.w` scale factor applies uniformly to either shape, and
// both shapes' own TOP point (local +Y = 1) lands at exactly world Y ==
// scale after that one multiply -- the shared fact this file's probe
// derivation (topWorldPositionForInstance()) leans on.
constexpr float kInstanceScale = 0.85F;
// Spacing wide enough that two adjacent unit-scale instances (radius/half-
// extent kInstanceScale each) never touch: 2 * kInstanceScale is the
// minimum non-overlap distance; this leaves visible margin between them.
constexpr float kInstanceSpacing = 2.4F;

constexpr uint32_t kVariantCount = 4;

// --- CLI arguments ------------------------------------------------------

struct Args {
    uint32_t draws = kDefaultDraws;
    // 0 == "use the Scheduler default" (hardware_concurrency() - 1) --
    // Scheduler::create()'s own sentinel, reused verbatim rather than
    // inventing a second one (docs/threading.md: "--threads exists only in
    // the stress benchmark... it configures how many workers the
    // benchmark's own Scheduler is constructed with").
    uint32_t threads = 0;
    rx::rhi::PresentMode vsyncMode = rx::rhi::PresentMode::VsyncOn;
    bool validate = false;
    bool present = false;
};

std::optional<Args> parseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--present") {
            args.present = true;
        } else if (arg == "--validate") {
            args.validate = true;
        } else if (arg == "--draws" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            uint32_t parsed = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), parsed).ec != std::errc{} ||
                parsed == 0) {
                RX_LOG_ERROR("sample_07_stress: --draws expects a positive integer, got '{}'", value);
                return std::nullopt;
            }
            args.draws = parsed;
        } else if (arg == "--threads" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            uint32_t parsed = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), parsed).ec != std::errc{} ||
                parsed == 0) {
                RX_LOG_ERROR("sample_07_stress: --threads expects a positive integer, got '{}'", value);
                return std::nullopt;
            }
            args.threads = parsed;
        } else if (arg == "--vsync" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            if (value == "off") {
                args.vsyncMode = rx::rhi::PresentMode::VsyncOff;
            } else if (value == "on") {
                args.vsyncMode = rx::rhi::PresentMode::VsyncOn;
            } else {
                RX_LOG_ERROR("sample_07_stress: --vsync expects 'on' or 'off', got '{}' -- defaulting to on", value);
            }
        } else {
            RX_LOG_ERROR("sample_07_stress: unrecognized argument '{}'", arg);
            return std::nullopt;
        }
    }
    return args;
}

// --- Procedural geometry: position + normal, unit-sized (see kInstanceScale
// above) -- byte-identical generation approach to samples/05_multipass's
// own generateCube()/generateSphere(), just at h=1/radius=1 instead of that
// sample's own half-extents. ------------------------------------------------

struct Vertex {
    float position[3];
    float normal[3];
};

struct HostMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

void addQuad(HostMesh& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 normal) {
    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    for (const glm::vec3& p : {a, b, c, d}) {
        mesh.vertices.push_back(Vertex{{p.x, p.y, p.z}, {normal.x, normal.y, normal.z}});
    }
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 3);
}

HostMesh generateCube(float h) {
    HostMesh mesh;
    addQuad(mesh, {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h}, {1, 0, 0});      // +X
    addQuad(mesh, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h}, {-1, 0, 0});  // -X
    addQuad(mesh, {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}, {0, 1, 0});       // +Y (top)
    addQuad(mesh, {-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {0, -1, 0});  // -Y
    addQuad(mesh, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});       // +Z
    addQuad(mesh, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});  // -Z
    return mesh;
}

HostMesh generateSphere(float radius, uint32_t rings, uint32_t segments) {
    HostMesh mesh;
    constexpr float kPi = 3.14159265358979323846F;

    for (uint32_t y = 0; y <= rings; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(rings);
        const float theta = v * kPi;
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        for (uint32_t x = 0; x <= segments; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(segments);
            const float phi = u * 2.0F * kPi;
            const glm::vec3 dir(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
            Vertex vert{};
            vert.position[0] = radius * dir.x;
            vert.position[1] = radius * dir.y;
            vert.position[2] = radius * dir.z;
            vert.normal[0] = dir.x;
            vert.normal[1] = dir.y;
            vert.normal[2] = dir.z;
            mesh.vertices.push_back(vert);
        }
    }

    const uint32_t stride = segments + 1;
    for (uint32_t y = 0; y < rings; ++y) {
        for (uint32_t x = 0; x < segments; ++x) {
            const uint32_t i0 = y * stride + x;
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + stride;
            const uint32_t i3 = i2 + 1;
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i3);
        }
    }
    return mesh;
}

// --- Small helpers shared with every other sample's own copy --------------

VkShaderModule createShaderModuleFromSpirv(VkDevice device, const std::vector<uint32_t>& code) {
    if (code.empty()) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

std::string resolveAssetPath(const char* filename) {
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        RX_LOG_WARN("sample_07_stress: SDL_GetBasePath failed ({}); looking for {} in the current directory instead",
                    SDL_GetError(), filename);
        return filename;
    }
    return std::string(basePath) + filename;
}

std::optional<std::string> readAndConcatenate(const std::vector<std::string>& filenames) {
    std::string combined;
    for (const auto& filename : filenames) {
        const std::string path = resolveAssetPath(filename.c_str());
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            RX_LOG_ERROR("sample_07_stress: failed to open shader file '{}'", path);
            return std::nullopt;
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        combined += contents.str();
        combined += "\n";
    }
    return combined;
}

struct ReflectedModule {
    rx::shader::CompileResult compileResult;
    rx::shader::ShaderLayoutInfo layoutInfo;
};

std::optional<ReflectedModule> compileAndReflect(rx::shader::Compiler& compiler, const std::string& moduleName,
                                                  const std::vector<std::string>& filenames,
                                                  const std::vector<std::string>& entryPoints) {
    auto source = readAndConcatenate(filenames);
    if (!source.has_value()) {
        return std::nullopt;
    }
    rx::shader::CompileResult compileResult = compiler.compileFromSource(moduleName, *source, entryPoints);
    if (!compileResult.ok) {
        RX_LOG_ERROR("sample_07_stress: shader compile failed for module '{}':\n{}", moduleName,
                     compileResult.diagnostics);
        return std::nullopt;
    }
    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("sample_07_stress: reflect() failed for module '{}'", moduleName);
        return std::nullopt;
    }
    return ReflectedModule{std::move(compileResult), std::move(*layoutInfo)};
}

bool assignShaderModules(VkDevice device, const rx::shader::CompileResult& compileResult, const char* vertEntryName,
                          const char* fragEntryName, VkShaderModule& vertModule, VkShaderModule& fragModule) {
    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModule module = createShaderModuleFromSpirv(device, blob.code);
        if (module == VK_NULL_HANDLE) {
            RX_LOG_ERROR("sample_07_stress: vkCreateShaderModule failed for entry point '{}'", blob.entryPointName);
            return false;
        }
        if (blob.entryPointName == vertEntryName) {
            vertModule = module;
        } else if (fragEntryName != nullptr && blob.entryPointName == fragEntryName) {
            fragModule = module;
        } else {
            RX_LOG_ERROR("sample_07_stress: unexpected entry point '{}'", blob.entryPointName);
            vkDestroyShaderModule(device, module, nullptr);
            return false;
        }
    }
    if (vertModule == VK_NULL_HANDLE || (fragEntryName != nullptr && fragModule == VK_NULL_HANDLE)) {
        RX_LOG_ERROR("sample_07_stress: missing an expected entry point after a successful compile");
        return false;
    }
    return true;
}

// --- GPU-side scene ---------------------------------------------------------

struct GpuMesh {
    std::optional<rx::rhi::MeshBuffers> buffers;
};

// Mirrors shaders/stress/instanced.vert.slang's InstanceData exactly --
// float4s (not float3s) throughout, sidestepping any float3-in-a-
// StructuredBuffer packing ambiguity, same convention as
// samples/05_multipass's ObjectTransform (scene_types.slang's own header
// comment).
struct InstanceData {
    float positionAndScale[4];  // xyz = world position, w = uniform scale
    float color[4];              // rgb = albedo, a unused
};

// Mirrors shaders/stress/instanced.vert.slang's PushConstants -- viewProj
// FIRST (offset 0, 64 bytes), instanceIndex SECOND (offset 64, 4 bytes),
// `_pad` LAST (offset 68, 12 bytes, never read by either side) -- Slang
// rounds a push-constant BLOCK's total size up to its largest member's own
// alignment (16, float4x4's), so a 64+4=68-byte block reflects as 80 bytes
// (EMPIRICALLY VERIFIED against this exact shader's own reflected
// pushRanges[0].size during this sample's development, not assumed) --
// `_pad` makes sizeof(InstancedPushConstants) match that 80 bytes exactly,
// so vkCmdPushConstants() (which always copies the REFLECTED size, per
// CompiledPass::pushConstantSize -- see buildForwardPipelines()' own
// comment) never reads 12 bytes past this struct's own end.
//
// TRANSPOSED BEFORE EVERY PUSH -- see recordForwardChunked()'s own comment
// for why: GLM stores column-major, Slang's `float4x4` + `mul(M, v)`
// convention expects row-major data for a buffer-sourced matrix (the exact
// same transpose samples/05_multipass's own updateFrame() already applies
// to its ObjectTransform::mvp/lightMvp before uploading -- this is that
// established convention, not a novel one, just this sample's first time
// applying it to a PUSH CONSTANT rather than a storage-buffer row).
struct InstancedPushConstants {
    glm::mat4 viewProj;
    uint32_t instanceIndex;
    uint32_t _pad[3];
};

struct TonemapPushConstants {
    uint32_t hdrTextureIndex;
    uint32_t hdrSamplerIndex;
};

struct CompiledPass {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    uint32_t pushConstantOffset = 0;
    uint32_t pushConstantSize = 0;
    VkShaderStageFlags pushConstantStages = 0;
};

void destroyCompiledPass(VkDevice device, CompiledPass& pass) {
    if (pass.fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, pass.fragModule, nullptr);
        pass.fragModule = VK_NULL_HANDLE;
    }
    if (pass.vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, pass.vertModule, nullptr);
        pass.vertModule = VK_NULL_HANDLE;
    }
    pass.layoutBundle = rx::rhi::PipelineLayoutBundle{};
    pass.pushConstantOffset = 0;
    pass.pushConstantSize = 0;
    pass.pushConstantStages = 0;
}

// One of the 4 mesh/pipeline-state variants [task-7-brief.md: "4 mesh/
// pipeline-state variations across 4 material-ish pipeline permutations so
// state changes are non-trivial"]. `meshIndex` selects cube (0) or sphere
// (1); `cullMode` is the one fixed-function axis this sample varies to
// force a genuinely different VkPipeline object per variant (back-face
// culling vs none -- visually identical for a closed, opaque, depth-tested
// mesh, so this sample's rendered output never depends on which; the point
// is a real vkCmdBindPipeline state change between variants, not a visual
// difference). `color` is DOMINANCE-DISTINCT from every other variant's own
// (see this file's header comment) -- the headless gate's probe check for
// this variant.
struct VariantSpec {
    uint32_t meshIndex;
    VkCullModeFlagBits cullMode;
    glm::vec3 color;
};

const std::array<VariantSpec, kVariantCount> kVariants{{
    {0, VK_CULL_MODE_BACK_BIT, glm::vec3(1.0F, 0.05F, 0.05F)},  // cube,   cull-back -- red
    {1, VK_CULL_MODE_BACK_BIT, glm::vec3(0.05F, 1.0F, 0.05F)},  // sphere, cull-back -- green
    {0, VK_CULL_MODE_NONE, glm::vec3(0.05F, 0.05F, 1.0F)},      // cube,   no culling -- blue
    {1, VK_CULL_MODE_NONE, glm::vec3(0.9F, 0.05F, 0.9F)},       // sphere, no culling -- magenta
}};

struct Scene {
    rx::rhi::BindlessTable bindlessTable;
    std::array<GpuMesh, 2> meshes;  // [0] = cube, [1] = sphere

    std::optional<rx::rhi::Buffer> instanceBuffer;
    rx::rhi::BindlessHandle instanceBufferHandle;

    CompiledPass forwardPass;
    std::array<VkPipeline, kVariantCount> forwardPipelines{};

    CompiledPass tonemapPass;
    VkPipeline tonemapPipeline = VK_NULL_HANDLE;

    VkSampler sampler = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle samplerHandle;

    rx::rhi::BindlessHandle hdrHandle;
    VkImageView lastHdrView = VK_NULL_HANDLE;

    uint32_t drawCount = 0;
    glm::mat4 viewProj{1.0F};

    // Phase 4 Task 7: this sample's own "draws submitted" counter -- reset
    // to 0 (main thread, before executor->execute()) each frame, incremented
    // (std::memory_order_relaxed fetch_add) once per vkCmdDrawIndexed call by
    // however many chunks' own threads are recording concurrently, read back
    // (main thread, after executor->execute() returns -- synchronous/
    // blocking, so every chunk has already finished) for the headless gate's
    // exact-count assertion and this sample's own per-second stdout stats.
    // std::unique_ptr, not a bare std::atomic<uint64_t> member: Scene is
    // returned by value out of createScene() (moved into an
    // std::optional<Scene>), and std::atomic's own copy/move constructors
    // are deleted -- a bare atomic member would make that implicit Scene
    // move ill-formed. A heap-allocated atomic behind a freely-movable
    // unique_ptr sidesteps this entirely; the pointer itself is never
    // reseated after createScene() returns, so every chunk thread's
    // fetch_add() below dereferences the same, stable atomic for as long as
    // this Scene lives.
    std::unique_ptr<std::atomic<uint64_t>> drawsSubmitted = std::make_unique<std::atomic<uint64_t>>(0);
};

void destroyScene(VkDevice device, Scene& scene) {
    vkDeviceWaitIdle(device);
    if (scene.hdrHandle.isValid()) {
        scene.bindlessTable.release(scene.hdrHandle);
    }
    if (scene.samplerHandle.isValid()) {
        scene.bindlessTable.release(scene.samplerHandle);
    }
    if (scene.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, scene.sampler, nullptr);
    }
    if (scene.instanceBufferHandle.isValid()) {
        scene.bindlessTable.release(scene.instanceBufferHandle);
    }
    for (VkPipeline pipeline : scene.forwardPipelines) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
    }
    if (scene.tonemapPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, scene.tonemapPipeline, nullptr);
    }
    destroyCompiledPass(device, scene.forwardPass);
    destroyCompiledPass(device, scene.tonemapPass);
}

// --- Pipeline builders ------------------------------------------------------

bool buildForwardPipelines(VkDevice device, rx::shader::Compiler& compiler, VkDescriptorSetLayout bindlessSetLayout,
                            Scene& scene) {
    auto reflected =
        compileAndReflect(compiler, "StressInstancedModule", {"instanced.vert.slang", "instanced.frag.slang"},
                           {"vsMain", "fsMain"});
    if (!reflected.has_value()) {
        return false;
    }
    if (reflected->layoutInfo.pushRanges.size() != 1) {
        RX_LOG_ERROR("sample_07_stress: instanced shader reflects {} push-constant range(s), expected exactly 1",
                     reflected->layoutInfo.pushRanges.size());
        return false;
    }
    // This shader only ever declares gInstances[] (binding 2, storage
    // buffers) -- no Texture2D/SamplerState at all, unlike
    // samples/05_multipass's lit.vert.slang -- a legal STRICT SUBSET of
    // BindlessTable's fixed 3-binding scheme
    // (rx_rhi_vk/pipeline_layout.h's own PipelineLayoutBuilder::build()
    // doc comment: "a shader is free to use a strict subset of the three
    // slots"), so this checks "at most 3", not "exactly 3".
    if (reflected->layoutInfo.bindings.size() > 3) {
        RX_LOG_ERROR(
            "sample_07_stress: instanced shader reflects {} set-0 bindings; expected at most 3 (images/samplers/"
            "storage buffers, matching BindlessTable's fixed scheme)",
            reflected->layoutInfo.bindings.size());
        return false;
    }

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, reflected->layoutInfo, bindlessSetLayout);
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("sample_07_stress: PipelineLayoutBuilder::build failed for the forward pass");
        return false;
    }
    scene.forwardPass.layoutBundle = std::move(*layoutBundle);
    scene.forwardPass.pushConstantOffset = reflected->layoutInfo.pushRanges[0].offset;
    scene.forwardPass.pushConstantSize = reflected->layoutInfo.pushRanges[0].size;
    scene.forwardPass.pushConstantStages = reflected->layoutInfo.pushRanges[0].stages;
    // Defensive: InstancedPushConstants's own header comment documents
    // EXACTLY why this must equal 80 (64-byte mat4 + 4-byte uint, rounded up
    // to float4x4's own 16-byte alignment) -- a mismatch here means either
    // this struct or the shader's own field order/types have drifted,
    // matching samples/05_multipass's identical "does the reflected push
    // range match the C++ struct" defensive check on its own pipelines.
    if (scene.forwardPass.pushConstantSize != sizeof(InstancedPushConstants)) {
        RX_LOG_ERROR(
            "sample_07_stress: instanced shader reflects a push-constant size of {} bytes; expected {} "
            "(sizeof(InstancedPushConstants))",
            scene.forwardPass.pushConstantSize, sizeof(InstancedPushConstants));
        destroyCompiledPass(device, scene.forwardPass);
        return false;
    }

    if (!assignShaderModules(device, reflected->compileResult, "vsMain", "fsMain", scene.forwardPass.vertModule,
                              scene.forwardPass.fragModule)) {
        destroyCompiledPass(device, scene.forwardPass);
        return false;
    }

    std::array<VkVertexInputBindingDescription, 1> bindings{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(Vertex);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(Vertex, position);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(Vertex, normal);

    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertexInputState.pVertexBindingDescriptions = bindings.data();
    vertexInputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInputState.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
    inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencilState{};
    depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState.depthTestEnable = VK_TRUE;
    depthStencilState.depthWriteEnable = VK_TRUE;
    depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS;

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
    renderingCreateInfo.pColorAttachmentFormats = &kHdrFormat;
    renderingCreateInfo.depthAttachmentFormat = kDepthFormat;

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = scene.forwardPass.vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = scene.forwardPass.fragModule;
    stages[1].pName = "main";

    // One VkGraphicsPipelineCreateInfo per variant, differing ONLY in
    // rasterizationState.cullMode -- everything else (shader modules,
    // layout, vertex input, attachment formats) is shared, matching every
    // variant's own identical shader/layout shape [VariantSpec's own
    // comment]. Built via ONE vkCreateGraphicsPipelines call
    // (pipelineCount == kVariantCount) rather than kVariantCount separate
    // calls -- a trivial batching a driver can parallelize internally, and
    // this project's own established idiom wherever more than one sibling
    // pipeline is built together.
    std::array<VkPipelineRasterizationStateCreateInfo, kVariantCount> rasterizationStates{};
    std::array<VkGraphicsPipelineCreateInfo, kVariantCount> pipelineInfos{};
    for (uint32_t i = 0; i < kVariantCount; ++i) {
        VkPipelineRasterizationStateCreateInfo& rs = rasterizationStates[i];
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = kVariants[i].cullMode;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0F;

        VkGraphicsPipelineCreateInfo& info = pipelineInfos[i];
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &renderingCreateInfo;
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInputState;
        info.pInputAssemblyState = &inputAssemblyState;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &multisampleState;
        info.pDepthStencilState = &depthStencilState;
        info.pColorBlendState = &colorBlendState;
        info.pDynamicState = &dynamicState;
        info.layout = scene.forwardPass.layoutBundle.layout;
        info.renderPass = VK_NULL_HANDLE;
        info.basePipelineIndex = -1;
    }

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, kVariantCount, pipelineInfos.data(), nullptr,
                                   scene.forwardPipelines.data()) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_07_stress: vkCreateGraphicsPipelines failed for the forward pass's variants");
        destroyCompiledPass(device, scene.forwardPass);
        return false;
    }
    return true;
}

bool buildTonemapPipeline(VkDevice device, rx::shader::Compiler& compiler, VkDescriptorSetLayout bindlessSetLayout,
                          VkFormat backbufferFormat, Scene& scene) {
    auto reflected =
        compileAndReflect(compiler, "StressTonemapModule", {"tonemap.vert.slang", "tonemap.frag.slang"},
                           {"vsMain", "fsMain"});
    if (!reflected.has_value()) {
        return false;
    }
    if (reflected->layoutInfo.pushRanges.size() != 1 ||
        reflected->layoutInfo.pushRanges[0].size != sizeof(TonemapPushConstants)) {
        RX_LOG_ERROR("sample_07_stress: tonemap shader reflects an unexpected push-constant shape");
        return false;
    }

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, reflected->layoutInfo, bindlessSetLayout);
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("sample_07_stress: PipelineLayoutBuilder::build failed for the tonemap pass");
        return false;
    }
    scene.tonemapPass.layoutBundle = std::move(*layoutBundle);
    scene.tonemapPass.pushConstantOffset = reflected->layoutInfo.pushRanges[0].offset;
    scene.tonemapPass.pushConstantSize = reflected->layoutInfo.pushRanges[0].size;
    scene.tonemapPass.pushConstantStages = reflected->layoutInfo.pushRanges[0].stages;

    if (!assignShaderModules(device, reflected->compileResult, "vsMain", "fsMain", scene.tonemapPass.vertModule,
                              scene.tonemapPass.fragModule)) {
        destroyCompiledPass(device, scene.tonemapPass);
        return false;
    }

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
    renderingCreateInfo.pColorAttachmentFormats = &backbufferFormat;

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = scene.tonemapPass.vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = scene.tonemapPass.fragModule;
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
    pipelineInfo.pColorBlendState = &colorBlendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = scene.tonemapPass.layoutBundle.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &scene.tonemapPipeline) !=
        VK_SUCCESS) {
        RX_LOG_ERROR("sample_07_stress: vkCreateGraphicsPipelines failed for the tonemap pass");
        destroyCompiledPass(device, scene.tonemapPass);
        return false;
    }
    return true;
}

// --- Grid layout: shared by scene setup (instance upload) and the headless
// probe derivation (must compute the SAME positions/colors both places, by
// construction, not by two hand-synchronized copies) -----------------------

uint32_t gridSizeForDrawCount(uint32_t drawCount) {
    return static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(drawCount))));
}

// World position (Y == 0, the grid's base plane) for instance `i` -- see
// this file's header comment for why this never overlaps a neighbor's
// footprint regardless of drawCount.
glm::vec3 gridPositionForInstance(uint32_t i, uint32_t drawCount) {
    const uint32_t gridSize = gridSizeForDrawCount(drawCount);
    const uint32_t gx = i % gridSize;
    const uint32_t gz = i / gridSize;
    const float half = static_cast<float>(gridSize - 1) * 0.5F;
    const float worldX = (static_cast<float>(gx) - half) * kInstanceSpacing;
    const float worldZ = (static_cast<float>(gz) - half) * kInstanceSpacing;
    return glm::vec3(worldX, 0.0F, worldZ);
}

// Instance `i`'s own world TOP point -- both procedural meshes are unit-
// sized (kInstanceScale's own comment), so this is exactly
// gridPositionForInstance(i) + (0, kInstanceScale, 0) for either shape, no
// per-variant branch needed.
glm::vec3 topWorldPositionForInstance(uint32_t i, uint32_t drawCount) {
    return gridPositionForInstance(i, drawCount) + glm::vec3(0.0F, kInstanceScale, 0.0F);
}

// Fixed, non-orbiting top-down orthographic camera framing the whole grid
// for `drawCount` -- see this file's header comment for why this sample's
// camera never animates. up = (0,0,-1): looking straight down -Y makes
// (0,1,0) degenerate as an up vector, the standard top-down-camera fix.
glm::mat4 makeCameraViewProj(uint32_t drawCount, uint32_t viewportWidth, uint32_t viewportHeight) {
    const uint32_t gridSize = gridSizeForDrawCount(drawCount);
    const float halfExtent = static_cast<float>(gridSize - 1) * 0.5F * kInstanceSpacing + kInstanceScale + 1.0F;
    const float camHeight = halfExtent * 2.0F + 10.0F;

    const glm::vec3 eye(0.0F, camHeight, 0.0F);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, -1.0F));

    float halfWidth = halfExtent;
    float halfHeight = halfExtent;
    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
    if (aspect > 1.0F) {
        halfWidth = halfHeight * aspect;
    } else {
        halfHeight = halfWidth / aspect;
    }

    // Vulkan clip space is Y-down; glm::ortho assumes OpenGL's Y-up NDC --
    // the same fix every other sample in this codebase applies.
    glm::mat4 proj = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, 0.1F, camHeight + 10.0F);
    proj[1][1] *= -1.0F;

    return proj * view;
}

struct PixelCoord {
    uint32_t x;
    uint32_t y;
};

// Orthographic projection never touches w (clip.w is always exactly 1.0
// here) -- no perspective divide needed, same reasoning
// samples/06_materials' own projectToPixel() documents.
PixelCoord projectToPixel(glm::vec3 worldPos, const glm::mat4& viewProj, uint32_t width, uint32_t height) {
    const glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0F);
    const float pixelX = (clip.x * 0.5F + 0.5F) * static_cast<float>(width);
    const float pixelY = (clip.y * 0.5F + 0.5F) * static_cast<float>(height);
    const uint32_t x = static_cast<uint32_t>(std::clamp(pixelX, 0.0F, static_cast<float>(width) - 1.0F));
    const uint32_t y = static_cast<uint32_t>(std::clamp(pixelY, 0.0F, static_cast<float>(height) - 1.0F));
    return PixelCoord{x, y};
}

// --- Scene setup -------------------------------------------------------

std::optional<Scene> createScene(VkPhysicalDevice physicalDevice, VkDevice device, rx::rhi::Allocator& allocator,
                                  rx::rhi::Uploader& uploader, VkFormat backbufferFormat, uint32_t drawCount) {
    rx::rhi::BindlessTable::Capacities capacities;
    capacities.sampledImages = 4;
    capacities.samplers = 2;
    capacities.storageBuffers = 2;
    auto bindless = rx::rhi::BindlessTable::create(physicalDevice, device, capacities);
    if (!bindless.has_value()) {
        RX_LOG_ERROR("sample_07_stress: BindlessTable::create failed");
        return std::nullopt;
    }

    Scene scene{std::move(*bindless), {}};
    scene.drawCount = drawCount;

    HostMesh cubeHost = generateCube(1.0F);
    HostMesh sphereHost = generateSphere(1.0F, /*rings=*/12, /*segments=*/16);
    const std::array<const HostMesh*, 2> hostMeshes{&cubeHost, &sphereHost};
    for (size_t i = 0; i < hostMeshes.size(); ++i) {
        auto buffers = rx::rhi::MeshBuffers::create(
            allocator, uploader, hostMeshes[i]->vertices.data(), hostMeshes[i]->vertices.size() * sizeof(Vertex),
            hostMeshes[i]->indices.data(), hostMeshes[i]->indices.size() * sizeof(uint32_t),
            static_cast<uint32_t>(hostMeshes[i]->indices.size()));
        if (!buffers.has_value()) {
            RX_LOG_ERROR("sample_07_stress: MeshBuffers::create failed for mesh {}", i);
            destroyScene(device, scene);
            return std::nullopt;
        }
        scene.meshes[i].buffers = std::move(buffers);
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &scene.sampler) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_07_stress: vkCreateSampler failed");
        destroyScene(device, scene);
        return std::nullopt;
    }
    scene.samplerHandle = scene.bindlessTable.registerSampler(scene.sampler);
    if (!scene.samplerHandle.isValid()) {
        RX_LOG_ERROR("sample_07_stress: BindlessTable::registerSampler failed");
        destroyScene(device, scene);
        return std::nullopt;
    }

    // --- Instance data: uploaded ONCE, registered ONCE -- see this file's
    // header comment for why (never re-touched per frame).
    std::vector<InstanceData> instances(drawCount);
    for (uint32_t i = 0; i < drawCount; ++i) {
        const glm::vec3 pos = gridPositionForInstance(i, drawCount);
        const glm::vec3 color = kVariants[i % kVariantCount].color;
        instances[i] = InstanceData{{pos.x, pos.y, pos.z, kInstanceScale}, {color.r, color.g, color.b, 0.0F}};
    }
    const VkDeviceSize instanceBufferSize = static_cast<VkDeviceSize>(drawCount) * sizeof(InstanceData);
    auto instanceBuffer = allocator.createDeviceLocalBuffer(
        instanceBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!instanceBuffer.has_value()) {
        RX_LOG_ERROR("sample_07_stress: createDeviceLocalBuffer failed for the instance buffer");
        destroyScene(device, scene);
        return std::nullopt;
    }
    scene.instanceBuffer = std::move(instanceBuffer);
    if (!uploader.uploadToBuffer(*scene.instanceBuffer, 0, instances.data(), instanceBufferSize)) {
        RX_LOG_ERROR("sample_07_stress: uploadToBuffer failed for the instance buffer");
        destroyScene(device, scene);
        return std::nullopt;
    }
    uploader.flush();
    scene.instanceBufferHandle =
        scene.bindlessTable.registerStorageBuffer(scene.instanceBuffer->handle(), instanceBufferSize);
    if (!scene.instanceBufferHandle.isValid()) {
        RX_LOG_ERROR("sample_07_stress: BindlessTable::registerStorageBuffer failed");
        destroyScene(device, scene);
        return std::nullopt;
    }

    scene.viewProj = makeCameraViewProj(drawCount, kHeadlessWidth, kHeadlessHeight);

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        RX_LOG_ERROR("sample_07_stress: rx::shader::Compiler::create failed");
        destroyScene(device, scene);
        return std::nullopt;
    }
    if (!buildForwardPipelines(device, *compiler, scene.bindlessTable.descriptorSetLayout(), scene) ||
        !buildTonemapPipeline(device, *compiler, scene.bindlessTable.descriptorSetLayout(), backbufferFormat,
                               scene)) {
        destroyScene(device, scene);
        return std::nullopt;
    }

    return scene;
}

// --- Render-graph declaration + per-pass draw recording --------------------

rx::graph::AttachmentDesc swapchainRelativeDesc(VkFormat format) {
    rx::graph::AttachmentDesc desc;
    desc.format = format;
    desc.sizeClass = rx::graph::SizeClass::SwapchainRelative;
    desc.width = 1.0F;
    desc.height = 1.0F;
    return desc;
}

// Phase 4 Task 7 [spec D4 amendment]: the CHUNKED forward pass -- one
// vkCmdDrawIndexed call PER INSTANCE, deliberately, never real GPU-side
// instancing (a single vkCmdDrawIndexed with instanceCount == drawCount).
// This sample exists to measure CPU-side per-DRAW-CALL recording cost at
// scale -- that is exactly the cost real GPU instancing would eliminate,
// which would leave nothing for parallel recording to meaningfully speed
// up (one draw call is trivial to record regardless of how many workers
// are available). --draws is this sample's own dial on that CPU cost, not
// on vertex/triangle throughput.
//
// Ceil-division slice of [0, drawCount) this chunk owns -- ascending
// chunkIndex claims ascending instance index, exactly
// samples/05_multipass's own recordLitDrawsChunked() convention.
void recordForwardChunked(rx::graph::PassContext& ctx, Scene& scene, uint32_t chunkIndex, uint32_t chunkCount) {
    VkCommandBuffer cmd = ctx.chunkCommandBuffer();

    VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width), static_cast<float>(ctx.renderArea.height),
                         0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, ctx.renderArea};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDescriptorSet set = scene.bindlessTable.descriptorSet();
    // Legal before any vkCmdBindPipeline: descriptor-set binding is
    // layout-compatibility-checked at DRAW time, not at bind time, and
    // every one of this pass's 4 variant pipelines shares the identical
    // VkPipelineLayout (buildForwardPipelines()'s own comment) -- so
    // binding set 0 once, up front, is correct regardless of which variant
    // pipeline ends up bound by the time a draw actually happens.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.forwardPass.layoutBundle.layout,
                             /*firstSet=*/0, 1, &set, 0, nullptr);

    const uint32_t drawCount = scene.drawCount;
    const uint32_t perChunk = (drawCount + chunkCount - 1) / chunkCount;
    const uint32_t begin = std::min(chunkIndex * perChunk, drawCount);
    const uint32_t end = std::min(begin + perChunk, drawCount);

    // Transposed ONCE per chunk, not once per draw -- GLM stores
    // column-major, Slang's `float4x4` + `mul(M, v)` convention expects
    // row-major buffer-sourced matrix data (InstancedPushConstants's own
    // header comment has the full account, including the empirical proof
    // this sample's own development needed); `scene.viewProj` is the SAME
    // value for every one of this chunk's draws (a fixed, non-orbiting
    // per-frame camera -- this file's own header comment), so transposing
    // it 30000 times over instead of once would be pure, avoidable CPU
    // waste inside the exact call this sample exists to measure.
    const glm::mat4 viewProjTransposed = glm::transpose(scene.viewProj);

    int32_t lastVariant = -1;
    uint64_t localDraws = 0;
    for (uint32_t i = begin; i < end; ++i) {
        const uint32_t variant = i % kVariantCount;
        if (static_cast<int32_t>(variant) != lastVariant) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.forwardPipelines[variant]);
            lastVariant = static_cast<int32_t>(variant);
        }

        InstancedPushConstants push{};
        push.viewProj = viewProjTransposed;
        push.instanceIndex = i;
        vkCmdPushConstants(cmd, scene.forwardPass.layoutBundle.layout, scene.forwardPass.pushConstantStages,
                           scene.forwardPass.pushConstantOffset, scene.forwardPass.pushConstantSize, &push);

        const GpuMesh& mesh = scene.meshes[kVariants[variant].meshIndex];
        VkBuffer vertexBuffer = mesh.buffers->vertexBuffer();
        VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(cmd, mesh.buffers->indexBuffer(), 0, mesh.buffers->indexType());
        vkCmdDrawIndexed(cmd, mesh.buffers->indexCount(), 1, 0, 0, 0);
        ++localDraws;
    }

    if (localDraws > 0) {
        scene.drawsSubmitted->fetch_add(localDraws, std::memory_order_relaxed);
    }
}

void recordTonemapDraw(rx::graph::PassContext& ctx, Scene& scene) {
    VkImageView hdrView = ctx.imageView("hdr");
    if (hdrView != scene.lastHdrView) {
        if (scene.hdrHandle.isValid()) {
            scene.bindlessTable.release(scene.hdrHandle);
        }
        scene.hdrHandle = scene.bindlessTable.registerSampledImage(hdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        scene.lastHdrView = hdrView;
    }

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.tonemapPipeline);
    VkDescriptorSet set = scene.bindlessTable.descriptorSet();
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.tonemapPass.layoutBundle.layout, 0, 1,
                             &set, 0, nullptr);

    VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width), static_cast<float>(ctx.renderArea.height),
                         0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, ctx.renderArea};
    vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

    TonemapPushConstants push{};
    push.hdrTextureIndex = scene.hdrHandle.index();
    push.hdrSamplerIndex = scene.samplerHandle.index();
    vkCmdPushConstants(ctx.cmd, scene.tonemapPass.layoutBundle.layout, scene.tonemapPass.pushConstantStages,
                       scene.tonemapPass.pushConstantOffset, scene.tonemapPass.pushConstantSize, &push);

    vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
}

void declareGraph(rx::graph::RenderGraph& graph, Scene& scene, VkFormat backbufferFormat) {
    graph.addPass("forward")
        .addColorOutput("hdr", swapchainRelativeDesc(kHdrFormat))
        .setDepthStencilOutput("depth", swapchainRelativeDesc(kDepthFormat))
        .setExecuteChunked([&scene](rx::graph::PassContext& ctx, uint32_t chunkIndex, uint32_t chunkCount) {
            recordForwardChunked(ctx, scene, chunkIndex, chunkCount);
        });

    graph.addPass("tonemap")
        .addTextureInput("hdr")
        .addColorOutput("backbuffer", swapchainRelativeDesc(backbufferFormat))
        .setExecute([&scene](rx::graph::PassContext& ctx) { recordTonemapDraw(ctx, scene); });

    graph.setBackbufferSource("backbuffer");
}

// --- Headless probe derivation -----------------------------------------
// DOMINANCE-style checks, channel-order-exact -- see this file's own header
// comment for the short proof that dominance survives both this pass's own
// lighting scale and the tonemap pass's Reinhard curve.

struct ChannelIndices {
    int r;
    int g;
    int b;
};

// Copied from samples/05_multipass/06_materials' own identically-named
// helper (not re-derived) -- see either file's own comment for the Vulkan-
// format-naming-convention rationale.
std::optional<ChannelIndices> channelIndicesForFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SNORM:
            return ChannelIndices{2, 1, 0};
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SNORM:
            return ChannelIndices{0, 1, 2};
        default:
            return std::nullopt;
    }
}

// A clear separation margin (out of 0..255) between a variant's own
// dominant channel(s) and its non-dominant one(s) -- generous enough to
// absorb the ambient+diffuse lighting scale, the Reinhard tonemap curve,
// and any GPU-specific rounding/dithering, while still being a real,
// meaningful margin (not "any positive difference counts").
constexpr int kDominanceMargin = 25;

bool probeMatchesVariant(const uint8_t* pixel, const ChannelIndices& channels, uint32_t variant) {
    const int r = pixel[channels.r];
    const int g = pixel[channels.g];
    const int b = pixel[channels.b];
    switch (variant) {
        case 0:  // red
            return (r - g) > kDominanceMargin && (r - b) > kDominanceMargin;
        case 1:  // green
            return (g - r) > kDominanceMargin && (g - b) > kDominanceMargin;
        case 2:  // blue
            return (b - r) > kDominanceMargin && (b - g) > kDominanceMargin;
        case 3:  // magenta -- R and B both clearly above G
            return (r - g) > kDominanceMargin && (b - g) > kDominanceMargin;
        default:
            return false;
    }
}

}  // namespace

// --- Headless mode: offscreen render + pixel readback + counter gate -------

int runHeadless(const Args& args) {
    auto window = rx::platform::Window::create("rx_stress_sample", static_cast<int>(kHeadlessWidth),
                                                 static_cast<int>(kHeadlessHeight), /*visible=*/false);
    if (!window.has_value()) {
        RX_LOG_ERROR("Window::create failed: no display backend available");
        return 1;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        RX_LOG_ERROR("video driver reports no Vulkan surface extensions (e.g. dummy driver)");
        return 1;
    }
    auto context = rx::rhi::Context::create(extensions, args.validate);
    if (!context.has_value()) {
        RX_LOG_ERROR("Context::create failed");
        return 1;
    }
    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    if (surface == VK_NULL_HANDLE) {
        RX_LOG_ERROR("createVulkanSurface failed");
        return 1;
    }
    auto device = rx::rhi::Device::create(*context, surface);
    if (!device.has_value()) {
        RX_LOG_ERROR("Device::create failed");
        return 1;
    }
    auto allocator = rx::rhi::Allocator::create(*context, *device);
    if (!allocator.has_value()) {
        RX_LOG_ERROR("Allocator::create failed");
        return 1;
    }
    auto uploader = rx::rhi::Uploader::create(*allocator, *device);
    if (!uploader.has_value()) {
        RX_LOG_ERROR("Uploader::create failed");
        return 1;
    }
    auto cmdCtx =
        rx::rhi::CommandContext::create(device->device(), device->graphicsQueue(), device->graphicsQueueFamily());
    if (!cmdCtx.has_value()) {
        RX_LOG_ERROR("CommandContext::create failed");
        return 1;
    }

    // Phase 4 Task 7: --threads overrides this sample's own Scheduler's
    // worker count (0 == Scheduler's own "auto" default) -- see this file's
    // header comment.
    auto scheduler = rx::task::Scheduler::create(args.threads);
    if (scheduler == nullptr) {
        RX_LOG_ERROR("rx::task::Scheduler::create failed");
        return 1;
    }

    auto executor = rx::graph::Executor::create(*device, *scheduler);
    if (executor == nullptr) {
        RX_LOG_ERROR("rx::graph::Executor::create failed");
        return 1;
    }

    const VkDevice vkDevice = device->device();
    const VkFormat targetFormat = device->swapchainFormat();

    auto scene = createScene(device->physicalDevice(), vkDevice, *allocator, *uploader, targetFormat, args.draws);
    if (!scene.has_value()) {
        RX_LOG_ERROR("createScene failed");
        return 1;
    }

    rx::graph::RenderGraph graph;
    declareGraph(graph, *scene, targetFormat);

    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = kHeadlessWidth;
    compileInfo.swapchainHeight = kHeadlessHeight;
    compileInfo.swapchainFormat = targetFormat;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);
    executor->realize(graph);

    // Offscreen "backbuffer" -- same raw (non-RAII) construction pattern
    // every other sample's own headless mode already uses.
    VkImage offscreenImage = VK_NULL_HANDLE;
    VkDeviceMemory offscreenMemory = VK_NULL_HANDLE;
    VkImageView offscreenView = VK_NULL_HANDLE;
    auto destroyRawResources = [&]() {
        if (offscreenView != VK_NULL_HANDLE) {
            vkDestroyImageView(vkDevice, offscreenView, nullptr);
        }
        if (offscreenImage != VK_NULL_HANDLE) {
            vkDestroyImage(vkDevice, offscreenImage, nullptr);
        }
        if (offscreenMemory != VK_NULL_HANDLE) {
            vkFreeMemory(vkDevice, offscreenMemory, nullptr);
        }
    };

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = targetFormat;
    imageInfo.extent = {kHeadlessWidth, kHeadlessHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vkDevice, &imageInfo, nullptr, &offscreenImage) != VK_SUCCESS) {
        RX_LOG_ERROR("vkCreateImage(offscreen target) failed");
        destroyRawResources();
        return 1;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(vkDevice, offscreenImage, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(device->physicalDevice(), &memProps);
    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == UINT32_MAX) {
        RX_LOG_ERROR("no device-local memory type found for the offscreen target");
        destroyRawResources();
        return 1;
    }
    VkMemoryAllocateInfo memAllocInfo{};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReq.size;
    memAllocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(vkDevice, &memAllocInfo, nullptr, &offscreenMemory) != VK_SUCCESS) {
        RX_LOG_ERROR("vkAllocateMemory(offscreen target) failed");
        destroyRawResources();
        return 1;
    }
    if (vkBindImageMemory(vkDevice, offscreenImage, offscreenMemory, 0) != VK_SUCCESS) {
        RX_LOG_ERROR("vkBindImageMemory(offscreen target) failed");
        destroyRawResources();
        return 1;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = offscreenImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = targetFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &offscreenView) != VK_SUCCESS) {
        RX_LOG_ERROR("vkCreateImageView(offscreen target) failed");
        destroyRawResources();
        return 1;
    }

    const VkExtent2D extent{kHeadlessWidth, kHeadlessHeight};
    const uint32_t expectedChunkCount = rx::graph::detail::chunkCountForWorkerCount(scheduler->workerCount());
    // Pool-allocation budget -- CORRECTED [fix round, found by a genuinely
    // flaky ctest failure this exact assertion caused: the original
    // formula assumed "each (frameSlot, threadIndex) pair ever needs at
    // most one secondary", which is WRONG -- enkiTS's own work-stealing
    // gives no guarantee that a single execute() call's `chunkCount`
    // chunks land on `chunkCount` DISTINCT threads; one thread can
    // legitimately grab two (or more) of this pass's chunks in the SAME
    // frame while another grabs none, needing that many buffers from its
    // OWN pool that same frame -- this is correct, expected
    // work-stealing behavior, not a defect, and asserting against it was
    // this test's own bug, not the executor's. The bound that IS
    // provably correct: within any ONE execute() call, the pass has
    // exactly `expectedChunkCount` chunks total, each consuming exactly
    // one buffer (freshly allocated or reused) from whichever thread
    // records it -- so a single execute() call can never contribute MORE
    // than `expectedChunkCount` new allocations, regardless of how
    // work-stealing happens to distribute them. Over
    // `kHeadlessFrameCount` calls, the budget is that many multiples of
    // it -- looser than the old (wrong) formula in the cases that used to
    // pass, but actually correct in every case, including the ones that
    // used to fail intermittently.
    const uint64_t poolBudget = static_cast<uint64_t>(kHeadlessFrameCount) * expectedChunkCount;

    bool gateOk = true;
    for (uint32_t frame = 0; frame < kHeadlessFrameCount; ++frame) {
        scene->drawsSubmitted->store(0, std::memory_order_relaxed);
        // Timed around ONLY executor->execute() itself, inside runOnce()'s
        // own record callback -- NOT runOnce() as a whole, which also
        // vkQueueSubmit()s and vkQueueWaitIdle()s (a synchronous setup/test
        // convenience -- rx_rhi_vk/command.h's own doc comment -- whose GPU
        // wait time has nothing to do with CPU recording cost). This is the
        // same "cpu_record_ms" this sample's --present loop reports, just
        // computed here so a plain headless run -- the shape CI can run
        // without a real display -- produces the identical published metric
        // [task-7-brief.md item 8: "single-thread vs default worker count
        // CPU record-time for the forward pass"].
        double recordMs = 0.0;
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            const auto recordStart = std::chrono::steady_clock::now();
            executor->execute(graph, cmd, offscreenImage, offscreenView, extent);
            const auto recordEnd = std::chrono::steady_clock::now();
            recordMs = std::chrono::duration<double, std::milli>(recordEnd - recordStart).count();
        });
        RX_FRAME_MARK;

        const uint64_t drawsThisFrame = scene->drawsSubmitted->load(std::memory_order_relaxed);
        if (drawsThisFrame != args.draws) {
            RX_LOG_ERROR("sample_07_stress: frame {}: drawsSubmitted={} != --draws={}", frame, drawsThisFrame,
                         args.draws);
            gateOk = false;
        }

        const rx::graph::detail::ExecutorChunkDebugStats stats = rx::graph::detail::debugChunkStats(*executor);
        if (stats.lastChunkCount != expectedChunkCount) {
            RX_LOG_ERROR("sample_07_stress: frame {}: lastChunkCount={} != expected chunkCountForWorkerCount({})={}",
                         frame, stats.lastChunkCount, scheduler->workerCount(), expectedChunkCount);
            gateOk = false;
        }
        if (stats.totalPoolAllocations > poolBudget) {
            RX_LOG_ERROR("sample_07_stress: frame {}: totalPoolAllocations={} exceeds budget {}", frame,
                         stats.totalPoolAllocations, poolBudget);
            gateOk = false;
        }
        // "stress:" prefix matches --present's own per-second stats line --
        // this is the line tools/*, CI, and task-7-report.md all grep for
        // (see this file's header comment).
        RX_LOG_INFO("stress: frame={} threads={} cpu_record_ms={:.3f} draws={} chunkCount={} poolAllocations={}",
                    frame, scheduler->workerCount(), recordMs, drawsThisFrame, stats.lastChunkCount,
                    stats.totalPoolAllocations);
    }

    auto readback = allocator->createHostVisibleBuffer(kHeadlessPixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!readback.has_value()) {
        RX_LOG_ERROR("createHostVisibleBuffer(readback) failed");
        destroyRawResources();
        return 1;
    }
    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {kHeadlessWidth, kHeadlessHeight, 1};
        vkCmdCopyImageToBuffer(cmd, offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1,
                                &region);
    });
    readback->invalidate();
    std::vector<uint8_t> pixels(static_cast<size_t>(kHeadlessPixelBytes));
    std::memcpy(pixels.data(), readback->mappedData(), pixels.size());
    auto pixelAt = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return pixels.data() + (static_cast<size_t>(y) * kHeadlessWidth + x) * 4;
    };

    const auto channels = channelIndicesForFormat(targetFormat);
    if (!channels.has_value()) {
        RX_LOG_ERROR("sample_07_stress: backbuffer format {} is not one of the R8G8B8A8/B8G8R8A8 families this "
                     "check knows how to decode",
                     static_cast<int>(targetFormat));
        gateOk = false;
    } else {
        const uint32_t probeCount = std::min<uint32_t>(kVariantCount, args.draws);
        for (uint32_t variant = 0; variant < probeCount; ++variant) {
            const glm::vec3 worldTop = topWorldPositionForInstance(variant, args.draws);
            const PixelCoord probe = projectToPixel(worldTop, scene->viewProj, kHeadlessWidth, kHeadlessHeight);
            const uint8_t* px = pixelAt(probe.x, probe.y);
            const bool matched = probeMatchesVariant(px, *channels, variant);
            RX_LOG_INFO("sample_07_stress: variant {} probe world=({:.2f},{:.2f},{:.2f}) pixel=({},{}) "
                        "channels=({},{},{},{}) matched={}",
                        variant, worldTop.x, worldTop.y, worldTop.z, probe.x, probe.y, px[0], px[1], px[2], px[3],
                        matched);
            if (!matched) {
                RX_LOG_ERROR("sample_07_stress: variant {} probe did not match its expected dominant channel(s)",
                             variant);
                gateOk = false;
            }
        }
        if (args.draws < kVariantCount) {
            RX_LOG_ERROR("sample_07_stress: --draws={} is smaller than kVariantCount={} -- not every variant has a "
                         "probe instance",
                         args.draws, kVariantCount);
            gateOk = false;
        }
    }

    vkDeviceWaitIdle(vkDevice);
    destroyScene(vkDevice, *scene);
    destroyRawResources();

    if (args.validate && context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during this run");
        gateOk = false;
    }

    if (gateOk) {
        RX_LOG_INFO("stress headless gate PASSED");
        return 0;
    }
    RX_LOG_ERROR("stress headless gate FAILED");
    return 1;
}

// --- Present mode: interactive window + per-second stdout stats ------------

int runPresent(const Args& args) {
    auto window = rx::platform::Window::create("RendererX -- 07_stress", 1280, 720, /*visible=*/true);
    if (!window.has_value()) {
        RX_LOG_ERROR("Window::create failed: no display backend available");
        return 1;
    }
    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        RX_LOG_ERROR("video driver reports no Vulkan surface extensions (e.g. dummy driver)");
        return 1;
    }
    auto context = rx::rhi::Context::create(extensions, args.validate);
    if (!context.has_value()) {
        RX_LOG_ERROR("Context::create failed");
        return 1;
    }
    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    if (surface == VK_NULL_HANDLE) {
        RX_LOG_ERROR("createVulkanSurface failed");
        return 1;
    }
    auto device = rx::rhi::Device::create(*context, surface);
    if (!device.has_value()) {
        RX_LOG_ERROR("Device::create failed");
        return 1;
    }
    const VkDevice vkDevice = device->device();

    // --vsync off, applied before any per-swapchain-image resource is built
    // -- same ordering samples/05_multipass/06_materials already establish.
    if (args.vsyncMode == rx::rhi::PresentMode::VsyncOff) {
        device->setPresentMode(args.vsyncMode);
        if (!device->recreateSwapchain(surface)) {
            RX_LOG_ERROR("Device::recreateSwapchain failed while applying --vsync off");
            return 1;
        }
    }
    RX_LOG_INFO("--present: present mode in use: {}", rx::rhi::presentModeName(device->presentMode()));

    auto allocator = rx::rhi::Allocator::create(*context, *device);
    if (!allocator.has_value()) {
        RX_LOG_ERROR("Allocator::create failed");
        return 1;
    }
    auto uploader = rx::rhi::Uploader::create(*allocator, *device);
    if (!uploader.has_value()) {
        RX_LOG_ERROR("Uploader::create failed");
        return 1;
    }
    auto frameSync = rx::rhi::FrameSync::create(vkDevice, device->graphicsQueueFamily(),
                                                 static_cast<uint32_t>(device->swapchainImages().size()));
    if (!frameSync.has_value()) {
        RX_LOG_ERROR("FrameSync::create failed");
        return 1;
    }

    auto scheduler = rx::task::Scheduler::create(args.threads);
    if (scheduler == nullptr) {
        RX_LOG_ERROR("rx::task::Scheduler::create failed");
        return 1;
    }
    RX_LOG_INFO("--present: {} worker thread(s) (--threads {})", scheduler->workerCount(),
                args.threads == 0 ? "default" : std::to_string(args.threads).c_str());

    auto executor = rx::graph::Executor::create(*device, *scheduler);
    if (executor == nullptr) {
        RX_LOG_ERROR("rx::graph::Executor::create failed");
        return 1;
    }

    auto scene =
        createScene(device->physicalDevice(), vkDevice, *allocator, *uploader, device->swapchainFormat(), args.draws);
    if (!scene.has_value()) {
        RX_LOG_ERROR("createScene failed");
        return 1;
    }
    scene->viewProj = makeCameraViewProj(args.draws, device->swapchainExtent().width, device->swapchainExtent().height);

    rx::graph::RenderGraph graph;
    declareGraph(graph, *scene, device->swapchainFormat());

    rx::graph::CompileInfo info;
    info.swapchainWidth = device->swapchainExtent().width;
    info.swapchainHeight = device->swapchainExtent().height;
    info.swapchainFormat = device->swapchainFormat();
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    graph.compile(info);
    executor->realize(graph);

    std::vector<VkImageView> swapchainViews;
    auto rebuildSwapchainViews = [&]() -> bool {
        for (VkImageView view : swapchainViews) {
            vkDestroyImageView(vkDevice, view, nullptr);
        }
        swapchainViews.clear();
        for (VkImage image : device->swapchainImages()) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = device->swapchainFormat();
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &view) != VK_SUCCESS) {
                RX_LOG_ERROR("vkCreateImageView(swapchain) failed");
                return false;
            }
            swapchainViews.push_back(view);
        }
        return true;
    };
    if (!rebuildSwapchainViews()) {
        destroyScene(vkDevice, *scene);
        return 1;
    }

    bool ok = true;
    bool running = true;
    auto statsWindowStart = std::chrono::steady_clock::now();
    uint32_t framesThisWindow = 0;
    double lastRecordMs = 0.0;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        VkFence fence = frameSync->currentFence();
        vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);

        auto acquire = device->acquireNextImage(frameSync->currentImageAvailableSemaphore());
        if (acquire.status == rx::rhi::SwapchainStatus::NeedsRecreate) {
            vkDeviceWaitIdle(vkDevice);
            if (!device->recreateSwapchain(surface) || !rebuildSwapchainViews() ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size()))) {
                ok = false;
                break;
            }
            executor->realize(graph);
            scene->viewProj =
                makeCameraViewProj(args.draws, device->swapchainExtent().width, device->swapchainExtent().height);
            continue;
        }
        if (acquire.status == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("device lost during acquireNextImage; exiting present loop");
            ok = false;
            break;
        }

        vkResetFences(vkDevice, 1, &fence);
        VkCommandBuffer cmd = frameSync->currentCommandBuffer();
        vkResetCommandPool(vkDevice, frameSync->currentCommandPool(), 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        scene->drawsSubmitted->store(0, std::memory_order_relaxed);
        const auto recordStart = std::chrono::steady_clock::now();
        executor->execute(graph, cmd, device->swapchainImages()[acquire.imageIndex],
                           swapchainViews[acquire.imageIndex], device->swapchainExtent());
        const auto recordEnd = std::chrono::steady_clock::now();
        lastRecordMs = std::chrono::duration<double, std::milli>(recordEnd - recordStart).count();

        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        VkSemaphore waitSem = frameSync->currentImageAvailableSemaphore();
        submitInfo.pWaitSemaphores = &waitSem;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        VkSemaphore signalSem = frameSync->renderFinishedSemaphore(acquire.imageIndex);
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSem;
        if (vkQueueSubmit(device->graphicsQueue(), 1, &submitInfo, fence) != VK_SUCCESS) {
            ok = false;
            break;
        }

        auto presentStatus = device->present(acquire.imageIndex, signalSem);
        if (presentStatus == rx::rhi::SwapchainStatus::NeedsRecreate) {
            vkDeviceWaitIdle(vkDevice);
            if (!device->recreateSwapchain(surface) || !rebuildSwapchainViews() ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size()))) {
                ok = false;
                break;
            }
            executor->realize(graph);
            scene->viewProj =
                makeCameraViewProj(args.draws, device->swapchainExtent().width, device->swapchainExtent().height);
        } else if (presentStatus == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("device lost during present; exiting present loop");
            ok = false;
            break;
        }

        frameSync->advanceFrame();
        RX_FRAME_MARK;

        // Per-second stdout stats [task-7-brief.md: "per-second stdout
        // stats (fps, cpu-record ms, draws)"] -- fps over the window just
        // elapsed; cpu-record ms/draws from the MOST RECENT frame (not
        // averaged) -- a simple, always-available measurement independent
        // of whether a Tracy profiler is connected.
        ++framesThisWindow;
        const auto now = std::chrono::steady_clock::now();
        const double windowSeconds = std::chrono::duration<double>(now - statsWindowStart).count();
        if (windowSeconds >= 1.0) {
            const double fps = static_cast<double>(framesThisWindow) / windowSeconds;
            RX_LOG_INFO("stress: fps={:.1f} cpu_record_ms={:.3f} draws={}", fps, lastRecordMs,
                        scene->drawsSubmitted->load(std::memory_order_relaxed));
            framesThisWindow = 0;
            statsWindowStart = now;
        }
    }

    vkDeviceWaitIdle(vkDevice);
    for (VkImageView view : swapchainViews) {
        vkDestroyImageView(vkDevice, view, nullptr);
    }
    destroyScene(vkDevice, *scene);

    if (args.validate && context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during the present loop");
        return 1;
    }
    if (!ok) {
        return 1;
    }
    RX_LOG_INFO("--present: window closed cleanly");
    return 0;
}

int main(int argc, char** argv) {
    rx::core::log::init();

    auto args = parseArgs(argc, argv);
    if (!args.has_value()) {
        return 1;
    }

    if (args->present) {
        return runPresent(*args);
    }
    return runHeadless(*args);
}
