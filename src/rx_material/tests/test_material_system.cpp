#include <doctest/doctest.h>
#include <rx_material/draw_data.h>
#include <rx_material/material_system.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/command.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>

#include <rx_core/debug_checks.h>
#include <rx_graph/executor.h>
#include <rx_graph/render_graph.h>
#include <rx_task/scheduler.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// RX_MATERIAL_TEST_DATA_DIR (src/rx_material/tests/CMakeLists.txt) is the
// absolute, checked-out-repo-relative path to this directory's own data/
// subfolder -- the same "bake an absolute build-relative path at compile
// time" idiom material_system.cpp's own RX_MATERIAL_SHADER_DIR uses (see
// that file's CMakeLists.txt comment), applied here so this test binary
// finds test_unlit.slang/test_solid.slang/test_bad_syntax.slang regardless
// of ctest's own working directory.

namespace {

std::filesystem::path testDataPath(const char* filename) {
    return std::filesystem::path(RX_MATERIAL_TEST_DATA_DIR) / filename;
}

// A real headless (but validated) rx::rhi::Device + rx::rhi::BindlessTable
// -- every test_material_system.cpp case needs both (MaterialSystem::
// create()'s own signature requires a real Device& and BindlessTable&),
// exactly mirroring src/rx_rhi_vk/tests/device_test.cpp's own
// DeviceTestFixture/makeFixture() pattern (a hidden window for the
// VkSurfaceKHR Device::create() requires -- there is no headless-only
// Device construction path in this codebase) plus
// pipeline_layout_test.cpp's own BindlessTable::create() call. Returns
// nullopt (with a MESSAGE explaining why) on a machine with no real
// display/Vulkan backend, matching every other GPU-backed test fixture in
// this repo -- never silently skipped on a machine that actually has one.
struct MaterialTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    rx::rhi::Device device;
    rx::rhi::BindlessTable bindless;
};

std::optional<MaterialTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping MaterialSystem test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping MaterialSystem "
                "test");
        return std::nullopt;
    }

    auto context = rx::rhi::Context::create(extensions, /*enableValidation=*/true);
    REQUIRE(context.has_value());

    VkSurfaceKHR surface = window->createVulkanSurface(context->instance());
    REQUIRE(surface != VK_NULL_HANDLE);

    auto device = rx::rhi::Device::create(*context, surface);
    REQUIRE(device.has_value());

    rx::rhi::BindlessTable::Capacities capacities;
    capacities.sampledImages = 4;
    capacities.samplers = 2;
    // [Phase 4 Task 16, D26.1] MaterialSystem::create() itself now
    // registers ONE storage buffer (its own default per-draw
    // DrawDataGpu row -- Impl::defaultDrawDataBuffer's own comment) before
    // any test gets a chance to register one of its own -- bumped from 1 so
    // a D26.1 test building its own real per-draw buffer still has a free
    // slot.
    capacities.storageBuffers = 2;
    auto bindless = rx::rhi::BindlessTable::create(device->physicalDevice(), device->device(), capacities);
    REQUIRE(bindless.has_value());

    return MaterialTestFixture{std::move(*window), std::move(*context), surface, std::move(*device),
                                std::move(*bindless)};
}

// A fresh, uniquely-named pipeline-cache path under the system temp
// directory for each test -- removed first so a stale file left behind by
// an earlier failed run never leaks state into this one (except
// "pipeline-cache-persists" below, which deliberately reuses the SAME path
// across two MaterialSystem instances within the one test to prove
// persistence).
std::filesystem::path freshCachePath(const char* name) {
    std::filesystem::path path = std::filesystem::temp_directory_path() / (std::string("rx_material_test_") + name + ".cache");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

rx::graph::PassSignature makeColorOnlySignature() {
    rx::graph::PassSignature sig;
    sig.colorCount = 1;
    sig.colorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    sig.depthFormat = VK_FORMAT_UNDEFINED;
    sig.samples = VK_SAMPLE_COUNT_1_BIT;
    return sig;
}

// --- Hot-reload test fixtures [Task 7, mandatory 3-version regression] --
// Same shape as test_unlit.slang (a single `float4 tint` at set 1, binding
// 0), so the reflected parameter shape never changes across v1/v2/v3 --
// only the tint MATH differs (v1 vs v2), which is exactly what changes the
// module's content hash without changing anything reload needs to
// re-validate. v3 is a deliberate syntax error (missing semicolon, the
// exact same fixture shape test_bad_syntax.slang already uses) -- the
// "syntactically broken" half of the mandatory regression test.
constexpr const char* kReloadV1 = R"(
import material;

struct UnlitParams {
    float4 tint;
};

[[vk::binding(0, 1)]]
ParameterBlock<UnlitParams> gParams;

struct Unlit : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        float4 tint = gParams.tint;
        tint.x += v.worldPos.x * 1e-4;
        tint.y += v.normal.y * 1e-4;
        tint.z += v.uv.x * 1e-4;
        // [Phase 4 Task 16] Extended to tangent/lightDirWorld/lightColor/
        // ambientColor -- same dead-code-elimination-avoidance rationale
        // as the three original fields above.
        tint.w += v.tangent.x * 1e-4;
        tint.x += v.lightDirWorld.x * 1e-4;
        tint.y += v.lightColor.x * 1e-4;
        tint.z += v.ambientColor.x * 1e-4;
        tint.w += v.cameraPosWorld.x * 1e-4;
        return tint;
    }
};

export struct MaterialImpl : IMaterialShader = Unlit;
)";

constexpr const char* kReloadV2 = R"(
import material;

struct UnlitParams {
    float4 tint;
};

[[vk::binding(0, 1)]]
ParameterBlock<UnlitParams> gParams;

struct Unlit : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        float4 tint = gParams.tint * 0.5;
        tint.x += v.worldPos.x * 1e-4;
        tint.y += v.normal.y * 1e-4;
        tint.z += v.uv.x * 1e-4;
        tint.w += v.tangent.x * 1e-4;
        tint.x += v.lightDirWorld.x * 1e-4;
        tint.y += v.lightColor.x * 1e-4;
        tint.z += v.ambientColor.x * 1e-4;
        tint.w += v.cameraPosWorld.x * 1e-4;
        return tint;
    }
};

export struct MaterialImpl : IMaterialShader = Unlit;
)";

constexpr const char* kReloadV3Broken = R"(
import material;

struct UnlitParams {
    float4 tint
};

[[vk::binding(0, 1)]]
ParameterBlock<UnlitParams> gParams;

struct Unlit : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        return gParams.tint;
    }
};

export struct MaterialImpl : IMaterialShader = Unlit;
)";

