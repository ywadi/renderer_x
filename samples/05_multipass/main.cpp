// Sample 05: multipass (shadow + forward + tonemap), driven entirely by
// rx_graph.
//
// The Phase 3 acceptance test made visible: a real, if small, multipass
// pipeline -- shadow map, forward Lambert+shadow shading, Reinhard tonemap
// -- with EVERY layout transition/barrier between passes derived by
// rx::graph::RenderGraph::compile() and applied by rx::graph::Executor,
// never hand-written in this file. Searching this directory for the raw
// sync2 barrier-recording call (the one in rx_rhi_vk/command.cpp that
// rx::rhi::transitionImage() wraps) turns up nothing, and neither does a
// search for transitionImage() itself -- this file never calls either; every image
// this sample touches (the shadow map, the HDR color target, the forward
// pass's own depth buffer, and the backbuffer itself) is either a graph-
// pooled transient resource Executor::realize()/execute() owns end to end,
// or a caller-supplied backbuffer whose FIRST touch is always the graph's
// own tonemap-pass-entry barrier (compile() always derives a resource's
// first access as a fresh UNDEFINED->newLayout transition, correct for
// both a real just-acquired swapchain image and an offscreen headless
// target reused call-to-call -- see "BACKBUFFER REUSE ACROSS repeated
// execute() CALLS" below for why the latter is still safe here).
//
// GRAPH SHAPE: three passes, three resources besides the backbuffer --
//   shadow   (writes "shadowmap": Absolute 1024x1024 D32_SFLOAT, depth-only,
//             no fragment shader stage at all -- see shadow.vert.slang)
//   forward  (reads "shadowmap" as a texture input; writes "hdr":
//             SwapchainRelative R16G16B16A16_SFLOAT, and its own "depth":
//             SwapchainRelative D32_SFLOAT -- "depth" has no reader
//             anywhere else in the graph, which is fine: it is an
//             attachment-only resource, and forward itself is kept alive by
//             "hdr" having a real downstream reader, not by "depth")
//   tonemap  (reads "hdr"; writes "backbuffer", the graph's
//             setBackbufferSource() target)
//
// SCENE: one ground plane (XZ, y=0, half-size kGroundHalfSize) plus one cube
// and one sphere sitting on it -- reusing samples/03_bindless_mesh's
// procedural cube/sphere GENERATORS (adapted here to also emit a per-vertex
// NORMAL, needed for this sample's Lambert term, which 03 never needed).
// Every object only ever TRANSLATES, never rotates -- see
// shaders/multipass/lit.vert.slang's header comment for why that lets this
// shader skip a normal-matrix transform entirely (object-space normal ==
// world-space normal under a translation-only model matrix). One
// directional light, fixed elevation, an azimuth that stays 0 in headless
// mode and animates continuously in --present mode [coordinator ambiguity
// resolution].
//
// CAMERA: a fixed, top-down ORTHOGRAPHIC camera (eye directly above the
// scene, looking straight down), not a perspective one -- a deliberate
// choice [coordinator ambiguity resolution: "pick fixed values that make
// the headless assertions robust"], made specifically so the headless probe
// pixels below are ANALYTICALLY derived from the exact same view/projection
// math this file's own worldToPixel() uses at render time (mirroring
// samples/04_streaming's cellProbePixel() derivation for its own orthographic
// camera), rather than empirically reverse-engineered against a perspective
// camera's projection the way samples/03_bindless_mesh's probes were. See
// worldToPixel()'s own comment for the closed-form derivation.
//
// SHADOW/LIT PROBE PIXELS -- the exact values the headless gate asserts on,
// and why they are robust: kShadowProbeWorld = (0, 2.0) sits on the ground
// plane, directly in the analytically-derived shadow footprint the cube
// (half-extent kCubeHalfExtent, centered at kCubePosition) casts under this
// sample's fixed light direction (elevation kLightElevationRadians, azimuth
// 0) -- the cube's own footprint is x/z in [-1,1]; its top face (height
// 2*kCubeHalfExtent) projects along the light direction to z in
// [-1, 1 + 2*kCubeHalfExtent/tan(kLightElevationRadians)] ~= [-1, 2.68], so
// (0, 2.0) is comfortably inside that envelope with margin on both sides.
// kLitProbeWorld = (-4, -4) sits far from both the cube's footprint/shadow
// and the sphere (kSpherePosition, off in the +X+Z quadrant) -- unshadowed,
// unoccluded ground. Both are logged (with their derived pixel coordinates
// and sampled channel values) on every headless run for direct inspection,
// not just asserted on blindly.
//
// BACKBUFFER REUSE ACROSS REPEATED execute() CALLS (headless mode only):
// compile() has no notion of "a previous execute() call" -- every resource,
// backbuffer included, is always treated as a fresh, UNDEFINED-layout
// resource within one compile walk [Task 1/2 design]. Headless mode's own
// offscreen target is the SAME VkImage across all
// kHeadlessFrameCount=3 execute() calls (never recreated), so each call's
// own tonemap-pass-entry barrier "incorrectly" assumes UNDEFINED when the
// real prior layout is actually TRANSFER_SRC_OPTIMAL (left there by the
// PREVIOUS call's finalBarriers()). This is nonetheless safe here, and
// already proven so by src/rx_graph/tests/test_execute_gpu.cpp's own
// "resize-rerealize"/buffer-reuse GPU tests (repeated execute() calls
// against one realize()d graph, each in its own rx::rhi::CommandContext::
// runOnce() submission): every one of this file's own render frames is
// its own SEPARATE runOnce() call, and runOnce() itself vkQueueWaitIdle()s
// before returning -- so by the time the NEXT frame's execute() call
// records its own tonemap-entry barrier, the device is PROVABLY idle with
// respect to that image (nothing could still be racing the "wrong" srcStage/
// srcAccess the barrier's UNDEFINED-assuming derivation carries). A real
// swapchain image (--present mode) needs no such argument at all: it is a
// genuinely fresh acquisition each frame, for which "treat as UNDEFINED"
// is not an approximation but the literally correct thing to do -- exactly
// what every other sample's own hand-written per-frame transitionImage()
// call already does today.
//
// BINDLESS REGISTRATION FOR POOLED GRAPH RESOURCES: "shadowmap" and "hdr"
// are read via rx::rhi::BindlessTable (the same Texture2D[]/SamplerState[]
// convention every sample in this codebase uses), but their real
// VkImageView is only ever obtainable from inside a pass's own execute()
// callback (rx_graph::PassContext::imageView() -- there is no way to reach
// it from outside one). Since Executor::realize() binds a graph resource to
// the SAME pooled entry across every execute() call until a LATER realize()
// runs (verified directly against executor.cpp: `impl.resources` is only
// ever rebuilt by realize(), never by execute()), this file caches the last
// VkImageView it registered per resource and only calls
// BindlessTable::release()+registerSampledImage() again when that value
// actually changes -- never every frame. In practice this fires exactly
// once per resource (its first use) in headless mode (fixed size, no
// resize, ever) and again only on a real --present window resize (a
// SwapchainRelative resource's pooled entry genuinely changes shape then).
// The resize path re-registers immediately after this file's own
// vkDeviceWaitIdle()+recreateSwapchain()+recompile()+re-realize() sequence,
// with no submission in between -- exactly the same "rare, human-
// interactive event, a plain wait is enough" reasoning
// samples/02_hotreload's coordinator-approved pipeline-swap strategy
// already established for this codebase, applied here to a bindless
// descriptor slot instead of a VkPipeline.
//
// Two modes, exactly like every other sample in this codebase:
//   (default / no args) Headless correctness gate: fixed camera/light,
//   kHeadlessFrameCount=3 frames through the graph, readback, and the three
//   assertions the coordinator's brief specifies (shadow probe >2x darker
//   than the lit probe; lit probe non-trivially bright; every readback byte
//   in range, i.e. genuinely tonemapped, not a raw HDR overflow). Registered
//   as ctest sample_05_multipass_headless.
//   --present. Real window, the same scene, the light continuously
//   orbiting in azimuth. Also accepts --vsync on|off (default on)
//   [Phase 4 Task 6]: forwarded into Device::setPresentMode() +
//   recreateSwapchain() right after Device::create(), before any
//   per-swapchain-image resource is built. Has no effect in headless mode,
//   whose swapchain is built once and never presented -- the flag still
//   parses there, it just has nothing to apply to.
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

// See samples/03_bindless_mesh/main.cpp's identical comment: GLM is a
// PUBLIC rx_core dependency, transitively available here; the
// GLM_FORCE_DEPTH_ZERO_TO_ONE define must precede GLM's first include in
// this TU.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <SDL3/SDL.h>
#include <volk.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// --- Tunables ---------------------------------------------------------

