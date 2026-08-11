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
#include <mutex>
#include <string>

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

// [Stage 0 audit F3 regression, device-free half] api_impl.cpp:534 used to
// construct std::filesystem::path OUTSIDE loadMaterial()'s try block --
// on the windows-cross-zig target (path::value_type == wchar_t there), a
// byte sequence invalid as UTF-8 makes that narrow->wide construction
// throw std::system_error, which would unwind straight across this ABI
// boundary uncaught. The fix moved that construction inside the try.
//
// This device-free case (internal_ == nullptr) is the honest boundary of
// what a device-free test CAN prove here: loadMaterial()'s own
// internal_ == nullptr guard (this file's own header comment; see the
// "fails safely" test above) returns RX_E_FAIL before ever reaching the
// path construction line at all, on every input, malformed or not -- so
// this test does not, and cannot, exercise the actual moved-into-try
// construction. What it DOES prove: a byte sequence invalid as UTF-8
// flowing into loadMaterial()'s argument, on a device-free instance,
// still comes back as a plain documented RxResult, never a crash. The
// case that genuinely exercises the fixed construction line (a real
// internal_ MaterialSystem, which needs a real VkDevice) lives in
// test_api_factory.cpp instead ("loadMaterial against a real internal
// MaterialSystem rejects a malformed-UTF-8 ... module path"), which also
// states honestly why even that GPU-backed case cannot force the
// std::system_error half on Linux (path::value_type == char there; no
// narrow->wide conversion ever runs) -- that half of F3 is
// windows-cross-only and verified by inspection of the fix, not by a
// runnable test in this repo (no windows-cross GPU job exists to run one
// against, per the Stage 0 audit's own F8 observation).
TEST_CASE("loadMaterial on a device-free instance rejects a byte sequence invalid as UTF-8 with a documented "
          "error code, never throws or crashes") {
    IRxMaterialSystem* system = makeDeviceFreeSystem();

    // 0xFF/0xFE are not valid UTF-8 leading bytes; 0x80 is a lone
    // continuation byte with no leading byte before it -- all three make
    // this byte sequence invalid UTF-8 without embedding a NUL (which
    // would just truncate the C string instead of exercising this case).
    const char kMalformedUtf8Path[] = {'b', 'a', 'd', '_', static_cast<char>(0xFF), static_cast<char>(0xFE),
                                        static_cast<char>(0x80), '.', 's', 'l', 'a', 'n', 'g', '\0'};

    IRxMaterial* material = reinterpret_cast<IRxMaterial*>(static_cast<uintptr_t>(0x1));  // poisoned
    RxResult result = system->loadMaterial(kMalformedUtf8Path, &material);
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

TEST_CASE("reloadChanged on a device-free (null-internal) instance stays a safe no-op that always returns RX_OK") {
    // [Task 7] reloadChanged() now forwards to the real internal
    // MaterialSystem::reloadChanged() (D9) when internal_ is non-null --
    // see test_api_factory.cpp's own device-backed reload test for that
    // behavior. This device-free case is unchanged from Task 6: internal_
    // is null here, so this method never touches it at all and always
    // returns RX_OK, exactly like every other method that does not
    // require a real internal system.
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

// ===== rxSetLogCallback [spec Phase 4 design D23, seed 13] =================
// Process-wide, not tied to any IRxMaterialSystem instance -- every case
// below uninstalls (rxSetLogCallback(nullptr, nullptr)) before returning,
// via LogCallbackGuard, so a later TEST_CASE anywhere in this binary never
// observes a callback left over from one of these (and, since a failed
// REQUIRE unwinds via a C++ exception under doctest, the RAII guard covers
// that path too -- see its own comment).
namespace {

struct LogCallbackGuard {
    ~LogCallbackGuard() { rxSetLogCallback(nullptr, nullptr); }
};

}  // namespace

TEST_CASE("rxSetLogCallback returns RX_OK for every valid transition (install/replace/uninstall), regardless of "
          "userData") {
    LogCallbackGuard guard;

    // Uninstalling when nothing is installed is still a valid transition.
    CHECK(rxSetLogCallback(nullptr, nullptr) == RX_OK);

    RxLogCallback noop = [](RxLogSeverity, const char*, const char*, void*) {};
    CHECK(rxSetLogCallback(noop, nullptr) == RX_OK);

    int sentinel = 0;
    CHECK(rxSetLogCallback(noop, &sentinel) == RX_OK);  // replace with new userData

    CHECK(rxSetLogCallback(nullptr, &sentinel) == RX_OK);  // uninstall -- userData irrelevant when cb is null
}

TEST_CASE("rxSetLogCallback installs a process-wide callback that observes a real rx_material log record "
          "(severity/category/message), and nullptr uninstalls it") {
    struct Captured {
        std::mutex mutex;
        RxLogSeverity severity = -1;
        std::string category;
        std::string message;
        int callCount = 0;
    } captured;

    RxLogCallback callback = [](RxLogSeverity severity, const char* category, const char* message, void* userData) {
        auto* c = static_cast<Captured*>(userData);
        std::lock_guard<std::mutex> lock(c->mutex);
        c->severity = severity;
        c->category = category != nullptr ? category : "";
        c->message = message != nullptr ? message : "";
        c->callCount++;
    };

    CHECK(rxSetLogCallback(callback, &captured) == RX_OK);
    LogCallbackGuard guard;

    // Device-free loadMaterial() logs a real RX_LOG_ERROR through this
    // exact boundary (api_impl.cpp's own loadMaterial() -- see the
    // "device-free" test above this section) -- an authentic trigger,
    // not a synthetic log call made just for this test.
    IRxMaterialSystem* system = makeDeviceFreeSystem();
    IRxMaterial* material = nullptr;
    CHECK(system->loadMaterial("does/not/matter.slang", &material) == RX_E_FAIL);

    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        CHECK(captured.callCount >= 1);
        CHECK(captured.severity == RX_LOG_ERROR);
        CHECK(captured.category.empty());  // rx_core's RX_LOG_* macros use the unnamed default logger.
        CHECK(captured.message.find("loadMaterial") != std::string::npos);
        captured.callCount = 0;
    }

    // Uninstall: the next log record through this same boundary is no
    // longer delivered.
    CHECK(rxSetLogCallback(nullptr, nullptr) == RX_OK);
    CHECK(system->loadMaterial("does/not/matter.slang", &material) == RX_E_FAIL);
    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        CHECK(captured.callCount == 0);
    }

    system->release();
}

