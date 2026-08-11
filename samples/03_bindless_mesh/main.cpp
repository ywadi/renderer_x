// Sample 03: bindless mesh.
//
// The Phase 2 integration proof: reflection-driven pipeline layouts,
// bindless resource management, and the real upload path, all working
// together in one scene. Procedural geometry (cube, sphere, plane --
// generated in code below, no importer) textured with 4 procedurally
// generated images plus one real PNG decoded via stb_image, every texture
// uploaded through rx::rhi::Uploader into one rx::rhi::BindlessTable (set
// 0). The pipeline's descriptor set layout and push-constant range come
// entirely from rx::shader::reflect() on the shader source embedded below
// (kShaderSource) -- there is no hand-typed VkDescriptorSetLayoutBinding
// anywhere in this file for set 0; that is the entire point of this
// sample (Task 6 brief).
//
// SET-0 SUBSTITUTION: BindlessTable's real descriptor set layout (built
// from caller capacities, VARIABLE_DESCRIPTOR_COUNT on its last binding)
// and a from-scratch layout PipelineLayoutBuilder would otherwise invent
// for the same reflected shape are NOT compatible Vulkan pipeline layouts
// "by construction" (Task 3's review finding) -- this sample is what
// exercises the fix: rx::rhi::PipelineLayoutBuilder::build()'s new
// `externalSet0` parameter (rx_rhi_vk/pipeline_layout.h), passed
// `bindlessTable.descriptorSetLayout()` directly, below.
//
// PUSH CONSTANTS (12 bytes, well under the 128-byte budget):
//   { uint32 transformIndex; uint32 textureIndex; uint32 samplerIndex; }
// All three are UNIFORM across a single draw call (every vertex/fragment
// invocation within one vkCmdDrawIndexed call reads the exact same push-
// constant bytes) -- never wrapped in NonUniformResourceIndex() in the
// shader below [spec Fixed decision #6, R:B3/E4]. `transformIndex` selects
// a row inside ONE bindless storage buffer holding every object's
// precomputed MVP for the frame currently in flight (see "Double-buffered
// transforms" below); `textureIndex`/`samplerIndex` select this object's
// sampled image / sampler directly.
//
// DOUBLE-BUFFERED TRANSFORMS, NO RE-REGISTRATION: rather than reallocating
// a fresh bindless storage-buffer slot every frame (churning the
// descriptor set) or racing a single-buffered transform buffer against an
// in-flight frame still reading it (FrameSync keeps 2 frames in flight),
// this sample allocates ONE storage buffer sized for
// `FrameSync::framesInFlight() * kObjectCount` mat4 rows, registers it
// ONCE into the bindless table, and every frame writes only the row range
// belonging to the frame-in-flight slot currently being recorded --
// `transformIndex = frameSync.currentFrameIndex() * kObjectCount +
// objectIndex`. That row range was last read by a draw using this exact
// slot, and this sample's present loop (mirroring 01_triangle/
// 02_hotreload) already waits on that slot's own fence before reaching
// this point in the loop -- so the row range is provably idle before being
// overwritten, with no extra synchronization needed beyond what the
// standard frames-in-flight loop already provides. (Headless mode only
// ever uses frame-in-flight slot 0's rows.)
//
// Depth buffer: rx::rhi::Texture2D, VK_FORMAT_D32_SFLOAT, mipLevels=1, no
// upload (never touched by Uploader -- created, transitioned once, used
// as a depth attachment only) [Task 6 brief: "Depth buffer via
// Texture2D... create with mipLevels=1, no upload"].
//
// Two modes, exactly like 01_triangle/02_hotreload:
//   (default / no args) Headless correctness gate: one frame, offscreen,
//   256x256, camera at its orbit's t=0 pose (a fixed, well-known front
//   view -- see kCamera* below), readback + probe pixels asserting >=3
//   distinct texture colors actually landed on screen, validation clean,
//   exit 0/1. Registered as ctest sample_03_bindless_mesh_headless.
//   --present. Real window, the same scene, camera continuously orbiting
//   (see runPresent()'s use of elapsed time to drive the orbit angle the
//   headless gate always evaluates at t=0).
#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_rhi_vk/mesh_buffers.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_rhi_vk/texture.h>
#include <rx_rhi_vk/upload.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>

// GLM is already a PUBLIC dependency of rx_core (see
// src/rx_core/CMakeLists.txt), transitively available here through
// rx_rhi_vk/rx_platform -- no separate link needed. GLM_FORCE_DEPTH_ZERO_TO_ONE
// makes glm::perspective() emit Vulkan's [0,1] clip-space depth range
// instead of OpenGL's default [-1,1]; must be defined before the first GLM
// include in any TU that calls glm::perspective(), which is only this one.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <stb_image.h>

#include <SDL3/SDL.h>
#include <volk.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kHeadlessWidth = 256;
constexpr uint32_t kHeadlessHeight = 256;
constexpr VkDeviceSize kHeadlessPixelBytes = static_cast<VkDeviceSize>(kHeadlessWidth) * kHeadlessHeight * 4;

// --present mode's window size -- arbitrary, viewport/scissor are dynamic
// state (same reasoning as 01_triangle/02_hotreload).
constexpr uint32_t kPresentWidth = 900;
constexpr uint32_t kPresentHeight = 700;

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;

constexpr const char* kTextureFilename = "texture.png";

// --- Scene layout -----------------------------------------------------
//
// 5 objects along the X axis (2 cubes, 2 spheres, 1 plane), one texture
// each -- every one of the 4 procedurally generated textures plus the one
// real decoded PNG gets used by exactly one object, so ">=3 distinct
// texture samples" (the headless gate's own requirement) is trivially
// satisfied with margin to spare.
enum class Shape : uint32_t { Cube, Sphere, Plane };

struct SceneObject {
    Shape shape;
    float x;
    float spinRadPerSec;
    uint32_t textureIndex;
    uint32_t samplerIndex;
};

constexpr float kObjectSpacing = 4.0F;
constexpr std::array<SceneObject, 5> kObjects{{
    {Shape::Cube, -2.0F * kObjectSpacing, 0.6F, 0, 0},
    {Shape::Cube, -1.0F * kObjectSpacing, -0.4F, 1, 1},
    {Shape::Sphere, 0.0F, 0.5F, 2, 0},
    {Shape::Sphere, 1.0F * kObjectSpacing, -0.5F, 3, 1},
    {Shape::Plane, 2.0F * kObjectSpacing, 0.0F, 4, 0},
}};
constexpr uint32_t kObjectCount = static_cast<uint32_t>(kObjects.size());