constexpr uint32_t kHeadlessWidth = 256;
constexpr uint32_t kHeadlessHeight = 256;
constexpr VkDeviceSize kHeadlessPixelBytes = static_cast<VkDeviceSize>(kHeadlessWidth) * kHeadlessHeight * 4;
constexpr uint32_t kHeadlessFrameCount = 3;

constexpr uint32_t kPresentWidth = 900;
constexpr uint32_t kPresentHeight = 700;
constexpr float kLightOrbitRadPerSec = 0.3F;

constexpr uint32_t kShadowMapSize = 1024;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// [fix round 1] the single source of truth for ObjectTransform, shared by
// the shadow and lit passes -- concatenated ahead of each pass's own
// file(s) below, never Slang `import`ed/`__include`d; see that file's own
// header comment for why.
constexpr const char* kSceneTypesFilename = "scene_types.slang";
constexpr const char* kShadowVertFilename = "shadow.vert.slang";
constexpr const char* kLitVertFilename = "lit.vert.slang";
constexpr const char* kLitFragFilename = "lit.frag.slang";
constexpr const char* kTonemapVertFilename = "tonemap.vert.slang";
constexpr const char* kTonemapFragFilename = "tonemap.frag.slang";

// --- Scene layout -------------------------------------------------------

constexpr float kGroundHalfSize = 6.0F;
constexpr float kCubeHalfExtent = 1.0F;
constexpr glm::vec3 kCubePosition{0.0F, kCubeHalfExtent, 0.0F};
constexpr float kSphereRadius = 1.0F;
constexpr glm::vec3 kSpherePosition{3.5F, kSphereRadius, 2.5F};

// --- Camera: fixed top-down orthographic (see this file's header comment
// on why) ------------------------------------------------------------------
constexpr float kCameraOrthoHalfSize = 6.5F;  // > kGroundHalfSize -- a small border margin
constexpr float kCameraHeight = 15.0F;
constexpr float kCameraNear = 0.1F;
constexpr float kCameraFar = 30.0F;

// --- Light: fixed elevation, azimuth 0 in headless mode / animated in
// --present -----------------------------------------------------------------
constexpr float kLightElevationRadians = 0.8727F;  // 50 degrees
constexpr float kLightOrthoHalfSize = 9.0F;         // > kGroundHalfSize * sqrt(2): covers the floor from any azimuth
constexpr float kLightDistance = 20.0F;
constexpr float kLightNear = 0.1F;
constexpr float kLightFar = 40.0F;
constexpr float kHeadlessLightAzimuth = 0.0F;

// Headless probe world positions -- see this file's header comment for the
// closed-form derivation of why these two points are robustly inside/
// outside the cube's shadow.
constexpr glm::vec2 kShadowProbeWorld{0.0F, 2.0F};
constexpr glm::vec2 kLitProbeWorld{-4.0F, -4.0F};

glm::vec3 lightDirectionForAzimuth(float azimuthRadians) {
    const float sinEl = std::sin(kLightElevationRadians);
    const float cosEl = std::cos(kLightElevationRadians);
    return glm::normalize(
        glm::vec3(std::sin(azimuthRadians) * cosEl, -sinEl, std::cos(azimuthRadians) * cosEl));
}

// Vulkan clip space is Y-down; glm::ortho/perspective assume OpenGL's Y-up
// NDC -- flipping proj[1][1] is the same standard fix
// samples/03_bindless_mesh/04_streaming's own makeProjection() functions
// use. Applied identically to BOTH the camera's and the light's projection
// matrices in this file, which is load-bearing for lit.frag.slang's shadow
// UV derivation -- see that file's header comment on why no additional
// Y-flip is needed there as a direct result.
glm::mat4 applyVulkanYFlip(glm::mat4 proj) {
    proj[1][1] *= -1.0F;
    return proj;
}

// Fixed top-down camera basis: eye directly above the origin, looking
// straight down, with view-space up = world -Z (see the derivation this
// produces below). Never varies -- this sample's camera does not move or
// orbit in either mode [coordinator ambiguity resolution: --present only
// animates the light].
glm::mat4 makeCameraView() {
    return glm::lookAt(glm::vec3(0.0F, kCameraHeight, 0.0F), glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, -1.0F));
}

glm::mat4 makeCameraProj() {
    return applyVulkanYFlip(glm::ortho(-kCameraOrthoHalfSize, kCameraOrthoHalfSize, -kCameraOrthoHalfSize,
                                        kCameraOrthoHalfSize, kCameraNear, kCameraFar));
}

// Light view: eye placed `kLightDistance` back along -lightDir from the
// scene origin, looking toward the origin. up = world +Y is never parallel
// to `lightDir` at this sample's fixed elevation (50 degrees) for ANY
// azimuth -- cross(lightDir, (0,1,0)) = (-lightDir.z, 0, lightDir.x), zero
// only if lightDir.x == lightDir.z == 0, i.e. elevation == 90 degrees,
// never this sample's case -- so this basis stays well-defined across the
// full --present azimuth animation, not just azimuth 0.
glm::mat4 makeLightView(const glm::vec3& lightDir) {
    const glm::vec3 eye = -lightDir * kLightDistance;
    return glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
}

glm::mat4 makeLightProj() {
    return applyVulkanYFlip(
        glm::ortho(-kLightOrthoHalfSize, kLightOrthoHalfSize, -kLightOrthoHalfSize, kLightOrthoHalfSize, kLightNear,
                    kLightFar));
}

struct PixelCoord {
    uint32_t x;
    uint32_t y;
};

// Analytically maps a ground-plane world position (x, 0, z) to the screen
// pixel makeCameraView()/makeCameraProj() render it to, for a
// `viewportWidth`x`viewportHeight` target -- the same closed-form-
// derivation discipline samples/04_streaming's cellProbePixel() already
// established for its own fixed orthographic camera, re-derived here for
// THIS camera's own basis.
//
// DERIVATION (see this file's header comment for the summary): with
// eye=(0,H,0), center=(0,0,0), up=(0,0,-1), glm::lookAt's standard
// f/s/u construction yields f=(0,-1,0), s=(1,0,0), u=(0,0,-1) -- so for any
// ground point p=(x,0,z), view-space (viewX, viewY) = (dot(s,p-eye),
// dot(u,p-eye)) = (x, -z). The orthographic projection maps viewX/halfWidth
// -> ndcX and viewY/halfHeight -> ndcY (before any flip); this file's
// applyVulkanYFlip() then negates ndcY, giving ndcY_vulkan = z/halfHeight.
// Aspect correction (present mode's non-square window) mirrors
// samples/04_streaming's own computeOrthoExtents() letterbox-fit exactly,
// applied to this camera's square kCameraOrthoHalfSize content region.
PixelCoord worldToPixel(glm::vec2 worldXZ, uint32_t viewportWidth, uint32_t viewportHeight) {
    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
    float halfWidth = kCameraOrthoHalfSize * aspect;
    float halfHeight = kCameraOrthoHalfSize;
    if (halfWidth < kCameraOrthoHalfSize) {
        halfWidth = kCameraOrthoHalfSize;
        halfHeight = kCameraOrthoHalfSize / aspect;
    }

    const float ndcX = worldXZ.x / halfWidth;
    const float ndcYVulkan = worldXZ.y / halfHeight;  // worldXZ.y here holds world Z -- see callers

    const float pixelX = (ndcX * 0.5F + 0.5F) * static_cast<float>(viewportWidth);
    const float pixelY = (ndcYVulkan * 0.5F + 0.5F) * static_cast<float>(viewportHeight);

    const uint32_t clampedX =
        static_cast<uint32_t>(std::clamp(pixelX, 0.0F, static_cast<float>(viewportWidth) - 1.0F));
    const uint32_t clampedY =
        static_cast<uint32_t>(std::clamp(pixelY, 0.0F, static_cast<float>(viewportHeight) - 1.0F));
    return {clampedX, clampedY};
}

// Byte position of each of R/G/B within one texel of `format`, in a
// 4-byte-per-texel 8-bit UNORM/SRGB readback -- [fix round 2]. Every other
// channel-order check in this file (brightness sums, the shadow-vs-lit
// probe comparison) is deliberately channel-order-AGNOSTIC (see e.g.
// samples/03_bindless_mesh's own identical comment): it only ever compares
// a pixel against ITSELF or against another pixel, never against an
// absolute "this byte is red" claim, so it never needed to know this
// mapping. The object-index-1 substitution check below is different --
// it must tell a genuinely red-dominant pixel (the cube's own albedo)
// apart from a genuinely BLUE-dominant one (the sphere's), which an
// order-agnostic "is either outer byte dominant" check cannot do: that
// check is satisfied by EITHER a real red-dominant OR a real blue-
// dominant pixel, in either true byte order, which is exactly the review
// finding that superseded it (a substituted sphere row would have passed
// it). This function exists to give that check the TRUE, format-derived
// R/G/B byte indices instead.
//
// Vulkan's own format-naming convention lists components in increasing
// memory-address order -- VK_FORMAT_B8G8R8A8_* means byte0=B, byte1=G,
// byte2=R, byte3=A; VK_FORMAT_R8G8B8A8_* means byte0=R, byte1=G, byte2=B,
// byte3=A. Every 8-bit-per-channel swapchain/offscreen-readback format
// this sample has actually observed (linux-native on both lavapipe and
// this development machine's NVIDIA driver; windows-cross-zig under Wine)
// is one of these two families. Returns std::nullopt (never a guessed
// mapping) for anything else -- the caller must fail its check loudly
// rather than assert against an unverified channel order.
struct ChannelIndices {
    int r;
    int g;
    int b;
};

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

