#pragma once
// rx_ibl/bake.h -- Phase 5 Stage 1 Task 9 [#45, gate rulings T9/RC1]: the
// compute IBL bake chain -- equirect HDR (or a pre-baked cubemap, e.g.
// Task 6's own KTX2-cube loader output) -> base cubemap -> diffuse
// irradiance cubemap + prefiltered specular cubemap (roughness-indexed
// mips) + a DFG/BRDF-integration LUT (the real, backed source Task 8's
// waiting `energyCompensation(f0, dfgY)` [shaders/material/brdf.slang]
// needed). First production consumer of Task 2's ComputePipelineCache +
// storage-image graph API.
//
// SCOPE [ticket #45, matrix-p5t09-ibl-bake-chain.md]: the BAKE and its
// correctness proofs. Runtime integration (Scene::setEnvironment(),
// skybox rendering, feeding the standard_pbr.slang lit path) is Task 10's
// job -- this module returns plain rx::rhi::Texture2D outputs a caller
// (Task 10, or this task's own minimal consumption test) can bind
// directly; it has no rx_scene/rx_material dependency at all.
//
// MODULE OWNERSHIP [implementer decision, not escalated -- the matrix's
// own "spec rules the owner" note names src/rx_scene OR src/rx_asset as
// candidates but no spec document actually rules it (grepped; neither
// module has an "Environment"/"IBL" build-orchestration concept as of
// this task's BASE commit -- rx_asset's own Environment concept, Task 6,
// is a TextureRole/TextureCache LOADING concept, not a bake-orchestration
// one)]: a new, self-contained library, consuming only rx_graph/
// rx_rhi_vk/rx_shader/rx_task -- matching this codebase's own established
// "one small library per cohesive phase-5 subsystem" precedent (rx_shadow,
// rx_frame_loop, rx_debug_ui), and keeping this task's own deliverable
// (the bake) decoupled from Task 10's not-yet-built Scene/Environment API
// surface, per the ticket's own "full sample wiring is not [in scope]"
// line.

#include <rx_rhi_vk/buffer.h>
#include <rx_rhi_vk/texture.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace rx::rhi {
class Device;
class CommandContext;
}  // namespace rx::rhi

namespace rx::task {
class Scheduler;
}

