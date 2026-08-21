// [Task 2 (#38), gate rulings: per-ticket ruling "compute as a parallel
// rx::rhi::ComputePipeline facility in rx_rhi_vk"] Device-backed coverage
// for rx::rhi::ComputePipelineCache -- PSO creation from a real compiled +
// reflected Slang compute module, content-hash caching, and the bindless
// set-0 substitution this ticket's own "RT-compatibility rationale"
// requires. test_compute_gpu.cpp (rx_graph_gpu_tests) proves the full
// end-to-end dispatch + readback path this class' pipelines are bound and
// run through; this file proves the PIPELINE-OBJECT mechanics in isolation.
#include <doctest/doctest.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/compute_pipeline.h>
#include <rx_rhi_vk/device.h>
#include <rx_platform/window.h>
#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace {

// [Phase 5 Task 6 bundled fix, ticket #42] The three ComputePipelineCache::create()
// calls below used to pass a bare relative filename ("rx_compute_pipeline_test*.cache")
// straight through to ComputePipelineCache::create()'s std::filesystem::path
// parameter -- ctest's own default working directory is wherever `ctest`
// itself was invoked from (this repo's own root when run via `ctest
// --preset ...` from a checkout root, NOT this binary's own build
// directory), so those three files landed as untracked stray artifacts at
// the REPO ROOT on every local test run. std::filesystem::temp_directory_path()
// keeps every pre-existing behavior (content-hash cache creation/reuse
// across the 3 distinct TEST_CASEs below, each with its own distinct
// filename so they never collide with each other) while writing nowhere
// this repository's own working tree can be polluted.
std::filesystem::path testPipelineCachePath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

// Same skip-guarded windowed-device fixture pattern as texture_test.cpp/
// storage_image_test.cpp.
struct ComputePipelineTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    rx::rhi::Device device;
};

std::optional<ComputePipelineTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping ComputePipelineCache test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping "
                 "ComputePipelineCache test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    return ComputePipelineTestFixture{std::move(*window), std::move(*context), std::move(*device)};
}

// A trivial compute module: one RWStructuredBuffer<uint> at set 1 binding
// 0, [numthreads(8,1,1)], no set-0 use at all -- deliberately the SIMPLEST
// possible shape, since this file's own scope is the pipeline-OBJECT
// mechanics (creation/caching/layout), not a real bindless-set-0 dispatch
// (test_compute_gpu.cpp's own job).
constexpr const char* kSimpleComputeSource = R"(
[[vk::binding(0, 1)]]
RWStructuredBuffer<uint> gOut;

[shader("compute")]
[numthreads(8, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    gOut[id.x] = id.x;
}
)";

// A second module with the SAME set-0 shape the engine's real BindlessTable
// uses (sampled image / sampler / storage buffer at bindings 0/1/2) plus its
// own set-1 storage buffer -- the shape this ticket's own "bindless set 0
// accessible from compute" criterion needs a compute PSO to build against.
constexpr const char* kBindlessComputeSource = R"(
[[vk::binding(0, 0)]]
Texture2D gTextures[];

[[vk::binding(1, 0)]]
SamplerState gSamplers[];

[[vk::binding(2, 0)]]
RWStructuredBuffer<uint> gBindlessBuffers[];

[[vk::binding(0, 1)]]
RWStructuredBuffer<uint> gOut;

[shader("compute")]
[numthreads(8, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    gOut[id.x] = id.x;
}
)";

std::optional<rx::shader::CompileResult> compileCompute(const char* moduleName, const char* source) {
    auto compiler = rx::shader::Compiler::create();
    if (!compiler.has_value()) {
        return std::nullopt;
    }
    rx::shader::CompileResult result = compiler->compileFromSource(moduleName, source, {"csMain"});
    if (!result.ok) {
        MESSAGE("compute shader compile failed: ", result.diagnostics);
        return std::nullopt;
    }
    return result;
}

}  // namespace