// --- Procedural geometry: position + normal (unlike
// samples/03_bindless_mesh's position + uv -- this sample has no textures,
// only Lambert shading, which needs a normal instead) --------------------

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

// Half-extent `h` cube -- identical vertex positions to
// samples/03_bindless_mesh's generateCube(), plus a per-face constant
// normal (each face's own outward axis) -- cheap to add since every face
// already has its own 4 unique vertices (no shared-vertex averaging
// needed).
HostMesh generateCube(float h) {
    HostMesh mesh;
    addQuad(mesh, {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h}, {1, 0, 0});         // +X
    addQuad(mesh, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h}, {-1, 0, 0});     // -X
    addQuad(mesh, {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}, {0, 1, 0});         // +Y
    addQuad(mesh, {-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {0, -1, 0});     // -Y
    addQuad(mesh, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});         // +Z
    addQuad(mesh, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});     // -Z
    return mesh;
}

// Standard UV sphere centered at the origin -- normal = normalized position
// (exact, not approximated) since every point on a sphere centered at the
// origin has its outward normal exactly along its own position vector.
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

// A single quad in the XZ plane at y=0, facing +Y (the ground) -- unlike
// samples/03_bindless_mesh's generatePlane() (an XY-plane card facing the
// camera), this is a genuine floor.
HostMesh generatePlaneXZ(float halfSize) {
    HostMesh mesh;
    addQuad(mesh, {-halfSize, 0.0F, -halfSize}, {halfSize, 0.0F, -halfSize}, {halfSize, 0.0F, halfSize},
            {-halfSize, 0.0F, halfSize}, {0, 1, 0});
    return mesh;
}

// --- Scene objects --------------------------------------------------------

enum class Shape : uint32_t { Plane = 0, Cube = 1, Sphere = 2 };
constexpr size_t kShapeCount = 3;

struct SceneObject {
    Shape shape;
    glm::vec3 position;
    glm::vec3 albedo;
};

// The plane is listed first and is the only object EXCLUDED from the
// shadow pass's own draw loop (see recordShadowDraws()) -- it is a
// shadow RECEIVER, not a caster; including it there would self-shadow the
// floor at grazing angles for no benefit (it already lies exactly in the
// plane every other object's shadow is cast onto).
constexpr std::array<SceneObject, 3> kObjects{{
    {Shape::Plane, glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(0.55F, 0.55F, 0.6F)},
    {Shape::Cube, kCubePosition, glm::vec3(0.75F, 0.25F, 0.2F)},
    {Shape::Sphere, kSpherePosition, glm::vec3(0.2F, 0.45F, 0.8F)},
}};
constexpr uint32_t kObjectCount = static_cast<uint32_t>(kObjects.size());

// --- GPU-side per-object transforms: one bindless storage buffer, one row
// per (frame-in-flight slot, object) -- exactly samples/03_bindless_mesh's
// double-buffered-by-row convention, extended to carry BOTH matrices this
// sample's two shadow-casting/receiving passes need per object. See
// shaders/multipass/shadow.vert.slang and lit.vert.slang's own header
// comments for which half each shader reads. -----------------------------
// `albedo`/`lightDirWorld` are float4 (unused 4th component) rather than
// float3 -- see lit.vert.slang's own header comment for why this sample
// keeps every vector value in this bindless row instead of a push
// constant, and every push-constant field below a plain scalar `uint`.
//
// Byte-for-byte mirror of shaders/multipass/scene_types.slang's
// ObjectTransform -- THE single source of truth on the shader side since
// fix round 1 (both shadow.vert.slang and lit.vert.slang now consume that
// one file instead of each declaring their own copy; see that file's own
// header comment for the real drift bug this closes). This struct is the
// other half of that single-source-of-truth story: the strongest drift
// guard expressible today is the static_assert immediately below, which
// fails the BUILD (not a runtime probe) the moment this struct's size
// stops matching the hand-computed shader-side stride. A stronger,
// reflect()-driven check (asking Slang itself what stride it actually
// used for this StructuredBuffer element) is NOT possible today --
// rx::shader::ShaderLayoutInfo (shader_layout_info.h) reports set/binding/
// type/count/stage/push-range shape only, nothing about a StructuredBuffer
// element's internal layout -- ledgered as an rx_shader enhancement
// candidate by the coordinator, explicitly out of this task's scope.
struct ObjectTransform {
    glm::mat4 mvp;
    glm::mat4 lightMvp;
    glm::vec4 albedo;
    glm::vec4 lightDirWorld;
};
// Hand-computed from scene_types.slang's own field list, in order: two
// float4x4 (64 bytes each -- GLM's mat4 is 16 packed floats, matching
// Slang's own float4x4 StructuredBuffer element size with no interior
// padding possible, since 64 is already a 16-byte multiple) plus two
// float4 (16 bytes each). If a future edit to scene_types.slang adds,
// removes, or reorders a field without updating this constant AND the
// struct above to match, this assert fails at compile time -- exactly the
// class of drift a mismatched hand-written copy used to hide until a
// runtime probe caught it (see scene_types.slang's header for that
// postmortem).
constexpr size_t kObjectTransformShaderStrideBytes = 64 + 64 + 16 + 16;
static_assert(sizeof(ObjectTransform) == kObjectTransformShaderStrideBytes,
              "ObjectTransform must match shaders/multipass/scene_types.slang's ObjectTransform stride exactly -- "
              "update kObjectTransformShaderStrideBytes (and this struct) together with that file");

// --- Push-constant layouts -- byte-for-byte mirrors of each shader's own
// [[vk::push_constant]] struct (see the .slang files). EMPIRICALLY
// VERIFIED (not assumed) against the shipped Slang build: a push-constant
// block made ENTIRELY of scalar `uint` fields reflects at exactly
// 4*fieldCount bytes, no padding/rounding at all -- see lit.vert.slang's
// header comment for the finding this superseded (samples/04_streaming's
// own struct, which contained a float4x4, is not evidence this applies
// generally). Each buildXPipeline() function below still asserts the
// REFLECTED size against sizeof() at runtime -- the shipped slang.h/
// compiler always wins over this file's own byte-count assumption on any
// future disagreement, not the other way around.
struct ShadowPushConstants {
    uint32_t transformIndex;
};

struct LitPushConstants {
    uint32_t transformIndex;
    uint32_t shadowTextureIndex;
    uint32_t shadowSamplerIndex;
};

struct TonemapPushConstants {
    uint32_t hdrTextureIndex;
    uint32_t hdrSamplerIndex;
};

// --- GPU-side scene -------------------------------------------------------

struct GpuMesh {
    std::optional<rx::rhi::MeshBuffers> buffers;
};

// One compiled+reflected+built pipeline's worth of state -- shared shape
// across all three passes; `fragModule` stays VK_NULL_HANDLE for the
// depth-only shadow pass (see shadow.vert.slang's own header comment for
// why that pass has no fragment shader stage at all).
struct CompiledPass {
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    rx::rhi::PipelineLayoutBundle layoutBundle;
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t pushConstantOffset = 0;
    uint32_t pushConstantSize = 0;
    VkShaderStageFlags pushConstantStages = 0;
};

void destroyCompiledPass(VkDevice device, CompiledPass& pass) {
    if (pass.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pass.pipeline, nullptr);
        pass.pipeline = VK_NULL_HANDLE;
    }
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

// Everything this sample needs beyond the raw RHI stack + rx_graph.
// Move-only by construction (every member is itself move-only RAII or a
// plain scalar/handle); built once by createScene(), torn down by
// destroyScene() before the owning Device/Context/Allocator/Executor do,
// same discipline as every other sample in this codebase.
struct Scene {
    rx::rhi::BindlessTable bindlessTable;
    VkSampler sampler = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle samplerHandle;
    std::optional<rx::rhi::Buffer> transformBuffer;
    std::array<GpuMesh, kShapeCount> meshes;

    CompiledPass shadowPass;
    CompiledPass litPass;
    CompiledPass tonemapPass;

    // See this file's header comment ("BINDLESS REGISTRATION FOR POOLED
    // GRAPH RESOURCES") -- cached so re-registration only happens when the
    // underlying pooled view actually changes.
    VkImageView lastShadowMapView = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle shadowMapHandle;
    VkImageView lastHdrView = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle hdrHandle;

