// Task 6, spec Phase 3 design D5: rxCreateMaterialSystem() +
// IRxMaterialSystem::loadMaterial()'s happy path against a REAL VkDevice-
// backed internal rx::material::MaterialSystem, plus
// IRxMaterialInstance::setFloat/setFloat4/setTexture's validation
// contract (name exists in the material's reflected parameters, type
// matches) exercised against real reflected data from test_unlit.slang
// (a `float4 tint`) and test_textured.slang (a `uint albedoIndex` --
// this engine's D8 bindless-index convention). The device-free half of
// this ABI's contract (queryInterface identity, refcount round-trips,
// error codes on malformed input) lives in test_api_contract.cpp
// (rx_material_tests) instead -- see that file's own header comment.
#include <doctest/doctest.h>
#include <rx_material/material_system.h>
#include <rx_material/rx_api.h>
#include <rx_material/rx_api_detail.h>
#include <rx_platform/window.h>
#include <rx_rhi_vk/bindless.h>
#include <rx_rhi_vk/context.h>
#include <rx_rhi_vk/device.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// RX_MATERIAL_TEST_DATA_DIR -- see test_material_system.cpp's own
// comment; identical convention, this file's own copy of the same tiny
// helper (matching that file's own precedent of not sharing private
// per-test-binary helpers across translation units).

