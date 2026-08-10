// Sample 04: texture streaming.
//
// The Phase 2 sample that most directly exercises eviction-while-in-flight
// safety [R:D1 sample 3]: 24 procedurally generated textures compete for a
// resident budget of only 8 bindless slots. Every few frames (headless) or
// roughly once a second (--present) the NEXT logical texture streams in
// through rx::rhi::Uploader and the OLDEST resident logical texture is
// evicted -- rx::rhi::BindlessTable::release() returns its descriptor slot
// to the free list, and its rx::rhi::Texture2D (the actual VkImage/
// VkImageView) is retired into rx::rhi::DeletionQueue rather than destroyed
// on the spot. This file's job is to get that retirement's timing exactly
// right; see "FRAME-LAG SAFETY ARGUMENT" below before touching
// evictOldestAndStreamInNext().
//
// GRID: 24 fixed grid cells, one per logical texture index (0..23) --
// NOT one cell per physical bindless slot. A cell is drawn only while its
// texture is currently resident (one of the 8 occupied slots); a
// non-resident cell is skipped entirely in the draw loop (recordGridDraws()
// below), which is exactly why this works with BindlessTable's descriptor
// set: VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT is what makes it legal for
// a bound descriptor set to contain slots no draw call this frame ever
// indexes -- skipping the draw is what keeps the set "valid with holes";
// the flag is what makes the validation layer agree. See bindless.h for
// where that flag is set.
//
// FRAME-LAG SAFETY ARGUMENT (read before changing the eviction/retirement
// code -- this covers BOTH halves of an eviction; an earlier revision of
// this file only deferred the destruction half and got the rewrite half
// wrong, a Critical review finding, see "DESCRIPTOR REWRITE SAFETY"
// below for that half specifically): this sample renders through a real
// rx::rhi::FrameSync, frames-in-flight == 2 loop (headless mode manually
// recreates that loop against 2 dedicated offscreen images instead of a
// swapchain -- see runHeadless() -- specifically so genuine CPU/GPU
// overlap is exercised; a fully-serialized "wait every frame" loop would
// never expose an eviction bug and the validation-error assertion at the
// end would be vacuously true).
//
// Both the destruction of the victim's old Texture2D AND the descriptor
// rewrite that hands the freed slot to the incoming texture are deferred
// together, in one rx::rhi::DeletionQueue::retire() call, tagged with
// `frameSync.frameNumber()` -- the CURRENT frame number, at the moment of
// eviction (evictOldestAndStreamInNext()'s `currentFrameNumber`
// parameter). Why that specific tag is correct for BOTH halves, not off
// by one either way:
//   - Any command buffer recorded and submitted STRICTLY BEFORE this frame
//     (frame N-1 and earlier) was recorded while the victim's slot still
//     held the OLD descriptor contents and may dynamically sample it --
//     it may still be executing on the GPU, so neither the old
//     VkImage/VkImageView may be destroyed NOR may the slot's descriptor
//     be rewritten yet (see "DESCRIPTOR REWRITE SAFETY" below -- rewriting
//     a slot a still-pending command buffer dynamically uses is itself a
//     spec violation, independent of the destroy-timing question).
//   - This frame (frame N) and every later frame are only ever recorded
//     with the victim's grid cell already marked non-resident (cleared
//     immediately, before any deferral) and the incoming texture's cell
//     not yet marked resident (deferred until the callback below runs) --
//     so frame N's own draws never reference the slot at all, in either
//     its old or new state, and never depend on either half completing
//     first.
//   - Vulkan queues execute (and, more importantly here, SIGNAL fences)
//     submissions in submission order on a single queue [Vulkan spec,
//     queue submission ordering] -- this project submits every frame to
//     exactly one graphics queue throughout. So once frame N's fence is
//     confirmed signaled, frame N-1 (and everything earlier) is
//     GUARANTEED to have already completed too -- this is the same
//     assumption every frames-in-flight double-buffering scheme in this
//     codebase already relies on (see frame_sync.h/deletion_queue.h).
//   - DeletionQueue::retire(..., N) only actually runs its callback once
//     onFrameFenceSignaled(N) is called, which this file only calls once
//     frame N's own fence has been waited on and confirmed -- by
//     construction, after frame N-1 has also completed. Tagging with N
//     (not N-1, which would be premature by the same argument in reverse,
//     and not N+1, which would just needlessly delay reclaiming the slot)
//     is exactly right for the destruction, and is also sufficient (if
//     one frame more conservative than the theoretical minimum of N+1's
//     own confirmation) for the rewrite -- reusing the identical tag/timing
//     for both halves keeps this file's synchronization story to ONE rule
//     to verify, not two independently-argued ones.
//
// DESCRIPTOR REWRITE SAFETY (the half a Critical review finding caught
// missing from an earlier revision): rewriting a descriptor slot via
// registerSampledImage() is legal under UPDATE_AFTER_BIND | PARTIALLY_BOUND
// only when that slot is NOT dynamically used by any command buffer whose
// submission has not yet completed execution -- see bindless.h's
// RELEASE-SAFETY CONTRACT for the full spec argument. release() then
// immediately register() into the just-freed index is NOT a rare
// coincidence here: BindlessTable's free list is LIFO
// (rx_core/handle.h's HandlePool::acquire() pops freeList_.back(), and
// release() pushes to freeList_.back()), so a release() immediately
// followed by a register() call deterministically reuses the exact same
// physical slot every single eviction cycle. If that rewrite happened
// immediately (as an earlier revision of this file did), frame N-1's
// command buffer -- recorded before the eviction decision, which may
// still be executing on the GPU since only frame N-2 is fence-confirmed
// at that point -- could still be dynamically sampling that exact slot
// through the victim's OLD descriptor contents. That is a genuine
// Vulkan spec violation (not merely a destroy-timing bug): the bounded
// consequence is a stale in-flight frame silently sampling the NEW
// texture's data one frame early (the new texture is always fully
// uploaded before its descriptor is ever written, so this is never a
// use-after-free/undefined-memory-read) -- invisible to validation
// layers, but still not what the spec permits, and it would defeat this
// sample's entire stated purpose to accept it as "harmless enough." The
// fix: registerSampledImage() for the incoming texture is called from
// inside the SAME DeletionQueue-retired callback that destroys the
// victim's old Texture2D, at the SAME frame-fence-confirmed point --
// i.e., the incoming texture's grid cell does not become resident
// (drawable) until 2 frames after the eviction decision, not
// immediately. This shifts this sample's "full rotation" timing (see
// runHeadless()'s stream-interval comment) but the assertion itself is
// unchanged.
//
// WHY PUSH-CONSTANT TRANSFORMS, NOT A BINDLESS STORAGE BUFFER: unlike
// 03_bindless_mesh (whose whole point includes a double-buffered bindless
// transform buffer), this sample's grid is a completely static camera over
// a completely static layout -- nothing about any object's MVP ever
// changes frame to frame (only WHICH cells are drawn changes). Recomputing
// each resident cell's small MVP on the CPU and pushing it as 64 bones... a
// mat4 [96 bytes total with the two uint32 indices, comfortably inside the
// 128-byte floor] avoids inventing a synchronization story for a resource
// that would never actually need one here -- the whole point of this
// sample is the TEXTURE eviction lifecycle, not transform double-buffering
// (already proven by 03_bindless_mesh). This shader's set 0 therefore uses
// only 2 of BindlessTable's 3 fixed bindings (images, samplers -- no
// storage buffers), which `rx::rhi::PipelineLayoutBuilder::build()`'s
// external-set-0 shape check explicitly allows: "a shader is free to use a
// strict subset of the three slots" (pipeline_layout.h).
//
// WHY NO DEPTH BUFFER: the 24 grid cells are coplanar and non-overlapping
// in screen space by construction (a regular grid with gaps between
// cells) -- there is nothing for a depth test to resolve, so this sample
// skips creating one entirely (VkPipelineRenderingCreateInfo has no
// depthAttachmentFormat, VkRenderingInfo has no pDepthAttachment), unlike
// 03_bindless_mesh's overlapping 3D objects which genuinely need one.
//
// WHY FLAT-HUE TEXTURES, NOT CHECKERBOARDS/GRADIENTS: 03_bindless_mesh
// already proves procedural checkerboard/gradient generation; this
// sample's correctness gate needs each of 24 textures to be
// unambiguously identifiable from a single readback pixel, so each
// logical texture is a flat fill of its own point on an HSV hue wheel
// (still genuinely "procedurally generated" -- computed from a formula at
// runtime, not loaded from any file) sampled with VK_FILTER_NEAREST (no
// mip chain, no linear-filter blending at the probe point) -- an honest
// simplification in service of a rock-solid assertion, not a shortcut on
// the sample's actual subject (streaming/eviction).
//
// Two modes, exactly like every other sample in this codebase:
//   (default / no args) Headless correctness gate: 60 real frames through
//   a genuine 2-frames-in-flight offscreen loop, readback probes at every
//   grid cell's screen position on every frame, asserting every one of
//   the 24 textures was actually observed resident on screen at some
//   point, plus zero validation errors (the assertion that would catch a
//   premature-destroy bug). Registered as ctest
//   sample_04_streaming_headless.
//   --present. Real window, the same 24-cell grid, static camera,
//   streaming continuing forever at roughly 1 texture/second.
#include <rx_core/log.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/deletion_queue.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/frame_sync.h>
#include <rx_rhi_vk/mesh_buffers.h>
#include <rx_rhi_vk/pipeline_layout.h>
#include <rx_rhi_vk/texture.h>
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
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// --- Tunables --------------------------------------------------------------

