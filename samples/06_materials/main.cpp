// Sample 06: materials -- the Phase 3 showcase for rx_material's PUBLIC
// COM-lite API (rx_api.h), driven end to end: material loading, instance
// creation, per-instance parameter overrides, texture creation, and
// hot-reload polling ALL go through IRxMaterialSystem/IRxMaterial/
// IRxMaterialInstance/IRxTexture -- never an internal rx::material:: type,
// except inside the one clearly marked bridge section below (system
// creation, and the draw-time bindInstance()/getPipeline()/pipelineLayout()
// calls, which have no public equivalent in Phase 3 by design -- draw
// submission is not part of the ABI surface this phase, spec D5/D11).
//
// SCENE: 4 procedural objects (2 cubes, 2 spheres -- generators adapted from
// samples/03_bindless_mesh's own position+uv convention, extended with a
// per-vertex normal like samples/05_multipass's), drawn through a SINGLE
// rx_graph forward pass (one color + one depth attachment, no shadow/
// tonemap passes -- unlike 05_multipass, this sample's whole point is the
// material API, not the render graph). Two materials, `materials/
// checker.slang` (UV checker x `tint`) and `materials/rim.slang` (rim-light
// x `rimColor`), each instanced TWICE with different parameter values:
//   cube A   -- checker, tint   = warm orange (1.00, 0.55, 0.15)
//   cube B   -- checker, tint   = cool teal   (0.15, 0.85, 0.90)
//   sphere A -- rim,     rimColor = magenta    (0.95, 0.15, 0.85)
//   sphere B -- rim,     rimColor = golden yellow (0.95, 0.85, 0.10)
// checker.slang also takes a bindless texture-index parameter
// (`checkerTexIndex`), bound via a real, procedurally generated checker
// texture created through the PUBLIC IRxMaterialSystem::createTexture2D()
// and IRxMaterialInstance::setTexture() -- exercising that public path end
// to end. See checker.slang's own header comment for exactly why that
// texture is read (numerically) rather than sampled from inside the
// material's own evaluate(): Phase 3's material reflection contract
// supports exactly one ParameterBlock<TParams> global per material and
// nothing else, so a material module has no way to declare its own
// bindless Texture2D[] array to sample through yet -- the same documented
// scope boundary src/rx_material/tests/data/test_textured.slang's own
// `albedoIndex` field already established.
//
// THE CENTRAL ENGINEERING PROBLEM THIS SAMPLE SOLVES -- NO CAMERA TRANSFORM
// IN THE SHARED MATERIAL PIPELINE: shaders/material/forward_entry.slang's
// shared vertexMain has no model/view/projection transform at all (its own
// "PHASE 3 SCOPE NOTE" says so explicitly) -- it writes
// `clipPosition = float4(position, 1.0)` and `worldPos = position` from the
// SAME incoming POSITION attribute, verbatim. There is no way for a
// material module (checker.slang/rim.slang) to inject a transform either:
// MaterialSystem's own reflection rejects any top-level global other than
// `gParams`, and rx_api.h's IRxMaterialInstance has no setMatrix() to push a
// per-instance MVP through even if there were one. This sample is the first
// one to actually need a moving/orbiting camera through rx_material (Task 5
// through 7's own tests only ever exercise a single fixed pipeline bind,
// never a real draw with camera motion; samples/05_multipass sidesteps this
// entirely by NOT using rx_material at all, driving its own hand-written
// shaders/pipelines instead) -- so it resolves the gap the way
// forward_entry.slang's own comment anticipates ("a real sample wires an
// actual transform in before this stage"): by pre-transforming every
// vertex's POSITION into clip space and every vertex's NORMAL into VIEW
// space, on the CPU, before upload (transformAndUploadObjectVertices()
// below), using this sample's own camera view/projection matrices. This is
// possible without any precision loss specifically because the camera is
// ORTHOGRAPHIC: an orthographic projection matrix's last row is always
// (0, 0, 0, 1), so `proj * view * model * vec4(localPos, 1)` always has
// w == 1.0 EXACTLY, matching forward_entry.slang's own hardcoded
// `float4(position, 1.0)` assumption bit-for-bit -- a perspective camera
// could not do this (its w varies per-vertex, which this fixed vertex
// stage has no way to carry). See makeCameraPose()/
// transformAndUploadObjectVertices() below for the mechanism, and
// rim.slang's own header comment for why the normal is pre-rotated into
// VIEW space specifically (so its rim term can read `v.normal.z` directly
// as the cosine of the view angle, with no separate camera-position
// parameter needed at all).
//
// HEADLESS ANALYTIC PROBE DERIVATION (read together with checker.slang's
// and rim.slang's own header comments): the camera is a fixed ORTHOGRAPHIC
// camera at azimuth 0 (eye at (0, 0, kOrbitRadius), looking straight down
// -Z, up = +Y) for the whole headless run. Two facts this camera's own
// geometry makes EXACT, not approximate:
//   (a) Orthographic projection is invariant to a shift purely along the
//       view axis -- it never changes a projected point's screen X/Y, only
//       its depth. So probing at an object's own world-space CENTER
//       (`projectToPixel(objectPosition, ...)`) lands on the exact same
//       pixel as probing at whichever surface point the camera actually
//       sees there (the visible front face for a cube, the near point for
//       a sphere) -- there is no need to hand-derive a separate "visible
//       surface point" formula the way a perspective camera would require.
//   (b) Every cube in this scene is unrotated (model = pure translation)
//       and every face's UV corners are laid out (0,0)-(1,0)-(1,1)-(0,1)
//       symmetrically (see generateCube() below) -- so the quad facing the
//       camera at azimuth 0 (the local +Z face, closest to the eye at
//       (0,0,+R)) has its geometric CENTER at UV (0.5, 0.5) exactly.
//       checker.slang's own kCellsPerAxis = 3 grid puts that exact point
//       robustly inside the grid's middle cell (parity 0, "light", 0.5-cell
//       margin either side) -- giving `expected color == tint` exactly (see
//       expectedCheckerColor() below).
//   (c) A sphere's own near point (the surface point closest to an external
//       camera) always lies exactly along the line from the sphere's center
//       to the camera, by definition -- so its LOCAL normal at that point
//       equals the unit vector from center to camera, which, after this
//       sample's own view-space normal pre-rotation (see the header comment
//       above), becomes exactly (0, 0, 1) in view space: ndotv == 1.0
//       exactly, independent of camera azimuth (true for every point on
//       this sample's orbit, not just azimuth 0 -- see rim.slang's own
//       comment). The mesh is a finite tessellation (not a perfect
//       analytic sphere), so the ACTUAL rendered ndotv at that pixel is
//       only very close to, not bit-identical to, 1.0 -- negligible given
//       this sample's fine tessellation (kSphereRings/kSphereSegments
//       below) and the tolerance-based (not exact-equality) pixel match
//       this file's own headless gate uses (matchesExpectedColorChannelExact()).
//
// SRGB SWAPCHAIN CAVEAT [found by samples/04_streaming, cited here rather
// than re-derived]: this project's swapchain/offscreen-readback format
// resolves to an _SRGB-suffixed VkFormat on at least one real driver this
// codebase has been developed against, which makes the GPU apply the
// linear -> sRGB transfer function to every fragment-shader output before
// it lands in the framebuffer -- so a probed byte can be either the raw
// linear-to-byte value OR its sRGB-encoded counterpart, depending on the
// format the current machine's driver actually handed back. This file
// checks against BOTH (matchesExpectedColorChannelExact() below), exactly
// like 04_streaming's own matchesExpectedColor() -- but, per this task's
// own brief ("channel-order-exact per 05's probe pattern"), resolves the
// REAL R/G/B byte order first (channelIndicesForFormat(), copied from
// samples/05_multipass) rather than accepting either byte order the way
// 04_streaming's own order-agnostic check does: this sample needs to tell
// four genuinely different expected colors apart, not just "some non-black
// pixel showed up."
//
// Two modes, exactly like every other sample in this codebase:
//   (default / no args) Headless correctness gate: fixed camera, 3 frames
//   through the graph (mirroring 05_multipass's own "repeated execute()
//   calls against the same reused offscreen backbuffer" argument -- see
//   that sample's header comment for why this is safe), readback, and the
//   four analytic pixel assertions above. Registered as ctest
//   sample_06_materials_headless.
//   --present. Real window, the camera orbiting continuously (azimuth-only,
//   fixed radius/elevation -- see makeCameraPose()), polling
//   materials/checker.slang and materials/rim.slang for on-disk changes
//   roughly once a second and calling the PUBLIC
//   IRxMaterialSystem::reloadChanged() (samples/02_hotreload's own
//   keep-last-good UX, applied here to materials instead of a raw
//   pipeline). See this file's own recordDraws() for an ADDITIONAL,
//   sample-local keep-last-good safety net: a hot-reloaded edit that
//   changes a material's PARAMETER BLOCK SHAPE (adds/removes/retypes a
//   `gParams` field) is out of this sample's documented hot-reload scope
//   (matching src/rx_material/tests/test_material_system.cpp's own
//   reload-fixture convention of only ever changing a material's MATH, not
//   its params' shape, across reload versions) -- rather than let that
//   crash a running --present session, that one object's draw is skipped
//   for the frame and the failure is logged. Also accepts --vsync on|off
//   (default on) [Phase 4 Task 6]: forwarded into Device::setPresentMode()
//   + recreateSwapchain() right after Device::create(), before any
//   per-swapchain-image resource is built. Has no effect in headless mode,
//   whose swapchain is built once and never presented -- the flag still
//   parses there, it just has nothing to apply to.
#include <rx_material/rx_api.h>  // PUBLIC surface -- used throughout this file below.