// Writes `content` to `path` and forces its mtime to `mtime` -- explicit,
// rather than relying on real wall-clock time between two writes, so this
// test is not at the mercy of a filesystem's mtime resolution (some
// filesystems only resolve to whole seconds -- two writes issued back to
// back from a single test process can otherwise land on the identical
// timestamp, which would make MaterialSystem::reloadChanged() see no
// change at all).
void writeFileWithMtime(const std::filesystem::path& path, const char* content,
                         std::filesystem::file_time_type mtime) {
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out << content;
    }
    std::error_code ec;
    std::filesystem::last_write_time(path, mtime, ec);
    REQUIRE_FALSE(ec);
}

// --- bindInstance() end-to-end fixture [Task 7 coordinator addition 3] --
// A minimal real rx::graph::RenderGraph + Executor rig -- the only
// legitimate way to obtain a real rx::graph::PassContext&
// (MaterialSystem::bindInstance()'s own second parameter), since
// PassContext's constructor is private and friend-gated to Executor alone
// [pass.h/executor.h's own "Fix round 1" comment]. One pass, one color
// attachment, no actual draw call recorded (proving the pipeline bind +
// descriptor-set allocate/write/bind sequence itself is validation-clean
// is this test's entire point -- exactly mirroring rx_graph's own
// test_execute_gpu.cpp "draw" pass, which similarly never issues a draw).
struct OffscreenColorImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

std::optional<OffscreenColorImage> createOffscreenColorImage(VkDevice device, VkPhysicalDevice physicalDevice,
                                                               VkFormat format, VkExtent2D extent) {
    OffscreenColorImage result;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &result.image) != VK_SUCCESS) {
        return std::nullopt;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, result.image, &memReq);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1U << i)) != 0U &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == UINT32_MAX) {
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &result.memory) != VK_SUCCESS) {
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }
    if (vkBindImageMemory(device, result.image, result.memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, result.memory, nullptr);
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &result.view) != VK_SUCCESS) {
        vkFreeMemory(device, result.memory, nullptr);
        vkDestroyImage(device, result.image, nullptr);
        return std::nullopt;
    }

    return result;
}

void destroyOffscreenColorImage(VkDevice device, OffscreenColorImage& img) {
    if (img.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, img.view, nullptr);
    }
    if (img.image != VK_NULL_HANDLE) {
        vkDestroyImage(device, img.image, nullptr);
    }
    if (img.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, img.memory, nullptr);
    }
    img = OffscreenColorImage{};
}

}  // namespace

// [Task 4] material.slang now unconditionally declares its own set-0
// `gTextures`/`gSamplers` bindless arrays and `gMaterialGlobals` push
// constant (see that file's own header comment) -- verified directly
// against this project's shipped Slang build (not assumed) that these are
// NOT dead-code-eliminated from a material's own reflected program even
// when its evaluate() never calls rx_sampleTexture at all: `import
// material;` pulls in every one of material.slang's own top-level globals
// regardless of which of them the importing module's entry points actually
// reach. So EVERY material -- test_unlit.slang included, which references
// none of them -- now reflects 3 bindings (its own set-1 gParams plus
// material.slang's set-0 gTextures/gSamplers) and 1 push range
// (gMaterialGlobals), not just the one gParams binding Task 5/7 originally
// established. This is by design, not a regression: the same external
// BindlessTable substitution (rx::rhi::PipelineLayoutBuilder::build()) is
// wired in either way, so a material's pipeline layout is correct whether
// or not it actually samples a texture.
//
// Finds the one binding in `bindings` at (set, binding) -- rather than
// assume a fixed index -- since this test no longer controls the SHAPE
// (1 fixed binding) precisely enough to hardcode positions the way it used
// to; Slang's own top-level-parameter ordering for this composite is not a
// documented contract this test should pin down beyond "some order".
const rx::shader::ShaderLayoutInfo::Binding& findBinding(const rx::shader::ShaderLayoutInfo& layout, uint32_t set,
                                                          uint32_t binding) {
    for (const auto& b : layout.bindings) {
        if (b.set == set && b.binding == binding) {
            return b;
        }
    }
    FAIL("no binding found at set " << set << " binding " << binding);
    static rx::shader::ShaderLayoutInfo::Binding kNotFound;
    return kNotFound;
}