constexpr uint32_t kTextureCount = 24;
constexpr uint32_t kResidentBudget = 8;
constexpr uint32_t kTextureSize = 32;
constexpr VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;

constexpr uint32_t kGridCols = 6;
constexpr uint32_t kGridRows = 4;
static_assert(kGridCols * kGridRows == kTextureCount, "grid must have exactly one cell per logical texture");
constexpr float kCellSpacing = 1.2F;
constexpr float kCellHalfSize = 0.45F;
constexpr float kGridMargin = 0.25F;
constexpr float kCameraDistance = 5.0F;
constexpr float kOrthoNear = 0.1F;
constexpr float kOrthoFar = 100.0F;

constexpr uint32_t kHeadlessWidth = 256;
constexpr uint32_t kHeadlessHeight = 256;
constexpr VkDeviceSize kHeadlessPixelBytes = static_cast<VkDeviceSize>(kHeadlessWidth) * kHeadlessHeight * 4;
constexpr uint32_t kHeadlessTotalFrames = 60;
// See this task's brief: "each second (or every N frames headlessly)".
// 19 stream events occur in a 60-frame run at this interval (frames 3, 6,
// ..., 57) -- comfortably more than the 16 needed to complete one full
// rotation past the initial 8-texture fill (loading logical textures
// 8..23) with margin to spare for the analysis in this file's header
// comment to hold even if a probe or two lands imperfectly.
constexpr uint64_t kHeadlessStreamIntervalFrames = 3;

constexpr uint32_t kPresentWidth = 900;
constexpr uint32_t kPresentHeight = 700;
constexpr float kPresentStreamIntervalSeconds = 1.0F;

// Generous vs. this sample's exact-reproduction rendering path (NEAREST
// filtering, flat-fill textures, no blending) -- see this file's header
// comment on why flat hues were chosen. Matches the threshold style
// 03_bindless_mesh's own probe check uses (40*40), applied the same way:
// sum of squared per-channel differences.
constexpr int kMatchThresholdSq = 40 * 40;

// --- Procedural texture colors ---------------------------------------------

std::array<uint8_t, 3> hsvToRgb(float hueDegrees, float saturation, float value) {
    float wrappedHue = std::fmod(hueDegrees, 360.0F);
    if (wrappedHue < 0.0F) {
        wrappedHue += 360.0F;
    }
    float c = value * saturation;
    float hPrime = wrappedHue / 60.0F;
    float x = c * (1.0F - std::fabs(std::fmod(hPrime, 2.0F) - 1.0F));
    float m = value - c;
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    if (hPrime < 1.0F) {
        r = c;
        g = x;
    } else if (hPrime < 2.0F) {
        r = x;
        g = c;
    } else if (hPrime < 3.0F) {
        g = c;
        b = x;
    } else if (hPrime < 4.0F) {
        g = x;
        b = c;
    } else if (hPrime < 5.0F) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    auto toByte = [m](float channel) {
        return static_cast<uint8_t>(std::clamp((channel + m) * 255.0F + 0.5F, 0.0F, 255.0F));
    };
    return {toByte(r), toByte(g), toByte(b)};
}

// One flat, maximally-saturated hue per logical texture, spread evenly
// around the color wheel (360/24 = 15 degrees apart) -- computed once at
// startup, used both to fill each texture's pixels (as-is, no encoding --
// the texture format is plain UNORM, so these bytes land in the texture
// completely unmodified) and, via srgbEncodeColor() below, to derive the
// SEPARATE table the probe comparison actually needs.
std::array<std::array<uint8_t, 3>, kTextureCount> computeExpectedColors() {
    std::array<std::array<uint8_t, 3>, kTextureCount> colors{};
    for (uint32_t i = 0; i < kTextureCount; ++i) {
        float hue = static_cast<float>(i) * (360.0F / static_cast<float>(kTextureCount));
        colors[i] = hsvToRgb(hue, 0.85F, 0.95F);
    }
    return colors;
}

// FOUND EMPIRICALLY DURING THIS SAMPLE'S DEVELOPMENT (worth documenting
// since it is easy to reproduce and easy to get wrong): both this
// project's headless offscreen target and its real swapchains
// (Device::swapchainFormat()) resolve to an _SRGB-suffixed VkFormat (e.g.
// VK_FORMAT_B8G8R8A8_SRGB on this development machine's NVIDIA driver) --
// writing to an _SRGB color attachment makes the GPU apply the linear ->
// sRGB transfer function to the fragment shader's output before storing
// the encoded bytes, REGARDLESS of what format the sampled TEXTURE itself
// uses (this sample's textures are plain UNORM, so the value the
// fragment shader receives from `Sample()` is exactly the raw uploaded
// byte, unencoded). So a readback of the rendered image sees the
// SRGB-ENCODED version of each flat fill color, not the raw uploaded
// bytes -- confirmed by rendering frame 0, dumping it to a PPM, and
// comparing a probed pixel against a hand-computed sRGB encode of logical
// texture 0's fill color during development. Since this project's other samples
// never compare readback bytes against an absolute precomputed table
// (they either compare colors only against each other, or accept whole-
// pixel values straight from a known-good pipeline), this sRGB encode was
// not something an earlier sample had reason to document -- it is
// load-bearing here specifically because this sample's headless gate
// needs ABSOLUTE per-texture expected colors, not just "N distinct
// colors."
uint8_t srgbEncodeByte(uint8_t linearByte) {
    float v = static_cast<float>(linearByte) / 255.0F;
    float encoded = (v <= 0.0031308F) ? (12.92F * v) : (1.055F * std::pow(v, 1.0F / 2.4F) - 0.055F);
    return static_cast<uint8_t>(std::clamp(encoded * 255.0F + 0.5F, 0.0F, 255.0F));
}