#include <rx_core/log.h>
#include <rx_core/profile.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_rhi_vk/upload.h>
#include <rx_task/scheduler.h>

// ============================================================================
// rx_material INTERNAL BRIDGE -- #include section (BEGIN)
// ============================================================================
// The ONLY two rx_material headers this file includes other than
// rx_api.h -- confined here, and to the `materialBridge` namespace further
// down (also clearly marked), per this task's own binding constraint: every
// #include <rx_material/...> other than rx_api.h sits in this bridge, and no
// IRx* object is ever cast to an internal type outside it. Both uses are
// exactly the two the coordinator's brief carves out as having no public
// equivalent in Phase 3 (D5/D11 -- draw submission is not part of the public
// ABI surface this phase): (1) SYSTEM CREATION -- rx_api.h's own factory
// (rxCreateMaterialSystem) takes an opaque `void*` bridge pointer to an
// already-constructed rx::material::MaterialSystem*, per RxMaterialSystemDesc's
// own doc comment, so *something* has to call MaterialSystem::create()
// itself; (2) DRAW-TIME BINDING -- MaterialSystem::bindInstance()/
// getPipeline()/pipelineLayout() are the only way to actually resolve a
// VkPipeline and bind a material instance's parameters for a real
// vkCmdDrawIndexed call, and rx_api_detail.h's materialHandle()/
// materialInstanceBlobData()/materialInstanceBlobSize() are the only way to
// get from a public IRxMaterial*/IRxMaterialInstance* (obtained entirely
// through rx_api.h) to the internal rx::material::MaterialHandle/raw
// parameter bytes those three methods need.
#include <rx_material/material_system.h>
#include <rx_material/rx_api_detail.h>
// ============================================================================
// rx_material INTERNAL BRIDGE -- #include section (END). The bridge CODE
// itself lives in `namespace materialBridge` below (also marked BEGIN/END)
// -- nothing outside those two marked regions in this file ever names an
// rx_material-internal type or includes an rx_material header other than
// rx_api.h.
// ============================================================================

// GLM is a PUBLIC rx_core dependency, transitively available here, matching
// every other sample's identical comment; GLM_FORCE_DEPTH_ZERO_TO_ONE must
// precede GLM's first include in this TU.
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
#include <exception>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// --- Small local RAII helper for rx_api.h's COM-lite refcounted objects ---
// No such helper exists anywhere in rx_material today (every existing test
// manages IRx* lifetime with plain raw pointers + explicit release() calls
// -- see e.g. src/rx_material/tests/test_api_factory.cpp) -- this sample is
// the first REAL, sustained draw-loop consumer of the public API (as
// opposed to a short, linear test case), so a tiny move-only wrapper here
// (local to this file, not part of rx_material's own surface) buys the same
// leak-safety every other RAII type in this codebase already has, without
// changing rx_material's own ABI-discipline surface at all. Never casts,
// never touches an internal type -- purely calls release()/addRef() through
// IRxUnknown's own public vtable.
template <typename T>
class RxRef {
public:
    RxRef() = default;
    explicit RxRef(T* ptr) : ptr_(ptr) {}  // Takes ownership of an existing +1 ref (factory/create*() convention).
    RxRef(const RxRef&) = delete;
    RxRef& operator=(const RxRef&) = delete;
    RxRef(RxRef&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    RxRef& operator=(RxRef&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }
    ~RxRef() { reset(); }

    void reset() {
        if (ptr_ != nullptr) {
            ptr_->release();
            ptr_ = nullptr;
        }
    }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

// --- Tunables --------------------------------------------------------------

constexpr uint32_t kHeadlessWidth = 256;
constexpr uint32_t kHeadlessHeight = 256;
constexpr VkDeviceSize kHeadlessPixelBytes = static_cast<VkDeviceSize>(kHeadlessWidth) * kHeadlessHeight * 4;
constexpr uint32_t kHeadlessFrameCount = 3;

constexpr uint32_t kPresentWidth = 900;
constexpr uint32_t kPresentHeight = 700;
constexpr float kOrbitAzimuthRadPerSec = 0.35F;
constexpr auto kMaterialPollInterval = std::chrono::seconds(1);  // brief: "~1s", not 02_hotreload's ~250ms.

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// --- Scene layout -----------------------------------------------------------
// See this file's own header comment for the analytic reasoning this
// arrangement (camera + object placement) makes exact.

constexpr float kCubeHalfExtent = 0.85F;
constexpr float kSphereRadius = 0.85F;
constexpr uint32_t kSphereRings = 24;
constexpr uint32_t kSphereSegments = 32;

enum class Shape : uint32_t { Cube = 0, Sphere = 1 };
enum class MaterialKind : uint32_t { Checker = 0, Rim = 1 };

struct ObjectSpec {
    Shape shape;
    MaterialKind materialKind;
    glm::vec3 position;
    std::array<float, 4> param4;  // "tint" (Checker) or "rimColor" (Rim), by name -- see createScene().
};

// cube A/B get `checker`; sphere A/B get `rim` -- checker's UV pattern reads
// best on a cube's flat faces, rim's view-dependent glow reads best on a
// sphere's curved surface, and each material still gets its own two
// differently-parameterized instances either way.
constexpr std::array<ObjectSpec, 4> kObjectSpecs{{
    {Shape::Cube, MaterialKind::Checker, {-2.0F, 1.3F, 0.0F}, {1.00F, 0.55F, 0.15F, 1.0F}},    // checker A -- warm orange
    {Shape::Cube, MaterialKind::Checker, {2.0F, 1.3F, 0.0F}, {0.15F, 0.85F, 0.90F, 1.0F}},      // checker B -- cool teal
    {Shape::Sphere, MaterialKind::Rim, {-2.0F, -1.3F, 0.0F}, {0.95F, 0.15F, 0.85F, 1.0F}},      // rim A -- magenta
    {Shape::Sphere, MaterialKind::Rim, {2.0F, -1.3F, 0.0F}, {0.95F, 0.85F, 0.10F, 1.0F}},       // rim B -- golden yellow
}};
constexpr uint32_t kObjectCount = static_cast<uint32_t>(kObjectSpecs.size());

// --- Camera: fixed-radius/elevation orbit, azimuth-only (see this file's
// header comment on why an ORTHOGRAPHIC camera is load-bearing here, not
// just a style choice) ------------------------------------------------------

constexpr float kOrbitRadius = 8.0F;
constexpr float kOrbitNear = 4.0F;
constexpr float kOrbitFar = 12.0F;
constexpr float kCameraOrthoHalfWidth = 3.3F;
constexpr float kCameraOrthoHalfHeight = 2.6F;
constexpr float kHeadlessAzimuth = 0.0F;  // eye at (0, 0, kOrbitRadius), looking straight down -Z.

// Vulkan clip space is Y-down; glm::ortho assumes OpenGL's Y-up NDC -- the
// same standard fix every other sample in this codebase applies to its own
// projection matrix.
glm::mat4 applyVulkanYFlip(glm::mat4 proj) {
    proj[1][1] *= -1.0F;
    return proj;
}

glm::vec3 cameraEyeForAzimuth(float azimuthRadians) {
    return glm::vec3(std::sin(azimuthRadians) * kOrbitRadius, 0.0F, std::cos(azimuthRadians) * kOrbitRadius);
}

struct CameraPose {
    glm::mat4 viewProj;
    glm::mat3 viewRotation;
};

// `viewportWidth`/`viewportHeight` only affect the ASPECT-CORRECTED
// half-extent (letterbox-fit, same convention samples/04_streaming/
// 05_multipass already use for their own orthographic cameras) -- never the
// camera's own position/orientation, which is a pure function of `azimuth`.
CameraPose makeCameraPose(float azimuthRadians, uint32_t viewportWidth, uint32_t viewportHeight) {
    const glm::vec3 eye = cameraEyeForAzimuth(azimuthRadians);
    // up = +Y is never parallel to the view direction at any azimuth (the
    // view direction stays exactly horizontal, elevation 0, for this
    // sample's whole orbit) -- no degenerate glm::lookAt case to guard.
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));