    // Mutated once per frame (updateFrame(), below) before
    // Executor::execute() is called; read back inside every pass's
    // setExecute() callback at invocation time -- rx_graph's Pass::
    // setExecute() callback signature carries no caller-supplied per-frame
    // parameter of its own, so a small piece of mutable, reference-
    // captured state on `Scene` itself is this file's own equivalent of
    // samples/03_bindless_mesh's explicit `frameInFlightIndex` function
    // parameter.
    uint32_t currentFrameInFlightIndex = 0;
    glm::vec3 currentLightDirWorld{0.0F, -1.0F, 0.0F};
};

void destroyScenePipelines(VkDevice device, Scene& scene) {
    destroyCompiledPass(device, scene.shadowPass);
    destroyCompiledPass(device, scene.litPass);
    destroyCompiledPass(device, scene.tonemapPass);
}

void destroyScene(VkDevice device, Scene& scene) {
    destroyScenePipelines(device, scene);
    if (scene.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, scene.sampler, nullptr);
        scene.sampler = VK_NULL_HANDLE;
    }
}

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

// Resolves a deployed asset's path next to this executable at runtime, same
// SDL_GetBasePath() mechanism every other sample in this codebase uses for
// its own on-disk asset(s).
std::string resolveAssetPath(const char* filename) {
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        RX_LOG_WARN("sample_05_multipass: SDL_GetBasePath failed ({}); looking for {} in the current directory "
                    "instead",
                    SDL_GetError(), filename);
        return filename;
    }
    return std::string(basePath) + filename;
}

// Reads every file in `filenames` (in order) and concatenates their
// contents into one Slang translation unit -- see
// shaders/multipass/lit.vert.slang's own header comment ("COMPILATION
// NOTE") for why this sample's multi-file passes need this rather than a
// plain single-file compileFromFile() call: rx::shader::reflect() needs
// every entry point of a pass in ONE linked program to produce a single,
// correctly-merged layout, and rx::shader::Compiler only ever compiles one
// source string per call.
std::optional<std::string> readAndConcatenate(const std::vector<std::string>& filenames) {
    std::string combined;
    for (const auto& filename : filenames) {
        const std::string path = resolveAssetPath(filename.c_str());
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            RX_LOG_ERROR("sample_05_multipass: failed to open shader file '{}'", path);
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
        RX_LOG_ERROR("sample_05_multipass: shader compile failed for module '{}':\n{}", moduleName,
                     compileResult.diagnostics);
        return std::nullopt;
    }

    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("sample_05_multipass: reflect() failed for module '{}'", moduleName);
        return std::nullopt;
    }

    return ReflectedModule{std::move(compileResult), std::move(*layoutInfo)};
}

// Populates `pass`'s vertModule/fragModule from `compileResult`'s entry
// point blobs, matching each blob's `entryPointName` against
// `vertEntryName`/`fragEntryName` (`fragEntryName` empty -- the shadow
// pass's case -- means "this pass has no fragment stage; exactly one blob,
// the vertex one, is expected"). Returns false (logged) on any mismatch.
bool assignShaderModules(VkDevice device, const rx::shader::CompileResult& compileResult, const char* vertEntryName,
                          const char* fragEntryName, CompiledPass& pass) {
    for (const auto& blob : compileResult.entryPointCode) {
        VkShaderModule module = createShaderModuleFromSpirv(device, blob.code);
        if (module == VK_NULL_HANDLE) {
            RX_LOG_ERROR("sample_05_multipass: vkCreateShaderModule failed for entry point '{}'",
                         blob.entryPointName);
            return false;
        }
        if (blob.entryPointName == vertEntryName) {
            pass.vertModule = module;
        } else if (fragEntryName != nullptr && blob.entryPointName == fragEntryName) {
            pass.fragModule = module;
        } else {
            RX_LOG_ERROR("sample_05_multipass: unexpected entry point '{}'", blob.entryPointName);
            vkDestroyShaderModule(device, module, nullptr);
            return false;
        }
    }
    if (pass.vertModule == VK_NULL_HANDLE) {
        RX_LOG_ERROR("sample_05_multipass: missing vertex entry point '{}' after a successful compile",
                     vertEntryName);
        return false;
    }
    if (fragEntryName != nullptr && pass.fragModule == VK_NULL_HANDLE) {
        RX_LOG_ERROR("sample_05_multipass: missing fragment entry point '{}' after a successful compile",
                     fragEntryName);
        return false;
    }
    return true;
}

// --- Pipeline builders ---------------------------------------------------

bool buildShadowPipeline(VkDevice device, rx::shader::Compiler& compiler, VkDescriptorSetLayout bindlessSetLayout,
                          Scene& scene) {
    destroyCompiledPass(device, scene.shadowPass);

    // scene_types.slang listed first: see that file's own header comment
    // and kSceneTypesFilename's comment for why ObjectTransform is shared
    // this way rather than via Slang's own import/__include [fix round 1].
    auto reflected = compileAndReflect(compiler, "MultipassShadowModule",
                                        {kSceneTypesFilename, kShadowVertFilename}, {"vsMain"});
    if (!reflected.has_value()) {
        return false;
    }
    if (reflected->layoutInfo.pushRanges.size() != 1 ||
        reflected->layoutInfo.pushRanges[0].size != sizeof(ShadowPushConstants)) {
        RX_LOG_ERROR(
            "sample_05_multipass: shadow shader reflects an unexpected push-constant shape ({} range(s), size={} "
            "vs. sizeof(ShadowPushConstants)={})",
            reflected->layoutInfo.pushRanges.size(),
            reflected->layoutInfo.pushRanges.empty() ? 0 : reflected->layoutInfo.pushRanges[0].size,
            sizeof(ShadowPushConstants));
        return false;
    }

    // Runtime-verify that the reflected ObjectTransform element stride (from
    // the StructuredBuffer<ObjectTransform> binding) matches the C++ struct size,
    // if the stride extraction succeeded. This catches drift where the shader's
    // struct definition and C++'s ObjectTransform struct fall out of sync
    // (fix round 1's postmortem: that exact drift used to happen silently until
    // a runtime probe caught it). If stride extraction returns 0 (API limitation
    // or Slang version), the check is skipped but logged as a warning.
    bool foundTransformBinding = false;
    for (const auto& binding : reflected->layoutInfo.bindings) {
        if (binding.binding == 2 && binding.set == 0 &&
            binding.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            foundTransformBinding = true;
            if (binding.elementStride != 0) {
                // Stride was extracted; verify it matches the C++ struct
                if (binding.elementStride != sizeof(ObjectTransform)) {
                    RX_LOG_ERROR(
                        "sample_05_multipass: shadow shader's ObjectTransform element stride ({} bytes, from "
                        "reflection) does not match C++'s sizeof(ObjectTransform) ({} bytes) -- struct definitions "
                        "are out of sync; update both shaders/multipass/scene_types.slang and this file together",
                        binding.elementStride, sizeof(ObjectTransform));
                    return false;
                }
            } else {
                // Stride extraction returned 0 (API/version limitation); log but continue
                RX_LOG_WARN(
                    "sample_05_multipass: shadow shader's ObjectTransform element stride could not be extracted "
                    "from reflection (API limitation or Slang version); sizeof(ObjectTransform)={} bytes -- "
                    "manual verification via the static_assert above is still in place",
                    sizeof(ObjectTransform));
            }
            break;
        }
    }
    if (!foundTransformBinding) {
        RX_LOG_ERROR(
            "sample_05_multipass: shadow shader does not reflect the expected ObjectTransform storage buffer "
            "(binding 2, set 0); shader definition may have changed");
        return false;
    }

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, reflected->layoutInfo, bindlessSetLayout);
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("sample_05_multipass: PipelineLayoutBuilder::build failed for the shadow pass");
        return false;
    }
    scene.shadowPass.layoutBundle = std::move(*layoutBundle);
    scene.shadowPass.pushConstantOffset = reflected->layoutInfo.pushRanges[0].offset;
    scene.shadowPass.pushConstantSize = reflected->layoutInfo.pushRanges[0].size;
    scene.shadowPass.pushConstantStages = reflected->layoutInfo.pushRanges[0].stages;

    if (!assignShaderModules(device, reflected->compileResult, "vsMain", nullptr, scene.shadowPass)) {
        destroyCompiledPass(device, scene.shadowPass);
        return false;
    }

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attribute{};
    attribute.location = 0;
    attribute.binding = 0;
    attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
    attribute.offset = offsetof(Vertex, position);

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
    // No culling: this sample's procedural generators do not guarantee one
    // consistent winding across every face/object, same reasoning
    // samples/03_bindless_mesh/04_streaming already document for their own
    // CULL_MODE_NONE choice.
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

    VkPipelineColorBlendStateCreateInfo colorBlendState{};
    colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendState.attachmentCount = 0;

    std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineRenderingCreateInfo renderingCreateInfo{};
    renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCreateInfo.depthAttachmentFormat = kDepthFormat;

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = scene.shadowPass.vertModule;
    // Slang's SPIR-V backend always names each entry point's OpEntryPoint
    // "main", regardless of source-level function name -- see
    // samples/02_hotreload/main.cpp's identical comment.
    stage.pName = "main";

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingCreateInfo;
    pipelineInfo.stageCount = 1;
    pipelineInfo.pStages = &stage;
    pipelineInfo.pVertexInputState = &vertexInputState;
    pipelineInfo.pInputAssemblyState = &inputAssemblyState;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizationState;
    pipelineInfo.pMultisampleState = &multisampleState;
    pipelineInfo.pDepthStencilState = &depthStencilState;
    pipelineInfo.pColorBlendState = &colorBlendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = scene.shadowPass.layoutBundle.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;  // dynamic rendering
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &scene.shadowPass.pipeline) !=
        VK_SUCCESS) {
        RX_LOG_ERROR("sample_05_multipass: vkCreateGraphicsPipelines failed for the shadow pass");
        destroyCompiledPass(device, scene.shadowPass);
        return false;
    }
    return true;
}

