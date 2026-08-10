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
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

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
