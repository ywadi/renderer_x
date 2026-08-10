// Device-free COM-lite ABI contract tests for rx_api.h [Task 6, spec
// Phase 3 design D5]. None of these touch a VkDevice: every case here
// exercises queryInterface/addRef/release/reloadChanged on an
// IRxMaterialSystem created via rxCreateMaterialSystem with a
// deliberately null `internalMaterialSystem` -- a legal, documented
// state per rx_api.h's own comment on RxMaterialSystemDesc, since none
// of those four methods ever touch the internal pointer. Only
// loadMaterial() needs a real internal MaterialSystem (a real VkDevice),
// which is exactly why its happy path lives in test_api_factory.cpp
// (rx_material_gpu_tests) instead -- this file only checks that calling
// it on a device-free instance fails safely (RX_E_FAIL), never crashes.
#include <doctest/doctest.h>
#include <rx_material/rx_api.h>
#include <rx_material/rx_api_detail.h>

#include <cstdint>

// Re-pinned here (not just in rx_api.h) so this contract test file
// itself exercises the exact sizes a real consumer would depend on --
// mirrors test_api_header_self_contained.cpp's identical pair, kept
// here too per this task's own test-list wording.
static_assert(sizeof(RxGuid) == 16);
static_assert(sizeof(RxMaterialSystemDesc) == sizeof(void*));

namespace {

// A fresh, device-free IRxMaterialSystem -- `internalMaterialSystem`
// is deliberately null; see this file's own header comment for why
// that is a legal state for every method these contract tests call.
IRxMaterialSystem* makeDeviceFreeSystem() {
    RxMaterialSystemDesc desc{nullptr};
    IRxMaterialSystem* system = nullptr;
    REQUIRE(rxCreateMaterialSystem(&desc, &system) == RX_OK);
    REQUIRE(system != nullptr);
    return system;
}

// [Fix round 1, task-6-review.md F1] Cheap structural guard, not an OOM
// injection test: proves an entry point returned a value from the fixed
// RxResult enum (i.e. it returned normally at all, rather than an
// uncaught C++ exception unwinding out of it -- doctest itself would
// catch and FAIL the enclosing TEST_CASE on that, per its default
// exception-trapping behavior, so "every CHECK below actually ran" is
// itself part of what this test proves). Forcing a genuine bad_alloc to
// distinguish try/catch-present from try/catch-absent code was
// considered and rejected as not worth the fault-injection scaffolding
// it would need (per the coordinator's own fix-round-1 instructions);
// this is the cheaper, always-available substitute.
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

TEST_CASE("rxCreateMaterialSystem rejects null desc/outSystem with RX_E_INVALIDARG") {
    IRxMaterialSystem* system = nullptr;
    CHECK(rxCreateMaterialSystem(nullptr, &system) == RX_E_INVALIDARG);
    CHECK(system == nullptr);

    RxMaterialSystemDesc desc{nullptr};
    CHECK(rxCreateMaterialSystem(&desc, nullptr) == RX_E_INVALIDARG);

    CHECK(rxCreateMaterialSystem(nullptr, nullptr) == RX_E_INVALIDARG);
}

TEST_CASE("rxCreateMaterialSystem with a null internalMaterialSystem still succeeds (device-free instance)") {
    IRxMaterialSystem* system = makeDeviceFreeSystem();
    system->release();
}

TEST_CASE("queryInterface null out-param returns RX_E_INVALIDARG") {
    IRxMaterialSystem* system = makeDeviceFreeSystem();
    CHECK(system->queryInterface(kIID_IRxUnknown, nullptr) == RX_E_INVALIDARG);
    system->release();
}

TEST_CASE("queryInterface with an unrelated IID returns RX_E_NOINTERFACE and nulls the out-param") {
    IRxMaterialSystem* system = makeDeviceFreeSystem();

    // Poisoned before the call -- queryInterface must overwrite it to
    // null on the RX_E_NOINTERFACE path, not leave it untouched.
    void* out = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1));
    RxResult result = system->queryInterface(kIID_IRxMaterial, &out);
    CHECK(result == RX_E_NOINTERFACE);
    CHECK(out == nullptr);

    system->release();
}

TEST_CASE("queryInterface identity: IRxUnknown and the object's own interface resolve to the SAME pointer") {
    IRxMaterialSystem* system = makeDeviceFreeSystem();

    void* asUnknown = nullptr;
    REQUIRE(system->queryInterface(kIID_IRxUnknown, &asUnknown) == RX_OK);
    REQUIRE(asUnknown != nullptr);

    void* asMaterialSystem = nullptr;
    REQUIRE(system->queryInterface(kIID_IRxMaterialSystem, &asMaterialSystem) == RX_OK);
    REQUIRE(asMaterialSystem != nullptr);

    // The COM identity rule [R:M§1.5]: the SAME object answers both
    // queries with the SAME pointer value, regardless of which IID was
    // asked for -- guaranteed here by single inheritance (there is only
    // ever one IRxUnknown sub-object per concrete instance).
    CHECK(asUnknown == asMaterialSystem);

    static_cast<IRxUnknown*>(asUnknown)->release();
    static_cast<IRxUnknown*>(asMaterialSystem)->release();
    system->release();
}

