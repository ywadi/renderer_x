#pragma once

// rx_cluster/cluster_lighting.h -- Phase 5 Stage 2 Task 14 [#50, gate
// rulings-2026-08-20.md's "T14 (#50)" section + gate/
// matrix-p5t14-froxel-clustering.md]: the camera-frustum froxel grid +
// GPU-compute clustered light assignment. The charter's clustered Forward+
// core (docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md:
// 414-416) -- given a Scene's live point/spot lights and a Camera,
// produces a per-froxel light-INDEX list (a [offset,count) pair per
// froxel into a global light-index buffer) via three deterministic
// compute dispatches (count / prefix-sum / scatter), through Task 2's
// compute-class render-graph API. First production consumer of Task 2's
// ComputePipelineCache alongside Task 9's IBL bake (rx_ibl), and the
// SECOND -- rx_scene/froxel_grid.h is the first, device-free-math-only one.
//
// PORT PROVENANCE [gate ruling: "T14 (#50): froxel ALGORITHM ported from
// Filament v1.75.0's CPU froxelizer (grid math, exponential Z-slicing,
// bounds tests); the compute-shader expression is RendererX's own; light
// lists via two-pass count+prefix-sum+scatter (NO 256-light bitset cap)"]
// -- see rx_scene/froxel_grid.h's own top comment for the grid-math
// citations (google/filament v1.75.0, 0e58877c09afb1aacd09ff640f74d2adcd2a7e80,
// Froxelizer.{h,cpp} + Intersections.h) and shaders/cluster/*.slang's own
// header comments for the compute-EXPRESSION design (this project's own,
// per the gate matrix's Open Question #1 licensed scope -- Filament's own
// froxelizer is CPU-side, no GLSL compute source exists to translate).
//
// MODULE OWNERSHIP [implementer decision, matching rx_ibl's own precedent
// exactly (bake.h's "MODULE OWNERSHIP" comment) -- not escalated]: a new,
// self-contained library, NOT folded into rx_scene. rx_scene is explicitly,
// deliberately device-free (rx_scene/CMakeLists.txt's own top comment: "no
// VkDevice, no rx_rhi_vk") -- this module's whole job is dispatching real
// compute passes through rx_graph/rx_rhi_vk, which would break that
// invariant. The plan's own Task 14 file list ("src/rx_scene froxel/
// cluster orchestration (graph compute passes)") is followed in SPIRIT,
// split at the natural device-free/device-touching seam this codebase
// already established for the IDENTICAL problem in Task 9 (bake.h's own
// module-ownership precedent): the device-free GRID MATH lives in
// rx_scene (froxel_grid.h, mirroring light_math.h), the GPU-compute
// ORCHESTRATION lives here, in a new library depending on rx_scene (for
// that math + Scene's light data) + rx_graph + rx_rhi_vk + rx_shader +
// rx_task.
#include <rx_graph/render_graph.h>
#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_scene/camera.h>
#include <rx_scene/froxel_grid.h>
#include <rx_scene/scene.h>
#include <rx_shader/compiler.h>

#include <glm/glm.hpp>
#include <volk.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace rx::cluster {

// Capacity + grid-sizing knobs. Defaults are deliberately modest (this
// module's own GPU test suite runs the full 3-pass chain many times per
// process -- see froxel_grid.h's own kDefault* constants for why a small
// default keeps lavapipe-driven test iteration fast); a production caller
// is expected to size `maxLightsPerFroxel`/`maxTotalLightIndices` to its
// own content's real scale (this task's own stress benchmark reports real
// numbers at a content-scale well past these defaults -- see
// task-14-report.md).
struct ClusterParams {
    uint32_t targetFroxelBudget = rx::scene::froxel::kDefaultFroxelBudget;
    uint32_t sliceCountZ = rx::scene::froxel::kDefaultSliceCountZ;
    float zLightNear = rx::scene::froxel::kDefaultZLightNear;
    float zLightFar = rx::scene::froxel::kDefaultZLightFar;

    // Per-froxel capacity axis [plan:504-505's own "max lights per froxel"
    // acceptance line]. A froxel whose TRUE intersecting-light count
    // exceeds this is truncated (never corrupted/wrapped) -- see
    // shaders/cluster/froxel_prefix_sum.slang's own header comment.
    uint32_t maxLightsPerFroxel = 128;

    // GLOBAL capacity axis [plan:504-505's own "...  / total" acceptance
    // line] -- the light-index buffer's own declared total size. Bounded
    // only by this declared value, NOT by any fixed per-scene light count
    // (the gate ruling's own "NO 256-light bitset cap" -- this generalizes
    // to an arbitrary total light count, matrix Open Question #2).
    uint32_t maxTotalLightIndices = 1U << 16;