std::array<uint8_t, 3> srgbEncodeColor(const std::array<uint8_t, 3>& linear) {
    return {srgbEncodeByte(linear[0]), srgbEncodeByte(linear[1]), srgbEncodeByte(linear[2])};
}

std::vector<uint8_t> makeFlatColorTexture(uint32_t size, std::array<uint8_t, 3> color) {
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = color[0];
        pixels[i + 1] = color[1];
        pixels[i + 2] = color[2];
        pixels[i + 3] = 255;
    }
    return pixels;
}

// --- Grid layout / camera --------------------------------------------------

// World-space center of grid cell `k` (row-major, row 0 at the top -- see
// cellProbePixel()'s Y-flip for why "top" maps to the smaller screen-space
// pixel-y as expected).
glm::vec2 cellPosition(uint32_t k) {
    uint32_t col = k % kGridCols;
    uint32_t row = k / kGridCols;
    float x = (static_cast<float>(col) - (static_cast<float>(kGridCols) - 1.0F) * 0.5F) * kCellSpacing;
    float y = ((static_cast<float>(kGridRows) - 1.0F) * 0.5F - static_cast<float>(row)) * kCellSpacing;
    return {x, y};
}

glm::mat4 makeViewMatrix() {
    return glm::lookAt(glm::vec3(0.0F, 0.0F, kCameraDistance), glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
}

struct OrthoExtents {
    float halfWidth;
    float halfHeight;
};

// "Fit the whole grid, plus a margin, inside the viewport" -- fits by
// whichever axis is the binding constraint for `aspect`, exactly the same
// shape of computation as a letterboxed/pillarboxed camera. Kept as its
// own function specifically so cellProbePixel() (below) can replicate the
// exact same extents the real projection matrix uses, without needing to
// re-derive them from a constructed glm::mat4.
OrthoExtents computeOrthoExtents(float aspect) {
    float contentHalfWidth = static_cast<float>(kGridCols) * kCellSpacing * 0.5F + kGridMargin;
    float contentHalfHeight = static_cast<float>(kGridRows) * kCellSpacing * 0.5F + kGridMargin;

    float halfWidth = contentHalfHeight * aspect;
    float halfHeight = contentHalfHeight;
    if (halfWidth < contentHalfWidth) {
        halfWidth = contentHalfWidth;
        halfHeight = contentHalfWidth / aspect;
    }
    return {halfWidth, halfHeight};
}

// Vulkan clip space is Y-down; glm::ortho (like glm::perspective) assumes
// OpenGL's Y-up NDC -- flipping proj[1][1] is the same standard fix
// 03_bindless_mesh's makeProjection() uses for glm::perspective.
glm::mat4 makeProjection(float aspect) {
    OrthoExtents extents = computeOrthoExtents(aspect);
    glm::mat4 proj =
        glm::ortho(-extents.halfWidth, extents.halfWidth, -extents.halfHeight, extents.halfHeight, kOrthoNear, kOrthoFar);
    proj[1][1] *= -1.0F;
    return proj;
}

struct PixelCoord {
    uint32_t x;
    uint32_t y;
};

// Analytically derives the screen-space pixel this sample's static,
// axis-aligned orthographic camera maps grid cell `k`'s center to, given a
// `viewportWidth`x`viewportHeight` render target. Because the camera is a
// pure axis-aligned translation (no rotation -- see makeViewMatrix()) and
// the projection is orthographic, this is a direct linear map, not an
// empirically-reverse-engineered constant the way 03_bindless_mesh's
// perspective/orbit probe positions had to be: the same
// computeOrthoExtents()/proj[1][1]-flip math the real vertex shader's mvp
// uses is replicated here in plain host-side arithmetic.
PixelCoord cellProbePixel(uint32_t k, uint32_t viewportWidth, uint32_t viewportHeight) {
    float aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
    OrthoExtents extents = computeOrthoExtents(aspect);
    glm::vec2 pos = cellPosition(k);

    float ndcX = pos.x / extents.halfWidth;
    float ndcY = -(pos.y / extents.halfHeight);  // matches the proj[1][1] *= -1 flip above

    float pixelX = (ndcX * 0.5F + 0.5F) * static_cast<float>(viewportWidth);
    float pixelY = (ndcY * 0.5F + 0.5F) * static_cast<float>(viewportHeight);

    uint32_t clampedX = static_cast<uint32_t>(std::clamp(pixelX, 0.0F, static_cast<float>(viewportWidth) - 1.0F));
    uint32_t clampedY = static_cast<uint32_t>(std::clamp(pixelY, 0.0F, static_cast<float>(viewportHeight) - 1.0F));
    return {clampedX, clampedY};
}

int colorDistanceSq(const std::array<uint8_t, 3>& a, const std::array<uint8_t, 3>& b) {
    int dr = static_cast<int>(a[0]) - static_cast<int>(b[0]);
    int dg = static_cast<int>(a[1]) - static_cast<int>(b[1]);
    int db = static_cast<int>(a[2]) - static_cast<int>(b[2]);
    return dr * dr + dg * dg + db * db;
}

// Compares `pixel`'s first 3 channels against `expectedLinear` (the raw
// fill color) and `expectedSrgb` (its sRGB-encoded form -- see
// srgbEncodeColor()'s comment above), each both as-is and with the
// first/third channel swapped, accepting any of the 4 combinations as a
// match. The channel-order check makes this correct regardless of
// whether the render target's real channel order is RGBA or BGRA
// (device->swapchainFormat(), whatever the driver reports) without
// needing to detect which; the linear-vs-sRGB check makes this correct
// regardless of whether that same format happens to carry an _SRGB or a
// plain UNORM suffix on a given platform/driver -- this sample never
// hardcodes an assumption about either, deliberately, since both presets
// (and potentially different GPUs/drivers within linux-native) must stay
// clean. Unlike 03_bindless_mesh's probe (which only ever compares colors
// against each other, never against an absolute expected value), this
// sample DOES need absolute expected colors, hence this explicit
// 4-combination check instead.
bool matchesExpectedColor(const uint8_t* pixel, const std::array<uint8_t, 3>& expectedLinear,
                            const std::array<uint8_t, 3>& expectedSrgb) {
    std::array<uint8_t, 3> asIs{pixel[0], pixel[1], pixel[2]};
    std::array<uint8_t, 3> reversed{pixel[2], pixel[1], pixel[0]};
    return colorDistanceSq(asIs, expectedLinear) < kMatchThresholdSq ||
           colorDistanceSq(reversed, expectedLinear) < kMatchThresholdSq ||
           colorDistanceSq(asIs, expectedSrgb) < kMatchThresholdSq ||
           colorDistanceSq(reversed, expectedSrgb) < kMatchThresholdSq;
}

// --- Shader source (compiled at runtime, reflected, exactly like
// 02_hotreload/03_bindless_mesh -- see this file's header comment for why
// set 0 only uses 2 of BindlessTable's 3 fixed bindings) -----------------
constexpr const char* kShaderSource = R"(
struct PushConstants {
    float4x4 mvp;
    uint textureIndex;
    uint samplerIndex;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> gPush;

[[vk::binding(0, 0)]]
Texture2D gTextures[];

[[vk::binding(1, 0)]]
SamplerState gSamplers[];

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
    VSOut o;
    o.position = mul(gPush.mvp, float4(input.position, 1.0));
    o.uv = input.uv;
    return o;
}

[shader("fragment")]
float4 fsMain(VSOut input) : SV_Target
{
    // gPush.textureIndex/samplerIndex: uniform across this whole draw call
    // (every invocation reads the same push-constant bytes) -- never
    // NonUniformResourceIndex-wrapped [spec Fixed decision #6, R:B3/E4].
    return gTextures[gPush.textureIndex].Sample(gSamplers[gPush.samplerIndex], input.uv);
}
)";