TEST_CASE("MaterialSystem::loadMaterial reflects the set-1 parameter block and builds a pipeline layout") {
    auto fixture = makeFixture("rx_material_load_reflect");
    if (!fixture.has_value()) {
        return;
    }

    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("load_reflect"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle handle = system->loadMaterial(testDataPath("test_unlit.slang"));
    CHECK(handle.isValid());

    const rx::shader::ShaderLayoutInfo& layout = system->layoutInfo(handle);
    // [Phase 4 Task 16, D26.1] grew from 3 to 4: material.slang's new
    // `gDrawData` bindless storage-buffer array joins gParams/gTextures/
    // gSamplers -- see this file's own header comment for why every
    // material reflects all of material.slang's set-0 globals regardless
    // of whether its own evaluate() touches them.
    REQUIRE(layout.bindings.size() == 4);

    const auto& paramBlock = findBinding(layout, 1, 0);
    CHECK(paramBlock.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    CHECK(paramBlock.count == 1);
    CHECK_FALSE(paramBlock.unboundedArray);

    const auto& textures = findBinding(layout, 0, rx::rhi::BindlessTable::kSampledImageBinding);
    CHECK(textures.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    CHECK(textures.unboundedArray);

    const auto& samplers = findBinding(layout, 0, rx::rhi::BindlessTable::kSamplerBinding);
    CHECK(samplers.type == VK_DESCRIPTOR_TYPE_SAMPLER);
    CHECK(samplers.unboundedArray);

    // [Phase 4 Task 16, D26.1] gDrawData -- the new bindless per-draw
    // StructuredBuffer<RxDrawData> array.
    const auto& drawData = findBinding(layout, 0, rx::rhi::BindlessTable::kStorageBufferBinding);
    CHECK(drawData.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    CHECK(drawData.unboundedArray);

    // [Task 4, Phase 4 Task 16] gMaterialGlobals -- same "always present
    // regardless of use" reasoning as the three bindings above; grew from
    // one `uint` to `rx::material::MaterialGlobalsPush`'s three scalar
    // fields (D26.1's `drawDataBufferIndex` + sample 08's `exposure`).
    REQUIRE(layout.pushRanges.size() == 1);
    CHECK(layout.pushRanges[0].size == sizeof(rx::material::MaterialGlobalsPush));

    CHECK(system->pipelineLayout(handle) != VK_NULL_HANDLE);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MaterialSystem::getPipeline caches by (material, pass, specialization) and never recompiles Slang on a "
          "hit") {
    auto fixture = makeFixture("rx_material_cache_hit");
    if (!fixture.has_value()) {
        return;
    }

    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("cache_hit"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle handle = system->loadMaterial(testDataPath("test_unlit.slang"));
    uint64_t compileCountAfterLoad = rx::material::detail::debugCompileCount(*system);
    CHECK(compileCountAfterLoad > 0);

    rx::material::PipelineRequest request;
    request.material = handle;
    request.pass = makeColorOnlySignature();

    VkPipeline first = system->getPipeline(request);
    REQUIRE(first != VK_NULL_HANDLE);
    CHECK(rx::material::detail::debugCompileCount(*system) == compileCountAfterLoad);

    VkPipeline second = system->getPipeline(request);
    CHECK(second == first);
    CHECK(rx::material::detail::debugCompileCount(*system) == compileCountAfterLoad);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MaterialSystem::getPipeline keys on the pass signature: a different depth format yields a different "
          "pipeline") {
    auto fixture = makeFixture("rx_material_cache_key_pass");
    if (!fixture.has_value()) {
        return;
    }

    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("cache_key_pass"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle handle = system->loadMaterial(testDataPath("test_unlit.slang"));

    rx::material::PipelineRequest colorOnly;
    colorOnly.material = handle;
    colorOnly.pass = makeColorOnlySignature();

    rx::material::PipelineRequest colorAndDepth;
    colorAndDepth.material = handle;
    colorAndDepth.pass = makeColorOnlySignature();
    colorAndDepth.pass.depthFormat = VK_FORMAT_D32_SFLOAT;

    CHECK(colorOnly.pass.hash() != colorAndDepth.pass.hash());

    VkPipeline a = system->getPipeline(colorOnly);
    VkPipeline b = system->getPipeline(colorAndDepth);
    REQUIRE(a != VK_NULL_HANDLE);
    REQUIRE(b != VK_NULL_HANDLE);
    CHECK(a != b);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MaterialSystem::getPipeline keys on the material: two different modules yield different pipelines for "
          "the identical pass") {
    auto fixture = makeFixture("rx_material_cache_key_material");
    if (!fixture.has_value()) {
        return;
    }

    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("cache_key_material"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle unlit = system->loadMaterial(testDataPath("test_unlit.slang"));
    rx::material::MaterialHandle solid = system->loadMaterial(testDataPath("test_solid.slang"));
    CHECK(system->moduleHash(unlit) != system->moduleHash(solid));

    rx::graph::PassSignature pass = makeColorOnlySignature();

    rx::material::PipelineRequest unlitRequest;
    unlitRequest.material = unlit;
    unlitRequest.pass = pass;

    rx::material::PipelineRequest solidRequest;
    solidRequest.material = solid;
    solidRequest.pass = pass;

    VkPipeline a = system->getPipeline(unlitRequest);
    VkPipeline b = system->getPipeline(solidRequest);
    REQUIRE(a != VK_NULL_HANDLE);
    REQUIRE(b != VK_NULL_HANDLE);
    CHECK(a != b);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MaterialSystem::loadMaterial throws with Slang's diagnostic text for a module with a syntax error") {
    auto fixture = makeFixture("rx_material_bad_module");
    if (!fixture.has_value()) {
        return;
    }

    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("bad_module"));
    REQUIRE(system != nullptr);

    bool threw = false;
    std::string message;
    try {
        system->loadMaterial(testDataPath("test_bad_syntax.slang"));
    } catch (const std::runtime_error& e) {
        threw = true;
        message = e.what();
    }

    CHECK(threw);
    // Slang's own diagnostic text always includes the substring "error"
    // (e.g. "error[E20001]: unexpected token") -- checked directly against
    // this project's shipped Slang v2026.14.1 build before writing this
    // assertion (see task-5-report.md), rather than assumed.
    CHECK(message.find("error") != std::string::npos);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MaterialSystem persists its VkPipelineCache to disk across instances") {
    auto fixture = makeFixture("rx_material_cache_persists");
    if (!fixture.has_value()) {
        return;
    }

    std::filesystem::path cachePath = freshCachePath("persists");
    CHECK_FALSE(std::filesystem::exists(cachePath));

    {
        auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, cachePath);
        REQUIRE(system != nullptr);

        rx::material::MaterialHandle handle = system->loadMaterial(testDataPath("test_unlit.slang"));
        rx::material::PipelineRequest request;
        request.material = handle;
        request.pass = makeColorOnlySignature();
        REQUIRE(system->getPipeline(request) != VK_NULL_HANDLE);
        // system destroyed at end of this scope -- saves the pipeline
        // cache to `cachePath` per MaterialSystem's own destructor
        // contract.
    }

    REQUIRE(std::filesystem::exists(cachePath));
    CHECK(std::filesystem::file_size(cachePath) > 0);

    // Recreate against the same path: MaterialSystem::create()'s own
    // best-effort load path (RX_LOG_INFO "loading N bytes of pipeline
    // cache data from...") must accept this file without treating it as
    // an error -- asserted here by the mere fact that create() still
    // succeeds (a corrupt/unreadable cache is logged and downgraded to a
    // fresh one per Task 5's ambiguity resolution, never fatal, so this
    // alone would not distinguish "loaded" from "ignored"; the file's
    // continued non-empty existence plus a successful create() together
    // are what this test can assert without capturing log output).
    auto secondSystem = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, cachePath);
    REQUIRE(secondSystem != nullptr);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MaterialSystem::create succeeds with an invalid pipeline-cache file; the driver discards invalid data") {
    auto fixture = makeFixture("rx_material_cache_corrupt");
    if (!fixture.has_value()) {
        return;
    }

    std::filesystem::path cachePath = freshCachePath("corrupt");

    // Write REAL garbage (512 bytes of 0xDE pattern) -- definitively not a
    // valid VkPipelineCache header. The Vulkan spec guarantees that drivers
    // ignore invalid initialData passed to vkCreatePipelineCache (see
    // material_system.cpp's comment on this behavior).
    {
        std::ofstream corruptFile(cachePath, std::ios::binary);
        REQUIRE(corruptFile.is_open());
        std::vector<uint8_t> garbage(512);
        for (size_t i = 0; i < garbage.size(); ++i) {
            garbage[i] = static_cast<uint8_t>(0xDE + (i & 0xFF));
        }
        corruptFile.write(reinterpret_cast<const char*>(garbage.data()), garbage.size());
    }

    REQUIRE(std::filesystem::exists(cachePath));

    // MaterialSystem::create() must succeed despite the corrupt file,
    // relying on Vulkan's documented contract: vkCreatePipelineCache
    // discards invalid initialData and proceeds with a fresh cache. This
    // resilience is inherent to the Vulkan driver, not explicit logging
    // or detection in this code.
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, cachePath);
    REQUIRE(system != nullptr);

    // Verify the system is fully functional after loading an invalid cache
    rx::material::MaterialHandle handle = system->loadMaterial(testDataPath("test_unlit.slang"));
    rx::material::PipelineRequest request;
    request.material = handle;
    request.pass = makeColorOnlySignature();
    CHECK(system->getPipeline(request) != VK_NULL_HANDLE);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Task 7 =============================================================

TEST_CASE("MaterialSystem::materialParams/paramBlockSize reflect real per-field name/kind/offset/size, computed "
          "once from the already-linked program") {
    auto fixture = makeFixture("rx_material_params_reflect");
    if (!fixture.has_value()) {
        return;
    }

    auto system =
        rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("params_reflect"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle unlit = system->loadMaterial(testDataPath("test_unlit.slang"));
    const std::vector<rx::material::MaterialParamInfo>& unlitParams = system->materialParams(unlit);
    REQUIRE(unlitParams.size() == 1);
    CHECK(unlitParams[0].name == "tint");
    CHECK(unlitParams[0].kind == rx::material::MaterialParamKind::Float4);
    CHECK(unlitParams[0].offset == 0);
    CHECK(unlitParams[0].size == sizeof(float) * 4);
    CHECK(system->paramBlockSize(unlit) >= unlitParams[0].offset + unlitParams[0].size);

    rx::material::MaterialHandle textured = system->loadMaterial(testDataPath("test_textured.slang"));
    const std::vector<rx::material::MaterialParamInfo>& texturedParams = system->materialParams(textured);
    REQUIRE(texturedParams.size() == 1);
    CHECK(texturedParams[0].name == "albedoIndex");
    CHECK(texturedParams[0].kind == rx::material::MaterialParamKind::TextureIndex);
    CHECK(texturedParams[0].offset == 0);
    CHECK(texturedParams[0].size == sizeof(uint32_t));
    CHECK(system->paramBlockSize(textured) >= texturedParams[0].offset + texturedParams[0].size);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MaterialSystem::materialParams/paramBlockSize throw std::out_of_range for an invalid handle") {
    auto fixture = makeFixture("rx_material_params_invalid_handle");
    if (!fixture.has_value()) {
        return;
    }
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("params_invalid_handle"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle bogus;
    CHECK_THROWS_AS(static_cast<void>(system->materialParams(bogus)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(system->paramBlockSize(bogus)), std::out_of_range);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// --- Mandatory 3-version hot-reload regression [Task 7 brief step 1] -----
TEST_CASE("MaterialSystem::reloadChanged recompiles a changed module (v1->v2), invalidates its stale pipeline-cache "
          "entries, and keeps the last-good pipeline when a later edit (v3) is syntactically broken") {
    auto fixture = makeFixture("rx_material_reload");
    if (!fixture.has_value()) {
        return;
    }

    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("reload"));
    REQUIRE(system != nullptr);

    std::filesystem::path modulePath = std::filesystem::temp_directory_path() / "rx_material_reload_test.slang";
    auto baseTime = std::filesystem::file_time_type::clock::now();
    writeFileWithMtime(modulePath, kReloadV1, baseTime);

    rx::material::MaterialHandle handle = system->loadMaterial(modulePath);
    uint64_t hashV1 = system->moduleHash(handle);

    rx::material::PipelineRequest request;
    request.material = handle;
    request.pass = makeColorOnlySignature();

    VkPipeline p1 = system->getPipeline(request);
    REQUIRE(p1 != VK_NULL_HANDLE);

    // A reloadChanged() call before anything on disk changes must be a
    // complete no-op: same hash, same cached pipeline.
    system->reloadChanged();
    CHECK(system->moduleHash(handle) == hashV1);
    CHECK(system->getPipeline(request) == p1);

    // --- v1 -> v2: different tint math, IDENTICAL reflected shape --
    // content hash must change, and getPipeline() for the SAME request
    // must now build a genuinely different VkPipeline (P2 != P1) [D9].
    writeFileWithMtime(modulePath, kReloadV2, baseTime + std::chrono::seconds(1));
    system->reloadChanged();
    uint64_t hashV2 = system->moduleHash(handle);
    CHECK(hashV2 != hashV1);

    VkPipeline p2 = system->getPipeline(request);
    REQUIRE(p2 != VK_NULL_HANDLE);
    CHECK(p2 != p1);

    // The reflected parameter shape survives the reload unchanged -- v2
    // declares the identical `float4 tint` field, so a caller's existing
    // instance blob offsets stay valid across a reload.
    const std::vector<rx::material::MaterialParamInfo>& paramsAfterV2 = system->materialParams(handle);
    REQUIRE(paramsAfterV2.size() == 1);
    CHECK(paramsAfterV2[0].name == "tint");
    CHECK(paramsAfterV2[0].kind == rx::material::MaterialParamKind::Float4);

    // --- v2 -> v3: a syntax error -- reloadChanged() must log a warning
    // and leave v2's last-good compiled state completely untouched: same
    // module hash, and getPipeline() for the SAME request still returns
    // P2, not a new/broken pipeline [D9: "keep-last-good on compile
    // failure"].
    writeFileWithMtime(modulePath, kReloadV3Broken, baseTime + std::chrono::seconds(2));
    system->reloadChanged();
    CHECK(system->moduleHash(handle) == hashV2);
    CHECK(system->getPipeline(request) == p2);

    // Retired (v1-hash-keyed) pipelines are pending in the internal
    // DeletionQueue until onFrameCompleted() confirms their frame is
    // done -- exercise that path too, proving it does not crash or
    // double-destroy anything already-current.
    system->beginFrame(0, 7);
    system->onFrameCompleted(7);

    std::error_code ec;
    std::filesystem::remove(modulePath, ec);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// --- bindInstance(): the actual "connect the CPU blob to GPU binding" -----
// mechanism [Task 7 coordinator addition 3], exercised end to end against
// a real rx::graph::RenderGraph + Executor (the only legitimate source of
// a real PassContext& -- see this file's own createOffscreenColorImage()
// comment above).
TEST_CASE("MaterialSystem::bindInstance binds a real pipeline + set-1 UBO descriptor set with zero validation "
          "errors") {
    auto fixture = makeFixture("rx_material_bind_instance");
    if (!fixture.has_value()) {
        return;
    }

    auto system =
        rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("bind_instance"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle handle = system->loadMaterial(testDataPath("test_unlit.slang"));
    uint32_t blockSize = system->paramBlockSize(handle);
    REQUIRE(blockSize > 0);
    std::vector<uint8_t> blob(blockSize, 0);
    const std::vector<rx::material::MaterialParamInfo>& params = system->materialParams(handle);
    REQUIRE(params.size() == 1);
    float tint[4] = {0.25F, 0.5F, 0.75F, 1.0F};
    std::memcpy(blob.data() + params[0].offset, tint, sizeof(tint));

    // Phase 4 Task 7 [spec D4 amendment]: Executor::create() now requires a
    // real rx::task::Scheduler& (parallelism is the engine default -- see
    // that method's own doc comment). This test never touches a chunked
    // pass, so a plain default-worker-count Scheduler local to this
    // TEST_CASE is all it needs -- declared before `executor` so it
    // outlives it (reverse-destruction-order local variables).
    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto executor = rx::graph::Executor::create(fixture->device, *scheduler);
    REQUIRE(executor != nullptr);

    constexpr VkExtent2D kExtent{64, 64};
    constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    rx::graph::RenderGraph graph;
    rx::graph::Pass& pass = graph.addPass("material_bind_test");
    rx::graph::AttachmentDesc colorDesc;
    colorDesc.format = kColorFormat;
    pass.addColorOutput("color", colorDesc);
    graph.setBackbufferSource("color");

    rx::material::InstanceBinding binding{handle, blob.data(), blob.size()};
    bool bound = false;
    pass.setExecute([&](rx::graph::PassContext& ctx) {
        system->beginFrame(/*frameInFlightIndex=*/0, /*frameNumber=*/0);
        system->bindInstance(ctx.cmd, ctx, binding);
        bound = true;
    });

    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = kExtent.width;
    compileInfo.swapchainHeight = kExtent.height;
    compileInfo.swapchainFormat = kColorFormat;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);
    executor->realize(graph);

    auto offscreen =
        createOffscreenColorImage(fixture->device.device(), fixture->device.physicalDevice(), kColorFormat, kExtent);
    REQUIRE(offscreen.has_value());

    {
        auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                        fixture->device.graphicsQueueFamily());
        REQUIRE(cmdCtx.has_value());
        cmdCtx->runOnce(
            [&](VkCommandBuffer cmd) { executor->execute(graph, cmd, offscreen->image, offscreen->view, kExtent); });
    }
    CHECK(bound);

    destroyOffscreenColorImage(fixture->device.device(), *offscreen);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MaterialSystem::bindInstance rejects a paramSize that does not match the material's paramBlockSize") {
    auto fixture = makeFixture("rx_material_bind_instance_mismatch");
    if (!fixture.has_value()) {
        return;
    }
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("bind_instance_mismatch"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle handle = system->loadMaterial(testDataPath("test_unlit.slang"));

    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto executor = rx::graph::Executor::create(fixture->device, *scheduler);
    REQUIRE(executor != nullptr);

    constexpr VkExtent2D kExtent{64, 64};
    constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    rx::graph::RenderGraph graph;
    rx::graph::Pass& pass = graph.addPass("material_bind_mismatch_test");
    rx::graph::AttachmentDesc colorDesc;
    colorDesc.format = kColorFormat;
    pass.addColorOutput("color", colorDesc);
    graph.setBackbufferSource("color");

    std::array<uint8_t, 1> tooSmall{0};
    rx::material::InstanceBinding badBinding{handle, tooSmall.data(), tooSmall.size()};
    bool threw = false;
    pass.setExecute([&](rx::graph::PassContext& ctx) {
        system->beginFrame(0, 0);
        try {
            system->bindInstance(ctx.cmd, ctx, badBinding);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
    });

    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = kExtent.width;
    compileInfo.swapchainHeight = kExtent.height;
    compileInfo.swapchainFormat = kColorFormat;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);
    executor->realize(graph);

    auto offscreen =
        createOffscreenColorImage(fixture->device.device(), fixture->device.physicalDevice(), kColorFormat, kExtent);
    REQUIRE(offscreen.has_value());
    {
        auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                        fixture->device.graphicsQueueFamily());
        REQUIRE(cmdCtx.has_value());
        cmdCtx->runOnce(
            [&](VkCommandBuffer cmd) { executor->execute(graph, cmd, offscreen->image, offscreen->view, kExtent); });
    }
    CHECK(threw);

    destroyOffscreenColorImage(fixture->device.device(), *offscreen);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

#ifdef RX_DEBUG_CHECKS
// --- RX_ASSERT_MAIN_THREAD integration [Phase 4 Task 7 fix round 1, review
// Medium finding] ------------------------------------------------------------
// Proves the guard placed at the very top of MaterialSystem::bindInstance()
// (and, transitively, the one inside getPipeline() it calls internally) is
// genuinely wired into the real rx_graph/rx_task chunked-pass machinery --
// not just unit-testable in isolation (rx_core's own debug_checks_test.cpp
// covers the mechanism itself with a plain std::thread). Both cases install
// a capture hook (rx::core::debug::detail::setViolationHookForTests) that
// RECORDS a violation and returns normally rather than aborting/throwing --
// documented as safe here for the exact reason debug_checks.h's own
// ViolationHook contract comment requires: each TEST_CASE below arranges for
// only ONE thread to ever touch this MaterialSystem's shared, unsynchronized
// state during the one execute() call it makes (every chunk but the single
// one under test is a deliberate no-op), so letting a violating call
// continue past the guard never races a REAL concurrent access here, even
// though the hook does not itself prevent it.
namespace {

struct BindInstanceGuardCapture {
    std::mutex mutex;
    std::vector<std::string> contexts;
};

std::atomic<BindInstanceGuardCapture*> g_bindInstanceCapture{nullptr};

void captureBindInstanceViolation(const char* context) {
    BindInstanceGuardCapture* capture = g_bindInstanceCapture.load(std::memory_order_relaxed);
    if (capture == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->contexts.emplace_back(context != nullptr ? context : "");
}

// RAII guard mirroring log_test.cpp's ForwardCallbackGuard/debug_checks_test.
// cpp's ViolationHookGuard: restores the production default (abort-on-
// violation) on scope exit regardless of how the TEST_CASE exits, so a
// left-over capture hook can never swallow a real violation in a later test.
struct BindInstanceGuardHookScope {
    ~BindInstanceGuardHookScope() {
        g_bindInstanceCapture.store(nullptr, std::memory_order_relaxed);
        rx::core::debug::detail::setViolationHookForTests(nullptr);
    }
};

bool contextsContain(const std::vector<std::string>& contexts, const std::string& needle) {
    for (const std::string& context : contexts) {
        if (context == needle) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("MaterialSystem::bindInstance's RX_ASSERT_MAIN_THREAD guard does not fire for the legal chunk-0-only "
          "call (chunk 0 always runs synchronously on the main thread, per spec D4's chunk-0 guarantee)") {
    auto fixture = makeFixture("rx_material_bind_instance_guard_legal");
    if (!fixture.has_value()) {
        return;
    }

    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("bind_instance_guard_legal"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle handle = system->loadMaterial(testDataPath("test_unlit.slang"));
    uint32_t blockSize = system->paramBlockSize(handle);
    REQUIRE(blockSize > 0);
    std::vector<uint8_t> blob(blockSize, 0);
    const std::vector<rx::material::MaterialParamInfo>& params = system->materialParams(handle);
    REQUIRE(params.size() == 1);
    float tint[4] = {0.1F, 0.2F, 0.3F, 1.0F};
    std::memcpy(blob.data() + params[0].offset, tint, sizeof(tint));
    rx::material::InstanceBinding binding{handle, blob.data(), blob.size()};

    auto scheduler = rx::task::Scheduler::create();
    REQUIRE(scheduler != nullptr);
    auto executor = rx::graph::Executor::create(fixture->device, *scheduler);
    REQUIRE(executor != nullptr);

    constexpr VkExtent2D kExtent{64, 64};
    constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    rx::graph::RenderGraph graph;
    rx::graph::AttachmentDesc colorDesc;
    colorDesc.format = kColorFormat;
    std::atomic<uint32_t> chunk0Calls{0};
    graph.addPass("bind_instance_guard_legal")
        .addColorOutput("color", colorDesc)
        .setExecuteChunked([&](rx::graph::PassContext& ctx, uint32_t chunkIndex, uint32_t /*chunkCount*/) {
            if (chunkIndex != 0) {
                return;  // every non-zero chunk is a deliberate no-op for this test.
            }
            chunk0Calls.fetch_add(1, std::memory_order_relaxed);
            system->beginFrame(/*frameInFlightIndex=*/0, /*frameNumber=*/0);
            system->bindInstance(ctx.chunkCommandBuffer(), ctx, binding);
        });
    graph.setBackbufferSource("color");

    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = kExtent.width;
    compileInfo.swapchainHeight = kExtent.height;
    compileInfo.swapchainFormat = kColorFormat;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);
    executor->realize(graph);

    BindInstanceGuardCapture capture;
    g_bindInstanceCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureBindInstanceViolation);
    BindInstanceGuardHookScope hookScope;

    auto offscreen =
        createOffscreenColorImage(fixture->device.device(), fixture->device.physicalDevice(), kColorFormat, kExtent);
    REQUIRE(offscreen.has_value());
    {
        auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                        fixture->device.graphicsQueueFamily());
        REQUIRE(cmdCtx.has_value());
        cmdCtx->runOnce(
            [&](VkCommandBuffer cmd) { executor->execute(graph, cmd, offscreen->image, offscreen->view, kExtent); });
    }
    CHECK(chunk0Calls.load() == 1);

    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        CHECK(capture.contexts.empty());
    }

    destroyOffscreenColorImage(fixture->device.device(), *offscreen);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MaterialSystem::bindInstance's RX_ASSERT_MAIN_THREAD guard fires, naming bindInstance, for an illegal "
          "call from a chunk PROVEN to run on a genuine worker thread by a two-chunk rendezvous barrier -- "
          "deterministic by construction, not by luck (fix round 2: the CAS-only version below it replaced had "
          "a real, reviewer-reproduced vacuous-pass window -- 2/30 runs where every chunk landed on the main "
          "thread and the test silently passed having exercised nothing)") {
    auto fixture = makeFixture("rx_material_bind_instance_guard_illegal");
    if (!fixture.has_value()) {
        return;
    }

    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("bind_instance_guard_illegal"));
    REQUIRE(system != nullptr);

    rx::material::MaterialHandle handle = system->loadMaterial(testDataPath("test_unlit.slang"));
    uint32_t blockSize = system->paramBlockSize(handle);
    REQUIRE(blockSize > 0);
    std::vector<uint8_t> blob(blockSize, 0);
    const std::vector<rx::material::MaterialParamInfo>& params = system->materialParams(handle);
    REQUIRE(params.size() == 1);
    float tint[4] = {0.4F, 0.5F, 0.6F, 1.0F};
    std::memcpy(blob.data() + params[0].offset, tint, sizeof(tint));
    rx::material::InstanceBinding binding{handle, blob.data(), blob.size()};

    // A generous worker count (6) so chunkCountForWorkerCount() derives 6
    // chunks -- chunks [1, 6) (5 items) fan out via ONE parallelFor() call
    // across 6 real background workers, already parked on a semaphore per
    // docs/threading.md. See the barrier design below for why this test no
    // longer depends on any chunk landing on a worker "probably" -- it is
    // now forced by construction. Needs chunkCount >= 3 (checked via
    // REQUIRE below) so the fanned range has at least 2 items for the
    // barrier (chunks 1 and 2); 6 workers leaves ample headroom above that
    // floor.
    auto scheduler = rx::task::Scheduler::create(/*workerCount=*/6);
    REQUIRE(scheduler != nullptr);
    auto executor = rx::graph::Executor::create(fixture->device, *scheduler);
    REQUIRE(executor != nullptr);

    constexpr VkExtent2D kExtent{64, 64};
    constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    rx::graph::RenderGraph graph;
    rx::graph::AttachmentDesc colorDesc;
    colorDesc.format = kColorFormat;

    const std::thread::id mainThreadId = std::this_thread::get_id();

    // --- The barrier [fix round 2, coordinator's own "same barrier trick
    // as the Task 2 fix" instruction] -----------------------------------
    //
    // The coordinator's literal wording ("chunk 0 blocks until a later
    // chunk signals it") does not fit this executor's actual, VERIFIED
    // execution order -- Executor::recordChunkedPass() (executor.cpp)
    // calls chunk 0's callback synchronously TO COMPLETION
    // (`recordOneChunk(0, 0);`) and only THEN, on the very next line,
    // calls `scheduler->parallelFor(chunkCount - 1, ...)` to dispatch
    // chunks [1, chunkCount). If chunk 0's own callback blocks waiting for
    // a flag only a later chunk can set, that later chunk's dispatch is
    // never reached at all -- the wait times out 100% of the time, by
    // construction, not rarely. This is not a hypothetical: it follows
    // directly from code already cited in this task's own fix-round-1
    // report. Chunk 0 stays a plain no-op below, exactly as in every
    // earlier revision of this test.
    //
    // What DOES fit -- and is the SAME rendezvous-barrier trick the
    // coordinator pointed at (rx_task's own scheduler_test.cpp: two units
    // of ONE parallelFor(2, 1, fn) call, each incrementing a shared atomic
    // then busy-waiting for it to reach 2, bounded by a timeout that
    // records rather than hangs) -- is applying it to two chunks that
    // *are* both dispatched by the SAME parallelFor call: chunks 1 and 2,
    // both members of chunks [1, chunkCount) above. A single thread cannot
    // resolve this barrier alone: if one thread runs chunk 1 and reaches
    // the wait (having already incremented the counter to 1), it cannot
    // also go on to run chunk 2 itself -- it is not yet back from chunk
    // 1's own callback, so the scheduler has no opportunity to hand it
    // chunk 2 too. The counter can only reach 2 if some OTHER thread
    // (necessarily one of the 6 real, already-parked workers, or main --
    // main is unavailable here since it is inside the very
    // `parallelFor()` call INITIATING chunk 1 and chunk 2, not itself one
    // of the two, until/unless it ALSO helps by grabbing one of these two
    // sub-ranges while waiting for the whole set to finish) runs chunk 2
    // concurrently. The barrier resolving (no timeout) therefore proves at
    // least one of {chunk 1, chunk 2} ran on a thread distinct from
    // whichever thread ran the other -- and since a single thread cannot
    // be both callers' own thread AND wait on itself, at most ONE of the
    // two can be `mainThreadId`; the other is unconditionally a genuine
    // worker. This is forced by construction, exactly like the Task 2
    // fix's own proof, just relocated to the two chunks this executor's
    // real sequencing actually allows to participate in a mutual wait.
    std::atomic<uint32_t> barrierArrived{0};
    std::atomic<bool> barrierTimedOut{false};
    constexpr auto kBarrierTimeout = std::chrono::seconds(10);

    // CAS-claimed, same as fix round 1: whichever of {chunk 1, chunk 2}
    // resolves the barrier AND observes it is not on the main thread
    // claims the one illegal call this test makes. The claim happens
    // strictly AFTER the barrier has already resolved (both arrived) --
    // the violation hook records and returns rather than aborting (see
    // BindInstanceGuardHookScope/captureBindInstanceViolation above), so
    // the claiming chunk completes its own callback normally either way;
    // there is no path where the barrier's OWN resolution depends on the
    // illegal call finishing, which is exactly what rules out the
    // coordinator's flagged deadlock hazard here.
    std::atomic<bool> claimed{false};
    std::mutex recordedThreadMutex;
    std::optional<std::thread::id> recordedThreadId;
    std::atomic<uint32_t> illegalChunkCalls{0};

    graph.addPass("bind_instance_guard_illegal")
        .addColorOutput("color", colorDesc)
        .setExecuteChunked([&](rx::graph::PassContext& ctx, uint32_t chunkIndex, uint32_t /*chunkCount*/) {
            if (chunkIndex != 1 && chunkIndex != 2) {
                return;  // chunk 0 and every fanned chunk beyond the barrier pair stay plain no-ops.
            }

            barrierArrived.fetch_add(1, std::memory_order_acq_rel);
            const auto waitStart = std::chrono::steady_clock::now();
            while (barrierArrived.load(std::memory_order_acquire) < 2) {
                if (std::chrono::steady_clock::now() - waitStart > kBarrierTimeout) {
                    barrierTimedOut.store(true, std::memory_order_relaxed);
                    return;  // fails loudly outside the callback, below -- see the REQUIRE_MESSAGE after execute().
                }
                std::this_thread::yield();
            }

            // Barrier resolved: both chunk 1 and chunk 2 have reached this
            // point. Per this TEST_CASE's own header comment, at most one
            // of them can be mainThreadId -- so at least one of the two
            // calls reaching here is unconditionally a genuine worker.
            if (std::this_thread::get_id() == mainThreadId) {
                return;  // this one happens to be main -- legal either way; the other one is the proven worker.
            }
            bool expected = false;
            if (!claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                return;  // the other barrier participant already claimed it (also non-main) -- avoid a double call.
            }
            illegalChunkCalls.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(recordedThreadMutex);
                recordedThreadId = std::this_thread::get_id();
            }
            // Deliberately illegal: bindInstance() is main-thread-only
            // (docs/threading.md D5) and this callback has already proven
            // (above) it is running on a genuine non-main thread -- exactly
            // the contract violation RX_ASSERT_MAIN_THREAD exists to
            // catch. Safe to let continue past the guard here: the CAS
            // claim above guarantees no other thread touches `system` for
            // the rest of this execute() call.
            system->beginFrame(/*frameInFlightIndex=*/0, /*frameNumber=*/0);
            system->bindInstance(ctx.chunkCommandBuffer(), ctx, binding);
        });
    graph.setBackbufferSource("color");

    const uint32_t expectedChunkCount = rx::graph::detail::chunkCountForWorkerCount(scheduler->workerCount());
    REQUIRE(expectedChunkCount >= 3);  // need chunks 1 AND 2 to both exist for the barrier.

    rx::graph::CompileInfo compileInfo;
    compileInfo.swapchainWidth = kExtent.width;
    compileInfo.swapchainHeight = kExtent.height;
    compileInfo.swapchainFormat = kColorFormat;
    compileInfo.backbufferFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    graph.compile(compileInfo);
    executor->realize(graph);

    BindInstanceGuardCapture capture;
    g_bindInstanceCapture.store(&capture, std::memory_order_relaxed);
    rx::core::debug::detail::setViolationHookForTests(&captureBindInstanceViolation);
    BindInstanceGuardHookScope hookScope;

    auto offscreen =
        createOffscreenColorImage(fixture->device.device(), fixture->device.physicalDevice(), kColorFormat, kExtent);
    REQUIRE(offscreen.has_value());
    {
        auto cmdCtx = rx::rhi::CommandContext::create(fixture->device.device(), fixture->device.graphicsQueue(),
                                                        fixture->device.graphicsQueueFamily());
        REQUIRE(cmdCtx.has_value());
        cmdCtx->runOnce(
            [&](VkCommandBuffer cmd) { executor->execute(graph, cmd, offscreen->image, offscreen->view, kExtent); });
    }

    REQUIRE_MESSAGE(!barrierTimedOut.load(std::memory_order_relaxed),
                     "the chunk 1 / chunk 2 rendezvous barrier did not resolve within 10s -- either a real "
                     "scheduler deadlock or a change in enkiTS's own parallelFor/work-stealing behavior that "
                     "invalidates this test's own documented assumptions about it (see this TEST_CASE's header "
                     "comment)");

    // No longer an "if claimed" branch, unlike fix round 1's CAS-only
    // version: the barrier above makes this unconditional. If it resolved
    // at all (checked above), at least one of {chunk 1, chunk 2} is
    // provably non-main, so the CAS claim below is guaranteed to have
    // happened -- REQUIRE, not a conditional CHECK.
    REQUIRE(claimed.load(std::memory_order_acquire));
    CHECK(illegalChunkCalls.load() == 1);
    REQUIRE(recordedThreadId.has_value());
    CHECK(*recordedThreadId != mainThreadId);
    {
        std::lock_guard<std::mutex> lock(capture.mutex);
        // getPipeline() is called internally by bindInstance() and carries
        // the identical guard -- both firing on this one violating call is
        // expected, not a double-count bug.
        CHECK(contextsContain(capture.contexts, "MaterialSystem::bindInstance"));
        CHECK(contextsContain(capture.contexts, "MaterialSystem::getPipeline"));
    }

    destroyOffscreenColorImage(fixture->device.device(), *offscreen);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}
#endif  // RX_DEBUG_CHECKS

// --- createTexture2D()/textureBindlessIndex()/releaseTexture() -----------
// [Task 7 coordinator addition 4].
TEST_CASE("MaterialSystem::createTexture2D registers a real bindless-indexed texture; releaseTexture defers real "
          "teardown until onFrameCompleted") {
    auto fixture = makeFixture("rx_material_create_texture");
    if (!fixture.has_value()) {
        return;
    }
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("create_texture"));
    REQUIRE(system != nullptr);

    constexpr uint32_t kWidth = 4;
    constexpr uint32_t kHeight = 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4, 0);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>(i % 256);
    }

    rx::material::TextureCreateInfo info;
    info.width = kWidth;
    info.height = kHeight;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.generateMips = false;
    info.pixels = pixels.data();
    info.pixelBytes = pixels.size();

    rx::material::TextureHandle handle = system->createTexture2D(info);
    CHECK(handle.isValid());
    uint32_t index = system->textureBindlessIndex(handle);

    rx::material::TextureHandle handle2 = system->createTexture2D(info);
    CHECK(handle2.isValid());
    uint32_t index2 = system->textureBindlessIndex(handle2);
    CHECK(index2 != index);  // a fresh registration, not a reused/aliased slot.

    system->beginFrame(/*frameInFlightIndex=*/0, /*frameNumber=*/5);
    system->releaseTexture(handle);
    // Releasing an already-released handle a second time is documented as
    // a logged no-op, never a crash.
    system->releaseTexture(handle);

    // Not yet actually destroyed -- confirming an EARLIER frame number
    // must not run this retirement.
    system->onFrameCompleted(4);
    // Now genuinely safe.
    system->onFrameCompleted(5);

    system->releaseTexture(handle2);
    system->onFrameCompleted(5);

    // An invalid/unknown handle throws, matching every other accessor's
    // contract.
    CHECK_THROWS_AS(static_cast<void>(system->textureBindlessIndex(handle)), std::out_of_range);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [Stage 0 audit F2 regression] releaseTexture() used to call
// impl.bindless->release(record->bindlessHandle) immediately, returning the
// slot to BindlessTable's free list right away even though the real
// Texture2D/VkImage/VkImageView teardown correctly waited for
// onFrameCompleted() -- a release-then-createTexture2D within the
// frames-in-flight window could hand that exact slot straight back out to
// a NEW registration while a still in-flight frame's draws were still
// dynamically indexing it (bindless.h:140-149's own release-safety
// contract). The fix folds the bindless release into the SAME
// DeletionQueue-retired closure as the real teardown.
//
// This test discriminates the fix from the bug the same way bindless.h's
// own contract comment does: rx::core::HandlePool's free list is LIFO
// (handle.h's acquire()/release()), so a premature release is not a rare
// race here -- it is the deterministic next-registration outcome. Filling
// the fixture's BindlessTable sampled-image capacity (4, see makeFixture()
// above) completely, releasing exactly one slot, then trying to register
// MORE textures than that single freed slot could ever satisfy turns "did
// the slot come back too early" into an observable throw/no-throw, not a
// probabilistic one.
TEST_CASE("MaterialSystem::releaseTexture defers returning the bindless slot itself to the free list until "
          "onFrameCompleted, not just the real Texture2D teardown") {
    auto fixture = makeFixture("rx_material_release_slot_deferred");
    if (!fixture.has_value()) {
        return;
    }
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("release_slot_deferred"));
    REQUIRE(system != nullptr);

    constexpr uint32_t kWidth = 2;
    constexpr uint32_t kHeight = 2;
    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4, 0);

    rx::material::TextureCreateInfo info;
    info.width = kWidth;
    info.height = kHeight;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.generateMips = false;
    info.pixels = pixels.data();
    info.pixelBytes = pixels.size();

    // Fixture's own BindlessTable::Capacities.sampledImages == 4 -- fill it
    // completely. MaterialSystem::create() itself only ever registers a
    // SAMPLER (the default sampler), never a sampled image, so all 4
    // sampled-image slots are available here (see makeFixture()/
    // MaterialSystem::create()'s own registerSampler() call).
    std::vector<rx::material::TextureHandle> handles;
    for (int i = 0; i < 4; ++i) {
        rx::material::TextureHandle h = system->createTexture2D(info);
        REQUIRE(h.isValid());
        handles.push_back(h);
    }
    uint32_t releasedIndex = system->textureBindlessIndex(handles[2]);

    system->beginFrame(/*frameInFlightIndex=*/0, /*frameNumber=*/42);
    system->releaseTexture(handles[2]);

    // If the slot were returned to bindless's free list immediately (the
    // pre-fix bug), the very next createTexture2D() would succeed and
    // reuse it -- LIFO, so deterministically EXACTLY `releasedIndex`, not
    // some other slot. With the fix, the slot has not come back yet (no
    // onFrameCompleted() has run at all): every attempt below must fail
    // with capacity exhausted, exactly as if nothing had ever been
    // released. Two attempts, not one, per this regression's own "enough
    // to exhaust the free list if the slot were recycled early" framing.
    CHECK_THROWS_AS(static_cast<void>(system->createTexture2D(info)), std::runtime_error);
    CHECK_THROWS_AS(static_cast<void>(system->createTexture2D(info)), std::runtime_error);

    // An earlier frame number must not run this retirement either (mirrors
    // the existing createTexture2D test's own "not yet actually destroyed"
    // check) -- the slot must still not be back.
    system->onFrameCompleted(41);
    CHECK_THROWS_AS(static_cast<void>(system->createTexture2D(info)), std::runtime_error);

    // NOW genuinely safe: this frame's fence has signaled.
    system->onFrameCompleted(42);

    // The slot is back -- and, per the free list's own LIFO order, it is
    // handed out to the very next registration, landing on the EXACT index
    // that was released above (not merely "some" slot becoming available),
    // positively confirming this is the deferred release actually taking
    // effect.
    rx::material::TextureHandle reused = system->createTexture2D(info);
    CHECK(reused.isValid());
    CHECK(system->textureBindlessIndex(reused) == releasedIndex);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("MaterialSystem::createTexture2D rejects zero width/height and null/empty pixel data") {
    auto fixture = makeFixture("rx_material_create_texture_invalid");
    if (!fixture.has_value()) {
        return;
    }
    auto system = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                          freshCachePath("create_texture_invalid"));
    REQUIRE(system != nullptr);

    std::vector<uint8_t> pixels(16, 0);

    rx::material::TextureCreateInfo zeroWidth;
    zeroWidth.width = 0;
    zeroWidth.height = 4;
    zeroWidth.pixels = pixels.data();
    zeroWidth.pixelBytes = pixels.size();
    CHECK_THROWS_AS(static_cast<void>(system->createTexture2D(zeroWidth)), std::invalid_argument);

    rx::material::TextureCreateInfo nullPixels;
    nullPixels.width = 4;
    nullPixels.height = 4;
    nullPixels.pixels = nullptr;
    nullPixels.pixelBytes = 0;
    CHECK_THROWS_AS(static_cast<void>(system->createTexture2D(nullPixels)), std::invalid_argument);

    CHECK_FALSE(fixture->context.hasValidationErrors());
}