    float halfWidth = kCameraOrthoHalfWidth;
    float halfHeight = kCameraOrthoHalfHeight;
    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
    const float contentAspect = halfWidth / halfHeight;
    if (aspect > contentAspect) {
        halfWidth = halfHeight * aspect;
    } else {
        halfHeight = halfWidth / aspect;
    }
    const glm::mat4 proj = applyVulkanYFlip(glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, kOrbitNear, kOrbitFar));

    CameraPose pose;
    pose.viewProj = proj * view;
    pose.viewRotation = glm::mat3(view);
    return pose;
}

struct PixelCoord {
    uint32_t x;
    uint32_t y;
};

// Projects `worldPos` through `viewProj` to a real screen pixel. Orthographic
// projection never touches w (see this file's header comment) -- clip.w is
// always exactly 1.0 here, so this never needs a perspective divide.
PixelCoord projectToPixel(glm::vec3 worldPos, const glm::mat4& viewProj, uint32_t width, uint32_t height) {
    const glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0F);
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    const float pixelX = (ndcX * 0.5F + 0.5F) * static_cast<float>(width);
    const float pixelY = (ndcY * 0.5F + 0.5F) * static_cast<float>(height);
    const uint32_t x = static_cast<uint32_t>(std::clamp(pixelX, 0.0F, static_cast<float>(width) - 1.0F));
    const uint32_t y = static_cast<uint32_t>(std::clamp(pixelY, 0.0F, static_cast<float>(height) - 1.0F));
    return {x, y};
}

// --- Procedural geometry: position + normal + uv (03_bindless_mesh's own
// position+uv convention, extended with 05_multipass's per-vertex normal --
// this sample's checker.slang needs uv, rim.slang needs normal, and both
// need to conform to rx_material's OWN fixed vertex-input layout, byte-for-
// byte -- see the Vertex struct's own static_assert below and forward_entry.
// slang's "VERTEX INPUT LAYOUT" header comment for the contract this must
// match exactly: MaterialSystem::getPipeline() builds its
// VkVertexInputAttributeDescription table internally, from this exact
// struct shape, not from anything this file declares to it directly. ------

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};
static_assert(sizeof(Vertex) == 32,
              "Vertex must byte-match rx_material's internal MaterialVertexLayout (material_system.cpp) exactly -- "
              "tightly packed float3 position + float3 normal + float2 uv, no padding, matching forward_entry.slang's "
              "own vertexMain(position, normal, uv) parameter list.");

struct HostMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Adds one quad (two triangles) with per-corner UV (0,0)-(1,0)-(1,1)-(0,1)
// -- the SAME parameterization on every face, which is exactly what makes
// every face's own geometric center land at UV (0.5, 0.5) (see this file's
// header comment, point (b)).
void addQuad(HostMesh& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 normal) {
    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    const std::array<glm::vec3, 4> corners{a, b, c, d};
    constexpr std::array<std::array<float, 2>, 4> kCornerUv{{{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}};
    for (size_t i = 0; i < corners.size(); ++i) {
        Vertex v{};
        v.position[0] = corners[i].x;
        v.position[1] = corners[i].y;
        v.position[2] = corners[i].z;
        v.normal[0] = normal.x;
        v.normal[1] = normal.y;
        v.normal[2] = normal.z;
        v.uv[0] = kCornerUv[i][0];
        v.uv[1] = kCornerUv[i][1];
        mesh.vertices.push_back(v);
    }
    // Winding [empirically verified, not assumed -- see this file's own
    // development history]: unlike every earlier sample (01-05, all of
    // which set VK_CULL_MODE_NONE for their own hand-built pipelines),
    // MaterialSystem's fixed rasterization state (material_system.cpp)
    // enables VK_CULL_MODE_BACK_BIT with VK_FRONT_FACE_COUNTER_CLOCKWISE --
    // the first sample in this codebase where a mesh's winding actually
    // matters. Combined with this file's own applyVulkanYFlip() (needed for
    // the projection matrix regardless), the winding that reads as
    // "counter-clockwise, outward-facing" in a plain Y-up authoring sense
    // reverses to CLOCKWISE in Vulkan's own framebuffer-space convention --
    // confirmed directly: with the OTHER winding, rim.slang's own
    // view-dependent probe read back the mesh's FAR side (ndotv == -1) at
    // every headless probe pixel, not the near side, while checker.slang's
    // own probe still happened to pass regardless (every cube face shares
    // the identical UV parameterization, front or back, so that probe
    // alone could not have caught this). (base, base+2, base+1) / (base,
    // base+3, base+2) -- reversed from the "natural" (base, base+1, base+2)
    // / (base, base+2, base+3) -- is what actually renders each face's
    // intended (outward, camera-facing) side.
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 3);
    mesh.indices.push_back(base + 2);
}

// Half-extent `h` cube, local-space, unrotated. The 5th face declared here
// (local +Z, normal (0,0,1)) is the one facing the camera at headless
// azimuth 0 (eye at (0,0,+kOrbitRadius) looking down -Z) -- its own
// geometric center, (0,0,h) in local space, is exactly where this file's
// header comment's point (b) applies.
HostMesh generateCube(float h) {
    HostMesh mesh;
    addQuad(mesh, {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h}, {1, 0, 0});        // +X
    addQuad(mesh, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h}, {-1, 0, 0});    // -X
    addQuad(mesh, {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}, {0, 1, 0});         // +Y
    addQuad(mesh, {-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {0, -1, 0});    // -Y
    addQuad(mesh, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});         // +Z -- faces the camera at azimuth 0.
    addQuad(mesh, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});    // -Z
    return mesh;
}

// Standard UV sphere centered at the origin -- normal = normalized position
// exactly (every point on an origin-centered sphere has its outward normal
// exactly along its own position vector). `u`/`v` (longitude/colatitude,
// [0,1]^2) are stored per-vertex too, even though rim.slang never reads
// them for shading -- checker.slang could be pointed at a sphere just as
// well, and a UV sphere is the established convention
// (samples/03_bindless_mesh/05_multipass) either way.
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
            vert.uv[0] = u;
            vert.uv[1] = v;
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
            // Winding reversed from samples/05_multipass's own identical
            // sphere generator -- see addQuad()'s own comment for why this
            // sample (the first with real backface culling active) needs
            // the opposite winding convention.
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i3);
            mesh.indices.push_back(i2);
        }
    }
    return mesh;
}

// Small procedural checkerboard texture's raw RGBA8 pixels -- created via
// the PUBLIC IRxMaterialSystem::createTexture2D() (see createScene() below)
// and bound via IRxMaterialInstance::setTexture() to exercise that public
// path end to end [binding global constraint]. See checker.slang's own
// header comment for why its CONTENT never actually reaches the rendered
// image (Phase 3 materials cannot sample a bindless texture from inside
// their own evaluate() yet) -- generated as a genuine checkerboard anyway,
// for the same reason a test fixture still exercises real behavior even
// when only part of its result is currently observable.
std::vector<uint8_t> generateCheckerTexturePixels(uint32_t size, uint32_t cellSize) {
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const uint32_t cellX = x / cellSize;
            const uint32_t cellY = y / cellSize;
            const uint8_t value = ((cellX + cellY) % 2 == 0) ? 255 : 0;
            const size_t idx = (static_cast<size_t>(y) * size + x) * 4;
            pixels[idx + 0] = value;
            pixels[idx + 1] = value;
            pixels[idx + 2] = value;
            pixels[idx + 3] = 255;
        }
    }
    return pixels;
}