// Camera: orbits the origin in the XZ plane at a fixed radius/height,
// always looking at the origin. At t=0 (angle=0) the camera sits on the
// +Z axis -- a fixed, well-known front view the headless gate always
// renders (see runHeadless()). --present mode advances `angle` with
// elapsed time (see runPresent()) -- this is the "camera orbits in
// present mode" the brief asks for; headless mode is just this same orbit
// function evaluated at time=0, not a separate camera path.
constexpr float kCameraRadius = 26.0F;
constexpr float kCameraHeight = 5.0F;
constexpr float kCameraOrbitRadPerSec = 0.35F;
constexpr float kCameraFovYRadians = 0.8F;  // ~46 degrees
constexpr float kCameraNear = 0.1F;
constexpr float kCameraFar = 200.0F;

glm::vec3 cameraEyeAtAngle(float angle) {
    return glm::vec3(kCameraRadius * std::sin(angle), kCameraHeight, kCameraRadius * std::cos(angle));
}

// Vulkan clip space's Y axis points down the screen; GLM's glm::perspective
// (even with GLM_FORCE_DEPTH_ZERO_TO_ONE) still assumes an OpenGL-style
// Y-up NDC -- flipping proj[1][1] is the standard, widely used fix (see
// e.g. Sascha Willems' Vulkan samples) for using GLM's projection helpers
// directly with Vulkan.
glm::mat4 makeProjection(float aspect) {
    glm::mat4 proj = glm::perspective(kCameraFovYRadians, aspect, kCameraNear, kCameraFar);
    proj[1][1] *= -1.0F;
    return proj;
}

// --- Shader source (compiled at RUNTIME via rx_shader::Compiler, per the
// Task 6 brief -- reflection must walk the actual shader driving this
// sample's layouts, which requires the runtime path, not a slangc
// build-time precompile like 01_triangle's shaders) -------------------
//
// Set 0's three bindings deliberately match rx::rhi::BindlessTable's own
// fixed scheme exactly (kSampledImageBinding=0/kSamplerBinding=1/
// kStorageBufferBinding=2, each an unbounded runtime array) -- this is
// what makes the externalSet0 substitution below valid: reflect() must
// report this exact shape for PipelineLayoutBuilder::build()'s shape
// check to accept rx::rhi::BindlessTable::descriptorSetLayout() as a
// substitute for a from-scratch set-0 layout.
constexpr const char* kShaderSource = R"(
struct PushConstants {
    uint transformIndex;
    uint textureIndex;
    uint samplerIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 0)]]
Texture2D gTextures[];

[[vk::binding(1, 0)]]
SamplerState gSamplers[];

[[vk::binding(2, 0)]]
StructuredBuffer<float4x4> gTransforms[];

struct VSIn {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(VSIn input)
{
    // gTransforms[0]: this sample only ever registers ONE bindless
    // storage buffer (the shared, double-buffered-by-row transform
    // table) -- a compile-time-constant descriptor-array index, uniform
    // by construction, never NonUniformResourceIndex-wrapped.
    // gPush.transformIndex: uniform across this whole draw call (every
    // invocation reading the same push-constant bytes) -- selects a ROW
    // inside that one buffer, not a different descriptor.
    float4x4 mvp = gTransforms[0][gPush.transformIndex];
    VSOut o;
    o.position = mul(mvp, float4(input.position, 1.0));
    o.uv = input.uv;
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    return gTextures[gPush.textureIndex].Sample(gSamplers[gPush.samplerIndex], input.uv);
}
)";

const std::vector<std::string> kEntryPointNames = {"vsMain", "fsMain"};

// --- Procedural geometry -------------------------------------------------

struct Vertex {
    float position[3];
    float uv[2];
};

struct HostMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

void addQuad(HostMesh& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
    uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(Vertex{{a.x, a.y, a.z}, {0.0F, 0.0F}});
    mesh.vertices.push_back(Vertex{{b.x, b.y, b.z}, {1.0F, 0.0F}});
    mesh.vertices.push_back(Vertex{{c.x, c.y, c.z}, {1.0F, 1.0F}});
    mesh.vertices.push_back(Vertex{{d.x, d.y, d.z}, {0.0F, 1.0F}});
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 3);
}

// Half-extent `h` cube, 24 vertices (4 per face -- each face needs its own
// UV corners, so faces cannot share corner vertices), 36 indices.
HostMesh generateCube(float h) {
    HostMesh mesh;
    addQuad(mesh, {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h});         // +X
    addQuad(mesh, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h});     // -X
    addQuad(mesh, {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h});         // +Y
    addQuad(mesh, {-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h});     // -Y
    addQuad(mesh, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h});         // +Z
    addQuad(mesh, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h});     // -Z
    return mesh;
}

// Standard UV sphere, radius `radius`, `rings` latitude bands x `segments`
// longitude bands.
HostMesh generateSphere(float radius, uint32_t rings, uint32_t segments) {
    HostMesh mesh;
    constexpr float kPi = 3.14159265358979323846F;

    for (uint32_t y = 0; y <= rings; ++y) {
        float v = static_cast<float>(y) / static_cast<float>(rings);
        float theta = v * kPi;
        float sinTheta = std::sin(theta);
        float cosTheta = std::cos(theta);
        for (uint32_t x = 0; x <= segments; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(segments);
            float phi = u * 2.0F * kPi;
            float sinPhi = std::sin(phi);
            float cosPhi = std::cos(phi);
            Vertex vert{};
            vert.position[0] = radius * sinTheta * cosPhi;
            vert.position[1] = radius * cosTheta;
            vert.position[2] = radius * sinTheta * sinPhi;
            vert.uv[0] = u;
            vert.uv[1] = v;
            mesh.vertices.push_back(vert);
        }
    }

    uint32_t stride = segments + 1;
    for (uint32_t y = 0; y < rings; ++y) {
        for (uint32_t x = 0; x < segments; ++x) {
            uint32_t i0 = y * stride + x;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + stride;
            uint32_t i3 = i2 + 1;
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

// A single quad in the XY plane (facing +Z), half-size `halfSize`.
HostMesh generatePlane(float halfSize) {
    HostMesh mesh;
    addQuad(mesh, {-halfSize, -halfSize, 0.0F}, {halfSize, -halfSize, 0.0F}, {halfSize, halfSize, 0.0F},
            {-halfSize, halfSize, 0.0F});
    return mesh;
}

// --- Procedural textures -------------------------------------------------

std::vector<uint8_t> makeCheckerboard(uint32_t size, uint32_t cellSize, std::array<uint8_t, 3> colorA,
                                       std::array<uint8_t, 3> colorB) {
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            bool useA = ((x / cellSize) + (y / cellSize)) % 2 == 0;
            const auto& c = useA ? colorA : colorB;
            size_t i = (static_cast<size_t>(y) * size + x) * 4;
            pixels[i + 0] = c[0];
            pixels[i + 1] = c[1];
            pixels[i + 2] = c[2];
            pixels[i + 3] = 255;
        }
    }
    return pixels;
}

std::vector<uint8_t> makeGradient(uint32_t size, std::array<uint8_t, 3> colorA, std::array<uint8_t, 3> colorB,
                                   bool diagonal) {
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float t = diagonal ? (static_cast<float>(x + y) / static_cast<float>(2 * (size - 1)))
                                : (static_cast<float>(x) / static_cast<float>(size - 1));
            size_t i = (static_cast<size_t>(y) * size + x) * 4;
            for (int c = 0; c < 3; ++c) {
                float value = static_cast<float>(colorA[c]) * (1.0F - t) + static_cast<float>(colorB[c]) * t;
                pixels[i + static_cast<size_t>(c)] = static_cast<uint8_t>(value + 0.5F);
            }
            pixels[i + 3] = 255;
        }
    }
    return pixels;
}