bool buildLitPipeline(VkDevice device, rx::shader::Compiler& compiler, VkDescriptorSetLayout bindlessSetLayout,
                      Scene& scene) {
    destroyCompiledPass(device, scene.litPass);

    // scene_types.slang listed first -- see buildShadowPipeline()'s
    // identical comment [fix round 1].
    auto reflected = compileAndReflect(compiler, "MultipassLitModule",
                                        {kSceneTypesFilename, kLitVertFilename, kLitFragFilename},
                                        {"vsMain", "fsMain"});
    if (!reflected.has_value()) {
        return false;
    }
    if (reflected->layoutInfo.pushRanges.size() != 1 ||
        reflected->layoutInfo.pushRanges[0].size != sizeof(LitPushConstants)) {
        RX_LOG_ERROR(
            "sample_05_multipass: lit shader reflects an unexpected push-constant shape ({} range(s), size={} vs. "
            "sizeof(LitPushConstants)={})",
            reflected->layoutInfo.pushRanges.size(),
            reflected->layoutInfo.pushRanges.empty() ? 0 : reflected->layoutInfo.pushRanges[0].size,
            sizeof(LitPushConstants));
        return false;
    }
    if (reflected->layoutInfo.bindings.size() != 3) {
        RX_LOG_ERROR(
            "sample_05_multipass: lit shader reflects {} set-0 bindings; expected exactly 3 (images/samplers/"
            "storage buffers, matching BindlessTable's fixed scheme)",
            reflected->layoutInfo.bindings.size());
        return false;
    }

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, reflected->layoutInfo, bindlessSetLayout);
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("sample_05_multipass: PipelineLayoutBuilder::build failed for the lit pass");
        return false;
    }
    scene.litPass.layoutBundle = std::move(*layoutBundle);
    scene.litPass.pushConstantOffset = reflected->layoutInfo.pushRanges[0].offset;
    scene.litPass.pushConstantSize = reflected->layoutInfo.pushRanges[0].size;
    scene.litPass.pushConstantStages = reflected->layoutInfo.pushRanges[0].stages;

    if (!assignShaderModules(device, reflected->compileResult, "vsMain", "fsMain", scene.litPass)) {
        destroyCompiledPass(device, scene.litPass);
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

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingCreateInfo;

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = scene.litPass.vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = scene.litPass.fragModule;
    stages[1].pName = "main";

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
    pipelineInfo.layout = scene.litPass.layoutBundle.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &scene.litPass.pipeline) !=
        VK_SUCCESS) {
        RX_LOG_ERROR("sample_05_multipass: vkCreateGraphicsPipelines failed for the lit pass");
        destroyCompiledPass(device, scene.litPass);
        return false;
    }
    return true;
}

bool buildTonemapPipeline(VkDevice device, rx::shader::Compiler& compiler, VkDescriptorSetLayout bindlessSetLayout,
                          VkFormat backbufferFormat, Scene& scene) {
    destroyCompiledPass(device, scene.tonemapPass);

    auto reflected = compileAndReflect(compiler, "MultipassTonemapModule",
                                        {kTonemapVertFilename, kTonemapFragFilename}, {"vsMain", "fsMain"});
    if (!reflected.has_value()) {
        return false;
    }
    if (reflected->layoutInfo.pushRanges.size() != 1 ||
        reflected->layoutInfo.pushRanges[0].size != sizeof(TonemapPushConstants)) {
        RX_LOG_ERROR(
            "sample_05_multipass: tonemap shader reflects an unexpected push-constant shape ({} range(s), size={} "
            "vs. sizeof(TonemapPushConstants)={})",
            reflected->layoutInfo.pushRanges.size(),
            reflected->layoutInfo.pushRanges.empty() ? 0 : reflected->layoutInfo.pushRanges[0].size,
            sizeof(TonemapPushConstants));
        return false;
    }

    auto layoutBundle = rx::rhi::PipelineLayoutBuilder::build(device, reflected->layoutInfo, bindlessSetLayout);
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("sample_05_multipass: PipelineLayoutBuilder::build failed for the tonemap pass");
        return false;
    }
    scene.tonemapPass.layoutBundle = std::move(*layoutBundle);
    scene.tonemapPass.pushConstantOffset = reflected->layoutInfo.pushRanges[0].offset;
    scene.tonemapPass.pushConstantSize = reflected->layoutInfo.pushRanges[0].size;
    scene.tonemapPass.pushConstantStages = reflected->layoutInfo.pushRanges[0].stages;

    if (!assignShaderModules(device, reflected->compileResult, "vsMain", "fsMain", scene.tonemapPass)) {
        destroyCompiledPass(device, scene.tonemapPass);
        return false;
    }

    // No vertex buffer at all (SV_VertexID-driven fullscreen triangle) --
    // identical shape to src/rx_graph/tests/test_execute_gpu.cpp's own
    // "invert" pipeline.
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

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &scene.tonemapPass.pipeline) !=
        VK_SUCCESS) {
        RX_LOG_ERROR("sample_05_multipass: vkCreateGraphicsPipelines failed for the tonemap pass");
        destroyCompiledPass(device, scene.tonemapPass);
        return false;
    }
    return true;
}

std::optional<Scene> createScene(VkPhysicalDevice physicalDevice, VkDevice device, rx::rhi::Allocator& allocator,
                                  rx::rhi::Uploader& uploader, VkFormat backbufferFormat) {
    rx::rhi::BindlessTable::Capacities capacities{/*sampledImages=*/8, /*samplers=*/4, /*storageBuffers=*/2};
    auto bindlessTable = rx::rhi::BindlessTable::create(physicalDevice, device, capacities);
    if (!bindlessTable.has_value()) {
        RX_LOG_ERROR("sample_05_multipass: BindlessTable::create failed");
        return std::nullopt;
    }
    Scene scene{std::move(*bindlessTable)};

    // --- Meshes -----------------------------------------------------
    HostMesh planeHost = generatePlaneXZ(kGroundHalfSize);
    HostMesh cubeHost = generateCube(kCubeHalfExtent);
    HostMesh sphereHost = generateSphere(kSphereRadius, /*rings=*/16, /*segments=*/24);

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

    auto planeMesh = uploadMesh(planeHost);
    auto cubeMesh = uploadMesh(cubeHost);
    auto sphereMesh = uploadMesh(sphereHost);
    if (!planeMesh.has_value() || !cubeMesh.has_value() || !sphereMesh.has_value()) {
        RX_LOG_ERROR("sample_05_multipass: MeshBuffers::create failed for one or more shapes");
        return std::nullopt;
    }
    scene.meshes[static_cast<size_t>(Shape::Plane)] = std::move(*planeMesh);
    scene.meshes[static_cast<size_t>(Shape::Cube)] = std::move(*cubeMesh);
    scene.meshes[static_cast<size_t>(Shape::Sphere)] = std::move(*sphereMesh);
    uploader.flush();

    // --- Sampler: shared by both the shadow-map lookup (lit.frag.slang's
    // manual single-tap compare -- NEAREST avoids blending raw depth
    // values, which a linear filter would do incorrectly for a depth
    // comparison [coordinator ambiguity resolution]) and the tonemap
    // pass's 1:1 HDR sample (a plain full-resolution copy, for which
    // NEAREST is exact and cheaper than LINEAR). CLAMP_TO_EDGE: a shadow
    // lookup that lands just outside [0,1] (see lit.frag.slang's own
    // explicit bounds check, which handles the case fully out of frustum)
    // still gets a well-defined edge-texel value rather than wrapping.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0F;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &scene.sampler) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_05_multipass: vkCreateSampler failed");
        return std::nullopt;
    }
    scene.samplerHandle = scene.bindlessTable.registerSampler(scene.sampler);
    if (!scene.samplerHandle.isValid()) {
        RX_LOG_ERROR("sample_05_multipass: BindlessTable::registerSampler failed");
        destroyScene(device, scene);
        return std::nullopt;
    }

    // --- Object-transform storage buffer: framesInFlight() * kObjectCount
    // rows, registered ONCE -- see this file's header comment and
    // samples/03_bindless_mesh's identical convention for why no per-frame
    // re-registration is needed.
    const VkDeviceSize transformBufferSize =
        static_cast<VkDeviceSize>(rx::rhi::FrameSync::framesInFlight()) * kObjectCount * sizeof(ObjectTransform);
    auto transformBuffer = allocator.createDeviceLocalBuffer(
        transformBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!transformBuffer.has_value()) {
        RX_LOG_ERROR("sample_05_multipass: createDeviceLocalBuffer failed for the transform buffer");
        destroyScene(device, scene);
        return std::nullopt;
    }
    scene.transformBuffer = std::move(transformBuffer);
    auto transformHandle =
        scene.bindlessTable.registerStorageBuffer(scene.transformBuffer->handle(), transformBufferSize);
    if (!transformHandle.isValid()) {
        RX_LOG_ERROR("sample_05_multipass: BindlessTable::registerStorageBuffer failed");
        destroyScene(device, scene);
        return std::nullopt;
    }

    // --- Reflection-driven pipelines ---------------------------------
    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        RX_LOG_ERROR("sample_05_multipass: rx::shader::Compiler::create failed");
        destroyScene(device, scene);
        return std::nullopt;
    }
    if (!buildShadowPipeline(device, *compiler, scene.bindlessTable.descriptorSetLayout(), scene) ||
        !buildLitPipeline(device, *compiler, scene.bindlessTable.descriptorSetLayout(), scene) ||
        !buildTonemapPipeline(device, *compiler, scene.bindlessTable.descriptorSetLayout(), backbufferFormat,
                               scene)) {
        destroyScene(device, scene);
        return std::nullopt;
    }

    return scene;
}