    // Illuminance-cutoff constant `rx::scene::froxel::pointLightCullRadius()`
    // uses for any point/spot light with `range <= 0` (unconfigured) --
    // see that function's own header comment.
    float cullCutoffLux = 1.0F;
};

// One point/spot light's view-space-transformed data for froxel
// assignment -- the C++ mirror of shaders/cluster/froxel_common.slang's
// own `ClusterLightGpu` (kept manually in sync -- no shared C++/Slang
// codegen exists in this toolchain). Every field is a glm::vec4 (never a
// bare glm::vec3) so this struct's layout matches the Slang
// StructuredBuffer element layout unambiguously (three consecutive
// float4s, 48 bytes, no vec3-padding reasoning needed on either side) --
// enforced by the static_assert below.
struct ClusterLightGpu {
    // xyz = view-space position, w = cull radius (rx::scene::froxel::
    // pointLightCullRadius()'s own output).
    glm::vec4 viewPositionRadius{0.0F};
    // xyz = spot cone axis in VIEW SPACE (a finite, valid unit vector even
    // for a point light -- unused there but never garbage), w =
    // coneSinInverse (rx::scene::froxel::spotConeViewSpace()'s own output,
    // spot only).
    glm::vec4 viewAxisSinInverse{0.0F, 0.0F, -1.0F, 0.0F};
    // x = coneCosSquared (spot only), y = isSpot (1.0F spot / 0.0F point,
    // read by the GPU as `> 0.5` -- shaders/cluster/froxel_common.slang's
    // own `lightIntersectsFroxel()`), z/w unused (zero).
    glm::vec4 cosSquaredFlags{0.0F};
};
static_assert(sizeof(ClusterLightGpu) == 48,
              "ClusterLightGpu must match shaders/cluster/froxel_common.slang's ClusterLightGpu layout exactly "
              "(three consecutive float4s) -- a size drift here silently corrupts the GPU-side StructuredBuffer read");

// Builds this frame's flat, tightly-packed light array from `scene`'s
// currently-alive Point/Spot lights (Directional lights never participate
// in clustering -- they light everything uniformly by definition, KHR
// spec: "not attenuated" -- froxel assignment has no meaning for them),
// transforming position/direction into `viewMatrix`'s own space (`camera.
// view()`, typically) and deriving each light's own cull radius
// (rx::scene::froxel::pointLightCullRadius()). Iterates
// `scene.lightRecordsSpan()`/`lightAliveSpan()` directly (Task 14's own
// addition to Scene, scene.h) -- O(sceneLightCount), zero per-light
// virtual dispatch/handle round-trip.
//
// Device-free (no VkDevice) -- directly reusable as the CPU ORACLE this
// task's own GPU membership tests build their "what SHOULD be assigned"
// answer from (rx::scene::froxel::sphereIntersectsFroxel()/
// spotIntersectsFroxel() applied to this SAME light list, entirely on the
// CPU, independent of the GPU dispatch under test).
[[nodiscard]] std::vector<ClusterLightGpu> buildClusterLightList(const rx::scene::Scene& scene,
                                                                    const glm::mat4& viewMatrix,
                                                                    float cullCutoffLux = 1.0F);

// Owns the three compiled+cached compute PSOs (count/prefix-sum/scatter)
// plus a small descriptor pool -- built ONCE (Slang compilation is real,
// non-negligible cost; a per-frame caller must not pay it every frame,
// this project's own performance-first standing rule) and reused across
// every addClusterPasses() call for this object's lifetime.
//
// DESCRIPTOR POOL / FRAMES-IN-FLIGHT SCOPE BOUNDARY [documented, not
// silently shipped]: this object owns ONE descriptor pool, reset
// (vkResetDescriptorPool) at the START of every addClusterPasses() call --
// safe for this task's own GPU test suite (each call's own graph is
// realized/executed/GPU-waited via CommandContext::runOnce() before the
// NEXT addClusterPasses() call ever runs, so no descriptor set from a
// prior call is still in flight when it is reset) and for any caller
// following the SAME discipline. A LIVE per-frame consumer with N frames
// in flight (Task 15's own integration) needs N such pools (or another
// frames-in-flight-safe allocation strategy) if it calls this without an
// intervening GPU wait every frame -- flagged here so that caller does not
// inherit a hidden race.
//
// Main-thread-only (D5) -- builds real Vulkan pipeline objects, same
// convention as every other rx_rhi_vk/rx_graph factory.
class ClusterPipelines {
public:
    ~ClusterPipelines();
    ClusterPipelines(ClusterPipelines&&) noexcept;
    ClusterPipelines& operator=(ClusterPipelines&&) noexcept;
    ClusterPipelines(const ClusterPipelines&) = delete;
    ClusterPipelines& operator=(const ClusterPipelines&) = delete;