// --- GPU-side scene -------------------------------------------------------

// `MeshBuffers` has no default constructor at all (only ever produced by
// its own `create()` factory) -- wrapping it in `std::optional` is what
// lets `GpuMesh`, and in turn `std::array<GpuMesh, 3>` (Scene::meshes,
// below), stay default-constructible without needing a real MeshBuffers
// up front.
struct GpuMesh {
    std::optional<rx::rhi::MeshBuffers> buffers;
};

// Everything the bindless-mesh scene needs beyond the raw RHI stack:
// BindlessTable, the 5 uploaded textures + 2 samplers each registered into
// it, the one double-buffered transform storage buffer (also registered
// into it, see this file's header comment), the 3 mesh types' GPU buffers,
// the reflection-driven pipeline, and the depth buffer. Move-only by
// construction (every member is itself move-only RAII); built once by
// createScene() below and torn down by simply going out of scope, in
// reverse declaration order, before the owning Device/Context/Allocator
// do -- same discipline every other sample in this codebase follows.
// `Buffer` also has no PUBLICLY accessible default constructor (private,
// friend-of-Allocator-only) -- same reasoning as GpuMesh::buffers above,
// `transformBuffer` is `std::optional<Buffer>` so Scene stays a plain
// aggregate constructible via `Scene scene{std::move(*bindlessTable)};`
// (only `bindlessTable`, the first member, needs an explicit initializer;
// every other member below is default-constructible on its own: vector,
// optional, and trivial handle/scalar types).
struct Scene {
    rx::rhi::BindlessTable bindlessTable;
    std::vector<rx::rhi::Texture2D> textures;
    std::array<VkSampler, 2> samplers{VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::optional<rx::rhi::Buffer> transformBuffer;
    std::array<GpuMesh, 3> meshes;  // indexed by Shape

    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t pushConstantOffset = 0;
    uint32_t pushConstantSize = 0;
    VkShaderStageFlags pushConstantStages = 0;
};

void destroyScenePipeline(VkDevice device, Scene& scene) {
    if (scene.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, scene.pipeline, nullptr);
        scene.pipeline = VK_NULL_HANDLE;
    }
    if (scene.fragModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, scene.fragModule, nullptr);
        scene.fragModule = VK_NULL_HANDLE;
    }
    if (scene.vertModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, scene.vertModule, nullptr);
        scene.vertModule = VK_NULL_HANDLE;
    }
    scene.layoutBundle = rx::rhi::PipelineLayoutBundle{};
}

void destroyScene(VkDevice device, Scene& scene) {
    destroyScenePipeline(device, scene);
    for (VkSampler sampler : scene.samplers) {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler, nullptr);
        }
    }
    scene.samplers.fill(VK_NULL_HANDLE);
}

// Builds the VkPipelineVertexInputStateCreateInfo-adjacent state (binding +
// attribute descriptions) for `Vertex` -- shared by every mesh type in this
// sample (they all share one vertex layout).
struct VertexInputState {
    VkVertexInputBindingDescription binding{};
    std::array<VkVertexInputAttributeDescription, 2> attributes{};
};

VertexInputState makeVertexInputState() {
    VertexInputState state;
    state.binding.binding = 0;
    state.binding.stride = sizeof(Vertex);
    state.binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    state.attributes[0].location = 0;
    state.attributes[0].binding = 0;
    state.attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    state.attributes[0].offset = offsetof(Vertex, position);

    state.attributes[1].location = 1;
    state.attributes[1].binding = 0;
    state.attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    state.attributes[1].offset = offsetof(Vertex, uv);
    return state;
}