// [Fix round 1, task-5-review.md item (c)] The one documented misuse:
// calling rxSetLogCallback() from inside the currently-installed
// callback's own invocation. Must be rejected with RX_E_FAIL, never
// deadlock -- this test's own completion is part of that proof, mirroring
// rx_core's own mechanism-level test (log_test.cpp) at the ABI layer.
TEST_CASE("rxSetLogCallback rejects (RX_E_FAIL, does not deadlock) a call made from inside the currently-installed "
          "callback's own invocation") {
    struct Captured {
        std::mutex mutex;
        int callCount = 0;
        RxResult selfCallResult = RX_OK;
    } captured;

    RxLogCallback selfCallingCallback = [](RxLogSeverity, const char*, const char*, void* userData) {
        auto* c = static_cast<Captured*>(userData);
        RxResult result = rxSetLogCallback(nullptr, nullptr);  // called from inside itself -- the forbidden case.
        std::lock_guard<std::mutex> lock(c->mutex);
        c->callCount++;
        c->selfCallResult = result;
    };

    CHECK(rxSetLogCallback(selfCallingCallback, &captured) == RX_OK);
    LogCallbackGuard guard;

    IRxMaterialSystem* system = makeDeviceFreeSystem();
    IRxMaterial* material = nullptr;
    CHECK(system->loadMaterial("does/not/matter.slang", &material) == RX_E_FAIL);  // triggers the callback above.

    {
        std::lock_guard<std::mutex> lock(captured.mutex);
        CHECK(captured.callCount == 1);
        CHECK(captured.selfCallResult == RX_E_FAIL);
    }

    system->release();
}