namespace {

std::filesystem::path testDataPath(const char* filename) {
    return std::filesystem::path(RX_MATERIAL_TEST_DATA_DIR) / filename;
}

// Identical fixture shape to test_material_system.cpp's own
// MaterialTestFixture/makeFixture() -- duplicated, not shared, matching
// that file's own precedent (see its comment) rather than reaching into
// another test binary's private helpers.
struct ApiTestFixture {
    rx::platform::Window window;
    rx::rhi::Context context;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    rx::rhi::Device device;
    rx::rhi::BindlessTable bindless;
};

std::optional<ApiTestFixture> makeFixture(const char* title) {
    auto window = rx::platform::Window::create(title, 64, 64, /*visible=*/false);
    if (!window.has_value()) {
        MESSAGE("no display backend available, skipping rx_api test");
        return std::nullopt;
    }

    auto extensions = window->requiredVulkanInstanceExtensions();
    if (extensions.empty()) {
        MESSAGE("video driver reports no Vulkan surface extensions (e.g. dummy driver), skipping rx_api test");
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

    return ApiTestFixture{std::move(*window), std::move(*context), surface, std::move(*device),
                            std::move(*bindless)};
}

std::filesystem::path freshCachePath(const char* name) {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / (std::string("rx_material_api_test_") + name + ".cache");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

// Minimal IRxTexture test double [Task 6] -- proves
// IRxMaterialInstance::setTexture()'s addRef()/release() lifetime
// handling (it stores the pointer with one added reference, releases
// any previously stored one on overwrite, and releases whatever is
// still stored when the instance itself is destroyed) without needing a
// real renderer-owned texture: nothing in Task 6's own surface creates a
// real IRxTexture yet (no rxCreateTexture factory -- see rx_api.h's own
// comment on IRxTexture), so a hand-rolled double is the only way to
// exercise this path at all before that exists. This is a TEST-ONLY
// implementation of a COM-lite interface from outside api_impl.cpp --
// legal per the ABI shape itself (any binary can implement a pure-
// virtual interface), but not a claim that third-party IRxTexture
// implementations are a supported Task 6 use case.
class FakeTexture final : public IRxTexture {
public:
    RxResult queryInterface(const RxGuid& iid, void** outObject) override {
        if (outObject == nullptr) {
            return RX_E_INVALIDARG;
        }
        if (guidEquals(iid, kIID_IRxUnknown) || guidEquals(iid, kIID_IRxTexture)) {
            addRef();
            *outObject = static_cast<IRxTexture*>(this);
            return RX_OK;
        }
        *outObject = nullptr;
        return RX_E_NOINTERFACE;
    }
    uint32_t addRef() override { return ++refCount_; }
    uint32_t release() override {
        uint32_t remaining = --refCount_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    [[nodiscard]] uint32_t refCount() const { return refCount_; }

private:
    static bool guidEquals(const RxGuid& a, const RxGuid& b) {
        return a.data1 == b.data1 && a.data2 == b.data2 && a.data3 == b.data3 &&
               std::equal(std::begin(a.data4), std::end(a.data4), std::begin(b.data4));
    }

    uint32_t refCount_ = 1;
};

// [Fix round 1, task-6-review.md F1] Same cheap structural guard as
// test_api_contract.cpp's own identically-named helper (duplicated, not
// shared -- matching this file's own established precedent of not
// reaching into another test binary's private helpers). See that file's
// comment on its own copy for exactly what this does and doesn't prove.
bool isDocumentedResult(RxResult result) {
    switch (result) {
        case RX_OK:
        case RX_E_FAIL:
        case RX_E_INVALIDARG:
        case RX_E_NOTFOUND:
        case RX_E_COMPILE:
        case RX_E_NOINTERFACE:
            return true;
        default:
            return false;
    }
}

}  // namespace

TEST_CASE("rxCreateMaterialSystem + loadMaterial: happy path against a real device, IRxMaterialInstance validates "
          "reflected float4 params") {
    auto fixture = makeFixture("rx_api_factory_happy_path");
    if (!fixture.has_value()) {
        return;
    }

    auto internal =
        rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("api_happy_path"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);
    REQUIRE(system != nullptr);

    IRxMaterial* material = nullptr;
    std::string unlitPath = testDataPath("test_unlit.slang").string();
    REQUIRE(system->loadMaterial(unlitPath.c_str(), &material) == RX_OK);
    REQUIRE(material != nullptr);
    CHECK(std::string(material->name()) == "test_unlit");

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);
    REQUIRE(instance != nullptr);

    // Real reflected field: `tint` is a `float4` in test_unlit.slang.
    float tint[4] = {1.0F, 0.5F, 0.25F, 1.0F};
    CHECK(instance->setFloat4("tint", tint) == RX_OK);

    // Same real field, wrong setter -- type mismatch, not "unknown".
    CHECK(instance->setFloat("tint", 1.0F) == RX_E_INVALIDARG);
    FakeTexture* wrongTypeTexture = new FakeTexture();
    CHECK(instance->setTexture("tint", wrongTypeTexture) == RX_E_INVALIDARG);
    wrongTypeTexture->release();

    // No such parameter on this material at all.
    CHECK(instance->setFloat("nonexistent", 1.0F) == RX_E_NOTFOUND);
    float bogus[4] = {0, 0, 0, 0};
    CHECK(instance->setFloat4("nonexistent", bogus) == RX_E_NOTFOUND);
    FakeTexture* notFoundTexture = new FakeTexture();
    CHECK(instance->setTexture("nonexistent", notFoundTexture) == RX_E_NOTFOUND);
    notFoundTexture->release();

    // Null-argument validation.
    CHECK(instance->setFloat(nullptr, 1.0F) == RX_E_INVALIDARG);
    CHECK(instance->setFloat4("tint", nullptr) == RX_E_INVALIDARG);
    CHECK(instance->setTexture("tint", nullptr) == RX_E_INVALIDARG);

    // queryInterface identity, one level down the object graph too.
    void* instanceAsUnknown = nullptr;
    REQUIRE(instance->queryInterface(kIID_IRxUnknown, &instanceAsUnknown) == RX_OK);
    void* instanceAsInstance = nullptr;
    REQUIRE(instance->queryInterface(kIID_IRxMaterialInstance, &instanceAsInstance) == RX_OK);
    CHECK(instanceAsUnknown == instanceAsInstance);
    static_cast<IRxUnknown*>(instanceAsUnknown)->release();
    static_cast<IRxUnknown*>(instanceAsInstance)->release();

    instance->release();
    material->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("IRxMaterialInstance::setTexture validates a real bindless-index (uint) parameter and manages the "
          "IRxTexture reference it stores") {
    auto fixture = makeFixture("rx_api_factory_texture_param");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_texture_param"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    IRxMaterial* material = nullptr;
    std::string texturedPath = testDataPath("test_textured.slang").string();
    REQUIRE(system->loadMaterial(texturedPath.c_str(), &material) == RX_OK);
    REQUIRE(material != nullptr);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);

    // Wrong setter for a real uint (bindless-index) field -- type
    // mismatch.
    CHECK(instance->setFloat("albedoIndex", 1.0F) == RX_E_INVALIDARG);

    auto* first = new FakeTexture();
    CHECK(first->refCount() == 1);
    REQUIRE(instance->setTexture("albedoIndex", first) == RX_OK);
    CHECK(first->refCount() == 2);  // instance now holds its own reference too

    // Overwriting the same parameter releases the previously stored
    // texture and addRef()s the new one.
    auto* second = new FakeTexture();
    REQUIRE(instance->setTexture("albedoIndex", second) == RX_OK);
    CHECK(first->refCount() == 1);   // instance's reference on `first` was released
    CHECK(second->refCount() == 2);  // instance now holds a reference on `second`

    first->release();  // drop the caller's own ref -- `first` is fully destroyed now.

    // Destroying the instance releases whatever it still holds
    // (`second`) exactly once.
    instance->release();
    CHECK(second->refCount() == 1);  // only the caller's own ref remains
    second->release();

    material->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("loadMaterial maps a real Slang compile failure to RX_E_COMPILE, never throws across the boundary, and "
          "leaves no orphaned live API object behind") {
    auto fixture = makeFixture("rx_api_factory_bad_module");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_bad_module"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    // [Fix round 1, task-6-review.md F2] Recorded AFTER the system object
    // itself exists (so this count only reflects what `loadMaterial`
    // below does, not construction of `system`), immediately before the
    // call that is expected to fail. A failed loadMaterial() must never
    // leave behind a live MaterialImpl API object -- this is the
    // detail::debugLiveApiObjectCount() seam's exact purpose (see
    // rx_api_detail.h). Note what this DOES and DOES NOT prove: it
    // observes only IRxUnknown-rooted API wrapper objects
    // (MaterialSystemImpl/MaterialImpl/MaterialInstanceImpl), not
    // rx::material::MaterialSystem's own internal MaterialRecord/GPU
    // resources (Task 5's MaterialSystem exposes no count accessor for
    // those) -- so it cannot directly observe F2's specific internal-
    // record-orphaning failure mode. It DOES prove the API layer itself
    // never constructs-then-abandons a MaterialImpl on this failure
    // path, which was already true before the F2 reorder (loadMaterial
    // only ever calls `new MaterialImpl(...)` after every prior step has
    // already succeeded) and remains true after it -- a real, if
    // narrower, regression guard.
    uint64_t liveBefore = rx::material::detail::debugLiveApiObjectCount();

    IRxMaterial* material = reinterpret_cast<IRxMaterial*>(static_cast<uintptr_t>(0x1));  // poisoned
    std::string badPath = testDataPath("test_bad_syntax.slang").string();
    CHECK(system->loadMaterial(badPath.c_str(), &material) == RX_E_COMPILE);
    CHECK(material == nullptr);
    CHECK(rx::material::detail::debugLiveApiObjectCount() == liveBefore);

    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("entry-point audit: IRxMaterialInstance's three setters return a documented RxResult across "
          "normal/edge/malformed inputs against a real instance, never crash") {
    auto fixture = makeFixture("rx_api_factory_setter_audit");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_setter_audit"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    IRxMaterial* material = nullptr;
    std::string unlitPath = testDataPath("test_unlit.slang").string();
    REQUIRE(system->loadMaterial(unlitPath.c_str(), &material) == RX_OK);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);

    auto* texture = new FakeTexture();
    float value4[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    // A long name deliberately pokes at std::string's allocation path
    // (the exact code F1 found without a try/catch) -- not an OOM
    // injection, just a normal-sized-heap-allocation input rather than a
    // short-string-optimized one.
    std::string longName(4096, 'x');

    const char* names[] = {"tint", "nonexistent", "", longName.c_str()};
    for (const char* name : names) {
        CHECK(isDocumentedResult(instance->setFloat(name, 1.0F)));
        CHECK(isDocumentedResult(instance->setFloat4(name, value4)));
        CHECK(isDocumentedResult(instance->setTexture(name, texture)));
    }

    // Null-argument edges on all three setters.
    CHECK(isDocumentedResult(instance->setFloat(nullptr, 1.0F)));
    CHECK(isDocumentedResult(instance->setFloat4("tint", nullptr)));
    CHECK(isDocumentedResult(instance->setFloat4(nullptr, value4)));
    CHECK(isDocumentedResult(instance->setTexture("tint", nullptr)));
    CHECK(isDocumentedResult(instance->setTexture(nullptr, texture)));

    texture->release();
    instance->release();
    material->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// ===== Task 7 =============================================================
// [coordinator addition 2] The setters below no longer validate against a
// second, throwaway Slang reflection session (Task 6's own
// reflectMaterialParams(), now deleted) -- they read straight from
// rx::material::MaterialSystem::materialParams()/paramBlockSize(), and
// write directly into MaterialInstanceImpl's own real CPU-side blob at
// each field's reflected byte offset. These tests observe that blob
// directly through rx_api_detail.h's bridge accessors (the same accessors
// a future sample's draw-time bindInstance() call would use) -- this is
// the mandatory "instance param write->readback of arena blob at
// reflected offsets (exact bytes)" case from the Task 7 brief's test list,
// exercised at the ABI layer (test_material_system.cpp covers the
// identical claim at the internal MaterialSystem layer already).
TEST_CASE("IRxMaterialInstance's setters write byte-exact values into the internal blob at the material's own "
          "reflected offsets") {
    auto fixture = makeFixture("rx_api_factory_blob_bytes");
    if (!fixture.has_value()) {
        return;
    }

    auto internal =
        rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("blob_bytes"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    IRxMaterial* material = nullptr;
    std::string unlitPath = testDataPath("test_unlit.slang").string();
    REQUIRE(system->loadMaterial(unlitPath.c_str(), &material) == RX_OK);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);

    rx::material::MaterialHandle handle = rx::material::detail::materialHandle(material);
    const std::vector<rx::material::MaterialParamInfo>& params = internal->materialParams(handle);
    REQUIRE(params.size() == 1);
    CHECK(params[0].name == "tint");
    CHECK(params[0].offset == 0);

    // Fresh instance: blob is zero-initialized at every reflected byte.
    const void* blobBefore = rx::material::detail::materialInstanceBlobData(instance);
    size_t blobSize = rx::material::detail::materialInstanceBlobSize(instance);
    REQUIRE(blobSize == internal->paramBlockSize(handle));
    REQUIRE(blobSize >= params[0].offset + params[0].size);
    std::vector<uint8_t> zeros(blobSize, 0);
    CHECK(std::memcmp(blobBefore, zeros.data(), blobSize) == 0);

    float tint[4] = {0.125F, 0.25F, 0.5F, 1.0F};
    REQUIRE(instance->setFloat4("tint", tint) == RX_OK);

    const void* blobAfter = rx::material::detail::materialInstanceBlobData(instance);
    CHECK(std::memcmp(static_cast<const uint8_t*>(blobAfter) + params[0].offset, tint, sizeof(tint)) == 0);

    instance->release();
    material->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [coordinator addition 4] createTexture2D() through the public ABI, and
// setTexture()'s real (not FakeTexture-double) bindless-index write path.
TEST_CASE("IRxMaterialSystem::createTexture2D creates a real texture; setTexture writes its REAL bindless index "
          "into the instance blob") {
    auto fixture = makeFixture("rx_api_factory_create_texture");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_create_texture"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    constexpr uint32_t kWidth = 2;
    constexpr uint32_t kHeight = 2;
    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4, 0x7F);

    RxTextureDesc textureDesc{};
    textureDesc.pixels = pixels.data();
    textureDesc.pixelBytes = pixels.size();
    textureDesc.width = kWidth;
    textureDesc.height = kHeight;
    textureDesc.format = RX_FORMAT_RGBA8_UNORM;
    textureDesc.generateMips = 0;

    IRxTexture* texture = nullptr;
    REQUIRE(system->createTexture2D(&textureDesc, &texture) == RX_OK);
    REQUIRE(texture != nullptr);

    IRxMaterial* material = nullptr;
    std::string texturedPath = testDataPath("test_textured.slang").string();
    REQUIRE(system->loadMaterial(texturedPath.c_str(), &material) == RX_OK);

    IRxMaterialInstance* instance = nullptr;
    REQUIRE(material->createInstance(&instance) == RX_OK);

    rx::material::MaterialHandle materialHandle = rx::material::detail::materialHandle(material);
    const std::vector<rx::material::MaterialParamInfo>& params = internal->materialParams(materialHandle);
    REQUIRE(params.size() == 1);
    CHECK(params[0].name == "albedoIndex");

    REQUIRE(instance->setTexture("albedoIndex", texture) == RX_OK);

    // The blob must contain the texture's REAL bindless index -- this
    // fixture builds a fresh rx::rhi::BindlessTable per test, and this is
    // the very first sampled-image registration against it, so the real
    // index is deterministically 0 (bindless.cpp registers sequentially
    // from index 0).
    const void* blob = rx::material::detail::materialInstanceBlobData(instance);
    uint32_t writtenIndex = 0;
    std::memcpy(&writtenIndex, static_cast<const uint8_t*>(blob) + params[0].offset, sizeof(uint32_t));
    CHECK(writtenIndex == 0);

    instance->release();
    material->release();
    texture->release();
    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

TEST_CASE("IRxMaterialSystem::createTexture2D validates desc/outTexture and rejects malformed input") {
    auto fixture = makeFixture("rx_api_factory_create_texture_invalid");
    if (!fixture.has_value()) {
        return;
    }

    auto internal = rx::material::MaterialSystem::create(fixture->device, fixture->bindless,
                                                            freshCachePath("api_create_texture_invalid"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    IRxTexture* texture = reinterpret_cast<IRxTexture*>(static_cast<uintptr_t>(0x1));  // poisoned
    CHECK(system->createTexture2D(nullptr, &texture) == RX_E_INVALIDARG);
    CHECK(texture == nullptr);

    std::vector<uint8_t> pixels(16, 0);
    RxTextureDesc validDesc{};
    validDesc.pixels = pixels.data();
    validDesc.pixelBytes = pixels.size();
    validDesc.width = 2;
    validDesc.height = 2;
    validDesc.format = RX_FORMAT_RGBA8_UNORM;
    CHECK(system->createTexture2D(&validDesc, nullptr) == RX_E_INVALIDARG);

    RxTextureDesc zeroWidth = validDesc;
    zeroWidth.width = 0;
    texture = nullptr;
    CHECK(system->createTexture2D(&zeroWidth, &texture) == RX_E_INVALIDARG);
    CHECK(texture == nullptr);

    RxTextureDesc noPixels = validDesc;
    noPixels.pixels = nullptr;
    noPixels.pixelBytes = 0;
    CHECK(system->createTexture2D(&noPixels, &texture) == RX_E_INVALIDARG);

    RxTextureDesc badFormat = validDesc;
    badFormat.format = 999;
    CHECK(system->createTexture2D(&badFormat, &texture) == RX_E_INVALIDARG);

    system->release();
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// [coordinator addition 1] reloadChanged() forwards to the real internal
// MaterialSystem::reloadChanged() (D9) rather than the Task 6 documented
// no-op -- verified by actually editing the loaded module on disk and
// confirming (via the SAME internal rx::material::MaterialSystem* this
// test already holds, per RxMaterialSystemDesc's own bridge contract) that
// the pipeline this material builds genuinely changes.
TEST_CASE("IRxMaterialSystem::reloadChanged forwards to the real internal MaterialSystem and actually reloads a "
          "changed module") {
    auto fixture = makeFixture("rx_api_factory_reload");
    if (!fixture.has_value()) {
        return;
    }

    auto internal =
        rx::material::MaterialSystem::create(fixture->device, fixture->bindless, freshCachePath("api_reload"));
    REQUIRE(internal != nullptr);

    RxMaterialSystemDesc desc{internal.get()};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);

    std::filesystem::path modulePath = std::filesystem::temp_directory_path() / "rx_api_factory_reload_test.slang";
    auto baseTime = std::filesystem::file_time_type::clock::now();
    {
        std::ofstream out(modulePath, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out << R"(
import material;
struct UnlitParams { float4 tint; };
[[vk::binding(0, 1)]] ParameterBlock<UnlitParams> gParams;
struct Unlit : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        float4 tint = gParams.tint;
        tint.x += v.worldPos.x * 1e-4;
        tint.y += v.normal.y * 1e-4;
        tint.z += v.uv.x * 1e-4;
        return tint;
    }
};
export struct MaterialImpl : IMaterialShader = Unlit;
)";
    }
    std::error_code ec;
    std::filesystem::last_write_time(modulePath, baseTime, ec);
    REQUIRE_FALSE(ec);

    IRxMaterial* material = nullptr;
    std::string modulePathStr = modulePath.string();
    REQUIRE(system->loadMaterial(modulePathStr.c_str(), &material) == RX_OK);
    rx::material::MaterialHandle handle = rx::material::detail::materialHandle(material);
    uint64_t hashBefore = internal->moduleHash(handle);

    // A reloadChanged() call before any edit must return RX_OK and change
    // nothing.
    CHECK(system->reloadChanged() == RX_OK);
    CHECK(internal->moduleHash(handle) == hashBefore);

    {
        std::ofstream out(modulePath, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out << R"(
import material;
struct UnlitParams { float4 tint; };
[[vk::binding(0, 1)]] ParameterBlock<UnlitParams> gParams;
struct Unlit : IMaterialShader {
    float4 evaluate(MaterialVertex v) {
        float4 tint = gParams.tint * 0.5;
        tint.x += v.worldPos.x * 1e-4;
        tint.y += v.normal.y * 1e-4;
        tint.z += v.uv.x * 1e-4;
        return tint;
    }
};
export struct MaterialImpl : IMaterialShader = Unlit;
)";
    }
    std::filesystem::last_write_time(modulePath, baseTime + std::chrono::seconds(1), ec);
    REQUIRE_FALSE(ec);

    CHECK(system->reloadChanged() == RX_OK);
    CHECK(internal->moduleHash(handle) != hashBefore);

    material->release();
    system->release();
    std::filesystem::remove(modulePath, ec);
    CHECK_FALSE(fixture->context.hasValidationErrors());
}

// reloadChanged() on a device-free (internal_ == nullptr) instance stays a
// safe, documented no-op -- Task 6's own contract, unchanged: this method
// never touches `internal_` when null.
TEST_CASE("IRxMaterialSystem::reloadChanged is a safe no-op on a device-free instance") {
    RxMaterialSystemDesc desc{nullptr};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);
    CHECK(system->reloadChanged() == RX_OK);
    system->release();
}