TEST_CASE("refcount round-trip: addRef/release destroys the object exactly once, verified via the live-object "
          "counter") {
    uint64_t before = rx::material::detail::debugLiveApiObjectCount();

    IRxMaterialSystem* system = makeDeviceFreeSystem();
    CHECK(rx::material::detail::debugLiveApiObjectCount() == before + 1);

    CHECK(system->addRef() == 2);
    CHECK(rx::material::detail::debugLiveApiObjectCount() == before + 1);  // addRef never constructs a new object

    CHECK(system->release() == 1);
    CHECK(rx::material::detail::debugLiveApiObjectCount() == before + 1);  // still alive: one ref remains

    CHECK(system->release() == 0);
    CHECK(rx::material::detail::debugLiveApiObjectCount() == before);  // destroyed exactly once
}

TEST_CASE("loadMaterial on a device-free IRxMaterialSystem fails safely (RX_E_FAIL), never crashes") {
    IRxMaterialSystem* system = makeDeviceFreeSystem();

    IRxMaterial* material = reinterpret_cast<IRxMaterial*>(static_cast<uintptr_t>(0x1));  // poisoned
    RxResult result = system->loadMaterial("does/not/matter.slang", &material);
    CHECK(result == RX_E_FAIL);
    CHECK(material == nullptr);

    system->release();
}

TEST_CASE("loadMaterial rejects null slangModulePath/outMaterial with RX_E_INVALIDARG before touching internal "
          "state") {
    IRxMaterialSystem* system = makeDeviceFreeSystem();

    IRxMaterial* material = nullptr;
    CHECK(system->loadMaterial(nullptr, &material) == RX_E_INVALIDARG);
    CHECK(system->loadMaterial("does/not/matter.slang", nullptr) == RX_E_INVALIDARG);

    system->release();
}

TEST_CASE("reloadChanged is a documented Task 6 no-op that always returns RX_OK (Task 7 wires the real behavior)") {
    IRxMaterialSystem* system = makeDeviceFreeSystem();
    CHECK(system->reloadChanged() == RX_OK);
    system->release();
}

// [Fix round 1, task-6-review.md F1] Entry-point audit: every
// RxResult-returning method reachable from a device-free (null-internal)
// IRxMaterialSystem, across a spread of normal/edge/malformed inputs,
// returns a value from the documented RxResult enum and never lets an
// exception escape (see isDocumentedResult()'s own comment for exactly
// what that does and doesn't prove). IRxMaterialInstance's three
// setters are NOT reachable from here: constructing a real
// IRxMaterialInstance requires a real IRxMaterial, which requires
// internal_->loadMaterial() to actually succeed against a real
// VkDevice-backed rx::material::MaterialSystem -- an architectural fact,
// not a shortcut (a null-internal IRxMaterialSystem's loadMaterial()
// always returns RX_E_FAIL before ever constructing one -- see the
// dedicated test above). The equivalent setter audit, with a real
// instance, lives in test_api_factory.cpp's own "entry-point audit" case
// instead.
TEST_CASE("entry-point audit: every RxResult-returning method on a device-free IRxMaterialSystem returns a "
          "documented code across normal/edge/malformed inputs, never crashes") {
    IRxMaterialSystem* system = makeDeviceFreeSystem();

    void* outUnknown = nullptr;
    CHECK(isDocumentedResult(system->queryInterface(kIID_IRxUnknown, &outUnknown)));
    if (outUnknown != nullptr) {
        static_cast<IRxUnknown*>(outUnknown)->release();
    }
    void* outUnrelated = nullptr;
    CHECK(isDocumentedResult(system->queryInterface(kIID_IRxMaterial, &outUnrelated)));
    CHECK(isDocumentedResult(system->queryInterface(kIID_IRxUnknown, nullptr)));

    IRxMaterial* material = nullptr;
    CHECK(isDocumentedResult(system->loadMaterial(nullptr, &material)));
    CHECK(isDocumentedResult(system->loadMaterial("does/not/matter.slang", nullptr)));
    CHECK(isDocumentedResult(system->loadMaterial("", &material)));
    CHECK(isDocumentedResult(system->loadMaterial("does/not/matter.slang", &material)));

    CHECK(isDocumentedResult(system->reloadChanged()));

    system->release();
}