// Compiles kShaderSource, reflects it, and builds the pipeline layout +
// VkPipeline -- the reflection-driven core of this sample. Returns false
// (logged) on any failure; on success, fills in scene.vertModule/
// fragModule/layoutBundle/pipeline/pushConstant* (any previous pipeline
// state in `scene` is destroyed first).
bool buildScenePipeline(VkDevice device, rx::rhi::BindlessTable& bindlessTable, VkFormat colorFormat, Scene& scene) {
    destroyScenePipeline(device, scene);

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        RX_LOG_ERROR("sample_03_bindless_mesh: rx::shader::Compiler::create failed");
        return false;
    }

    rx::shader::CompileResult compileResult =
        compiler->compileFromSource("BindlessMeshModule", kShaderSource, kEntryPointNames);
    if (!compileResult.ok) {
        RX_LOG_ERROR("sample_03_bindless_mesh: shader compile failed:\n{}", compileResult.diagnostics);
        return false;
    }

    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("sample_03_bindless_mesh: reflect() failed on an otherwise-successful compile");
        return false;
    }
    if (layoutInfo->bindings.size() != 3) {
        RX_LOG_ERROR(
            "sample_03_bindless_mesh: shader reflects {} set-0 bindings; expected exactly 3 (images/samplers/"
            "storage buffers, matching BindlessTable's fixed scheme)",
            layoutInfo->bindings.size());
        return false;
    }
    if (layoutInfo->pushRanges.size() != 1) {
        RX_LOG_ERROR("sample_03_bindless_mesh: shader reflects {} push-constant ranges; expected exactly 1",
                     layoutInfo->pushRanges.size());
        return false;
    }

    // THE substitution this whole sample exists to prove: set 0 becomes
    // the real BindlessTable's own layout, not a lookalike this builder
    // would otherwise invent from `layoutInfo` alone -- see
    // pipeline_layout.h's comment on build() and this file's header
    // comment for the full rationale.
    auto layoutBundle =
        rx::rhi::PipelineLayoutBuilder::build(device, *layoutInfo, bindlessTable.descriptorSetLayout());
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("sample_03_bindless_mesh: PipelineLayoutBuilder::build failed (externalSet0 shape mismatch?)");
        return false;
    }

    scene.layoutBundle = std::move(*layoutBundle);
    scene.pushConstantOffset = layoutInfo->pushRanges[0].offset;
    scene.pushConstantSize = layoutInfo->pushRanges[0].size;
    scene.pushConstantStages = layoutInfo->pushRanges[0].stages;

    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = blob.code.size() * sizeof(uint32_t);
        moduleInfo.pCode = blob.code.data();

        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            RX_LOG_ERROR("sample_03_bindless_mesh: vkCreateShaderModule failed for entry point '{}'",
                         blob.entryPointName);
            destroyScenePipeline(device, scene);
            return false;
        }
        if (blob.entryPointName == "vsMain") {
            scene.vertModule = module;
        } else if (blob.entryPointName == "fsMain") {
            scene.fragModule = module;
        } else {
            RX_LOG_ERROR("sample_03_bindless_mesh: unexpected entry point '{}'", blob.entryPointName);
            vkDestroyShaderModule(device, module, nullptr);
            destroyScenePipeline(device, scene);
            return false;
        }
    }
    if (scene.vertModule == VK_NULL_HANDLE || scene.fragModule == VK_NULL_HANDLE) {
        RX_LOG_ERROR("sample_03_bindless_mesh: missing vsMain/fsMain shader module after a successful compile");
        destroyScenePipeline(device, scene);
        return false;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = scene.vertModule;
    // Slang's SPIR-V backend always names each entry point's OpEntryPoint
    // "main" regardless of the source-level function name -- see
    // 02_hotreload/main.cpp's comment on this exact point.
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = scene.fragModule;
    stages[1].pName = "main";

    VertexInputState vertexInput = makeVertexInputState();
    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount = 1;
    vertexInputState.pVertexBindingDescriptions = &vertexInput.binding;
    vertexInputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInput.attributes.size());
    vertexInputState.pVertexAttributeDescriptions = vertexInput.attributes.data();

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
    // No culling: this sample's procedural geometry generators do not
    // guarantee a single consistent outward winding order across every
    // face (cube/sphere/plane each walk their own perimeter independently,
    // see generateCube()/generateSphere()/generatePlane() above), and
    // nothing in this shader depends on face orientation (no lighting) --
    // culling would only risk silently dropping real geometry for no
    // benefit here.
    rasterizationState.cullMode = VK_CULL_MODE_NONE;
    rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationState.lineWidth = 1.0F;

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
    renderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    renderingCreateInfo.depthAttachmentFormat = kDepthFormat;

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
    pipelineInfo.layout = scene.layoutBundle.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;  // dynamic rendering
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &scene.pipeline) !=
        VK_SUCCESS) {
        RX_LOG_ERROR("sample_03_bindless_mesh: vkCreateGraphicsPipelines failed");
        destroyScenePipeline(device, scene);
        return false;
    }

    return true;
}

// Resolves texture.png's path next to this executable at runtime (same
// SDL_GetBasePath() mechanism 02_hotreload's resolveShaderPath() uses for
// hotreload.slang), so a redistributed copy of this sample's build-output
// directory works identically outside the build tree.
std::string resolveTexturePath() {
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        RX_LOG_WARN(
            "sample_03_bindless_mesh: SDL_GetBasePath failed ({}); looking for {} in the current directory instead",
            SDL_GetError(), kTextureFilename);
        return kTextureFilename;
    }
    return std::string(basePath) + kTextureFilename;
}