const std::vector<std::string> kEntryPointNames = {"vsMain", "fsMain"};

// Slang pads a push-constant ConstantBuffer<T>'s total block size up to a
// 16-byte multiple (HLSL cbuffer-style packing: 64 [mat4] + 4 + 4 = 72
// rounds up to 80) -- found empirically (logged via reflect()'s reported
// range size during this sample's development, matching the "shipped
// slang.h wins" discrepancy-reporting rule this plan's Global Constraints
// call for). The explicit `_padding` keeps this struct's real size in
// sync with the reflected range size (asserted in buildScenePipeline())
// so vkCmdPushConstants's declared size always covers fully-initialized
// bytes, never an uninitialized tail past this struct's C++ layout.
struct PushConstants {
    glm::mat4 mvp;
    uint32_t textureIndex;
    uint32_t samplerIndex;
    uint32_t _padding[2];
};

struct Vertex {
    float position[3];
    float uv[2];
};

// --- GPU-side scene ---------------------------------------------------------

// A logical texture's GPU-side state while it is resident. Empty
// (`texture` == nullptr, `handle` invalid) while that logical texture is
// not currently loaded/resident. `shared_ptr`, not `optional`, because an
// evicted texture's ownership must be able to move into a
// DeletionQueue-retired closure (which requires a copy-constructible
// target, see deletion_queue.h's own documented pitfall) without an
// extra wrap-in-make_shared step at the call site, and because an
// incoming texture is built as a shared_ptr BEFORE it is known whether
// it will be registered immediately (initial fill) or via a deferred
// callback (eviction) -- see evictOldestAndStreamInNext().
struct LogicalTextureSlot {
    std::shared_ptr<rx::rhi::Texture2D> texture;
    rx::rhi::BindlessHandle handle;
};

// Everything this sample needs beyond the raw RHI stack. Move-only by
// construction (every member is itself move-only RAII); built once by
// createScene() and torn down by destroyScene() before the owning
// Device/Context/Allocator go out of scope, same discipline as every
// other sample in this codebase.
struct Scene {
    rx::rhi::BindlessTable bindlessTable;
    std::array<LogicalTextureSlot, kTextureCount> textures;
    // FIFO of logical texture indices currently resident -- front() is the
    // next eviction victim ("evict oldest"). Reaches size kResidentBudget
    // after the initial fill and stays there for the rest of the run
    // (every eviction removes exactly one and adds exactly one).
    std::deque<uint32_t> residencyOrder;
    // Next logical texture index to stream in, wrapping mod kTextureCount
    // -- this sample keeps cycling indefinitely (in --present mode) rather
    // than stopping after one pass through all 24, which also exercises
    // BindlessTable's release/re-register reuse path repeatedly rather
    // than just once.
    uint32_t nextToLoad = kResidentBudget;

    VkSampler sampler = VK_NULL_HANDLE;
    rx::rhi::BindlessHandle samplerHandle;

    std::optional<rx::rhi::MeshBuffers> quadMesh;

    rx::rhi::DeletionQueue deletionQueue;

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

// Full teardown: drains the DeletionQueue first (waiting the device idle
// first, per its own documented contract -- DeletionQueue's destructor
// alone does NOT do this, it only logs an error if anything is still
// pending), then destroys the pipeline/sampler. Every other Scene member
// (bindlessTable, textures' still-resident Texture2Ds, quadMesh) is real
// RAII and gets destroyed correctly when `scene` itself goes out of scope
// after this call returns -- those were never retired into the
// DeletionQueue (only EVICTED textures are), so there is nothing else to
// drain.
void destroyScene(VkDevice device, Scene& scene) {
    scene.deletionQueue.flushAll([device] { vkDeviceWaitIdle(device); });

    destroyScenePipeline(device, scene);
    if (scene.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, scene.sampler, nullptr);
        scene.sampler = VK_NULL_HANDLE;
    }
}

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
// VkPipeline -- same reflection-driven shape as 03_bindless_mesh's
// buildScenePipeline(), except this shader's set 0 only declares 2
// bindings (images, samplers -- no storage buffers), which is the
// documented "strict subset of the three slots" PipelineLayoutBuilder's
// external-set-0 substitution explicitly supports.
bool buildScenePipeline(VkDevice device, rx::rhi::BindlessTable& bindlessTable, VkFormat colorFormat, Scene& scene) {
    destroyScenePipeline(device, scene);

    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        RX_LOG_ERROR("sample_04_streaming: rx::shader::Compiler::create failed");
        return false;
    }

    rx::shader::CompileResult compileResult =
        compiler->compileFromSource("StreamingModule", kShaderSource, kEntryPointNames);
    if (!compileResult.ok) {
        RX_LOG_ERROR("sample_04_streaming: shader compile failed:\n{}", compileResult.diagnostics);
        return false;
    }

    auto layoutInfo = rx::shader::reflect(compileResult);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("sample_04_streaming: reflect() failed on an otherwise-successful compile");
        return false;
    }
    if (layoutInfo->bindings.size() != 2) {
        RX_LOG_ERROR(
            "sample_04_streaming: shader reflects {} set-0 bindings; expected exactly 2 (images + samplers, no "
            "storage buffers)",
            layoutInfo->bindings.size());
        return false;
    }
    if (layoutInfo->pushRanges.size() != 1 || layoutInfo->pushRanges[0].size != sizeof(PushConstants)) {
        RX_LOG_ERROR(
            "sample_04_streaming: shader reflects an unexpected push-constant shape ({} range(s), size={} vs. "
            "sizeof(PushConstants)={})",
            layoutInfo->pushRanges.size(), layoutInfo->pushRanges.empty() ? 0 : layoutInfo->pushRanges[0].size,
            sizeof(PushConstants));
        return false;
    }

    // Set-0 substitution: reuse BindlessTable's own real descriptor set
    // layout for set 0 rather than a lookalike PipelineLayoutBuilder would
    // otherwise invent -- see 03_bindless_mesh/main.cpp's header comment
    // and pipeline_layout.h for the full rationale; this is that same
    // mechanism, exercised with a 2-of-3-bindings shader instead of a
    // 3-of-3-bindings one.
    auto layoutBundle =
        rx::rhi::PipelineLayoutBuilder::build(device, *layoutInfo, bindlessTable.descriptorSetLayout());
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("sample_04_streaming: PipelineLayoutBuilder::build failed (externalSet0 shape mismatch?)");
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
            RX_LOG_ERROR("sample_04_streaming: vkCreateShaderModule failed for entry point '{}'", blob.entryPointName);
            destroyScenePipeline(device, scene);
            return false;
        }
        if (blob.entryPointName == "vsMain") {
            scene.vertModule = module;
        } else if (blob.entryPointName == "fsMain") {
            scene.fragModule = module;
        } else {
            RX_LOG_ERROR("sample_04_streaming: unexpected entry point '{}'", blob.entryPointName);
            vkDestroyShaderModule(device, module, nullptr);
            destroyScenePipeline(device, scene);
            return false;
        }
    }
    if (scene.vertModule == VK_NULL_HANDLE || scene.fragModule == VK_NULL_HANDLE) {
        RX_LOG_ERROR("sample_04_streaming: missing vsMain/fsMain shader module after a successful compile");
        destroyScenePipeline(device, scene);
        return false;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = scene.vertModule;
    // Slang's SPIR-V backend always names each entry point's OpEntryPoint
    // "main" regardless of source-level function name (see
    // 02_hotreload/main.cpp's comment on this exact point).
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
    // No culling needed either way (a single flat quad mesh, always facing
    // the camera) -- CULL_MODE_NONE keeps this consistent with
    // 03_bindless_mesh's own choice and costs nothing at this scale.
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

    // No depth attachment format at all -- see this file's header comment
    // on why this sample has no depth buffer.
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
    pipelineInfo.layout = scene.layoutBundle.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;  // dynamic rendering
    pipelineInfo.basePipelineIndex = -1;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &scene.pipeline) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_04_streaming: vkCreateGraphicsPipelines failed");
        destroyScenePipeline(device, scene);
        return false;
    }

    return true;
}