// ============================================================================
// rx_material INTERNAL BRIDGE -- namespace materialBridge (BEGIN)
// ============================================================================
// Every function below is the ONLY code in this file that names an
// rx_material-internal type (rx::material::MaterialSystem/MaterialHandle/
// InstanceBinding) or calls rx_api_detail.h's bridge accessors. Everything
// OUTSIDE this namespace (Scene, createScene(), recordDraws(), main(), ...)
// only ever touches `MaterialSystemHandle` below, whose own DEFINITION
// stores the internal system as an opaque `void*` -- so even a caller that
// holds one of these handles never has the internal type NAME in scope,
// let alone casts anything to it. That is deliberately stricter than the
// letter of this task's acceptance criterion ("no IRx* object is ever cast
// to an internal type outside [the bridge]") -- it also keeps the internal
// type's NAME itself out of every other struct/function signature in this
// file, which is a stronger, easier-to-grep version of the same rule.
namespace materialBridge {

struct MaterialSystemHandle {
    void* internal = nullptr;                // rx::material::MaterialSystem*, owned by this handle.
    IRxMaterialSystem* publicApi = nullptr;   // refcount 1; released by destroySystem() below.
};

// SYSTEM CREATION bridge: builds the real internal rx::material::
// MaterialSystem (material_system.h's own create(), including this sample's
// resolved `sharedShaderDir` -- see main()'s own comment on why this is a
// real runtime path here, not RX_MATERIAL_SHADER_DIR's compile-time
// default), then bridges it into a public IRxMaterialSystem via
// RxMaterialSystemDesc + rxCreateMaterialSystem() exactly as rx_api.h's own
// factory doc documents. Returns std::nullopt (logged) on either step's
// failure -- callers never end up holding a half-constructed handle.
std::optional<MaterialSystemHandle> createSystem(rx::rhi::Device& device, rx::rhi::BindlessTable& bindless,
                                                  const std::filesystem::path& pipelineCachePath,
                                                  const std::filesystem::path& sharedShaderDir) {
    auto internalSystem = rx::material::MaterialSystem::create(device, bindless, pipelineCachePath, sharedShaderDir);
    if (internalSystem == nullptr) {
        RX_LOG_ERROR("sample_06_materials: rx::material::MaterialSystem::create failed");
        return std::nullopt;
    }

    RxMaterialSystemDesc desc{internalSystem.get()};
    IRxMaterialSystem* publicSystem = nullptr;
    if (rxCreateMaterialSystem(&desc, &publicSystem) != RX_OK || publicSystem == nullptr) {
        RX_LOG_ERROR("sample_06_materials: rxCreateMaterialSystem failed");
        return std::nullopt;
    }

    MaterialSystemHandle handle;
    handle.internal = internalSystem.release();  // ownership moves into this handle now.
    handle.publicApi = publicSystem;
    return handle;
}

// Releases the public wrapper (its own release() drops rx_api.h's own
// refcount to 0, destroying api_impl.cpp's MaterialSystemImpl -- that
// object does NOT own the internal system, so this is safe regardless of
// order relative to the line below) and destroys the internal
// rx::material::MaterialSystem -- whose own destructor waits for the
// device to go idle first (material_system.h's own destructor contract).
// Callers must call this BEFORE the rx::rhi::BindlessTable this handle was
// created against is destroyed (MaterialSystem::create()'s own doc: "the
// returned MaterialSystem does NOT take ownership [of bindless]... bindless
// must outlive this MaterialSystem") -- see destroyScene() below for where
// that ordering is actually enforced.
void destroySystem(MaterialSystemHandle& handle) {
    if (handle.publicApi != nullptr) {
        handle.publicApi->release();
        handle.publicApi = nullptr;
    }
    if (handle.internal != nullptr) {
        std::unique_ptr<rx::material::MaterialSystem> owned(static_cast<rx::material::MaterialSystem*>(handle.internal));
        handle.internal = nullptr;
        // `owned` destructs here.
    }
}

void beginFrame(MaterialSystemHandle& handle, uint32_t frameInFlightIndex, uint64_t frameNumber) {
    static_cast<rx::material::MaterialSystem*>(handle.internal)->beginFrame(frameInFlightIndex, frameNumber);
}

void onFrameCompleted(MaterialSystemHandle& handle, uint64_t completedFrameNumber) {
    static_cast<rx::material::MaterialSystem*>(handle.internal)->onFrameCompleted(completedFrameNumber);
}

// DRAW-TIME BINDING bridge: resolves `instance`'s pipeline for `ctx`'s own
// pass shape and binds it plus its set-1 parameter UBO (bindInstance() --
// lazily compiles via getPipeline() internally, exactly as that method's
// own doc describes), then returns the material's VkPipelineLayout so the
// caller can separately bind descriptor SET 0 (the bindless table) itself
// -- bindInstance() never touches set 0 by design (material_system.h's own
// comment: "the caller binds the owning rx::rhi::BindlessTable's own
// descriptor set once per frame/pass"). Throws exactly what bindInstance()/
// materialHandle()/materialInstanceBlobData() themselves document throwing
// (std::out_of_range for a stale MaterialHandle, std::invalid_argument for
// a paramSize mismatch, std::runtime_error for an exhausted parameter arena
// or a pipeline-build failure) -- recordDraws() below is this function's
// only caller, and it is the one that decides what "keep last good" means
// for a failure here (see that function's own comment).
VkPipelineLayout bindAndGetLayout(MaterialSystemHandle& handle, VkCommandBuffer cmd, const rx::graph::PassContext& ctx,
                                   IRxMaterial* material, IRxMaterialInstance* instance) {
    auto* system = static_cast<rx::material::MaterialSystem*>(handle.internal);
    rx::material::MaterialHandle materialHandle = rx::material::detail::materialHandle(material);
    rx::material::InstanceBinding binding{materialHandle, rx::material::detail::materialInstanceBlobData(instance),
                                           rx::material::detail::materialInstanceBlobSize(instance)};
    system->bindInstance(cmd, ctx, binding);
    return system->pipelineLayout(materialHandle);
}

}  // namespace materialBridge
// ============================================================================
// rx_material INTERNAL BRIDGE -- namespace materialBridge (END)
// ============================================================================

// --- Scene ------------------------------------------------------------------
// Every material creation/instance/texture call from here on is a PUBLIC
// rx_api.h call (IRxMaterialSystem/IRxMaterial/IRxMaterialInstance/
// IRxTexture) -- the only non-public thing anything below touches is
// `materialBridge::MaterialSystemHandle` itself, whose own definition is
// just a `{void*, IRxMaterialSystem*}` pair (see that struct's own comment).

struct ObjectGpu {
    Shape shape;
    glm::vec3 position;
    std::optional<rx::rhi::Buffer> vertexBuffer;  // sized 2x (rx::rhi::FrameSync::kFramesInFlight) for double-buffering.
    VkDeviceSize vertexBytesPerFrame = 0;
    IRxMaterial* material = nullptr;          // non-owning -- Scene::checkerMaterial/rimMaterial owns the +1 ref.
    IRxMaterialInstance* instance = nullptr;  // owning (+1 ref) -- released in destroyScene().
};

struct Scene {
    // std::optional, not a plain member: rx::rhi::BindlessTable's own
    // default constructor is private (BindlessTable::create() is the only
    // way to build a real one), so Scene has no accessible default to
    // construct it with directly -- the same reason MaterialSystem::Impl's
    // own Allocator/Uploader/ParamArena members are std::optional
    // (material_system.cpp). LOAD-BEARING here, not just style: this is
    // what lets a caller declare `Scene scene;` ONCE, in its own stack
    // frame, and pass `scene` BY REFERENCE into createScene() below to be
    // populated in place -- see createScene()'s own header comment for the
    // real bug this fixes (a returned-by-value Scene relocates its
    // BindlessTable to a new address the moment it moves, which is exactly
    // the address MaterialSystem::create() stores a raw pointer to for its
    // own entire lifetime).
    std::optional<rx::rhi::BindlessTable> bindlessTable;
    materialBridge::MaterialSystemHandle materialSystem;

    IRxMaterial* checkerMaterial = nullptr;
    IRxMaterial* rimMaterial = nullptr;
    IRxTexture* checkerTexture = nullptr;

    HostMesh cubeHostMesh;
    HostMesh sphereHostMesh;
    std::optional<rx::rhi::Buffer> cubeIndexBuffer;
    std::optional<rx::rhi::Buffer> sphereIndexBuffer;
    uint32_t cubeIndexCount = 0;
    uint32_t sphereIndexCount = 0;

    std::array<ObjectGpu, kObjectCount> objects;

    // Mutated once per frame before Executor::execute() runs, read back
    // inside recordDraws() -- same small-piece-of-reference-captured-mutable-
    // state convention samples/05_multipass's own Scene::
    // currentFrameInFlightIndex establishes, for the identical reason
    // (Pass::setExecute()'s callback signature carries no per-frame
    // parameter of its own).
    uint32_t currentFrameInFlightIndex = 0;
};

// Explicit, ordered teardown -- same discipline every other sample in this
// codebase uses (destroyScene() called before the owning Device/Context/
// Allocator/Executor go out of scope). Order matters here specifically:
// materialBridge::destroySystem() must run while `scene.bindlessTable` is
// STILL ALIVE (MaterialSystem::create()'s own contract: "bindless must
// outlive this MaterialSystem") -- calling it here, before this function
// returns and before `scene` itself is ever destructed, guarantees that
// regardless of Scene's own member declaration order.
void destroyScene(Scene& scene) {
    for (ObjectGpu& obj : scene.objects) {
        if (obj.instance != nullptr) {
            obj.instance->release();
            obj.instance = nullptr;
        }
        obj.material = nullptr;  // non-owning.
        obj.vertexBuffer.reset();
    }
    scene.cubeIndexBuffer.reset();
    scene.sphereIndexBuffer.reset();
    if (scene.checkerTexture != nullptr) {
        scene.checkerTexture->release();
        scene.checkerTexture = nullptr;
    }
    if (scene.checkerMaterial != nullptr) {
        scene.checkerMaterial->release();
        scene.checkerMaterial = nullptr;
    }
    if (scene.rimMaterial != nullptr) {
        scene.rimMaterial->release();
        scene.rimMaterial = nullptr;
    }
    materialBridge::destroySystem(scene.materialSystem);
    // scene.bindlessTable destructs via RAII once `scene` itself does --
    // safe now, since materialBridge::destroySystem() above already ran.
}