// Builds the entire scene: BindlessTable, 3 mesh types uploaded via
// `uploader`, 5 textures (4 procedural + 1 decoded PNG) uploaded via
// `uploader` and registered into the table, 2 samplers registered into the
// table, the double-buffered transform storage buffer registered into the
// table, and the reflection-driven pipeline. `uploader.flush()` is called
// once at the end -- every upload above is batched into as few
// submissions as this Uploader's ring buffer allows, per its own
// documented batching behavior.
std::optional<Scene> createScene(VkPhysicalDevice physicalDevice, VkDevice device, rx::rhi::Allocator& allocator,
                                  rx::rhi::Uploader& uploader, VkFormat colorFormat) {
    // Capacities sized generously above this scene's real needs (5
    // images, 2 samplers, 1 storage buffer) -- BindlessTable rejects a
    // capacity of 0 outright, and small power-of-two headroom costs
    // nothing for a sample this size.
    rx::rhi::BindlessTable::Capacities capacities{/*sampledImages=*/32, /*samplers=*/8, /*storageBuffers=*/8};
    auto bindlessTable = rx::rhi::BindlessTable::create(physicalDevice, device, capacities);
    if (!bindlessTable.has_value()) {
        RX_LOG_ERROR("sample_03_bindless_mesh: BindlessTable::create failed");
        return std::nullopt;
    }

    Scene scene{std::move(*bindlessTable)};

    // --- Meshes -----------------------------------------------------
    HostMesh cubeHost = generateCube(1.0F);
    HostMesh sphereHost = generateSphere(1.1F, /*rings=*/16, /*segments=*/24);
    HostMesh planeHost = generatePlane(1.3F);

    auto uploadMesh = [&](const HostMesh& host) -> std::optional<GpuMesh> {
        auto buffers = rx::rhi::MeshBuffers::create(
            allocator, uploader, host.vertices.data(), host.vertices.size() * sizeof(Vertex), host.indices.data(),
            host.indices.size() * sizeof(uint32_t), static_cast<uint32_t>(host.indices.size()),
            VK_INDEX_TYPE_UINT32);
        if (!buffers.has_value()) {
            return std::nullopt;
        }
        return GpuMesh{std::move(*buffers)};
    };

    auto cubeMesh = uploadMesh(cubeHost);
    auto sphereMesh = uploadMesh(sphereHost);
    auto planeMesh = uploadMesh(planeHost);
    if (!cubeMesh.has_value() || !sphereMesh.has_value() || !planeMesh.has_value()) {
        RX_LOG_ERROR("sample_03_bindless_mesh: MeshBuffers::create failed for one or more shapes");
        return std::nullopt;
    }
    scene.meshes[static_cast<size_t>(Shape::Cube)] = std::move(*cubeMesh);
    scene.meshes[static_cast<size_t>(Shape::Sphere)] = std::move(*sphereMesh);
    scene.meshes[static_cast<size_t>(Shape::Plane)] = std::move(*planeMesh);

    // --- Textures: 4 procedural + 1 real decoded PNG -----------------
    constexpr uint32_t kProceduralSize = 64;
    std::vector<std::vector<uint8_t>> pixelSources;
    pixelSources.push_back(makeCheckerboard(kProceduralSize, 8, {220, 30, 30}, {250, 250, 250}));
    pixelSources.push_back(makeGradient(kProceduralSize, {30, 60, 220}, {30, 200, 90}, /*diagonal=*/false));
    pixelSources.push_back(makeCheckerboard(kProceduralSize, 16, {230, 210, 20}, {20, 40, 120}));
    pixelSources.push_back(makeGradient(kProceduralSize, {200, 20, 180}, {20, 200, 210}, /*diagonal=*/true));

    std::string texturePath = resolveTexturePath();
    int pngWidth = 0;
    int pngHeight = 0;
    int pngChannels = 0;
    stbi_uc* pngPixels = stbi_load(texturePath.c_str(), &pngWidth, &pngHeight, &pngChannels, /*desired_channels=*/4);
    if (pngPixels == nullptr) {
        RX_LOG_ERROR("sample_03_bindless_mesh: stbi_load('{}') failed: {}", texturePath, stbi_failure_reason());
        return std::nullopt;
    }
    std::vector<uint8_t> pngPixelData(pngPixels, pngPixels + (static_cast<size_t>(pngWidth) * pngHeight * 4));
    stbi_image_free(pngPixels);

    for (size_t i = 0; i < pixelSources.size(); ++i) {
        auto texture = rx::rhi::Texture2D::create(physicalDevice, device, allocator,
                                                    VkExtent2D{kProceduralSize, kProceduralSize}, kTextureFormat,
                                                    VK_IMAGE_USAGE_SAMPLED_BIT, /*requestedMipLevels=*/0);
        if (!texture.has_value()) {
            RX_LOG_ERROR("sample_03_bindless_mesh: Texture2D::create failed for procedural texture {}", i);
            return std::nullopt;
        }
        if (!uploader.uploadToImage(*texture, pixelSources[i].data(), pixelSources[i].size(), /*generateMips=*/true)) {
            RX_LOG_ERROR("sample_03_bindless_mesh: uploadToImage failed for procedural texture {}", i);
            return std::nullopt;
        }
        scene.textures.push_back(std::move(*texture));
    }

    auto pngTexture =
        rx::rhi::Texture2D::create(physicalDevice, device, allocator,
                                    VkExtent2D{static_cast<uint32_t>(pngWidth), static_cast<uint32_t>(pngHeight)},
                                    kTextureFormat, VK_IMAGE_USAGE_SAMPLED_BIT, /*requestedMipLevels=*/0);
    if (!pngTexture.has_value()) {
        RX_LOG_ERROR("sample_03_bindless_mesh: Texture2D::create failed for the decoded PNG");
        return std::nullopt;
    }
    if (!uploader.uploadToImage(*pngTexture, pngPixelData.data(), pngPixelData.size(), /*generateMips=*/true)) {
        RX_LOG_ERROR("sample_03_bindless_mesh: uploadToImage failed for the decoded PNG");
        return std::nullopt;
    }
    scene.textures.push_back(std::move(*pngTexture));

    uploader.flush();  // every mesh/texture upload above is now on the GPU.

    for (auto& texture : scene.textures) {
        auto handle = scene.bindlessTable.registerSampledImage(texture.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!handle.isValid()) {
            RX_LOG_ERROR("sample_03_bindless_mesh: BindlessTable::registerSampledImage failed");
            return std::nullopt;
        }
    }

    // --- Samplers: linear+repeat, nearest+clamp ----------------------
    VkSamplerCreateInfo linearInfo{};
    linearInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    linearInfo.magFilter = VK_FILTER_LINEAR;
    linearInfo.minFilter = VK_FILTER_LINEAR;
    linearInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    linearInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    linearInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    linearInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    linearInfo.maxLod = VK_LOD_CLAMP_NONE;

    VkSamplerCreateInfo nearestInfo{};
    nearestInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    nearestInfo.magFilter = VK_FILTER_NEAREST;
    nearestInfo.minFilter = VK_FILTER_NEAREST;
    nearestInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    nearestInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    nearestInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    nearestInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    nearestInfo.maxLod = 0.0F;

    if (vkCreateSampler(device, &linearInfo, nullptr, &scene.samplers[0]) != VK_SUCCESS ||
        vkCreateSampler(device, &nearestInfo, nullptr, &scene.samplers[1]) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_03_bindless_mesh: vkCreateSampler failed");
        return std::nullopt;
    }
    for (VkSampler sampler : scene.samplers) {
        auto handle = scene.bindlessTable.registerSampler(sampler);
        if (!handle.isValid()) {
            RX_LOG_ERROR("sample_03_bindless_mesh: BindlessTable::registerSampler failed");
            return std::nullopt;
        }
    }

    // --- Transform storage buffer: framesInFlight() * kObjectCount mat4
    // rows, registered ONCE (see this file's header comment for why no
    // per-frame re-registration is needed).
    VkDeviceSize transformBufferSize =
        static_cast<VkDeviceSize>(rx::rhi::FrameSync::framesInFlight()) * kObjectCount * sizeof(glm::mat4);
    auto transformBuffer = allocator.createDeviceLocalBuffer(
        transformBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!transformBuffer.has_value()) {
        RX_LOG_ERROR("sample_03_bindless_mesh: createDeviceLocalBuffer failed for the transform buffer");
        return std::nullopt;
    }
    scene.transformBuffer = std::move(transformBuffer);
    auto transformHandle =
        scene.bindlessTable.registerStorageBuffer(scene.transformBuffer->handle(), transformBufferSize);
    if (!transformHandle.isValid()) {
        RX_LOG_ERROR("sample_03_bindless_mesh: BindlessTable::registerStorageBuffer failed");
        return std::nullopt;
    }

    // --- Depth buffer: Texture2D, mipLevels=1, never uploaded to -----
    // Extent set to the color target's initial size by the caller via a
    // deferred create -- see createDepthBuffer() below, called separately
    // by both run modes (headless once; present once at startup and again
    // on every swapchain resize).

    // --- Reflection-driven pipeline -----------------------------------
    if (!buildScenePipeline(device, scene.bindlessTable, colorFormat, scene)) {
        return std::nullopt;
    }

    return scene;
}

std::optional<rx::rhi::Texture2D> createDepthBuffer(VkPhysicalDevice physicalDevice, VkDevice device,
                                                     rx::rhi::Allocator& allocator, VkExtent2D extent) {
    return rx::rhi::Texture2D::create(physicalDevice, device, allocator, extent, kDepthFormat,
                                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, /*requestedMipLevels=*/1);
}