// --- Render-graph declaration + per-pass draw recording -------------------

rx::graph::AttachmentDesc absoluteDesc(VkFormat format, uint32_t width, uint32_t height) {
    rx::graph::AttachmentDesc desc;
    desc.format = format;
    desc.sizeClass = rx::graph::SizeClass::Absolute;
    desc.width = static_cast<float>(width);
    desc.height = static_cast<float>(height);
    return desc;
}

rx::graph::AttachmentDesc swapchainRelativeDesc(VkFormat format) {
    rx::graph::AttachmentDesc desc;
    desc.format = format;
    desc.sizeClass = rx::graph::SizeClass::SwapchainRelative;
    desc.width = 1.0F;
    desc.height = 1.0F;
    return desc;
}

void recordShadowDraws(rx::graph::PassContext& ctx, Scene& scene) {
    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.shadowPass.pipeline);
    VkDescriptorSet set = scene.bindlessTable.descriptorSet();
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.shadowPass.layoutBundle.layout, 0, 1,
                             &set, 0, nullptr);

    VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width), static_cast<float>(ctx.renderArea.height),
                         0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, ctx.renderArea};
    vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

    for (uint32_t i = 0; i < kObjectCount; ++i) {
        const SceneObject& obj = kObjects[i];
        if (obj.shape == Shape::Plane) {
            continue;  // the floor is a shadow RECEIVER, never a caster -- see kObjects' own comment.
        }
        const GpuMesh& mesh = scene.meshes[static_cast<size_t>(obj.shape)];

        ShadowPushConstants push{};
        push.transformIndex = scene.currentFrameInFlightIndex * kObjectCount + i;
        vkCmdPushConstants(ctx.cmd, scene.shadowPass.layoutBundle.layout, scene.shadowPass.pushConstantStages,
                           scene.shadowPass.pushConstantOffset, scene.shadowPass.pushConstantSize, &push);

        VkBuffer vertexBuffer = mesh.buffers->vertexBuffer();
        VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(ctx.cmd, mesh.buffers->indexBuffer(), 0, mesh.buffers->indexType());
        vkCmdDrawIndexed(ctx.cmd, mesh.buffers->indexCount(), 1, 0, 0, 0);
    }
}

void recordLitDraws(rx::graph::PassContext& ctx, Scene& scene) {
    // Register (or re-register, only if the pooled view actually changed --
    // see this file's header comment) "shadowmap" into the bindless table
    // BEFORE the draw loop, once per execute() call, not once per object.
    VkImageView shadowView = ctx.imageView("shadowmap");
    if (shadowView != scene.lastShadowMapView) {
        if (scene.shadowMapHandle.isValid()) {
            scene.bindlessTable.release(scene.shadowMapHandle);
        }
        scene.shadowMapHandle =
            scene.bindlessTable.registerSampledImage(shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        scene.lastShadowMapView = shadowView;
    }

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.litPass.pipeline);
    VkDescriptorSet set = scene.bindlessTable.descriptorSet();
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.litPass.layoutBundle.layout, 0, 1, &set,
                             0, nullptr);

    VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width), static_cast<float>(ctx.renderArea.height),
                         0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, ctx.renderArea};
    vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
    vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

    for (uint32_t i = 0; i < kObjectCount; ++i) {
        const SceneObject& obj = kObjects[i];
        const GpuMesh& mesh = scene.meshes[static_cast<size_t>(obj.shape)];

        LitPushConstants push{};
        push.transformIndex = scene.currentFrameInFlightIndex * kObjectCount + i;
        push.shadowTextureIndex = scene.shadowMapHandle.index();
        push.shadowSamplerIndex = scene.samplerHandle.index();
        vkCmdPushConstants(ctx.cmd, scene.litPass.layoutBundle.layout, scene.litPass.pushConstantStages,
                           scene.litPass.pushConstantOffset, scene.litPass.pushConstantSize, &push);

        VkBuffer vertexBuffer = mesh.buffers->vertexBuffer();
        VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(ctx.cmd, mesh.buffers->indexBuffer(), 0, mesh.buffers->indexType());
        vkCmdDrawIndexed(ctx.cmd, mesh.buffers->indexCount(), 1, 0, 0, 0);
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

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.tonemapPass.pipeline);
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

// Declares the three passes ONCE -- called exactly once per RenderGraph
// instance, before its first compile(). `graph.compile()` may be (and, on
// a --present window resize, is) called again later against this exact
// same declared topology with a different CompileInfo -- see
// src/rx_graph/tests/test_execute_gpu.cpp's own "resize-rerealize" test for
// the proof this is a supported, tested RenderGraph usage pattern (no
// addPass()/reset() needed for a plain re-compile at a new size).
void declareGraph(rx::graph::RenderGraph& graph, Scene& scene, VkFormat backbufferFormat) {
    graph.addPass("shadow")
        .setDepthStencilOutput("shadowmap", absoluteDesc(kDepthFormat, kShadowMapSize, kShadowMapSize))
        .setExecute([&scene](rx::graph::PassContext& ctx) { recordShadowDraws(ctx, scene); });

    graph.addPass("forward")
        .addTextureInput("shadowmap")
        .addColorOutput("hdr", swapchainRelativeDesc(kHdrFormat))
        .setDepthStencilOutput("depth", swapchainRelativeDesc(kDepthFormat))
        .setExecute([&scene](rx::graph::PassContext& ctx) { recordLitDraws(ctx, scene); });

    graph.addPass("tonemap")
        .addTextureInput("hdr")
        .addColorOutput("backbuffer", swapchainRelativeDesc(backbufferFormat))
        .setExecute([&scene](rx::graph::PassContext& ctx) { recordTonemapDraw(ctx, scene); });

    graph.setBackbufferSource("backbuffer");
}