// Creates + uploads logical texture `logicalIndex`'s Texture2D and
// registers it into `bindlessTable` IMMEDIATELY, wiring it into
// `scene.textures`. Only safe to call when no frame has yet been
// recorded against `scene`'s BindlessTable at all (i.e. the initial
// fill in createScene(), before runHeadless()/runPresent()'s loop
// starts) -- at that point there is no pending command buffer for the
// immediate registerSampledImage() call to race against, so the
// "DESCRIPTOR REWRITE SAFETY" concern in this file's header comment does
// not apply. Eviction (evictOldestAndStreamInNext(), below) does NOT use
// this function for the incoming texture -- it must defer the
// registration itself, not just the destruction, so it builds its own
// shared_ptr<Texture2D> and calls registerSampledImage() from inside a
// DeletionQueue-retired callback instead.
bool loadLogicalTextureImmediate(VkPhysicalDevice physicalDevice, VkDevice device, rx::rhi::Allocator& allocator,
                                   rx::rhi::Uploader& uploader, Scene& scene,
                                   const std::array<std::array<uint8_t, 3>, kTextureCount>& expectedColors,
                                   uint32_t logicalIndex) {
    auto texture = rx::rhi::Texture2D::create(physicalDevice, device, allocator,
                                                VkExtent2D{kTextureSize, kTextureSize}, kTextureFormat,
                                                VK_IMAGE_USAGE_SAMPLED_BIT, /*requestedMipLevels=*/1);
    if (!texture.has_value()) {
        RX_LOG_ERROR("sample_04_streaming: Texture2D::create failed for logical texture {}", logicalIndex);
        return false;
    }

    std::vector<uint8_t> pixels = makeFlatColorTexture(kTextureSize, expectedColors[logicalIndex]);
    if (!uploader.uploadToImage(*texture, pixels.data(), pixels.size(), /*generateMips=*/false)) {
        RX_LOG_ERROR("sample_04_streaming: uploadToImage failed for logical texture {}", logicalIndex);
        return false;
    }

    auto handle = scene.bindlessTable.registerSampledImage(texture->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!handle.isValid()) {
        RX_LOG_ERROR("sample_04_streaming: registerSampledImage failed for logical texture {}", logicalIndex);
        return false;
    }

    scene.textures[logicalIndex].texture = std::make_shared<rx::rhi::Texture2D>(std::move(*texture));
    scene.textures[logicalIndex].handle = handle;
    return true;
}