// Populates `scene` IN PLACE (an out-parameter, not a return value) --
// LOAD-BEARING, not a style choice: MaterialSystem::create() (called via
// materialBridge::createSystem() below) stores a raw pointer to whatever
// rx::rhi::BindlessTable reference it is given, for this MaterialSystem
// instance's ENTIRE remaining lifetime (material_system.h's own "bindless
// must outlive this MaterialSystem" contract). An earlier version of this
// function built a local `Scene` and returned it BY VALUE
// (`std::optional<Scene>`) -- which worked (and passed every visible check)
// right up until the first real MaterialSystem::reloadChanged() call, and
// only then failed with real Vulkan validation errors: the returned Scene
// relocates its `bindlessTable` member to the CALLER's own storage the
// moment the by-value return actually moves it, silently invalidating the
// raw pointer MaterialSystem::create() had already captured against the
// LOCAL (about-to-be-destroyed) copy -- reads through that now-dangling
// pointer happened to come back as all-zero bytes on this development
// machine, which is exactly why `bindless.descriptorSetLayout()` returned
// VK_NULL_HANDLE (silently skipping the external-set-0 substitution) only
// on the SECOND (reload-triggered) PipelineLayoutBuilder::build() call, not
// the first -- load-time compileMaterial() calls, made from INSIDE this
// function before the old by-value return ever ran, always saw the still-
// live local object. Confirmed directly (not guessed) by instrumenting
// PipelineLayoutBuilder::build() and MaterialSystem::reloadChanged()'s own
// call site during this task's own development and reproducing the exact
// VK_NULL_HANDLE readback before this fix. Taking `scene` by reference,
// populated by a caller that declares it ONCE in its own stack frame and
// never moves/returns it again, gives `bindlessTable` one single, stable
// address for its entire lifetime.
bool createScene(Scene& scene, VkPhysicalDevice physicalDevice, rx::rhi::Device& device, rx::rhi::Allocator& allocator,
                  rx::rhi::Uploader& uploader, const std::filesystem::path& sharedShaderDir,
                  const std::filesystem::path& materialsDir, const std::filesystem::path& pipelineCachePath) {
    rx::rhi::BindlessTable::Capacities capacities{/*sampledImages=*/4, /*samplers=*/1, /*storageBuffers=*/1};
    auto bindlessTable = rx::rhi::BindlessTable::create(physicalDevice, device.device(), capacities);
    if (!bindlessTable.has_value()) {
        RX_LOG_ERROR("sample_06_materials: BindlessTable::create failed");
        return false;
    }
    scene.bindlessTable = std::move(*bindlessTable);

    // ===== rx_material bridge: system creation (materialBridge, above) ====
    auto systemHandle =
        materialBridge::createSystem(device, *scene.bindlessTable, pipelineCachePath, sharedShaderDir);
    if (!systemHandle.has_value()) {
        return false;
    }
    scene.materialSystem = std::move(*systemHandle);

    // ===== Public rx_api.h calls from here on ===========================
    const std::string checkerPath = (materialsDir / "checker.slang").string();
    if (scene.materialSystem.publicApi->loadMaterial(checkerPath.c_str(), &scene.checkerMaterial) != RX_OK) {
        RX_LOG_ERROR("sample_06_materials: loadMaterial('{}') failed", checkerPath);
        destroyScene(scene);
        return false;
    }
    const std::string rimPath = (materialsDir / "rim.slang").string();
    if (scene.materialSystem.publicApi->loadMaterial(rimPath.c_str(), &scene.rimMaterial) != RX_OK) {
        RX_LOG_ERROR("sample_06_materials: loadMaterial('{}') failed", rimPath);
        destroyScene(scene);
        return false;
    }

    const std::vector<uint8_t> checkerPixels = generateCheckerTexturePixels(/*size=*/8, /*cellSize=*/2);
    RxTextureDesc textureDesc{};
    textureDesc.pixels = checkerPixels.data();
    textureDesc.pixelBytes = checkerPixels.size();
    textureDesc.width = 8;
    textureDesc.height = 8;
    textureDesc.format = RX_FORMAT_RGBA8_UNORM;
    textureDesc.generateMips = 0;
    if (scene.materialSystem.publicApi->createTexture2D(&textureDesc, &scene.checkerTexture) != RX_OK) {
        RX_LOG_ERROR("sample_06_materials: createTexture2D failed");
        destroyScene(scene);
        return false;
    }

    scene.cubeHostMesh = generateCube(kCubeHalfExtent);
    scene.sphereHostMesh = generateSphere(kSphereRadius, kSphereRings, kSphereSegments);

    auto uploadIndexBuffer = [&](const HostMesh& mesh) -> std::optional<rx::rhi::Buffer> {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(mesh.indices.size()) * sizeof(uint32_t);
        auto buffer =
            allocator.createDeviceLocalBuffer(bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (!buffer.has_value()) {
            return std::nullopt;
        }
        uploader.uploadToBuffer(*buffer, 0, mesh.indices.data(), bytes);
        return buffer;
    };
    auto cubeIndexBuffer = uploadIndexBuffer(scene.cubeHostMesh);
    auto sphereIndexBuffer = uploadIndexBuffer(scene.sphereHostMesh);
    if (!cubeIndexBuffer.has_value() || !sphereIndexBuffer.has_value()) {
        RX_LOG_ERROR("sample_06_materials: createDeviceLocalBuffer/uploadToBuffer failed for an index buffer");
        destroyScene(scene);
        return false;
    }
    scene.cubeIndexBuffer = std::move(cubeIndexBuffer);
    scene.sphereIndexBuffer = std::move(sphereIndexBuffer);
    scene.cubeIndexCount = static_cast<uint32_t>(scene.cubeHostMesh.indices.size());
    scene.sphereIndexCount = static_cast<uint32_t>(scene.sphereHostMesh.indices.size());
    uploader.flush();

    for (size_t i = 0; i < kObjectSpecs.size(); ++i) {
        const ObjectSpec& spec = kObjectSpecs[i];
        ObjectGpu& obj = scene.objects[i];
        obj.shape = spec.shape;
        obj.position = spec.position;
        obj.material = (spec.materialKind == MaterialKind::Checker) ? scene.checkerMaterial : scene.rimMaterial;

        if (obj.material->createInstance(&obj.instance) != RX_OK) {
            RX_LOG_ERROR("sample_06_materials: createInstance failed for object {}", i);
            destroyScene(scene);
            return false;
        }

        if (spec.materialKind == MaterialKind::Checker) {
            if (obj.instance->setFloat4("tint", spec.param4.data()) != RX_OK) {
                RX_LOG_ERROR("sample_06_materials: setFloat4('tint') failed for object {}", i);
                destroyScene(scene);
                return false;
            }
            if (obj.instance->setTexture("checkerTexIndex", scene.checkerTexture) != RX_OK) {
                RX_LOG_ERROR("sample_06_materials: setTexture('checkerTexIndex') failed for object {}", i);
                destroyScene(scene);
                return false;
            }
        } else {
            if (obj.instance->setFloat4("rimColor", spec.param4.data()) != RX_OK) {
                RX_LOG_ERROR("sample_06_materials: setFloat4('rimColor') failed for object {}", i);
                destroyScene(scene);
                return false;
            }
        }

        const HostMesh& hostMesh = (spec.shape == Shape::Cube) ? scene.cubeHostMesh : scene.sphereHostMesh;
        const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(hostMesh.vertices.size()) * sizeof(Vertex);
        obj.vertexBytesPerFrame = vertexBytes;
        auto vertexBuffer = allocator.createDeviceLocalBuffer(
            vertexBytes * rx::rhi::FrameSync::kFramesInFlight, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (!vertexBuffer.has_value()) {
            RX_LOG_ERROR("sample_06_materials: createDeviceLocalBuffer failed for object {}'s vertex buffer", i);
            destroyScene(scene);
            return false;
        }
        obj.vertexBuffer = std::move(vertexBuffer);
    }

    return true;
}

// Transforms `object`'s host mesh into clip-space positions + view-space
// normals (see this file's header comment for why) and uploads the result
// into frame slot `frameInFlightIndex`'s half of its double-buffered vertex
// buffer. `pose.viewRotation` alone (not the full model matrix) is applied
// to normals -- correct because every object here only ever TRANSLATES
// (model = pure translation, per kObjectSpecs), so object-space normals
// equal world-space normals already, mirroring samples/05_multipass's
// identical "translation-only model matrix" simplification.
void transformAndUploadObjectVertices(rx::rhi::Uploader& uploader, const HostMesh& hostMesh, ObjectGpu& object,
                                       uint32_t frameInFlightIndex, const CameraPose& pose) {
    std::vector<Vertex> transformed(hostMesh.vertices.size());
    for (size_t i = 0; i < transformed.size(); ++i) {
        const Vertex& src = hostMesh.vertices[i];
        const glm::vec3 worldPos = glm::vec3(src.position[0], src.position[1], src.position[2]) + object.position;
        const glm::vec4 clip = pose.viewProj * glm::vec4(worldPos, 1.0F);
        const glm::vec3 localNormal(src.normal[0], src.normal[1], src.normal[2]);
        const glm::vec3 viewNormal = glm::normalize(pose.viewRotation * localNormal);

        Vertex& dst = transformed[i];
        dst.position[0] = clip.x;
        dst.position[1] = clip.y;
        dst.position[2] = clip.z;
        dst.normal[0] = viewNormal.x;
        dst.normal[1] = viewNormal.y;
        dst.normal[2] = viewNormal.z;
        dst.uv[0] = src.uv[0];
        dst.uv[1] = src.uv[1];
    }
    const VkDeviceSize dstOffset = static_cast<VkDeviceSize>(frameInFlightIndex) * object.vertexBytesPerFrame;
    uploader.uploadToBuffer(*object.vertexBuffer, dstOffset, transformed.data(), object.vertexBytesPerFrame);
}

void updateFrame(rx::rhi::Uploader& uploader, Scene& scene, uint32_t frameInFlightIndex, float azimuthRadians,
                  uint32_t viewportWidth, uint32_t viewportHeight) {
    scene.currentFrameInFlightIndex = frameInFlightIndex;
    const CameraPose pose = makeCameraPose(azimuthRadians, viewportWidth, viewportHeight);
    for (size_t i = 0; i < kObjectSpecs.size(); ++i) {
        ObjectGpu& obj = scene.objects[i];
        const HostMesh& hostMesh = (obj.shape == Shape::Cube) ? scene.cubeHostMesh : scene.sphereHostMesh;
        transformAndUploadObjectVertices(uploader, hostMesh, obj, frameInFlightIndex, pose);
    }
    uploader.flush();
}

// --- Render-graph declaration + draw recording ------------------------------

rx::graph::AttachmentDesc swapchainRelativeDesc(VkFormat format) {
    rx::graph::AttachmentDesc desc;
    desc.format = format;
    desc.sizeClass = rx::graph::SizeClass::SwapchainRelative;
    desc.width = 1.0F;
    desc.height = 1.0F;
    return desc;
}

// Phase 4 Task 7 [spec D4 amendment]: CHUNKED -- migrated from a single
// whole-pass setExecute() callback to setExecuteChunked() (declareGraph()
// below), but with ALL of its real work deliberately kept in chunk 0 alone
// (chunks [1, chunkCount) are legitimate, intentional no-ops for this
// sample). This is NOT a half-measure: rx::material::MaterialSystem::
// bindInstance() -- which bindAndGetLayout() below calls for every object,
// every frame, to resolve its pipeline and stream its per-frame parameter
// UBO -- is main-thread-only, no exception (docs/threading.md: "every other
// method on this type"), and rx_material exposes no split "resolve on main,
// record the bind commands anywhere" API this task's scope could build on
// (bindInstance() couples both into one call, deliberately, per Phase 3's
// own design). setExecuteChunked()'s chunk 0 is the one place this pass CAN
// safely call it: chunk 0 is guaranteed to run synchronously, on the
// calling (main) thread, before any other chunk begins [pass.h: Pass::
// setExecuteChunked()'s own doc comment] -- discovered load-bearing while
// writing this exact migration, and documented there in full.
//
// samples/05_multipass's forward pass has no such constraint (its own
// per-object work -- push constants + vertex/index binds + draws -- never
// touches MaterialSystem at all) and DOES fan real work out across every
// chunk; this sample's genuinely different constraint is exactly why the
// two migrations do not look alike, even though both use
// setExecuteChunked(). The parallel-recording INFRASTRUCTURE (secondary
// command buffers, per-thread pools, vkCmdExecuteCommands) is still fully
// exercised on every commit either way -- chunks 1..chunkCount-1 still get
// a real (empty) secondary recorded and stitched in.
//
// Binds descriptor SET 0 (the bindless table) exactly once, right after the
// first object's own pipeline is bound -- safe because every material this
// sample loads shares the IDENTICAL VkDescriptorSetLayout for set 0 (the
// same `bindlessTable.descriptorSetLayout()` handle, passed to every
// MaterialSystem::loadMaterial()-driven PipelineLayoutBuilder::build() call
// internally), so set 0 stays Vulkan-compatible across every later
// vkCmdBindPipeline call bindAndGetLayout() issues for a DIFFERENT
// material, per the spec's pipeline-layout-compatibility rule.
//
// KEEP-LAST-GOOD, EXTENDED TO THE DRAW-TIME BIND ITSELF [see this file's
// header comment]: a hot-reloaded materials/checker.slang or materials/
// rim.slang edit that changes `gParams`'s FIELD SHAPE (not just its math)
// leaves an already-created IRxMaterialInstance holding a blob sized to the
// OLD shape, which bindAndGetLayout()'s own bindInstance() call would then
// reject (std::invalid_argument, paramSize mismatch) -- rather than let
// that propagate up through Executor::execute() and crash a running
// --present session, this loop catches it per-object, logs it, and skips
// only that object's draw for this one frame. Every OTHER object (sharing
// neither the edited file nor, in headless mode, ever exercising this path
// at all) keeps rendering normally.
void recordDrawsChunked(rx::graph::PassContext& ctx, Scene& scene, uint32_t chunkIndex, uint32_t /*chunkCount*/) {
    if (chunkIndex != 0) {
        // Nothing to do -- see this function's own header comment. The
        // executor still records (and stitches in) this chunk's own real,
        // if empty, secondary command buffer regardless.
        return;
    }

    VkCommandBuffer cmd = ctx.chunkCommandBuffer();

    VkViewport viewport{0.0F, 0.0F, static_cast<float>(ctx.renderArea.width), static_cast<float>(ctx.renderArea.height),
                         0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, ctx.renderArea};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    bool bindlessSetBound = false;
    for (ObjectGpu& object : scene.objects) {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        try {
            layout = materialBridge::bindAndGetLayout(scene.materialSystem, cmd, ctx, object.material, object.instance);
        } catch (const std::exception& e) {
            RX_LOG_ERROR("sample_06_materials: bindAndGetLayout failed for material '{}': {} -- skipping this "
                         "object's draw for this frame (keep-last-good)",
                         object.material->name(), e.what());
            continue;
        }

        if (!bindlessSetBound) {
            VkDescriptorSet bindlessSet = scene.bindlessTable->descriptorSet();
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, /*firstSet=*/0, 1, &bindlessSet, 0,
                                     nullptr);
            bindlessSetBound = true;
        }

        VkBuffer vertexBuffer = object.vertexBuffer->handle();
        VkDeviceSize vertexOffset = static_cast<VkDeviceSize>(scene.currentFrameInFlightIndex) * object.vertexBytesPerFrame;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &vertexOffset);

        const rx::rhi::Buffer& indexBuffer = (object.shape == Shape::Cube) ? *scene.cubeIndexBuffer : *scene.sphereIndexBuffer;
        const uint32_t indexCount = (object.shape == Shape::Cube) ? scene.cubeIndexCount : scene.sphereIndexCount;
        vkCmdBindIndexBuffer(cmd, indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
    }
}

void declareGraph(rx::graph::RenderGraph& graph, Scene& scene, VkFormat backbufferFormat) {
    graph.addPass("forward")
        .addColorOutput("backbuffer", swapchainRelativeDesc(backbufferFormat))
        .setDepthStencilOutput("depth", swapchainRelativeDesc(kDepthFormat))
        .setExecuteChunked([&scene](rx::graph::PassContext& ctx, uint32_t chunkIndex, uint32_t chunkCount) {
            recordDrawsChunked(ctx, scene, chunkIndex, chunkCount);
        });
    graph.setBackbufferSource("backbuffer");
}

// --- Headless probe derivation ----------------------------------------------
// See this file's own header comment for the full analytic reasoning; this
// section only turns that reasoning into concrete expected-byte tables.

uint8_t srgbEncodeByte(uint8_t linearByte) {
    const float v = static_cast<float>(linearByte) / 255.0F;
    const float encoded = (v <= 0.0031308F) ? (12.92F * v) : (1.055F * std::pow(v, 1.0F / 2.4F) - 0.055F);
    return static_cast<uint8_t>(std::clamp(encoded * 255.0F + 0.5F, 0.0F, 255.0F));
}

std::array<uint8_t, 3> toByteColor(glm::vec3 linear) {
    auto toByte = [](float c) { return static_cast<uint8_t>(std::clamp(c * 255.0F + 0.5F, 0.0F, 255.0F)); };
    return {toByte(linear.r), toByte(linear.g), toByte(linear.b)};
}

std::array<uint8_t, 3> srgbEncodeColor(const std::array<uint8_t, 3>& linear) {
    return {srgbEncodeByte(linear[0]), srgbEncodeByte(linear[1]), srgbEncodeByte(linear[2])};
}

int colorDistanceSq(const std::array<uint8_t, 3>& a, const std::array<uint8_t, 3>& b) {
    const int dr = static_cast<int>(a[0]) - static_cast<int>(b[0]);
    const int dg = static_cast<int>(a[1]) - static_cast<int>(b[1]);
    const int db = static_cast<int>(a[2]) - static_cast<int>(b[2]);
    return dr * dr + dg * dg + db * db;
}
constexpr int kMatchThresholdSq = 40 * 40;  // matches samples/03_bindless_mesh/04_streaming's own probe tolerance.

// Byte position of R/G/B within one texel of `format` -- copied from
// samples/05_multipass's own channelIndicesForFormat() (see that file's
// header comment for the full Vulkan-format-naming-convention rationale;
// not re-derived here). Returns std::nullopt (never a guessed mapping) for
// any other format.
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

// CHANNEL-ORDER-EXACT (per this task's own brief), not the order-agnostic
// as-is/reversed check samples/04_streaming's own matchesExpectedColor()
// uses -- this sample needs to tell four genuinely different colors apart,
// which an order-agnostic check cannot reliably do (see samples/
// 05_multipass's own identical rationale for its cube-probe check). Still
// accepts EITHER the raw linear byte value or its sRGB-encoded counterpart
// (see this file's header comment's SRGB CAVEAT) at the real, format-
// resolved R/G/B byte positions.
bool matchesExpectedColorChannelExact(const uint8_t* pixel, const ChannelIndices& channels,
                                       const std::array<uint8_t, 3>& expectedLinear,
                                       const std::array<uint8_t, 3>& expectedSrgb) {
    const std::array<uint8_t, 3> actual{pixel[channels.r], pixel[channels.g], pixel[channels.b]};
    return colorDistanceSq(actual, expectedLinear) < kMatchThresholdSq ||
           colorDistanceSq(actual, expectedSrgb) < kMatchThresholdSq;
}

// Exact analytic derivation -- see checker.slang's own evaluate(): at the
// probed pixel, the checker cell is the grid's middle (kCellsPerAxis = 3)
// cell, parity 0, lightness 1.0 -- so `color = gParams.tint * 1.0`. The
// `checkerTexIndex` term contributes exactly 0.0 (see that file's own
// comment) and the two `* 1e-6` epsilon terms are far below one 8-bit
// quantization step (1/255 ~= 0.0039) for any component already >= 0 --
// negligible for this byte-level comparison, exactly like every epsilon
// term samples/05_multipass's own probes already carry.
glm::vec3 expectedCheckerColor(const std::array<float, 4>& tint) { return glm::vec3(tint[0], tint[1], tint[2]); }

// Exact analytic derivation -- see rim.slang's own evaluate(): at the
// probed pixel, ndotv == 1.0 exactly (this file's header comment, point
// (c)), so rimTerm = pow(1 - 1, 2.5) = 0, and
// `color = kBaseAlbedo * 1.0 + rimColor.rgb * (0.5 + 0.0)`.
glm::vec3 expectedRimColor(const std::array<float, 4>& rimColor) {
    constexpr glm::vec3 kBaseAlbedo(0.12F, 0.12F, 0.12F);
    constexpr float kAmbientFactor = 0.5F;
    return kBaseAlbedo + glm::vec3(rimColor[0], rimColor[1], rimColor[2]) * kAmbientFactor;
}

glm::vec3 expectedColorFor(const ObjectSpec& spec) {
    return (spec.materialKind == MaterialKind::Checker) ? expectedCheckerColor(spec.param4)
                                                          : expectedRimColor(spec.param4);
}

// --- Runtime asset-path resolution [Task 8 ledger item] ---------------------
// SDL_GetBasePath()-relative, exactly like samples/02_hotreload's own
// resolveShaderPath() -- so a packaged copy of this sample's own build-
// output directory (binary + materials/ + material_shaders/ + the Slang
// runtime libs -- see this sample's CMakeLists.txt and tools/
// package_samples.sh) runs identically outside the build tree, with no
// reference to CMAKE_SOURCE_DIR/RX_MATERIAL_SHADER_DIR at all. This is
// exactly the mechanism MaterialSystem::create()'s own new
// `sharedShaderDir` parameter (material_system.h) exists to accept.
std::filesystem::path basePathDirectory() {
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        RX_LOG_WARN("sample_06_materials: SDL_GetBasePath failed ({}); using the current directory instead",
                    SDL_GetError());
        return ".";
    }
    return std::filesystem::path(basePath);
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
    auto window = rx::platform::Window::create("rx_materials_sample", static_cast<int>(kHeadlessWidth),
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

    // Phase 4 Task 7 [spec D4 amendment]: parallelism is the engine default
    // now, not an opt-in mode -- Executor::create() requires a real
    // rx::task::Scheduler&. Worker default (Scheduler::create(0)): this
    // sample is a standalone consumer that owns the whole machine, exactly
    // the case that default is for (docs/threading.md's "Host-engine
    // coexistence" section).
    auto scheduler = rx::task::Scheduler::create();
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

    const std::filesystem::path basePath = basePathDirectory();
    const std::filesystem::path pipelineCachePath =
        std::filesystem::temp_directory_path() / "rx_sample_06_materials_pipeline.cache";
    // Declared here, once, and never moved/returned again -- see
    // createScene()'s own header comment for why that address stability is
    // load-bearing (MaterialSystem::create() stores a raw pointer to
    // `scene.bindlessTable` for this instance's whole lifetime).
    Scene scene;
    if (!createScene(scene, device->physicalDevice(), *device, *allocator, *uploader, basePath / "material_shaders",
                      basePath / "materials", pipelineCachePath)) {
        RX_LOG_ERROR("createScene failed");
        return 1;
    }

    rx::graph::RenderGraph graph;
    declareGraph(graph, scene, targetFormat);

    rx::graph::CompileInfo info;
    info.swapchainWidth = kHeadlessWidth;
    info.swapchainHeight = kHeadlessHeight;
    info.swapchainFormat = targetFormat;
    // Offscreen backbuffer, never presented -- see rx_graph's own
    // CompileInfo ambiguity resolution #1 (TRANSFER_SRC_OPTIMAL is what
    // this file's own readback copy needs finalBarriers() to leave
    // "backbuffer" in), matching every other sample's identical headless
    // setup.
    info.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(info);
    executor->realize(graph);

    // Fixed camera for the whole headless run, slot 0 only -- see this
    // file's header comment (mirrors samples/05_multipass's identical
    // choice for its own fixed-camera headless mode).
    updateFrame(*uploader, scene, /*frameInFlightIndex=*/0, kHeadlessAzimuth, kHeadlessWidth, kHeadlessHeight);

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
        destroyScene(scene);
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

    // Every render frame is its own SEPARATE runOnce() submission -- same
    // "BACKBUFFER REUSE ACROSS REPEATED execute() CALLS" argument
    // samples/05_multipass's own header comment makes in full: each call's
    // own runOnce() fully vkQueueWaitIdle()s before returning, so the next
    // call's own tonemap/forward-pass-entry barrier's UNDEFINED assumption
    // is provably correct despite compile()'s stateless derivation. Frame-
    // in-flight index stays 0 the whole run (fixed camera -- see
    // updateFrame()'s own call above); materialBridge::beginFrame()/
    // onFrameCompleted() are still exercised once per iteration each, with
    // a genuinely distinct frameNumber, matching MaterialSystem's own
    // frames-in-flight discipline (material_system.h).
    const VkExtent2D extent{kHeadlessWidth, kHeadlessHeight};
    for (uint32_t frame = 0; frame < kHeadlessFrameCount; ++frame) {
        materialBridge::beginFrame(scene.materialSystem, /*frameInFlightIndex=*/0, /*frameNumber=*/frame);
        cmdCtx->runOnce([&](VkCommandBuffer cmd) { executor->execute(graph, cmd, offscreenImage, offscreenView, extent); });
        materialBridge::onFrameCompleted(scene.materialSystem, frame);
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
        vkCmdCopyImageToBuffer(cmd, offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->handle(), 1, &region);
    });

    readback->invalidate();
    std::vector<uint8_t> pixels(static_cast<size_t>(kHeadlessPixelBytes));
    std::memcpy(pixels.data(), readback->mappedData(), pixels.size());

    destroyRawResources();

    auto pixelAt = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return pixels.data() + (static_cast<size_t>(y) * kHeadlessWidth + x) * 4;
    };

    bool pass = true;
    const auto channels = channelIndicesForFormat(targetFormat);
    if (!channels.has_value()) {
        RX_LOG_ERROR(
            "backbuffer format {} is not one of the R8G8B8A8/B8G8R8A8 families this check knows how to map to exact "
            "R/G/B byte positions -- refusing to guess",
            static_cast<int>(targetFormat));
        pass = false;
    }

    const CameraPose headlessPose = makeCameraPose(kHeadlessAzimuth, kHeadlessWidth, kHeadlessHeight);
    std::array<std::array<uint8_t, 3>, kObjectCount> expectedLinearBytes{};
    for (uint32_t i = 0; i < kObjectCount; ++i) {
        expectedLinearBytes[i] = toByteColor(expectedColorFor(kObjectSpecs[i]));
    }
    // Sanity guard: the 4 expected colors must themselves be pairwise
    // distinguishable at this sample's own match tolerance -- if two ever
    // collide (e.g. a future edit to kObjectSpecs picks near-identical
    // tint/rimColor values), a probe that reads either color could pass by
    // accident even under a real substitution bug, defeating the point of
    // this gate. Checked here, once, rather than trusted by construction.
    for (uint32_t i = 0; i < kObjectCount && pass; ++i) {
        for (uint32_t j = i + 1; j < kObjectCount; ++j) {
            if (colorDistanceSq(expectedLinearBytes[i], expectedLinearBytes[j]) < kMatchThresholdSq) {
                RX_LOG_ERROR("expected colors for objects {} and {} are not distinguishable at this sample's own "
                             "match tolerance -- fix kObjectSpecs",
                             i, j);
                pass = false;
            }
        }
    }

    for (uint32_t i = 0; i < kObjectCount && channels.has_value(); ++i) {
        const ObjectSpec& spec = kObjectSpecs[i];
        const PixelCoord probe = projectToPixel(spec.position, headlessPose.viewProj, kHeadlessWidth, kHeadlessHeight);
        const uint8_t* px = pixelAt(probe.x, probe.y);
        const std::array<uint8_t, 3> expectedSrgb = srgbEncodeColor(expectedLinearBytes[i]);
        const bool matched = matchesExpectedColorChannelExact(px, *channels, expectedLinearBytes[i], expectedSrgb);
        RX_LOG_INFO("object {} ({}) world=({:.1f},{:.1f},{:.1f}) pixel=({},{}) channels=({},{},{},{}) expected_linear=("
                     "{},{},{}) matched={}",
                     i, spec.materialKind == MaterialKind::Checker ? "checker" : "rim", spec.position.x, spec.position.y,
                     spec.position.z, probe.x, probe.y, px[0], px[1], px[2], px[3], expectedLinearBytes[i][0],
                     expectedLinearBytes[i][1], expectedLinearBytes[i][2], matched);
        if (!matched) {
            RX_LOG_ERROR("object {}'s probe pixel does not match its expected color", i);
            pass = false;
        }
    }

    if (enableValidation && context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during this run");
        pass = false;
    }

    if (pass) {
        RX_LOG_INFO("materials headless gate PASSED");
        return 0;
    }
    RX_LOG_ERROR("materials headless gate FAILED");
    return 1;
}