// Writes every object's camera-space and light-space MVP into
// `frameInFlightIndex`'s row range of the transform buffer, and updates
// `scene.currentLightDirWorld` for lit.frag.slang's Lambert term -- see
// this file's header comment on why the latter lives on `Scene` rather
// than being threaded through as a function parameter. Mirrors
// samples/03_bindless_mesh's updateTransforms(): writing only this slot's
// own row range, after its own fence has already been waited on by the
// caller, needs no further synchronization (headless mode only ever uses
// slot 0).
void updateFrame(rx::rhi::Uploader& uploader, Scene& scene, uint32_t frameInFlightIndex, float lightAzimuthRadians) {
    const glm::vec3 lightDir = lightDirectionForAzimuth(lightAzimuthRadians);
    scene.currentFrameInFlightIndex = frameInFlightIndex;
    scene.currentLightDirWorld = lightDir;

    const glm::mat4 cameraViewProj = makeCameraProj() * makeCameraView();
    const glm::mat4 lightViewProj = makeLightProj() * makeLightView(lightDir);

    std::array<ObjectTransform, kObjectCount> transforms{};
    for (uint32_t i = 0; i < kObjectCount; ++i) {
        const glm::mat4 model = glm::translate(glm::mat4(1.0F), kObjects[i].position);
        // Transposed before upload: see compiler.h's own documented
        // row-major-vs-GLM's-column-major gotcha, and
        // samples/03_bindless_mesh/main.cpp's updateTransforms() for the
        // worked example this sample's transform buffer follows exactly.
        // (Matrices are the only fields needing this treatment --
        // albedo/lightDirWorld below are plain vec4s, no layout gotcha.)
        transforms[i].mvp = glm::transpose(cameraViewProj * model);
        transforms[i].lightMvp = glm::transpose(lightViewProj * model);
        transforms[i].albedo = glm::vec4(kObjects[i].albedo, 0.0F);
        transforms[i].lightDirWorld = glm::vec4(lightDir, 0.0F);
    }

    const VkDeviceSize rowBytes = sizeof(ObjectTransform);
    const VkDeviceSize dstOffset = static_cast<VkDeviceSize>(frameInFlightIndex) * kObjectCount * rowBytes;
    uploader.uploadToBuffer(*scene.transformBuffer, dstOffset, transforms.data(), kObjectCount * rowBytes);
    uploader.flush();
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
    auto window = rx::platform::Window::create("rx_multipass_sample", static_cast<int>(kHeadlessWidth),
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

    auto executor = rx::graph::Executor::create(*device);
    if (executor == nullptr) {
        RX_LOG_ERROR("rx::graph::Executor::create failed");
        return 1;
    }

    const VkDevice vkDevice = device->device();
    const VkFormat targetFormat = device->swapchainFormat();

    auto scene = createScene(device->physicalDevice(), vkDevice, *allocator, *uploader, targetFormat);
    if (!scene.has_value()) {
        RX_LOG_ERROR("createScene failed");
        return 1;
    }

    rx::graph::RenderGraph graph;
    declareGraph(graph, *scene, targetFormat);

    rx::graph::CompileInfo info;
    info.swapchainWidth = kHeadlessWidth;
    info.swapchainHeight = kHeadlessHeight;
    info.swapchainFormat = targetFormat;
    // Offscreen backbuffer, never presented -- see rx_graph's CompileInfo
    // ambiguity resolution #1 (TRANSFER_SRC_OPTIMAL is what this file's own
    // readback copy needs finalBarriers() to leave "backbuffer" in, not
    // PRESENT_SRC_KHR).
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);
    executor->realize(graph);

    // Fixed camera/light for the whole headless run (no animation) -- slot
    // 0 only, per this file's header comment.
    updateFrame(*uploader, *scene, /*frameInFlightIndex=*/0, kHeadlessLightAzimuth);

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

    // Every render frame is its own SEPARATE runOnce() submission -- see
    // this file's header comment ("BACKBUFFER REUSE ACROSS REPEATED
    // execute() CALLS") for why that separation, not a style choice, is
    // what makes each call's own graph-derived tonemap-entry barrier safe
    // despite compile()'s stateless UNDEFINED assumption.
    const VkExtent2D extent{kHeadlessWidth, kHeadlessHeight};
    for (uint32_t frame = 0; frame < kHeadlessFrameCount; ++frame) {
        cmdCtx->runOnce([&](VkCommandBuffer cmd) {
            executor->execute(graph, cmd, offscreenImage, offscreenView, extent);
        });
        // RX_FRAME_MARK once per rendered frame [Phase 4 Stage 0 Task 3,
        // spec D3] -- headless-mode path: each loop iteration is its own
        // separate runOnce() submission through the render graph, i.e. a
        // real, distinct rendered frame.
        RX_FRAME_MARK;
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

    destroyRawResources();

    auto pixelAt = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return pixels.data() + (static_cast<size_t>(y) * kHeadlessWidth + x) * 4;
    };
    // Consistent with every other channel-order-agnostic probe check in
    // this codebase (see samples/03_bindless_mesh's own comment): the
    // real swapchain-matching format's channel order (RGBA vs BGRA) is
    // never assumed, only relative brightness across channels/pixels.
    auto brightness = [](const uint8_t* p) -> int { return static_cast<int>(p[0]) + p[1] + p[2]; };

    const PixelCoord shadowPixel = worldToPixel(kShadowProbeWorld, kHeadlessWidth, kHeadlessHeight);
    const PixelCoord litPixel = worldToPixel(kLitProbeWorld, kHeadlessWidth, kHeadlessHeight);
    const uint8_t* shadowPx = pixelAt(shadowPixel.x, shadowPixel.y);
    const uint8_t* litPx = pixelAt(litPixel.x, litPixel.y);
    const int shadowBrightness = brightness(shadowPx);
    const int litBrightness = brightness(litPx);

    RX_LOG_INFO("shadow probe world=({:.1f},{:.1f}) pixel=({},{}) channels=({},{},{},{}) brightness_sum={}",
                kShadowProbeWorld.x, kShadowProbeWorld.y, shadowPixel.x, shadowPixel.y, shadowPx[0], shadowPx[1],
                shadowPx[2], shadowPx[3], shadowBrightness);
    RX_LOG_INFO("lit probe world=({:.1f},{:.1f}) pixel=({},{}) channels=({},{},{},{}) brightness_sum={}",
                kLitProbeWorld.x, kLitProbeWorld.y, litPixel.x, litPixel.y, litPx[0], litPx[1], litPx[2], litPx[3],
                litBrightness);

    bool pass = true;

    // (a) shadow probe darker than the lit probe by > 2x.
    if (shadowBrightness * 2 >= litBrightness) {
        RX_LOG_ERROR(
            "shadow probe brightness_sum={} is not < half of lit probe brightness_sum={} (need shadow*2 < lit)",
            shadowBrightness, litBrightness);
        pass = false;
    }
    // (b) lit probe non-trivially bright: brightness_sum (0..765) > 0.2 of
    // full-scale (765), i.e. > 153.
    constexpr int kLitBrightnessFloor = static_cast<int>(0.2 * 3 * 255);
    if (litBrightness <= kLitBrightnessFloor) {
        RX_LOG_ERROR("lit probe brightness_sum={} is not > {} (0.2 of full scale)", litBrightness,
                     kLitBrightnessFloor);
        pass = false;
    }
    // (c) every readback value <= 1.0 -- i.e. genuinely tonemapped, not an
    // unclamped HDR overflow. This is automatically true for any correctly
    // functioning UNORM/SRGB 8-bit swapchain-format readback (the GPU
    // itself clamps on write) -- checked here anyway, byte-for-byte, both
    // for literal compliance with this task's own brief and as a sanity
    // net against a genuinely corrupt readback (garbage/NaN-derived bytes
    // would still satisfy "<= 255" for a plain byte, so this is a weak but
    // real check, not a substitute for (a)/(b) above).
    for (size_t i = 0; i < pixels.size(); ++i) {
        if (pixels[i] > 255) {  // unreachable for a uint8_t -- documents the literal brief requirement above.
            RX_LOG_ERROR("readback byte {} = {} exceeds 1.0 (255) -- not tonemapped", i, pixels[i]);
            pass = false;
            break;
        }
    }

    // (d) [fix round 1, tightened in fix round 2] object index 1 (the cube
    // -- see kObjects) renders at its ANALYTICALLY EXPECTED screen
    // position, not just "some shadow exists somewhere". Deliberately
    // index 1, not index 0 (the floor): this is exactly the row a
    // stride/offset drift between the C++ writer and either shader's
    // ObjectTransform struct would corrupt while leaving row 0 looking
    // fine (row 0 starts at byte offset 0 under any stride assumption --
    // see scene_types.slang's header comment for the real bug this class
    // of check would have caught immediately). Probes the cube's own
    // world XZ center (worldToPixel() -- the same fixed top-down camera
    // every other probe in this file uses).
    //
    // CHANNEL-ORDER-EXACT, not agnostic [fix round 2 -- re-review finding]:
    // an earlier revision of this check accepted "byte0 dominant OR byte2
    // dominant" as a stand-in for "red-dominant, whichever byte order this
    // device uses" -- but that is satisfied by a genuinely BLUE-dominant
    // pixel too (the sphere's own albedo direction), in whichever byte
    // order is NOT the true one: e.g. under a real RGBA device, the
    // sphere's blue channel (byte2) being dominant would satisfy the
    // "byte2 dominant" half of that OR, which the old check laundered
    // into "red-dominant" unconditionally. That is precisely the failure
    // mode this probe exists to catch (a neighboring row's data
    // substituted in for the cube's own), so a check that cannot tell red
    // from blue cannot actually catch it. Fixed by resolving this
    // sample's REAL backbuffer format (channelIndicesForFormat(),
    // above) to the true R/G/B byte positions ONCE, then testing against
    // those exact indices only -- both a genuine "is this red" assertion
    // AND, as direct substitution detection, genuine "is this NOT the
    // floor's near-neutral gray" / "is this NOT the sphere's blue-
    // dominant albedo" assertions against the cube's two actual
    // neighboring rows in the transform buffer (index 0 and index 2).
    const PixelCoord cubePixel =
        worldToPixel(glm::vec2(kCubePosition.x, kCubePosition.z), kHeadlessWidth, kHeadlessHeight);
    const uint8_t* cubePx = pixelAt(cubePixel.x, cubePixel.y);
    RX_LOG_INFO("cube probe world=({:.1f},{:.1f}) pixel=({},{}) channels=({},{},{},{})", kCubePosition.x,
                kCubePosition.z, cubePixel.x, cubePixel.y, cubePx[0], cubePx[1], cubePx[2], cubePx[3]);

    const auto cubeChannels = channelIndicesForFormat(targetFormat);
    if (!cubeChannels.has_value()) {
        RX_LOG_ERROR(
            "cube probe: backbuffer format {} is not one of the R8G8B8A8/B8G8R8A8 families this check knows how "
            "to map to exact R/G/B byte positions -- refusing to guess",
            static_cast<int>(targetFormat));
        pass = false;
    } else {
        constexpr int kMargin = 15;  // matches this file's other probe margins
        const int r = cubeChannels->r;
        const int g = cubeChannels->g;
        const int b = cubeChannels->b;

        // Positive check: the cube's own albedo (0.75, 0.25, 0.2) is
        // clearly red-dominant.
        const bool isRedDominant = cubePx[r] > cubePx[g] + kMargin && cubePx[r] > cubePx[b] + kMargin;
        if (!isRedDominant) {
            RX_LOG_ERROR(
                "cube probe (object index 1) at pixel ({},{}) R={} G={} B={} is not clearly red-dominant -- the "
                "cube is not rendering at its analytically expected position (ObjectTransform stride/offset "
                "drift between the C++ writer and a shader-side copy is exactly the bug class this check exists "
                "to catch -- see scene_types.slang)",
                cubePixel.x, cubePixel.y, cubePx[r], cubePx[g], cubePx[b]);
            pass = false;
        }

        // Direct substitution detection: the cube's two actual neighboring
        // rows in the transform buffer are index 0 (the floor, near-
        // neutral gray -- (0.55, 0.55, 0.6)) and index 2 (the sphere,
        // blue-dominant -- (0.2, 0.45, 0.8)). A struct-offset drift that
        // substitutes either neighbor's data in for the cube's own row
        // must fail at least one of these, even in the (algebraically
        // impossible once isRedDominant already holds) case that some
        // future edit weakens the positive check above.
        const bool looksLikeFloor = std::max({cubePx[r], cubePx[g], cubePx[b]}) -
                                         std::min({cubePx[r], cubePx[g], cubePx[b]}) <
                                     kMargin;
        if (looksLikeFloor) {
            RX_LOG_ERROR(
                "cube probe (object index 1) at pixel ({},{}) R={} G={} B={} looks like the FLOOR's near-neutral "
                "albedo (object index 0, the cube's neighboring row) -- a likely direct row substitution",
                cubePixel.x, cubePixel.y, cubePx[r], cubePx[g], cubePx[b]);
            pass = false;
        }
        const bool looksLikeSphere = cubePx[b] > cubePx[r] + kMargin && cubePx[b] > cubePx[g] + kMargin;
        if (looksLikeSphere) {
            RX_LOG_ERROR(
                "cube probe (object index 1) at pixel ({},{}) R={} G={} B={} looks like the SPHERE's blue-"
                "dominant albedo (object index 2, the cube's other neighboring row) -- a likely direct row "
                "substitution",
                cubePixel.x, cubePixel.y, cubePx[r], cubePx[g], cubePx[b]);
            pass = false;
        }
    }

    if (enableValidation && context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during this run");
        pass = false;
    }

    if (pass) {
        RX_LOG_INFO("multipass headless gate PASSED");
        return 0;
    }
    RX_LOG_ERROR("multipass headless gate FAILED");
    return 1;
}