// THE eviction-while-in-flight mechanism this whole sample exists to
// prove -- see this file's header comment ("FRAME-LAG SAFETY ARGUMENT"
// and "DESCRIPTOR REWRITE SAFETY") for why BOTH the destruction of the
// victim's old resource AND the descriptor-slot rewrite for the incoming
// resource must be deferred to the same fence-confirmed point, not just
// the destruction.
bool evictOldestAndStreamInNext(VkPhysicalDevice physicalDevice, VkDevice device, rx::rhi::Allocator& allocator,
                                  rx::rhi::Uploader& uploader, Scene& scene,
                                  const std::array<std::array<uint8_t, 3>, kTextureCount>& expectedColors,
                                  uint64_t currentFrameNumber) {
    if (scene.residencyOrder.empty()) {
        // Should not happen with this sample's stream interval (always
        // well above the 2-frame registration lag below), but a defensive
        // guard against calling front()/pop_front() on an empty deque is
        // cheap insurance if a future edit ever shortens that interval.
        RX_LOG_ERROR("sample_04_streaming: evictOldestAndStreamInNext called with no resident textures");
        return false;
    }
    uint32_t victim = scene.residencyOrder.front();
    scene.residencyOrder.pop_front();
    uint32_t incoming = scene.nextToLoad;
    scene.nextToLoad = (scene.nextToLoad + 1) % kTextureCount;

    // Host-side bookkeeping only -- safe immediately, per bindless.h's
    // RELEASE-SAFETY CONTRACT (release() never touches the GPU, never
    // rewrites the descriptor). The victim's grid cell stops being drawn
    // starting THIS frame's recording (recordGridDraws() only ever checks
    // `scene.textures[k].texture`, cleared right below) -- that is what
    // "evicted" means from the draw loop's perspective, independent of
    // when the underlying resource is actually destroyed or when its
    // slot is actually rewritten, both deferred below.
    scene.bindlessTable.release(scene.textures[victim].handle);
    std::shared_ptr<rx::rhi::Texture2D> victimTexture = std::move(scene.textures[victim].texture);
    scene.textures[victim].texture = nullptr;
    scene.textures[victim].handle = rx::rhi::BindlessHandle{};

    // Create + upload the incoming texture's Texture2D immediately: a
    // brand-new VkImage has no descriptor slot assigned to it at all yet,
    // so there is no in-flight hazard here whatsoever -- only the
    // eventual registerSampledImage() call (which WRITES a descriptor
    // slot -- deterministically the one just released above, see
    // "DESCRIPTOR REWRITE SAFETY") needs to wait.
    auto newTexture = rx::rhi::Texture2D::create(physicalDevice, device, allocator,
                                                  VkExtent2D{kTextureSize, kTextureSize}, kTextureFormat,
                                                  VK_IMAGE_USAGE_SAMPLED_BIT, /*requestedMipLevels=*/1);
    if (!newTexture.has_value()) {
        RX_LOG_ERROR("sample_04_streaming: Texture2D::create failed for logical texture {}", incoming);
        return false;
    }
    std::vector<uint8_t> pixels = makeFlatColorTexture(kTextureSize, expectedColors[incoming]);
    if (!uploader.uploadToImage(*newTexture, pixels.data(), pixels.size(), /*generateMips=*/false)) {
        RX_LOG_ERROR("sample_04_streaming: uploadToImage failed for logical texture {}", incoming);
        return false;
    }
    uploader.flush();
    std::shared_ptr<rx::rhi::Texture2D> incomingTexture = std::make_shared<rx::rhi::Texture2D>(std::move(*newTexture));

    // THE fix: both halves of this eviction cycle -- destroying the
    // victim's old resource, and writing the incoming resource into the
    // slot that (deterministically) just freed up -- are deferred into
    // ONE DeletionQueue retirement tagged with the CURRENT frame number,
    // run only once that frame's fence is confirmed signaled (i.e. once
    // frame N-1, and everything before it, is also guaranteed complete --
    // see this file's header comment). `scene` is captured by raw pointer:
    // safe because this callback only ever runs from inside
    // onFrameFenceSignaled()/flushAll(), both called on `scene`'s owning
    // frame loop / destroyScene() while `scene` itself is still alive.
    Scene* scenePtr = &scene;
    scene.deletionQueue.retire(
        [scenePtr, victimTexture, incomingTexture, incoming]() {
            // victimTexture's Texture2D is destroyed as an ordinary side
            // effect of this closure (and its captured shared_ptr) being
            // destroyed once DeletionQueue removes this entry -- nothing
            // to do here for it explicitly, same pattern as every other
            // retire() call in this codebase.
            auto handle = scenePtr->bindlessTable.registerSampledImage(incomingTexture->view(),
                                                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (!handle.isValid()) {
                RX_LOG_ERROR("sample_04_streaming: deferred registerSampledImage failed for logical texture {}",
                             incoming);
                return;
            }
            scenePtr->textures[incoming].texture = incomingTexture;
            scenePtr->textures[incoming].handle = handle;
            scenePtr->residencyOrder.push_back(incoming);
        },
        currentFrameNumber);

    return true;
}

std::optional<Scene> createScene(VkPhysicalDevice physicalDevice, VkDevice device, rx::rhi::Allocator& allocator,
                                  rx::rhi::Uploader& uploader, VkFormat colorFormat,
                                  const std::array<std::array<uint8_t, 3>, kTextureCount>& expectedColors) {
    // Capacities: sampledImages sized generously above the resident
    // budget (16 vs. 8 -- BindlessTable rejects capacity 0 outright, and
    // headroom costs nothing, same reasoning as 03_bindless_mesh).
    // storageBuffers=1 exists purely to satisfy BindlessTable::create's
    // own precondition (every capacity must be nonzero -- it always
    // builds all 3 fixed bindings regardless of what any one shader
    // actually uses) -- this sample's shader never declares or indexes
    // that binding at all (see this file's header comment).
    rx::rhi::BindlessTable::Capacities capacities{/*sampledImages=*/16, /*samplers=*/4, /*storageBuffers=*/1};
    auto bindlessTable = rx::rhi::BindlessTable::create(physicalDevice, device, capacities);
    if (!bindlessTable.has_value()) {
        RX_LOG_ERROR("sample_04_streaming: BindlessTable::create failed");
        return std::nullopt;
    }

    Scene scene{std::move(*bindlessTable)};

    // --- Shared quad mesh: one unit-ish quad, reused for every grid cell
    // via a per-draw push-constant translation, not per-cell vertex data.
    constexpr float h = kCellHalfSize;
    std::array<Vertex, 4> vertices{{
        {{-h, -h, 0.0F}, {0.0F, 0.0F}},
        {{h, -h, 0.0F}, {1.0F, 0.0F}},
        {{h, h, 0.0F}, {1.0F, 1.0F}},
        {{-h, h, 0.0F}, {0.0F, 1.0F}},
    }};
    std::array<uint32_t, 6> indices{0, 1, 2, 0, 2, 3};

    auto quadMesh = rx::rhi::MeshBuffers::create(allocator, uploader, vertices.data(), vertices.size() * sizeof(Vertex),
                                                   indices.data(), indices.size() * sizeof(uint32_t),
                                                   static_cast<uint32_t>(indices.size()), VK_INDEX_TYPE_UINT32);
    if (!quadMesh.has_value()) {
        RX_LOG_ERROR("sample_04_streaming: MeshBuffers::create failed for the shared quad");
        return std::nullopt;
    }
    scene.quadMesh = std::move(*quadMesh);

    // --- Sampler: NEAREST (see this file's header comment on why flat
    // hues + nearest sampling make the headless probe assertion exact),
    // CLAMP_TO_EDGE (no wrap needed -- every draw samples the whole [0,1]
    // UV range of its own single-color texture).
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
        RX_LOG_ERROR("sample_04_streaming: vkCreateSampler failed");
        return std::nullopt;
    }
    scene.samplerHandle = scene.bindlessTable.registerSampler(scene.sampler);
    if (!scene.samplerHandle.isValid()) {
        RX_LOG_ERROR("sample_04_streaming: registerSampler failed");
        destroyScene(device, scene);
        return std::nullopt;
    }

    // --- Initial fill: logical textures 0..kResidentBudget-1 resident
    // before the first frame is ever recorded -- immediate registration
    // is safe here specifically because no frame has been recorded yet
    // (see loadLogicalTextureImmediate()'s own comment).
    for (uint32_t i = 0; i < kResidentBudget; ++i) {
        if (!loadLogicalTextureImmediate(physicalDevice, device, allocator, uploader, scene, expectedColors, i)) {
            destroyScene(device, scene);
            return std::nullopt;
        }
        scene.residencyOrder.push_back(i);
    }
    uploader.flush();

    if (!buildScenePipeline(device, scene.bindlessTable, colorFormat, scene)) {
        destroyScene(device, scene);
        return std::nullopt;
    }

    return scene;
}

// Records one draw per currently-resident grid cell into `cmd` (already
// inside a vkCmdBeginRendering scope, viewport/scissor already set).
// Non-resident cells are skipped entirely -- see this file's header
// comment on why VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT is what makes
// that safe against the bound-but-not-fully-populated descriptor set.
void recordGridDraws(VkCommandBuffer cmd, const Scene& scene, const glm::mat4& viewProj) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.pipeline);

    VkDescriptorSet set = scene.bindlessTable.descriptorSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.layoutBundle.layout, 0, 1, &set, 0, nullptr);

    VkBuffer vertexBuffer = scene.quadMesh->vertexBuffer();
    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &vertexOffset);
    vkCmdBindIndexBuffer(cmd, scene.quadMesh->indexBuffer(), 0, scene.quadMesh->indexType());

    for (uint32_t k = 0; k < kTextureCount; ++k) {
        if (!scene.textures[k].texture) {
            continue;  // not resident right now -- this cell is simply not drawn this frame.
        }

        glm::vec2 pos = cellPosition(k);
        glm::mat4 mvp = viewProj * glm::translate(glm::mat4(1.0F), glm::vec3(pos, 0.0F));

        PushConstants push{};
        // Transposed before upload -- Slang's default matrix layout for
        // anything this rx::shader::Compiler compiles (including
        // push-constant members) is ROW-major, while GLM stores mat4
        // column-major; see rx_shader/compiler.h's "MATRIX LAYOUT" doc
        // comment and 03_bindless_mesh/main.cpp's updateTransforms() for
        // the empirically-found rationale this sample reuses verbatim.
        push.mvp = glm::transpose(mvp);
        push.textureIndex = scene.textures[k].handle.index();
        push.samplerIndex = scene.samplerHandle.index();

        vkCmdPushConstants(cmd, scene.layoutBundle.layout, scene.pushConstantStages, scene.pushConstantOffset,
                            scene.pushConstantSize, &push);
        vkCmdDrawIndexed(cmd, scene.quadMesh->indexCount(), 1, 0, 0, 0);
    }
}

// --- Headless offscreen render target (one per frame-in-flight slot) ------

// One dedicated offscreen color image + readback buffer, reused across the
// whole headless run (never recreated per frame) -- see runHeadless()'s
// own comment for why 2 of these (matching FrameSync::framesInFlight())
// are needed instead of 1: genuine CPU/GPU overlap between adjacent frames
// is the entire point of this sample's headless gate, and a single shared
// offscreen image would either force full serialization (defeating that
// point) or write-after-write-hazard across overlapping frames.
struct OffscreenSlot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::optional<rx::rhi::Buffer> readback;

    // True once this slot has been rendered into at least once -- decides
    // whether this frame's first layout transition source is UNDEFINED
    // (the image's real initial layout) or TRANSFER_SRC_OPTIMAL (the
    // layout this slot's own previous use always leaves it in, safe to
    // transition FROM again because we've just confirmed via fence wait
    // that previous use has completed).
    bool everUsed = false;
    // Which frame number's content currently sits in `image`/`readback` --
    // only meaningful once `everUsed` is true.
    uint64_t frameNumber = 0;
    // Which logical textures were actually resident (and therefore drawn)
    // at each grid cell WHEN this slot's content was recorded -- captured
    // at record time, consulted at probe time (2 frames later), since by
    // then the live scene.textures[] state has moved on to a different
    // eviction cycle. Without this snapshot there would be no way to know
    // what SHOULD be on screen in a frame recorded 2 evictions ago.
    std::array<bool, kTextureCount> residentAtRecordTime{};
};