// Writes this frame's `frameInFlightIndex`'s row range of the transform
// buffer with every object's MVP for camera angle `cameraAngle` (radians)
// and per-object spin time `spinSeconds`, then flushes -- see this file's
// header comment for why writing only this slot's own row range, after
// its own fence has already been waited on by the caller, needs no
// further synchronization.
void updateTransforms(rx::rhi::Uploader& uploader, rx::rhi::Buffer& transformBuffer, uint32_t frameInFlightIndex,
                       float cameraAngle, float spinSeconds, float aspect) {
    glm::mat4 proj = makeProjection(aspect);
    glm::mat4 view = glm::lookAt(cameraEyeAtAngle(cameraAngle), glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    glm::mat4 viewProj = proj * view;

    std::array<glm::mat4, kObjectCount> mvps{};
    for (uint32_t i = 0; i < kObjectCount; ++i) {
        const SceneObject& obj = kObjects[i];
        glm::mat4 model = glm::translate(glm::mat4(1.0F), glm::vec3(obj.x, 0.0F, 0.0F));
        model = glm::rotate(model, obj.spinRadPerSec * spinSeconds, glm::vec3(0.0F, 1.0F, 0.0F));
        // Transposed before upload: GLM stores mat4 column-major, but
        // Slang's default `float4x4` element read out of a
        // StructuredBuffer<float4x4> (no explicit row_major/column_major
        // qualifier) interprets the same 16 raw floats as ROW-major --
        // verified empirically (a debug image dump without this transpose
        // showed severely distorted, near-degenerate geometry, the classic
        // symptom of a transposed 4x4 transform reaching a perspective
        // divide). Transposing here, once, on the host, is simpler and
        // cheaper than fighting Slang's packing-qualifier syntax for a
        // storage buffer element type.
        mvps[i] = glm::transpose(viewProj * model);
    }

    VkDeviceSize rowBytes = sizeof(glm::mat4);
    VkDeviceSize dstOffset = static_cast<VkDeviceSize>(frameInFlightIndex) * kObjectCount * rowBytes;
    uploader.uploadToBuffer(transformBuffer, dstOffset, mvps.data(), kObjectCount * rowBytes);
    uploader.flush();
}

// Records the draws for every object in `kObjects` into `cmd`, which must
// already be inside a vkCmdBeginRendering scope with `scene.pipeline`'s
// viewport/scissor already set via the caller. `frameInFlightIndex`
// selects which row range of the transform buffer this frame's push
// constants index into (see this file's header comment).
void recordSceneDraws(VkCommandBuffer cmd, const Scene& scene, uint32_t frameInFlightIndex) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.pipeline);

    VkDescriptorSet set = scene.bindlessTable.descriptorSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.layoutBundle.layout, 0, 1, &set, 0, nullptr);

    for (uint32_t i = 0; i < kObjectCount; ++i) {
        const SceneObject& obj = kObjects[i];
        const GpuMesh& mesh = scene.meshes[static_cast<size_t>(obj.shape)];

        struct PushConstants {
            uint32_t transformIndex;
            uint32_t textureIndex;
            uint32_t samplerIndex;
        } push{frameInFlightIndex * kObjectCount + i, obj.textureIndex, obj.samplerIndex};

        vkCmdPushConstants(cmd, scene.layoutBundle.layout, scene.pushConstantStages, scene.pushConstantOffset,
                           scene.pushConstantSize, &push);

        VkBuffer vertexBuffer = mesh.buffers->vertexBuffer();
        VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(cmd, mesh.buffers->indexBuffer(), 0, mesh.buffers->indexType());
        vkCmdDrawIndexed(cmd, mesh.buffers->indexCount(), 1, 0, 0, 0);
    }
}

std::optional<uint32_t> findMemoryTypeIndex(const VkPhysicalDeviceMemoryProperties& memProps, uint32_t typeBits,
                                             VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1U << i)) != 0U && (memProps.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    return std::nullopt;
}