    // Compiles the 3 kernels from `shaderDir` (shaders/cluster/*.slang --
    // froxel_light_count.slang, froxel_prefix_sum.slang,
    // froxel_light_scatter.slang) via `compiler`, building/caching PSOs
    // through `cache` (a caller-owned rx::rhi::ComputePipelineCache --
    // shared with any other compute consumer the caller already has, or a
    // dedicated one; this module does not own or namespace one itself,
    // unlike rx_ibl's bakeEnvironment() -- `cache` is a required parameter
    // here, not created internally, since this object is expected to
    // outlive many calls rather than being built fresh per invocation).
    // Returns std::nullopt (logged) on any compile/reflect/pipeline-build
    // failure.
    static std::optional<ClusterPipelines> create(VkDevice device, rx::rhi::ComputePipelineCache& cache,
                                                   rx::shader::Compiler& compiler,
                                                   const std::filesystem::path& shaderDir);

    // Builds a `rx::scene::froxel::FroxelGridParams` for `camera`/
    // `viewportWidth`/`viewportHeight`/`params` (rx::scene::froxel::
    // buildFroxelGrid()) and declares the count/prefix-sum/scatter compute
    // passes into `graph` under SEVEN FIXED resource names:
    // "clusterTrueCounts", "clusterOffsets", "clusterWriteCounts",
    // "clusterPerFroxelOverflow", "clusterGlobalOverflow",
    // "clusterTotalUsed" (a 1-uint buffer), "clusterLightIndices" (the
    // final [offset,count)-addressed global light-index buffer,
    // `maxTotalLightIndices` uints).
    //
    // STANDALONE, INDEPENDENTLY-BINDABLE OUTPUT [matrix row 6's own
    // acceptance criterion, this task's own shared-grid design note --
    // see this header's own top comment]: every one of those seven names
    // is an ORDINARY named graph resource -- any LATER pass in the SAME
    // `graph` (a Task 15 opaque-lighting pass, a Task 30 froxel-fog pass)
    // reads it via the render graph's own `addStorageBufferInput(name)`,
    // with zero coupling to this class or to each other. This is what
    // makes the grid genuinely SHARED infrastructure from day one, per the
    // charter's own binding ("the grid's layout/bindings are authored for
    // two consumers... a Task 1 spec decision records the shared shape" --
    // this IS that shape: a set of independently-addressable named
    // buffers, not a monolithic opaque-lighting-only descriptor set).
    //
    // `lightsBuffer` is an EXTERNALLY-owned, already-populated storage
    // buffer holding `lightCount` `ClusterLightGpu` entries (typically one
    // frame-slot of an `rx::rhi::PerFrameStorageBuffer` this class does
    // NOT own -- a caller's own per-frame light-upload responsibility,
    // built from `buildClusterLightList()` above) -- captured BY
    // REFERENCE inside the recorded pass callbacks, never declared as a
    // graph resource itself, exactly mirroring `rx::ibl::bakeEnvironment()`'s
    // own `source` parameter convention (bake.h's own header comment: "an
    // EXTERNALLY-owned, already-populated... resource this bake never
    // writes -- bound DIRECTLY... not through a graph declaration").
    //
    // Returns the FroxelGridParams this call computed (a caller needing to
    // interpret froxel indices -- e.g. this task's own tests' CPU oracle,
    // or a future T15 shading pass mapping a pixel to its froxel -- uses
    // this SAME value, never re-derives it).
    [[nodiscard]] rx::scene::froxel::FroxelGridParams addClusterPasses(rx::graph::RenderGraph& graph,
                                                                         VkBuffer lightsBuffer, uint32_t lightCount,
                                                                         const rx::scene::Camera& camera,
                                                                         uint32_t viewportWidth,
                                                                         uint32_t viewportHeight,
                                                                         const ClusterParams& params = {});

    struct Kernel {
        rx::rhi::ComputePipelineCache::Pipeline pso;
        rx::shader::ShaderLayoutInfo layoutInfo;
    };

private:
    ClusterPipelines() = default;

    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    std::optional<Kernel> countKernel_;
    std::optional<Kernel> prefixSumKernel_;
    std::optional<Kernel> scatterKernel_;

    // set-0 (empty placeholder, allocated once at create() time) + set-1
    // (this call's real bindings, reallocated every addClusterPasses()
    // call after its own vkResetDescriptorPool()) per kernel.
    VkDescriptorSet countSet0_ = VK_NULL_HANDLE;
    VkDescriptorSet prefixSumSet0_ = VK_NULL_HANDLE;
    VkDescriptorSet scatterSet0_ = VK_NULL_HANDLE;
    VkDescriptorSet countSet1_ = VK_NULL_HANDLE;
    VkDescriptorSet prefixSumSet1_ = VK_NULL_HANDLE;
    VkDescriptorSet scatterSet1_ = VK_NULL_HANDLE;
};

}  // namespace rx::cluster
