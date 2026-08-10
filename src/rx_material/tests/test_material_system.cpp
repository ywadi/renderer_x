#include <doctest/doctest.h>
#include <rx_material/material_system.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

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
    capacities.storageBuffers = 1;
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

}  // namespace

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
    REQUIRE(layout.bindings.size() == 1);
    CHECK(layout.bindings[0].set == 1);
    CHECK(layout.bindings[0].binding == 0);
    CHECK(layout.bindings[0].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    CHECK(layout.bindings[0].count == 1);
    CHECK_FALSE(layout.bindings[0].unboundedArray);

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