// --- Headless mode: offscreen render + pixel readback ----------------------
int runHeadless(bool enableValidation) {
    auto window = rx::platform::Window::create("rx_bindless_mesh_sample", static_cast<int>(kHeadlessWidth),
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

    auto context = rx::rhi::Context::create(extensions, enableValidation);
    if (!context.has_value()) {
        RX_LOG_ERROR("Context::create failed");
        return 1;
    }

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    if (surface == VK_NULL_HANDLE) {
        RX_LOG_ERROR("createVulkanSurface failed");
        return 1;
    }

    // Device::create already enables the full descriptor-indexing feature
    // set BindlessTable needs (device.cpp, Task 3) -- no separate feature
    // wiring needed here, unlike this codebase's pure-headless unit tests
    // which build their own local device fixture.
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

    const VkDevice vkDevice = device->device();
    const VkFormat targetFormat = device->swapchainFormat();

    auto scene = createScene(device->physicalDevice(), vkDevice, *allocator, *uploader, targetFormat);
    if (!scene.has_value()) {
        RX_LOG_ERROR("createScene failed");
        return 1;
    }

    auto depth = createDepthBuffer(device->physicalDevice(), vkDevice, *allocator, VkExtent2D{kHeadlessWidth, kHeadlessHeight});
    if (!depth.has_value()) {
        RX_LOG_ERROR("createDepthBuffer failed");
        return 1;
    }

    // Camera at t=0 (angle=0), objects at spinSeconds=0 -- a fixed, known
    // pose the probe pixel assertions below are derived against.
    updateTransforms(*uploader, *scene->transformBuffer, /*frameInFlightIndex=*/0, /*cameraAngle=*/0.0F,
                      /*spinSeconds=*/0.0F, /*aspect=*/1.0F);

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
        destroyScene(vkDevice, *scene);
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
    auto memoryTypeIndex = findMemoryTypeIndex(memProps, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memoryTypeIndex.has_value()) {
        RX_LOG_ERROR("no device-local memory type found for the offscreen target");
        destroyRawResources();
        return 1;
    }

    VkMemoryAllocateInfo memAllocInfo{};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReq.size;
    memAllocInfo.memoryTypeIndex = *memoryTypeIndex;
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

    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
        rx::rhi::transitionImage(cmd, offscreenImage, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rx::rhi::transitionImage(cmd, depth->image(), VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = offscreenView;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = VkClearColorValue{{0.05F, 0.05F, 0.08F, 1.0F}};

        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = depth->view();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = VkClearDepthStencilValue{1.0F, 0};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = VkRect2D{{0, 0}, {kHeadlessWidth, kHeadlessHeight}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport viewport{0.0F, 0.0F, static_cast<float>(kHeadlessWidth), static_cast<float>(kHeadlessHeight), 0.0F,
                             1.0F};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {kHeadlessWidth, kHeadlessHeight}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        recordSceneDraws(cmd, *scene, /*frameInFlightIndex=*/0);

        vkCmdEndRendering(cmd);

        rx::rhi::transitionImage(cmd, offscreenImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    });
    // RX_FRAME_MARK once per rendered frame [Phase 4 Stage 0 Task 3, spec
    // D3] -- headless-mode path: this sample's headless mode renders
    // exactly one frame (the runOnce() call just above; the readback-only
    // runOnce() below is not itself a rendered frame), so this is the
    // single frame boundary to mark.
    RX_FRAME_MARK;

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

    destroyRawResources();

    auto pixelAt = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return pixels.data() + (static_cast<size_t>(y) * kHeadlessWidth + x) * 4;
    };

    // Probe pixels: known screen positions for each object at this fixed
    // camera pose, empirically determined against this exact scene/camera
    // configuration (see task-6-report.md for the verification method --
    // rendered once with a debug dump during development, confirmed each
    // probe lands on the intended object's body, not the background or a
    // neighboring object). Center row (y=128); x positions sweep
    // left-to-right matching kObjects' left-to-right x ordering.
    struct Probe {
        uint32_t x;
        uint32_t y;
        const char* label;
    };
    constexpr std::array<Probe, 5> kProbes{{
        {40, 128, "object 0 (cube, checkerboard red/white)"},
        {83, 128, "object 1 (cube, gradient blue/green)"},
        {128, 128, "object 2 (sphere, checkerboard yellow/blue)"},
        {173, 128, "object 3 (sphere, gradient magenta/cyan)"},
        {216, 128, "object 4 (plane, real PNG orange/teal)"},
    }};

    std::vector<std::array<uint8_t, 3>> sampledColors;
    for (const auto& probe : kProbes) {
        const uint8_t* p = pixelAt(probe.x, probe.y);
        sampledColors.push_back({p[0], p[1], p[2]});
        // Channel order deliberately unlabeled (RGB vs BGR) -- this
        // device's swapchainFormat() is BGRA (same as 01_triangle/
        // 02_hotreload note), but the distinctness check below only ever
        // compares these 3 bytes against each other consistently, never
        // against an assumed absolute channel semantic, so the real
        // component order never matters here.
        RX_LOG_INFO("probe '{}' at ({},{}): channels=({},{},{})", probe.label, probe.x, probe.y, p[0], p[1], p[2]);
    }

    // >=3 distinct colors among the probes -- the headless gate's actual
    // requirement. A simple pairwise-distance check (not exact equality)
    // since antialiasing/mip selection could perturb exact byte values
    // slightly without changing which underlying texture was sampled.
    auto colorDistance = [](const std::array<uint8_t, 3>& a, const std::array<uint8_t, 3>& b) {
        int dr = static_cast<int>(a[0]) - static_cast<int>(b[0]);
        int dg = static_cast<int>(a[1]) - static_cast<int>(b[1]);
        int db = static_cast<int>(a[2]) - static_cast<int>(b[2]);
        return dr * dr + dg * dg + db * db;
    };
    constexpr int kDistinctThresholdSq = 40 * 40;  // generous vs. antialiasing/mip noise

    int distinctCount = 0;
    std::vector<std::array<uint8_t, 3>> distinctColors;
    for (const auto& color : sampledColors) {
        bool isNew = true;
        for (const auto& existing : distinctColors) {
            if (colorDistance(color, existing) < kDistinctThresholdSq) {
                isNew = false;
                break;
            }
        }
        if (isNew) {
            distinctColors.push_back(color);
            ++distinctCount;
        }
    }

    bool pass = true;
    if (distinctCount < 3) {
        RX_LOG_ERROR("only {} distinct texture colors found among {} probes (need >= 3)", distinctCount,
                     kProbes.size());
        pass = false;
    } else {
        RX_LOG_INFO("{} distinct texture colors found among {} probes", distinctCount, kProbes.size());
    }

    if (enableValidation && context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during this run");
        pass = false;
    }

    if (pass) {
        RX_LOG_INFO("bindless mesh headless gate PASSED");
        return 0;
    }
    RX_LOG_ERROR("bindless mesh headless gate FAILED");
    return 1;
}

// --- --present mode: real window, orbiting camera --------------------------
int runPresent(bool enableValidation) {
    auto window = rx::platform::Window::create("rx_bindless_mesh_sample (--present)", static_cast<int>(kPresentWidth),
                                                 static_cast<int>(kPresentHeight), /*visible=*/true);
    if (!window.has_value()) {
        RX_LOG_ERROR("Window::create failed: no display backend available");
        return 1;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        RX_LOG_ERROR("video driver reports no Vulkan surface extensions (e.g. dummy driver)");
        return 1;
    }

    auto context = rx::rhi::Context::create(extensions, enableValidation);
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

    auto scene = createScene(device->physicalDevice(), vkDevice, *allocator, *uploader, device->swapchainFormat());
    if (!scene.has_value()) {
        RX_LOG_ERROR("createScene failed");
        return 1;
    }

    auto depth = createDepthBuffer(device->physicalDevice(), vkDevice, *allocator, device->swapchainExtent());
    if (!depth.has_value()) {
        RX_LOG_ERROR("createDepthBuffer failed");
        return 1;
    }
    {
        auto cmdCtx =
            rx::rhi::CommandContext::create(vkDevice, device->graphicsQueue(), device->graphicsQueueFamily());
        if (cmdCtx.has_value()) {
            cmdCtx->runOnce([&](VkCommandBuffer cmd) {
                rx::rhi::transitionImage(cmd, depth->image(), VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            });
        }
    }

    std::vector<VkImageView> swapchainViews;
    auto createSwapchainViews = [&]() -> bool {
        swapchainViews.assign(device->swapchainImages().size(), VK_NULL_HANDLE);
        for (size_t i = 0; i < swapchainViews.size(); ++i) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = device->swapchainImages()[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = device->swapchainFormat();
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &swapchainViews[i]) != VK_SUCCESS) {
                RX_LOG_ERROR("sample_03_bindless_mesh: vkCreateImageView(swapchain image {}) failed", i);
                return false;
            }
        }
        return true;
    };
    auto destroySwapchainViews = [&]() {
        for (VkImageView view : swapchainViews) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(vkDevice, view, nullptr);
            }
        }
        swapchainViews.clear();
    };

    if (!createSwapchainViews()) {
        destroySwapchainViews();
        destroyScene(vkDevice, *scene);
        return 1;
    }

    RX_LOG_INFO("--present: window open ({}x{}); camera orbiting -- close the window to exit", kPresentWidth,
                kPresentHeight);

    const auto startTime = std::chrono::steady_clock::now();

    bool ok = true;
    bool quit = false;
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                quit = true;
            }
        }
        if (quit) {
            break;
        }

        VkFence fence = frameSync->currentFence();
        if (vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            RX_LOG_ERROR("vkWaitForFences failed in present loop");
            ok = false;
            break;
        }

        auto acquire = device->acquireNextImage(frameSync->currentImageAvailableSemaphore());
        if (acquire.status == rx::rhi::SwapchainStatus::NeedsRecreate) {
            if (vkDeviceWaitIdle(vkDevice) != VK_SUCCESS) {
                RX_LOG_ERROR("vkDeviceWaitIdle failed before swapchain recreation");
                ok = false;
                break;
            }
            destroySwapchainViews();
            depth.reset();
            if (!device->recreateSwapchain(surface) ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())) ||
                !createSwapchainViews()) {
                RX_LOG_ERROR("swapchain recreation failed after acquireNextImage NeedsRecreate");
                ok = false;
                break;
            }
            depth = createDepthBuffer(device->physicalDevice(), vkDevice, *allocator, device->swapchainExtent());
            if (!depth.has_value()) {
                RX_LOG_ERROR("depth buffer recreation failed after swapchain resize");
                ok = false;
                break;
            }
            {
                auto cmdCtx =
                    rx::rhi::CommandContext::create(vkDevice, device->graphicsQueue(), device->graphicsQueueFamily());
                if (cmdCtx.has_value()) {
                    cmdCtx->runOnce([&](VkCommandBuffer cmd) {
                        rx::rhi::transitionImage(cmd, depth->image(), VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
                    });
                }
            }
            continue;
        }
        if (acquire.status == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("device lost during acquireNextImage; exiting present loop");
            ok = false;
            break;
        }

        if (vkResetFences(vkDevice, 1, &fence) != VK_SUCCESS) {
            RX_LOG_ERROR("vkResetFences failed in present loop");
            ok = false;
            break;
        }

        auto now = std::chrono::steady_clock::now();
        float elapsedSeconds = std::chrono::duration<float>(now - startTime).count();
        float cameraAngle = elapsedSeconds * kCameraOrbitRadPerSec;
        const VkExtent2D extent = device->swapchainExtent();
        float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);

        // Update this frame-in-flight slot's transform rows -- safe: this
        // slot's own fence was just waited on above, so no draw could
        // still be reading these bytes (see this file's header comment).
        updateTransforms(*uploader, *scene->transformBuffer, frameSync->currentFrameIndex(), cameraAngle,
                          elapsedSeconds, aspect);

        VkCommandPool pool = frameSync->currentCommandPool();
        if (vkResetCommandPool(vkDevice, pool, 0) != VK_SUCCESS) {
            RX_LOG_ERROR("vkResetCommandPool failed in present loop");
            ok = false;
            break;
        }
        VkCommandBuffer cmd = frameSync->currentCommandBuffer();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
            RX_LOG_ERROR("vkBeginCommandBuffer failed in present loop");
            ok = false;
            break;
        }

        VkImage swapchainImage = device->swapchainImages()[acquire.imageIndex];
        rx::rhi::transitionImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = swapchainViews[acquire.imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = VkClearColorValue{{0.05F, 0.05F, 0.08F, 1.0F}};

        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = depth->view();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = VkClearDepthStencilValue{1.0F, 0};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = VkRect2D{{0, 0}, extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F,
                             1.0F};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        recordSceneDraws(cmd, *scene, frameSync->currentFrameIndex());

        vkCmdEndRendering(cmd);

        rx::rhi::transitionImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            RX_LOG_ERROR("vkEndCommandBuffer failed in present loop");
            ok = false;
            break;
        }

        VkSemaphore waitSem = frameSync->currentImageAvailableSemaphore();
        VkSemaphore signalSem = frameSync->renderFinishedSemaphore(acquire.imageIndex);
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSem;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSem;

        if (vkQueueSubmit(device->graphicsQueue(), 1, &submitInfo, fence) != VK_SUCCESS) {
            RX_LOG_ERROR("vkQueueSubmit failed in present loop");
            ok = false;
            break;
        }

        auto presentStatus = device->present(acquire.imageIndex, signalSem);
        if (presentStatus == rx::rhi::SwapchainStatus::NeedsRecreate) {
            if (vkDeviceWaitIdle(vkDevice) != VK_SUCCESS) {
                RX_LOG_ERROR("vkDeviceWaitIdle failed before swapchain recreation");
                ok = false;
                break;
            }
            destroySwapchainViews();
            depth.reset();
            if (!device->recreateSwapchain(surface) ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())) ||
                !createSwapchainViews()) {
                RX_LOG_ERROR("swapchain recreation failed after present NeedsRecreate");
                ok = false;
                break;
            }
            depth = createDepthBuffer(device->physicalDevice(), vkDevice, *allocator, device->swapchainExtent());
            if (depth.has_value()) {
                auto cmdCtx =
                    rx::rhi::CommandContext::create(vkDevice, device->graphicsQueue(), device->graphicsQueueFamily());
                if (cmdCtx.has_value()) {
                    cmdCtx->runOnce([&](VkCommandBuffer cmd2) {
                        rx::rhi::transitionImage(cmd2, depth->image(), VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
                    });
                }
            }
        } else if (presentStatus == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("device lost during present; exiting present loop");
            ok = false;
            break;
        }

        // RX_FRAME_MARK once per rendered frame [Phase 4 Stage 0 Task 3,
        // spec D3] -- present-mode path, right after this frame's present/
        // submit has completed.
        RX_FRAME_MARK;
        frameSync->advanceFrame();
    }

    vkDeviceWaitIdle(vkDevice);
    destroySwapchainViews();
    depth.reset();
    destroyScene(vkDevice, *scene);

    if (enableValidation && context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during the present loop");
        return 1;
    }
    if (!ok) {
        return 1;
    }
    RX_LOG_INFO("--present: window closed cleanly");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    rx::core::log::init();

    bool presentMode = false;
    bool enableValidation = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--present") {
            presentMode = true;
        } else if (std::string_view(argv[i]) == "--validate") {
            enableValidation = true;
        }
    }

    if (presentMode) {
        return runPresent(enableValidation);
    }
    return runHeadless(enableValidation);
}