// --- --present mode: real window, orbiting camera, hot-reload polling ------
int runPresent(bool enableValidation, rx::rhi::PresentMode vsyncMode) {
    auto window = rx::platform::Window::create("rx_materials_sample (--present)", static_cast<int>(kPresentWidth),
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

    // Phase 4 Task 7 [spec D4 amendment] -- see runHeadless()'s identically-
    // worded comment above.
    auto scheduler = rx::task::Scheduler::create();
    if (scheduler == nullptr) {
        RX_LOG_ERROR("rx::task::Scheduler::create failed");
        return 1;
    }

    auto executor = rx::graph::Executor::create(*device, *scheduler);
    if (executor == nullptr) {
        RX_LOG_ERROR("rx::graph::Executor::create failed");
        return 1;
    }

    const std::filesystem::path basePath = basePathDirectory();
    const std::filesystem::path materialsDir = basePath / "materials";
    const std::filesystem::path pipelineCachePath =
        std::filesystem::temp_directory_path() / "rx_sample_06_materials_pipeline.cache";
    // Declared here, once, and never moved/returned again -- see
    // createScene()'s own header comment for why (MaterialSystem::create()
    // stores a raw pointer to `scene.bindlessTable` for this instance's
    // whole lifetime).
    Scene scene;
    if (!createScene(scene, device->physicalDevice(), *device, *allocator, *uploader, basePath / "material_shaders",
                      materialsDir, pipelineCachePath)) {
        RX_LOG_ERROR("createScene failed");
        return 1;
    }

    rx::graph::RenderGraph graph;
    declareGraph(graph, scene, device->swapchainFormat());

    auto compileForExtent = [&](VkExtent2D extent) {
        rx::graph::CompileInfo info;
        info.swapchainWidth = extent.width;
        info.swapchainHeight = extent.height;
        info.swapchainFormat = device->swapchainFormat();
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
                RX_LOG_ERROR("sample_06_materials: vkCreateImageView(swapchain image {}) failed", i);
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
        destroyScene(scene);
        return 1;
    }

    RX_LOG_INFO("--present: window open ({}x{}); camera orbiting -- watching {} and {} for changes (~1s poll); edit "
                "either while this window is open -- close the window to exit",
                kPresentWidth, kPresentHeight, (materialsDir / "checker.slang").string(),
                (materialsDir / "rim.slang").string());

    const auto startTime = std::chrono::steady_clock::now();
    auto lastPollTime = startTime;

    // Tracks which frameNumber was last submitted into each frame-in-flight
    // slot, so materialBridge::onFrameCompleted() can be told exactly which
    // frame just finished once this loop confirms (via vkWaitForFences)
    // that slot is free again -- mirrors samples/04_streaming's own
    // DeletionQueue::onFrameFenceSignaled(slot.frameNumber) bookkeeping,
    // applied here to rx_material's internal per-frame arena instead.
    std::array<std::optional<uint64_t>, rx::rhi::FrameSync::kFramesInFlight> lastSubmittedFrameNumber{};

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

        const auto now = std::chrono::steady_clock::now();
        if (now - lastPollTime >= kMaterialPollInterval) {
            lastPollTime = now;
            // Public API call -- reloadChanged() itself stats both files
            // this sample's loadMaterial() calls have been made with,
            // recompiles whichever changed, and keeps the last-good
            // compiled state on a failed recompile (material_system.h's own
            // documented contract); this sample never needs to know which
            // file changed or whether the reload actually did anything.
            scene.materialSystem.publicApi->reloadChanged();
        }

        VkFence fence = frameSync->currentFence();
        if (vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            RX_LOG_ERROR("vkWaitForFences failed in present loop");
            ok = false;
            break;
        }
        if (lastSubmittedFrameNumber[frameSync->currentFrameIndex()].has_value()) {
            materialBridge::onFrameCompleted(scene.materialSystem, *lastSubmittedFrameNumber[frameSync->currentFrameIndex()]);
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

        const float elapsedSeconds = std::chrono::duration<float>(now - startTime).count();
        const float azimuth = elapsedSeconds * kOrbitAzimuthRadPerSec;
        const VkExtent2D extent = device->swapchainExtent();
        updateFrame(*uploader, scene, frameSync->currentFrameIndex(), azimuth, extent.width, extent.height);

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

        materialBridge::beginFrame(scene.materialSystem, frameSync->currentFrameIndex(), frameSync->frameNumber());

        VkImage swapchainImage = device->swapchainImages()[acquire.imageIndex];
        executor->execute(graph, cmd, swapchainImage, swapchainViews[acquire.imageIndex], extent);

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
        lastSubmittedFrameNumber[frameSync->currentFrameIndex()] = frameSync->frameNumber();

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
    destroyScene(scene);
    // `executor`, `graph`, and `frameSync` go out of scope after this point,
    // in reverse declaration order -- Executor's own destructor already
    // vkDeviceWaitIdle()s before tearing down its transient pool, and
    // FrameSync's own destructor contract (frame_sync.h) requires exactly
    // the vkDeviceWaitIdle() call already issued above.

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