std::optional<uint32_t> findMemoryTypeIndex(const VkPhysicalDeviceMemoryProperties& memProps, uint32_t typeBits,
                                             VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1U << i)) != 0U && (memProps.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<OffscreenSlot> createOffscreenSlot(VkPhysicalDevice physicalDevice, VkDevice device,
                                                   rx::rhi::Allocator& allocator, VkFormat format) {
    OffscreenSlot slot;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {kHeadlessWidth, kHeadlessHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &slot.image) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_04_streaming: vkCreateImage(offscreen slot) failed");
        return std::nullopt;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, slot.image, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    auto memoryTypeIndex = findMemoryTypeIndex(memProps, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memoryTypeIndex.has_value()) {
        RX_LOG_ERROR("sample_04_streaming: no device-local memory type found for the offscreen slot");
        vkDestroyImage(device, slot.image, nullptr);
        return std::nullopt;
    }

    VkMemoryAllocateInfo memAllocInfo{};
    memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAllocInfo.allocationSize = memReq.size;
    memAllocInfo.memoryTypeIndex = *memoryTypeIndex;
    if (vkAllocateMemory(device, &memAllocInfo, nullptr, &slot.memory) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_04_streaming: vkAllocateMemory(offscreen slot) failed");
        vkDestroyImage(device, slot.image, nullptr);
        return std::nullopt;
    }
    if (vkBindImageMemory(device, slot.image, slot.memory, 0) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_04_streaming: vkBindImageMemory(offscreen slot) failed");
        vkFreeMemory(device, slot.memory, nullptr);
        vkDestroyImage(device, slot.image, nullptr);
        return std::nullopt;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = slot.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &slot.view) != VK_SUCCESS) {
        RX_LOG_ERROR("sample_04_streaming: vkCreateImageView(offscreen slot) failed");
        vkFreeMemory(device, slot.memory, nullptr);
        vkDestroyImage(device, slot.image, nullptr);
        return std::nullopt;
    }

    auto readback = allocator.createHostVisibleBuffer(kHeadlessPixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!readback.has_value()) {
        RX_LOG_ERROR("sample_04_streaming: createHostVisibleBuffer(readback) failed for offscreen slot");
        vkDestroyImageView(device, slot.view, nullptr);
        vkFreeMemory(device, slot.memory, nullptr);
        vkDestroyImage(device, slot.image, nullptr);
        return std::nullopt;
    }
    slot.readback = std::move(*readback);

    return slot;
}

void destroyOffscreenSlot(VkDevice device, OffscreenSlot& slot) {
    slot.readback.reset();
    if (slot.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, slot.view, nullptr);
        slot.view = VK_NULL_HANDLE;
    }
    if (slot.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, slot.image, nullptr);
        slot.image = VK_NULL_HANDLE;
    }
    if (slot.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, slot.memory, nullptr);
        slot.memory = VK_NULL_HANDLE;
    }
}