TEST_CASE("ComputePipelineCache::getOrCreate builds a valid compute VkPipeline from a reflected Slang module") {
    auto fixture = makeFixture("rx_rhi_vk_compute_pipeline_basic");
    if (!fixture.has_value()) {
        return;
    }

    auto compileResult = compileCompute("RxComputePipelineTestSimple", kSimpleComputeSource);
    REQUIRE(compileResult.has_value());
    REQUIRE(compileResult->entryPointCode.size() == 1);
    CHECK(compileResult->entryPointCode[0].stage == VK_SHADER_STAGE_COMPUTE_BIT);

    auto layoutInfo = rx::shader::reflect(*compileResult);
    REQUIRE(layoutInfo.has_value());

    auto cache = rx::rhi::ComputePipelineCache::create(fixture->device.device(),
                                                         testPipelineCachePath("rx_compute_pipeline_test.cache"));
    REQUIRE(cache.has_value());

    auto pso = cache->getOrCreate(compileResult->entryPointCode[0].code, *layoutInfo);
    REQUIRE(pso.has_value());
    CHECK(pso->pipeline != VK_NULL_HANDLE);
    CHECK(pso->layout != VK_NULL_HANDLE);
    REQUIRE(pso->setLayouts.size() >= 2);  // set 0 (empty placeholder) + set 1 (gOut)
    CHECK(pso->setLayouts[1] != VK_NULL_HANDLE);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("ComputePipelineCache::getOrCreate caches by SPIR-V content -- identical spirv returns the identical "
          "VkPipeline") {
    auto fixture = makeFixture("rx_rhi_vk_compute_pipeline_cache_hit");
    if (!fixture.has_value()) {
        return;
    }

    auto compileResult = compileCompute("RxComputePipelineTestCacheHit", kSimpleComputeSource);
    REQUIRE(compileResult.has_value());
    auto layoutInfo = rx::shader::reflect(*compileResult);
    REQUIRE(layoutInfo.has_value());

    auto cache = rx::rhi::ComputePipelineCache::create(fixture->device.device(),
                                                         testPipelineCachePath("rx_compute_pipeline_test2.cache"));
    REQUIRE(cache.has_value());

    auto first = cache->getOrCreate(compileResult->entryPointCode[0].code, *layoutInfo);
    REQUIRE(first.has_value());
    auto second = cache->getOrCreate(compileResult->entryPointCode[0].code, *layoutInfo);
    REQUIRE(second.has_value());

    CHECK(first->pipeline == second->pipeline);
    CHECK(first->layout == second->layout);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("ComputePipelineCache::getOrCreate substitutes a real BindlessTable layout at set 0 -- the ticket's own "
          "RT-compatibility rationale") {
    auto fixture = makeFixture("rx_rhi_vk_compute_pipeline_bindless_set0");
    if (!fixture.has_value()) {
        return;
    }

    auto bindlessTable = rx::rhi::BindlessTable::create(fixture->device.physicalDevice(), fixture->device.device(),
                                                          rx::rhi::BindlessTable::Capacities{4, 2, 4});
    REQUIRE(bindlessTable.has_value());

    auto compileResult = compileCompute("RxComputePipelineTestBindless", kBindlessComputeSource);
    REQUIRE(compileResult.has_value());
    auto layoutInfo = rx::shader::reflect(*compileResult);
    REQUIRE(layoutInfo.has_value());

    auto cache = rx::rhi::ComputePipelineCache::create(fixture->device.device(),
                                                         testPipelineCachePath("rx_compute_pipeline_test3.cache"));
    REQUIRE(cache.has_value());

    auto pso =
        cache->getOrCreate(compileResult->entryPointCode[0].code, *layoutInfo, bindlessTable->descriptorSetLayout());
    REQUIRE(pso.has_value());
    CHECK(pso->pipeline != VK_NULL_HANDLE);
    REQUIRE(pso->setLayouts.size() >= 2);
    // set 0 IS the real BindlessTable's own layout, not a lookalike this
    // cache invented -- the exact substitution PipelineLayoutBuilder::
    // build()'s own externalSet0 parameter documents (pipeline_layout.h),
    // reused verbatim here.
    CHECK(pso->setLayouts[0] == bindlessTable->descriptorSetLayout());

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Task 2 (#38), empirical finding -- see CompileResult::cachedLayout's own
// doc comment, rx_shader/include/rx_shader/compiler.h] Regression coverage
// for a real, reproduced crash this ticket's own work uncovered: reflecting
// a compute-only linked program, in a process that has already initialized
// Vulkan/vk-bootstrap (this file's own fixture-building tests above all do
// exactly that), used to crash -- SIGFPE inside Slang's own
// CompilerOptionSet::getProfile(), then (after the Profile compiler-option
// fix alone) SIGSEGV inside TargetProgram construction -- on
// rx::shader::reflect()'s own SECOND getLayout() call against the same
// linkedProgram compileFromSource() had already internally called
// getLayout() on once. Fixed by having reflect() reuse that first result
// (CompileResult::cachedLayout) instead of calling getLayout() again.
// Exercised here specifically (not just in rx_shader's own tests, which
// never build a Vulkan Context at all and could not have caught this) so a
// future regression is caught in the exact environment that surfaces it.
TEST_CASE("compute-only reflect() succeeds inside a process that has already initialized Vulkan/vk-bootstrap") {
    auto compileResult = compileCompute("RxComputeReflectRegression", kSimpleComputeSource);
    REQUIRE(compileResult.has_value());
    REQUIRE(compileResult->entryPointCode.size() == 1);
    CHECK(compileResult->entryPointCode[0].stage == VK_SHADER_STAGE_COMPUTE_BIT);

    auto layoutInfo = rx::shader::reflect(*compileResult);
    REQUIRE(layoutInfo.has_value());

    // The bindless-shaped module (set 0 populated) is exercised too --
    // covers the exact shape this ticket's own real compute consumers use.
    auto bindlessCompileResult = compileCompute("RxComputeReflectRegressionBindless", kBindlessComputeSource);
    REQUIRE(bindlessCompileResult.has_value());
    auto bindlessLayoutInfo = rx::shader::reflect(*bindlessCompileResult);
    REQUIRE(bindlessLayoutInfo.has_value());
}