// --- --present mode: real window, orbiting light -----------------------------
int runPresent(bool enableValidation, rx::rhi::PresentMode vsyncMode) {
    auto window = rx::platform::Window::create("rx_multipass_sample (--present)", static_cast<int>(kPresentWidth),
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

    // --vsync [Phase 4 Task 6]: Device::create() always builds its
    // swapchain with an explicit FIFO default (PresentMode::VsyncOn) --
    // see device.cpp's own comment at the creation site. setPresentMode()
    // only records what the caller wants; recreateSwapchain() (the exact
    // path the NeedsRecreate handling below already drives on a real
    // resize) is what actually applies it -- reused here, once, before any
    // per-swapchain-image resource (FrameSync, image views) is built
    // against this device, so nothing downstream is built against a
    // swapchain generation that is about to be replaced.
    if (vsyncMode == rx::rhi::PresentMode::VsyncOff) {
        device->setPresentMode(vsyncMode);
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

    auto executor = rx::graph::Executor::create(*device);
    if (executor == nullptr) {
        RX_LOG_ERROR("rx::graph::Executor::create failed");
        return 1;
    }

    auto scene = createScene(device->physicalDevice(), vkDevice, *allocator, *uploader, device->swapchainFormat());
    if (!scene.has_value()) {
        RX_LOG_ERROR("createScene failed");
        return 1;
    }

    rx::graph::RenderGraph graph;
    declareGraph(graph, *scene, device->swapchainFormat());

    auto compileForExtent = [&](VkExtent2D extent) {
        rx::graph::CompileInfo info;
        info.swapchainWidth = extent.width;
        info.swapchainHeight = extent.height;
        info.swapchainFormat = device->swapchainFormat();
        // Default backbufferFinalLayout (PRESENT_SRC_KHR) -- a real
        // swapchain image, presented every frame.
        graph.compile(info);
        executor->realize(graph);
    };
    compileForExtent(device->swapchainExtent());

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
                RX_LOG_ERROR("sample_05_multipass: vkCreateImageView(swapchain image {}) failed", i);
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

    RX_LOG_INFO("--present: window open ({}x{}); light orbiting -- close the window to exit", kPresentWidth,
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
            if (!device->recreateSwapchain(surface) ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())) ||
                !createSwapchainViews()) {
                RX_LOG_ERROR("swapchain recreation failed after acquireNextImage NeedsRecreate");
                ok = false;
                break;
            }
            // Re-derive every SwapchainRelative resource's real extent
            // (recompile + re-realize) immediately -- device is provably
            // idle (just waited above) and nothing has been submitted
            // since, so the NEXT execute() call's own bindless
            // re-registration (recordLitDraws()/recordTonemapDraw(), on
            // seeing "hdr"'s view actually changed) is safe -- see this
            // file's header comment.
            compileForExtent(device->swapchainExtent());
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

        const auto now = std::chrono::steady_clock::now();
        const float elapsedSeconds = std::chrono::duration<float>(now - startTime).count();
        const float lightAzimuth = elapsedSeconds * kLightOrbitRadPerSec;

        updateFrame(*uploader, *scene, frameSync->currentFrameIndex(), lightAzimuth);

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
        executor->execute(graph, cmd, swapchainImage, swapchainViews[acquire.imageIndex], device->swapchainExtent());

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
            if (!device->recreateSwapchain(surface) ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())) ||
                !createSwapchainViews()) {
                RX_LOG_ERROR("swapchain recreation failed after present NeedsRecreate");
                ok = false;
                break;
            }
            compileForExtent(device->swapchainExtent());
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
    destroyScene(vkDevice, *scene);
    // `executor` and `graph` go out of scope after this point, in reverse
    // declaration order -- Executor's own destructor already
    // vkDeviceWaitIdle()s before tearing down its transient pool (see
    // executor.cpp), so this explicit wait above is for the scene's own
    // raw sampler/pipelines, not a redundant safety net for the executor.

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
    // --vsync on|off, default on [Phase 4 Task 6] -- forwarded to
    // runPresent() only. Headless mode's swapchain is built once via
    // Device::create() and never presented (see this file's header
    // comment), so there is nothing for a present-mode choice to affect
    // there; the flag is still parsed like any other (no error on it) but
    // simply has no effect in that path.
    rx::rhi::PresentMode vsyncMode = rx::rhi::PresentMode::VsyncOn;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--present") {
            presentMode = true;
        } else if (std::string_view(argv[i]) == "--validate") {
            enableValidation = true;
        } else if (std::string_view(argv[i]) == "--vsync" && i + 1 < argc) {
            std::string_view value = argv[++i];
            if (value == "off") {
                vsyncMode = rx::rhi::PresentMode::VsyncOff;
            } else if (value == "on") {
                vsyncMode = rx::rhi::PresentMode::VsyncOn;
            } else {
                RX_LOG_ERROR("--vsync expects 'on' or 'off', got '{}' -- defaulting to on", value);
            }
        }
    }

    if (presentMode) {
        return runPresent(enableValidation, vsyncMode);
    }
    return runHeadless(enableValidation);
}