// --- Headless mode: 60 real frames through a genuine 2-frames-in-flight
// offscreen loop, with deferred readback probes -----------------------------
int runHeadless() {
    auto window = rx::platform::Window::create("rx_streaming_sample", static_cast<int>(kHeadlessWidth),
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

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
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
    const VkFormat targetFormat = device->swapchainFormat();

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

    // No swapchain images are ever acquired/presented in this mode -- 0
    // renderFinished semaphores needed; every other FrameSync facility
    // (fences, per-frame-in-flight command pools/buffers, frameNumber())
    // is used exactly as production code would use it.
    auto frameSync = rx::rhi::FrameSync::create(vkDevice, device->graphicsQueueFamily(), /*swapchainImageCount=*/0);
    if (!frameSync.has_value()) {
        RX_LOG_ERROR("FrameSync::create failed");
        return 1;
    }

    const auto expectedColors = computeExpectedColors();
    std::array<std::array<uint8_t, 3>, kTextureCount> expectedColorsSrgb{};
    for (uint32_t i = 0; i < kTextureCount; ++i) {
        expectedColorsSrgb[i] = srgbEncodeColor(expectedColors[i]);
    }

    auto scene = createScene(device->physicalDevice(), vkDevice, *allocator, *uploader, targetFormat, expectedColors);
    if (!scene.has_value()) {
        RX_LOG_ERROR("createScene failed");
        return 1;
    }

    constexpr uint32_t kSlotCount = rx::rhi::FrameSync::framesInFlight();
    std::array<std::optional<OffscreenSlot>, kSlotCount> slots;
    for (auto& slot : slots) {
        slot = createOffscreenSlot(device->physicalDevice(), vkDevice, *allocator, targetFormat);
        if (!slot.has_value()) {
            RX_LOG_ERROR("createOffscreenSlot failed");
            destroyScene(vkDevice, *scene);
            return 1;
        }
    }

    // Static camera, static grid -- computed once, reused for every
    // frame's draws (unlike 03_bindless_mesh's orbiting camera, nothing
    // here changes over time except which cells are resident).
    const glm::mat4 viewProj = makeProjection(1.0F) * makeViewMatrix();

    std::array<bool, kTextureCount> observedResident{};
    bool ok = true;

    for (uint32_t iter = 0; iter < kHeadlessTotalFrames + kSlotCount; ++iter) {
        uint32_t slotIndex = frameSync->currentFrameIndex();
        OffscreenSlot& slot = *slots[slotIndex];

        VkFence fence = frameSync->currentFence();
        if (vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            RX_LOG_ERROR("vkWaitForFences failed in headless streaming loop");
            ok = false;
            break;
        }

        // This slot's PREVIOUS content (if any) is now confirmed complete
        // on the GPU -- safe to read back and probe, and safe to tell the
        // DeletionQueue that frame `slot.frameNumber` (and everything
        // before it, transitively -- see this file's header comment) has
        // finished.
        if (slot.everUsed) {
            slot.readback->invalidate();
            const auto* pixels = static_cast<const uint8_t*>(slot.readback->mappedData());
            for (uint32_t k = 0; k < kTextureCount; ++k) {
                if (!slot.residentAtRecordTime[k]) {
                    continue;
                }
                PixelCoord probe = cellProbePixel(k, kHeadlessWidth, kHeadlessHeight);
                const uint8_t* p = pixels + (static_cast<size_t>(probe.y) * kHeadlessWidth + probe.x) * 4;
                if (matchesExpectedColor(p, expectedColors[k], expectedColorsSrgb[k])) {
                    observedResident[k] = true;
                }
            }
            scene->deletionQueue.onFrameFenceSignaled(slot.frameNumber);
        }

        if (iter >= kHeadlessTotalFrames) {
            // Drain-only iteration: every real frame has already been
            // submitted; this (and the other kSlotCount-1 trailing
            // iterations) exist purely to wait for and probe/drain the
            // last few frames' results.
            continue;
        }

        if (vkResetFences(vkDevice, 1, &fence) != VK_SUCCESS) {
            RX_LOG_ERROR("vkResetFences failed in headless streaming loop");
            ok = false;
            break;
        }

        uint64_t thisFrameNumber = frameSync->frameNumber();
        // Skip frame 0: an eviction here, before frame 0 has ever been
        // recorded, would evict logical texture 0 before it was ever
        // actually drawn/probed even once -- see this file's header
        // comment's frame-lag argument (which assumes the initial fill is
        // still fully intact for at least the first recorded frame).
        if (thisFrameNumber > 0 && thisFrameNumber % kHeadlessStreamIntervalFrames == 0) {
            if (!evictOldestAndStreamInNext(device->physicalDevice(), vkDevice, *allocator, *uploader, *scene,
                                             expectedColors, thisFrameNumber)) {
                ok = false;
                break;
            }
        }

        for (uint32_t k = 0; k < kTextureCount; ++k) {
            slot.residentAtRecordTime[k] = static_cast<bool>(scene->textures[k].texture);
        }

        VkCommandPool pool = frameSync->currentCommandPool();
        if (vkResetCommandPool(vkDevice, pool, 0) != VK_SUCCESS) {
            RX_LOG_ERROR("vkResetCommandPool failed in headless streaming loop");
            ok = false;
            break;
        }
        VkCommandBuffer cmd = frameSync->currentCommandBuffer();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
            RX_LOG_ERROR("vkBeginCommandBuffer failed in headless streaming loop");
            ok = false;
            break;
        }

        VkImageLayout fromLayout = slot.everUsed ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        rx::rhi::transitionImage(cmd, slot.image, fromLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = slot.view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = VkClearColorValue{{0.05F, 0.05F, 0.08F, 1.0F}};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = VkRect2D{{0, 0}, {kHeadlessWidth, kHeadlessHeight}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport viewport{0.0F, 0.0F, static_cast<float>(kHeadlessWidth), static_cast<float>(kHeadlessHeight), 0.0F,
                             1.0F};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, {kHeadlessWidth, kHeadlessHeight}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        recordGridDraws(cmd, *scene, viewProj);

        vkCmdEndRendering(cmd);

        rx::rhi::transitionImage(cmd, slot.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {kHeadlessWidth, kHeadlessHeight, 1};
        vkCmdCopyImageToBuffer(cmd, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.readback->handle(), 1,
                               &region);

        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            RX_LOG_ERROR("vkEndCommandBuffer failed in headless streaming loop");
            ok = false;
            break;
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        // No wait/signal semaphores -- there is no swapchain and no
        // cross-queue dependency in this mode; the fence alone is enough
        // to know when this submission's work (rendering + copy-out) has
        // completed.
        if (vkQueueSubmit(device->graphicsQueue(), 1, &submitInfo, fence) != VK_SUCCESS) {
            RX_LOG_ERROR("vkQueueSubmit failed in headless streaming loop");
            ok = false;
            break;
        }

        slot.frameNumber = thisFrameNumber;
        slot.everUsed = true;

        frameSync->advanceFrame();
    }

    if (vkDeviceWaitIdle(vkDevice) != VK_SUCCESS) {
        RX_LOG_ERROR("vkDeviceWaitIdle failed at headless streaming shutdown");
        ok = false;
    }
    for (auto& slot : slots) {
        destroyOffscreenSlot(vkDevice, *slot);
    }
    // destroyScene() itself drains the DeletionQueue (flushAll with its
    // own waitIdleFirst) before tearing down the pipeline/sampler -- by
    // this point every retirement tagged frame 0..kHeadlessTotalFrames-1
    // should already have been processed by the loop's own
    // onFrameFenceSignaled() calls above, so this is a defensive,
    // documented-as-safe no-op rather than load-bearing cleanup; kept
    // anyway for the same shutdown discipline every other sample follows.
    destroyScene(vkDevice, *scene);

    bool pass = ok;
    uint32_t missingCount = 0;
    for (uint32_t k = 0; k < kTextureCount; ++k) {
        if (!observedResident[k]) {
            RX_LOG_ERROR("sample_04_streaming: logical texture {} was never observed resident on screen", k);
            ++missingCount;
            pass = false;
        }
    }
    RX_LOG_INFO("sample_04_streaming: {} / {} logical textures observed resident at some point",
                kTextureCount - missingCount, kTextureCount);

    if (context->hasValidationErrors()) {
        RX_LOG_ERROR("Vulkan validation layer reported errors during this run");
        pass = false;
    }

    if (pass) {
        RX_LOG_INFO("streaming headless gate PASSED");
        return 0;
    }
    RX_LOG_ERROR("streaming headless gate FAILED");
    return 1;
}

// --- --present mode: real window, continuously cycling grid ----------------
int runPresent() {
    auto window = rx::platform::Window::create("rx_streaming_sample (--present)", static_cast<int>(kPresentWidth),
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

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
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

    const auto expectedColors = computeExpectedColors();

    auto scene =
        createScene(device->physicalDevice(), vkDevice, *allocator, *uploader, device->swapchainFormat(), expectedColors);
    if (!scene.has_value()) {
        RX_LOG_ERROR("createScene failed");
        return 1;
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
                RX_LOG_ERROR("sample_04_streaming: vkCreateImageView(swapchain image {}) failed", i);
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

    RX_LOG_INFO(
        "--present: window open ({}x{}); grid cycling roughly 1 texture/second -- close the window to exit",
        kPresentWidth, kPresentHeight);

    const auto startTime = std::chrono::steady_clock::now();
    float lastStreamSeconds = 0.0F;

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

        // This slot's fence just confirmed frame (frameNumber() -
        // framesInFlight()) -- and, transitively, everything before it --
        // has completed on the GPU (same single-queue submission-order
        // argument as this file's header comment / runHeadless()); safe
        // to run any DeletionQueue retirements tagged at or before that
        // frame now.
        if (frameSync->frameNumber() >= rx::rhi::FrameSync::framesInFlight()) {
            scene->deletionQueue.onFrameFenceSignaled(frameSync->frameNumber() - rx::rhi::FrameSync::framesInFlight());
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
        if (elapsedSeconds - lastStreamSeconds >= kPresentStreamIntervalSeconds) {
            lastStreamSeconds = elapsedSeconds;
            if (!evictOldestAndStreamInNext(device->physicalDevice(), vkDevice, *allocator, *uploader, *scene,
                                             expectedColors, frameSync->frameNumber())) {
                ok = false;
                break;
            }
        }

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
        rx::rhi::transitionImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = swapchainViews[acquire.imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = VkClearColorValue{{0.05F, 0.05F, 0.08F, 1.0F}};

        const VkExtent2D extent = device->swapchainExtent();
        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea = VkRect2D{{0, 0}, extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(cmd, &renderingInfo);

        VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F, 1.0F};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        glm::mat4 viewProj = makeProjection(aspect) * makeViewMatrix();
        recordGridDraws(cmd, *scene, viewProj);

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
            if (!device->recreateSwapchain(surface) ||
                !frameSync->onSwapchainRecreated(static_cast<uint32_t>(device->swapchainImages().size())) ||
                !createSwapchainViews()) {
                RX_LOG_ERROR("swapchain recreation failed after present NeedsRecreate");
                ok = false;
                break;
            }
        } else if (presentStatus == rx::rhi::SwapchainStatus::DeviceLost) {
            RX_LOG_ERROR("device lost during present; exiting present loop");
            ok = false;
            break;
        }

        frameSync->advanceFrame();
    }

    vkDeviceWaitIdle(vkDevice);
    destroySwapchainViews();
    destroyScene(vkDevice, *scene);  // drains the DeletionQueue (flushAll) before tearing down.

    if (context->hasValidationErrors()) {
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
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--present") {
            presentMode = true;
        }
    }

    if (presentMode) {
        return runPresent();
    }
    return runHeadless();
}