namespace rx::ibl {

// Bake resolution/quality knobs. Defaults are small (this module's own
// GPU test suite runs the FULL chain many times per process -- lavapipe
// software rasterization makes a large bake prohibitively slow for a
// value-asserted test's own iteration budget); a real load-time caller
// (Task 10) is expected to pass larger production values (real-driver
// bake timings are published against BOTH this default AND a production-
// scale configuration -- see task-09-report.md).
struct BakeParams {
    uint32_t baseCubemapFaceSize = 64;
    uint32_t irradianceFaceSize = 16;
    uint32_t irradianceSamples = 512;
    // Mip 0 is ALWAYS the linearRoughness==0 passthrough (CubemapIBL.cpp's
    // own special case) -- prefilteredMipCount therefore names the TOTAL
    // mip count including that passthrough mip, matching
    // Texture2D::createCubeForPresuppliedMips()'s own `mipLevels`
    // parameter shape.
    uint32_t prefilteredMipCount = 5;
    uint32_t prefilteredBaseFaceSize = 64;
    uint32_t specularSamples = 128;
    uint32_t dfgLutSize = 64;
    uint32_t dfgSamples = 512;
};

struct BakeResult {
    // Mip 0 only, 6 faces, R16G16B16A16_SFLOAT -- either the freshly
    // baked equirect->cubemap projection, or a direct copy of an
    // already-cube `source` (see bakeEnvironment()'s own `sourceIsCube`
    // parameter) -- BakeResult always owns its own copy either way, so a
    // caller's ownership story is uniform regardless of which path ran.
    rx::rhi::Texture2D baseCubemap;
    // Diffuse irradiance, mip 0 only, 6 faces, R16G16B16A16_SFLOAT. Every
    // texel is `Ed()/PI` in Filament's own pre-divided convention (see
    // shaders/ibl/irradiance_convolve.slang's own header comment) -- a
    // consumer multiplies this cubemap's own sampled value by `albedo`
    // alone, no extra `/PI`.
    rx::rhi::Texture2D irradianceCubemap;
    // GGX-prefiltered specular, `prefilteredMipCount` mips, 6 faces each,
    // R16G16B16A16_SFLOAT. Mip 0 is an exact (to fp16-storage rounding)
    // resample of baseCubemap; mip `prefilteredMipCount-1` is the
    // roughest (linearRoughness approaching 1.0).
    rx::rhi::Texture2D prefilteredCubemap;
    // DFG (BRDF-integration) LUT, 2D (NOT a cube), single mip,
    // R16G16_SFLOAT. R = DFV_Multiscatter(NoV,roughness).x, G =
    // DFV_Multiscatter(NoV,roughness).y -- G is exactly the `dfgY`
    // brdf.slang's energyCompensation(f0, dfgY) [Task 7/8] consumes.
    // x-axis = NoV in [0,1]; y-axis = sqrt(linearRoughness) in [0,1]
    // (texel row 0 = roughest, last row = smoothest -- see
    // shaders/ibl/dfg_lut.slang's own header comment for the exact
    // (x,y)<->(NoV,roughness) parameterization).
    rx::rhi::Texture2D dfgLut;
    uint32_t prefilteredMipCount = 0;
};

// Wall-clock bake timings (milliseconds, host-side std::chrono around each
// stage's own one-shot GPU submission-and-wait) -- published honestly per
// this phase's own binding "bake timing measured (Tracy zones) and
// reported" requirement (plan Task 9 acceptance sketch). Each stage is
// ALSO wrapped in an `RX_ZONE_NAMED` Tracy CPU zone (rx_core/profile.h)
// around the same span, for real Tracy-capture-based profiling -- these
// std::chrono numbers are what get printed/logged/reported, since a bake
// is a one-shot, load-time event a live Tracy capture session is not
// guaranteed to be attached for.
struct BakeTimings {
    double equirectToCubemapMs = 0.0;
    double irradianceMs = 0.0;
    double prefilterMs = 0.0;
    double dfgMs = 0.0;
    double totalMs = 0.0;
};

// Runs the full bake chain as FOUR SEPARATE single-purpose render-graph
// compute-pass graphs -- base cubemap (equirect projection OR, when
// `sourceIsCube`, a compute passthrough -- see below), irradiance,
// prefiltered specular, DFG LUT -- each its own compile()+realize()+
// execute() call and its own one-shot command-buffer submission
// (`CommandContext::runOnce()`), run back to back. NOT one graph/one
// submission (an earlier design intent this doc comment used to
// describe, before implementation found it structurally blocked): the
// render graph's own subresource validator only accepts IDENTICAL-or-
// DISJOINT declared ranges for one resource, and `baseCubemap`'s 6
// disjoint per-face writer passes cannot coexist in the SAME graph as a
// later whole-resource read of it -- see bake.cpp's own "Design
// decision: four graphs, not one" comment (and task-09-report.md) for
// the full reasoning. Every stage still runs exclusively through Task
// 2's compute-class Pass API (`addStorageImageOutput`/`Executor::
// execute()`) -- per this phase's own binding "compute passes through
// the graph's compute-class machinery, no hand-rolled dispatch" global
// constraint -- this comment's correction is about GRAPH COUNT, not
// about that constraint being relaxed. A caller reasoning about
// synchronization/timing (Task 10) should assume four independent
// GPU-idle points, not one.
//
// `source`: either an equirect-projection 2D texture (`sourceIsCube ==
// false` -- Task 6's own HDR equirect loader output, R16G16B16A16_SFLOAT
// per that task's own ruling, though this function reads it through a
// format-polymorphic `Texture2D<float4>` sampled read and accepts any
// float-interpretable sampled format) OR an already-baked/loaded cubemap
// (`sourceIsCube == true` -- e.g. Task 6's own KTX2-cube loader output,
// `Texture2D::isCube() == true`). EITHER WAY, `BakeResult::baseCubemap`
// is populated by a per-face COMPUTE PASS (never a raw `vkCmdCopyImage`
// from `source` directly -- that was tried and found broken for two
// independent reasons: format compatibility, since `source` need not
// share this bake's own `kCubeFormat`, and an undocumented
// `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` precondition Task 6's own loader
// output does not carry either; see task-09-report.md's "Bugs found and
// fixed" #4). The `sourceIsCube` compute pass is the SAME `prefilterKernel`
// used by the prefiltered-specular stage, dispatched at its own
// `linearRoughness == 0` literal-passthrough special case -- a format-
// agnostic `TextureCube` sampled resample, not a projection.
//
// `shaderDir`: the directory containing this module's own
// shaders/ibl/*.slang kernels (mirrors rx_material::MaterialSystem::
// create()'s own `sharedShaderDir` parameter shape) -- the real caller's
// default is `RX_IBL_SHADER_DIR` (baked in by CMakeLists.txt, same
// convention as RX_MATERIAL_SHADER_DIR).
//
// `cacheNamespace` [review round, LOW finding 3]: names this call's own
// `rx::rhi::ComputePipelineCache` disk-persisted file --
// `<temp_directory>/rx_ibl/<cacheNamespace>.pipeline_cache` -- distinct
// callers/purposes should pass DISTINCT, STABLE names (this module's own
// tests each use their own TEST_CASE-specific namespace; the bench tool
// uses its own). Vulkan's own per-entry vendor/device/pipelineCacheUUID
// header already makes ONE file safe to share across different physical
// devices/drivers (confirmed empirically this round -- lavapipe and
// NVIDIA runs interleave against the same file with zero corruption),
// so this parameter is not a correctness requirement; it exists so
// unrelated callers/purposes (this module's own several GPU test
// binaries' TEST_CASEs, ctest's own default parallel execution, a
// future Task 10 production caller running alongside this module's own
// tests) do not contend for ONE shared, unnamespaced `/tmp` file, which
// a prior round left as a mild robustness gap. Defaults to "default" --
// matches this parameter's absence before this round, for any caller
// that doesn't yet care.
//
// Main-thread-only (D5) -- builds/dispatches real Vulkan pipeline/compute
// objects, same convention as every other rx_rhi_vk/rx_graph factory.
// Returns std::nullopt (logged) on any compile/pipeline/image-creation
// failure.
std::optional<BakeResult> bakeEnvironment(rx::rhi::Device& device, rx::rhi::Allocator& allocator,
                                            rx::rhi::CommandContext& cmdCtx, rx::task::Scheduler& scheduler,
                                            const rx::rhi::Texture2D& source, bool sourceIsCube,
                                            const std::filesystem::path& shaderDir, const BakeParams& params = {},
                                            BakeTimings* outTimings = nullptr,
                                            const std::string& cacheNamespace = "default");

}  // namespace rx::ibl
