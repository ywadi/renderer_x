#include <rx_material/rx_api.h>

#include <rx_material/material_system.h>
#include <rx_material/rx_api_detail.h>

#include <rx_core/log.h>
#include <rx_core/log_forward_sink.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// api_impl.cpp -- the renderer-side implementation behind rx_api.h's
// COM-lite interfaces [spec Phase 3 design, D5]. Every public entry point
// here (the extern "C" factory, and every virtual method reachable from
// a caller holding only an IRx*-typed pointer) follows the same
// exception-boundary contract: internal C++ exceptions (thrown by
// rx::material::MaterialSystem, or by `new`) are caught HERE and mapped to
// an RxResult -- never allowed to propagate across the ABI [R:M§1.3 point
// 2]. Diagnostics that would be useful to a developer (Slang's own
// compile/link error text) are logged via spdlog on this side, matching
// the brief's "no strings across the ABI in Phase 3" decision.
//
// [Task 7] This file no longer drives Slang directly at all -- Task 6's
// SECOND, throwaway reflection-only Slang session (reflectMaterialParams(),
// classifyFieldType(), a private slang::IGlobalSession member on
// MaterialSystemImpl) is gone. Name/type/offset/size validation now reads
// straight from rx::material::MaterialSystem::materialParams()/
// paramBlockSize() -- the SAME already-linked program that produced the
// material's real SPIR-V (material_system.cpp's reflectMaterialLayout()),
// computed once at loadMaterial()/reloadChanged() time, not re-derived
// here. This closes the exact duplicate-session seam task-6-report.md
// flagged under "What Task 7 must pick up," item 2.
//
// setFloat/setFloat4/setTexture now write directly into a real, correctly
// laid-out CPU-side byte blob (MaterialInstanceImpl::blob_, sized to
// paramBlockSize() at construction) at each field's reflected byte offset,
// rather than a name-keyed map of tagged unions -- that blob is exactly
// what rx::material::MaterialSystem::bindInstance() (an internal, non-ABI
// API -- see material_system.h's own comment; draw submission is not part
// of the public surface this phase, D5/D11) copies into a real GPU-visible
// uniform buffer at record time. rx_api_detail.h's materialHandle()/
// materialInstanceBlobData()/materialInstanceBlobSize() are the bridge a
// caller already holding the internal rx::material::MaterialSystem* uses
// to reach that draw-time API from an ABI-obtained IRxMaterial*/
// IRxMaterialInstance* (sample 06, Task 8).
namespace rx::material {

namespace {

// --- GUID comparison -----------------------------------------------------
// RxGuid has no padding (16 contiguous bytes: rx_api.h's own static_assert
// pins this) -- a plain memcmp of the whole struct is exactly IsEqualGUID's
// own technique (Win32 COM) and correct here for the same reason.
bool guidEquals(const RxGuid& a, const RxGuid& b) { return std::memcmp(&a, &b, sizeof(RxGuid)) == 0; }

// --- Private, ABI-invisible internal texture bridge ----------------------
// Never declared in rx_api.h, never crosses the ABI as a documented
// capability -- TextureImpl (below) is the only class that ever answers to
// this GUID via queryInterface(). Exists purely so
// MaterialInstanceImpl::setTexture() can recover a real, engine-created
// texture's bindless-table index from a bare IRxTexture* argument using
// the SAME queryInterface()+GUID mechanism this whole ABI already relies
// on for identity/casting [D5 rule: "identity/casting is queryInterface()
// + GUID comparison"], rather than reaching for RTTI (dynamic_cast/typeid)
// -- which D5 explicitly forbids anywhere on this boundary's own object
// model -- to distinguish "a real TextureImpl" from "some other IRxTexture
// implementation" (e.g. a test-only double). A texture that does not
// answer to this GUID (anything not created via
// IRxMaterialSystem::createTexture2D()) has no real bindless registration
// to report at all -- setTexture() REJECTS it outright with
// RX_E_INVALIDARG (see that method's own comment) [Fix round 1,
// task-7-review.md F1: an earlier version of this file instead fell back
// to writing bindless index 0 for it, a real, arbitrary slot -- silent
// wrong-texture binding with zero diagnostic signal, on exactly the axis
// ("errors are RxResult codes, never silent wrong behavior") this
// codebase otherwise holds to rigorously. rx_api.h's own IRxTexture doc
// comment already documents that a valid IRxTexture "always" wraps an
// engine-created texture, so rejecting anything that fails this check
// enforces a contract the header already claims, not a new restriction].
// test_api_factory.cpp's Task 6 FakeTexture-based tests were adapted
// (not deleted) to assert this rejection directly; the refcount/lifecycle
// coverage those tests used to provide via FakeTexture now lives in a
// dedicated real-texture (IRxMaterialSystem::createTexture2D()-backed)
// test instead, since a rejected texture never reaches the refcount-
// mutating code path at all.
//
// Deliberately NOT IRxUnknown-rooted (no addRef/release of its own): this
// is a same-lifetime "peek" at data TextureImpl itself already owns, never
// a separately refcounted sub-object, so there is nothing for a caller to
// release. TextureImpl::queryInterface() below does NOT addRef() when
// answering THIS ONE GUID specifically (documented there too) -- the one
// deliberate exception to this file's otherwise-universal "every
// successful queryInterface() addRefs" rule, safe because the returned
// pointer's use is confined to the single expression that calls
// bindlessIndex() and is never stored or released.
struct IInternalTextureBridge {
    virtual uint32_t bindlessIndex() = 0;
};
constexpr RxGuid kIID_InternalTextureBridge = {
    0x4d4b392c, 0xc3ee, 0x4272, {0xb1, 0x24, 0x4f, 0x45, 0x47, 0xb4, 0x0a, 0x94}};

// --- Live-object bookkeeping (test seam) ----------------------------------
std::atomic<uint64_t> g_liveApiObjectCount{0};

// --- COM-lite root plumbing shared by every concrete object below --------
// CRTP, not a virtual base: `Interface` (IRxMaterialSystem, IRxMaterial,
// IRxMaterialInstance, IRxTexture) is the ONE IRxUnknown-rooted interface
// each concrete type below publicly, singly inherits -- RxUnknownBase sits
// between them in the C++ class hierarchy purely to share addRef()/
// release()'s identical bodies, which does not change the ABI-facing
// vtable's slot order/count at all (the consumer only ever holds an
// Interface*-typed pointer and only ever calls through the slots ITS OWN
// pure-virtual declarations specify, which RxUnknownBase overrides in
// place, adding none). No class in this entire hierarchy declares a
// virtual destructor (see release() below: it always deletes through the
// exact static Derived* type, so virtual dispatch to a destructor is
// never needed) -- consistent with the "no virtual destructor" ABI rule
// [R:M§1.3 point 1] applying transitively to every object that could ever
// be reached only through an interface pointer. TextureImpl additionally,
// separately inherits IInternalTextureBridge (above) -- a second,
// disjoint-vtable base with no virtual-function-name overlap with
// IRxUnknown at all, so this stays ordinary, unambiguous multiple
// inheritance with no diamond/refcount-sharing complexity to resolve.
template <typename Derived, typename Interface>
class RxUnknownBase : public Interface {
public:
    RxUnknownBase() { g_liveApiObjectCount.fetch_add(1, std::memory_order_relaxed); }

    uint32_t RX_CALL addRef() override { return refCount_.fetch_add(1, std::memory_order_relaxed) + 1; }

    uint32_t RX_CALL release() override {
        uint32_t remaining = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete static_cast<Derived*>(this);
        }
        return remaining;
    }

protected:
    ~RxUnknownBase() { g_liveApiObjectCount.fetch_sub(1, std::memory_order_relaxed); }

    // COM convention: a freshly constructed object's first reference is
    // already owned by whoever received the pointer (a factory/
    // createInstance/queryInterface out-param) -- starts at 1, not 0.
    std::atomic<uint32_t> refCount_{1};
};

// Forward declarations -- these four reference each other's pointers
// before each is fully declared (a real mutual dependency: instances own a
// ref to their material, materials own a ref to their system, textures own
// a ref to their system, and the system constructs materials/textures,
// which construct instances). Every method whose body needs another one of
// these types COMPLETE is declared here and DEFINED out-of-line, after all
// four classes are fully declared below.
class MaterialSystemImpl;
class MaterialImpl;
class MaterialInstanceImpl;
class TextureImpl;

// --- IRxMaterialInstance -----------------------------------------------
class MaterialInstanceImpl final : public RxUnknownBase<MaterialInstanceImpl, IRxMaterialInstance> {
public:
    explicit MaterialInstanceImpl(MaterialImpl* material);
    ~MaterialInstanceImpl();

    RxResult RX_CALL queryInterface(const RxGuid& iid, void** outObject) override;
    RxResult RX_CALL setFloat(const char* name, float value) override;
    RxResult RX_CALL setFloat4(const char* name, const float value[4]) override;
    RxResult RX_CALL setTexture(const char* name, IRxTexture* texture) override;

    // Internal (non-ABI) accessors -- see rx_api_detail.h's own comment on
    // exactly who consumes these and why (the draw-time bindInstance()
    // bridge, not a test-only seam).
    [[nodiscard]] const std::vector<uint8_t>& blob() const { return blob_; }

private:
    MaterialImpl* material_;
    std::vector<uint8_t> blob_;
    // Bindless-index (TextureIndex) fields' IRxTexture references, keyed
    // by param name -- addRef'd while stored here, released on overwrite
    // and on this instance's own destruction. The BYTES this engine
    // actually binds to the GPU live in `blob_` (the texture's own
    // bindless index, a plain u32, per D8); this map exists purely for
    // COM refcount lifecycle bookkeeping, not for GPU binding.
    std::unordered_map<std::string, IRxTexture*> textures_;
};

// --- IRxMaterial ----------------------------------------------------------
class MaterialImpl final : public RxUnknownBase<MaterialImpl, IRxMaterial> {
public:
    MaterialImpl(MaterialSystemImpl* system, rx::material::MaterialSystem* internalSystem,
                 rx::material::MaterialHandle handle, std::string name);
    ~MaterialImpl();

    RxResult RX_CALL queryInterface(const RxGuid& iid, void** outObject) override;
    RxResult RX_CALL createInstance(IRxMaterialInstance** outInstance) override;
    const char* RX_CALL name() override;

    // Internal accessors (not part of the ABI): the reflected shape of
    // parameter `paramName` (nullopt if this material has no such
    // reflected parameter at all), and the raw internal system/handle a
    // MaterialInstanceImpl (and rx_api_detail.h's bridge) need to size a
    // blob / drive bindInstance().
    [[nodiscard]] std::optional<rx::material::MaterialParamInfo> paramInfo(const std::string& paramName) const;
    [[nodiscard]] rx::material::MaterialSystem* internalSystem() const { return internalSystem_; }
    [[nodiscard]] rx::material::MaterialHandle handle() const { return handle_; }

private:
    MaterialSystemImpl* system_;
    rx::material::MaterialSystem* internalSystem_;
    rx::material::MaterialHandle handle_;
    std::string name_;
};

// --- IRxTexture ------------------------------------------------------------
class TextureImpl final : public RxUnknownBase<TextureImpl, IRxTexture>, public IInternalTextureBridge {
public:
    TextureImpl(MaterialSystemImpl* system, rx::material::MaterialSystem* internalSystem,
                rx::material::TextureHandle handle);
    ~TextureImpl();

    RxResult RX_CALL queryInterface(const RxGuid& iid, void** outObject) override;
    uint32_t bindlessIndex() override;

private:
    MaterialSystemImpl* system_;
    rx::material::MaterialSystem* internalSystem_;
    rx::material::TextureHandle handle_;
};

// --- IRxMaterialSystem ------------------------------------------------
class MaterialSystemImpl final : public RxUnknownBase<MaterialSystemImpl, IRxMaterialSystem> {
public:
    explicit MaterialSystemImpl(rx::material::MaterialSystem* internalSystem) : internal_(internalSystem) {}

    RxResult RX_CALL queryInterface(const RxGuid& iid, void** outObject) override;
    RxResult RX_CALL loadMaterial(const char* slangModulePath, IRxMaterial** outMaterial) override;
    RxResult RX_CALL reloadChanged() override;
    RxResult RX_CALL createTexture2D(const RxTextureDesc* desc, IRxTexture** outTexture) override;

private:
    rx::material::MaterialSystem* internal_;  // non-owning; may be null (see rx_api.h's own comment on
                                               // RxMaterialSystemDesc for why that is a legal, documented state).
};

// ===== Out-of-line definitions (every referenced type is complete now) ====

// --- MaterialInstanceImpl -------------------------------------------------
MaterialInstanceImpl::MaterialInstanceImpl(MaterialImpl* material) : material_(material) {
    material_->addRef();
    // Sized once, here, to the material's own fixed paramBlockSize() --
    // never resized afterward (every setFloat/setFloat4/setTexture call
    // below writes IN PLACE at a fixed reflected offset, it never grows
    // this vector). Zero-initialized: an instance that never calls a
    // setter for a given field still has well-defined (all-zero) bytes at
    // that field's offset when bindInstance() eventually copies this blob
    // to the GPU.
    blob_.assign(material_->internalSystem()->paramBlockSize(material_->handle()), 0);
}

MaterialInstanceImpl::~MaterialInstanceImpl() {
    for (auto& [paramName, texture] : textures_) {
        (void)paramName;
        if (texture != nullptr) {
            texture->release();
        }
    }
    material_->release();
}

RxResult RX_CALL MaterialInstanceImpl::queryInterface(const RxGuid& iid, void** outObject) {
    if (outObject == nullptr) {
        return RX_E_INVALIDARG;
    }
    if (guidEquals(iid, kIID_IRxUnknown) || guidEquals(iid, kIID_IRxMaterialInstance)) {
        addRef();
        *outObject = static_cast<IRxMaterialInstance*>(this);
        return RX_OK;
    }
    *outObject = nullptr;
    return RX_E_NOINTERFACE;
}

RxResult RX_CALL MaterialInstanceImpl::setFloat(const char* name, float value) {
    if (name == nullptr) {
        return RX_E_INVALIDARG;
    }
    try {
        std::optional<rx::material::MaterialParamInfo> info = material_->paramInfo(name);
        if (!info.has_value()) {
            return RX_E_NOTFOUND;
        }
        if (info->kind != rx::material::MaterialParamKind::Float) {
            return RX_E_INVALIDARG;
        }
        if (static_cast<size_t>(info->offset) + sizeof(float) > blob_.size()) {
            RX_LOG_ERROR(
                "rx_material: rx_api: MaterialInstanceImpl::setFloat('{}'): reflected offset {} + {} bytes "
                "exceeds this instance's blob size {} -- refusing to write out of bounds",
                name, info->offset, sizeof(float), blob_.size());
            return RX_E_FAIL;
        }
        std::memcpy(blob_.data() + info->offset, &value, sizeof(float));
        return RX_OK;
    } catch (const std::exception& e) {
        RX_LOG_ERROR("rx_material: rx_api: MaterialInstanceImpl::setFloat('{}') failed: {}", name, e.what());
        return RX_E_FAIL;
    }
}

RxResult RX_CALL MaterialInstanceImpl::setFloat4(const char* name, const float value[4]) {
    if (name == nullptr || value == nullptr) {
        return RX_E_INVALIDARG;
    }
    try {
        std::optional<rx::material::MaterialParamInfo> info = material_->paramInfo(name);
        if (!info.has_value()) {
            return RX_E_NOTFOUND;
        }
        if (info->kind != rx::material::MaterialParamKind::Float4) {
            return RX_E_INVALIDARG;
        }
        if (static_cast<size_t>(info->offset) + sizeof(float) * 4 > blob_.size()) {
            RX_LOG_ERROR(
                "rx_material: rx_api: MaterialInstanceImpl::setFloat4('{}'): reflected offset {} + {} bytes "
                "exceeds this instance's blob size {} -- refusing to write out of bounds",
                name, info->offset, sizeof(float) * 4, blob_.size());
            return RX_E_FAIL;
        }
        std::memcpy(blob_.data() + info->offset, value, sizeof(float) * 4);
        return RX_OK;
    } catch (const std::exception& e) {
        RX_LOG_ERROR("rx_material: rx_api: MaterialInstanceImpl::setFloat4('{}') failed: {}", name, e.what());
        return RX_E_FAIL;
    }
}

RxResult RX_CALL MaterialInstanceImpl::setTexture(const char* name, IRxTexture* texture) {
    if (name == nullptr || texture == nullptr) {
        return RX_E_INVALIDARG;
    }
    try {
        std::optional<rx::material::MaterialParamInfo> info = material_->paramInfo(name);
        if (!info.has_value()) {
            return RX_E_NOTFOUND;
        }
        if (info->kind != rx::material::MaterialParamKind::TextureIndex) {
            return RX_E_INVALIDARG;
        }
        if (static_cast<size_t>(info->offset) + sizeof(uint32_t) > blob_.size()) {
            RX_LOG_ERROR(
                "rx_material: rx_api: MaterialInstanceImpl::setTexture('{}'): reflected offset {} + {} bytes "
                "exceeds this instance's blob size {} -- refusing to write out of bounds",
                name, info->offset, sizeof(uint32_t), blob_.size());
            return RX_E_FAIL;
        }

        // [Fix round 1, task-7-review.md F1] `texture` must be a real,
        // engine-created texture (IRxMaterialSystem::createTexture2D()) --
        // recovered via the private queryInterface bridge (see
        // kIID_InternalTextureBridge's own comment). Anything that fails
        // this QI (a hand-rolled test double, a hypothetical third-party
        // IRxTexture implementation, ...) is REJECTED outright: there is
        // no real bindless index to report for it, and silently binding
        // index 0 -- a real, arbitrary slot -- would be a debugging-
        // nightmare failure mode with zero diagnostic signal. This
        // enforces the invariant rx_api.h's own IRxTexture/setTexture doc
        // comments already document: a valid `texture` argument always
        // wraps an engine-created resource.
        void* bridgeOut = nullptr;
        if (texture->queryInterface(kIID_InternalTextureBridge, &bridgeOut) != RX_OK || bridgeOut == nullptr) {
            RX_LOG_ERROR(
                "rx_material: rx_api: MaterialInstanceImpl::setTexture('{}'): `texture` was not created via "
                "IRxMaterialSystem::createTexture2D() (failed the internal engine-texture check) -- rejecting",
                name);
            return RX_E_INVALIDARG;
        }
        uint32_t bindlessIndex = static_cast<IInternalTextureBridge*>(bridgeOut)->bindlessIndex();

        // [Fix round 1, task-6-review.md F1 -- ordering preserved] capture
        // whatever this name previously held, perform the (possibly-
        // throwing) map write FIRST, and only THEN touch the blob bytes
        // (which cannot throw) and refcounts. If `textures_[name] = texture`
        // throws (bad_alloc during a rehash), nothing else has been
        // mutated yet -- no leaked reference, no partially-written blob.
        IRxTexture* previousTexture = nullptr;
        auto it = textures_.find(name);
        if (it != textures_.end()) {
            previousTexture = it->second;
        }

        textures_[name] = texture;

        std::memcpy(blob_.data() + info->offset, &bindlessIndex, sizeof(uint32_t));

        texture->addRef();
        if (previousTexture != nullptr) {
            previousTexture->release();
        }
        return RX_OK;
    } catch (const std::exception& e) {
        RX_LOG_ERROR("rx_material: rx_api: MaterialInstanceImpl::setTexture('{}') failed: {}", name, e.what());
        return RX_E_FAIL;
    }
}

// --- MaterialImpl ----------------------------------------------------------
MaterialImpl::MaterialImpl(MaterialSystemImpl* system, rx::material::MaterialSystem* internalSystem,
                             rx::material::MaterialHandle handle, std::string name)
    : system_(system), internalSystem_(internalSystem), handle_(handle), name_(std::move(name)) {
    system_->addRef();
}

MaterialImpl::~MaterialImpl() { system_->release(); }

RxResult RX_CALL MaterialImpl::queryInterface(const RxGuid& iid, void** outObject) {
    if (outObject == nullptr) {
        return RX_E_INVALIDARG;
    }
    if (guidEquals(iid, kIID_IRxUnknown) || guidEquals(iid, kIID_IRxMaterial)) {
        addRef();
        *outObject = static_cast<IRxMaterial*>(this);
        return RX_OK;
    }
    *outObject = nullptr;
    return RX_E_NOINTERFACE;
}

RxResult RX_CALL MaterialImpl::createInstance(IRxMaterialInstance** outInstance) {
    if (outInstance == nullptr) {
        return RX_E_INVALIDARG;
    }
    *outInstance = nullptr;
    try {
        auto* instance = new MaterialInstanceImpl(this);
        *outInstance = instance;
        return RX_OK;
    } catch (const std::exception& e) {
        RX_LOG_ERROR("rx_material: rx_api: MaterialImpl::createInstance failed: {}", e.what());
        return RX_E_FAIL;
    }
}

const char* RX_CALL MaterialImpl::name() { return name_.c_str(); }

std::optional<rx::material::MaterialParamInfo> MaterialImpl::paramInfo(const std::string& paramName) const {
    const std::vector<rx::material::MaterialParamInfo>& params = internalSystem_->materialParams(handle_);
    for (const rx::material::MaterialParamInfo& param : params) {
        if (param.name == paramName) {
            return param;
        }
    }
    return std::nullopt;
}

// --- TextureImpl -----------------------------------------------------------
TextureImpl::TextureImpl(MaterialSystemImpl* system, rx::material::MaterialSystem* internalSystem,
                          rx::material::TextureHandle handle)
    : system_(system), internalSystem_(internalSystem), handle_(handle) {
    system_->addRef();
}

TextureImpl::~TextureImpl() {
    // Per bindless.h's own release-safety contract and
    // MaterialSystem::releaseTexture()'s own comment: this only defers the
    // real GPU-object teardown (through this MaterialSystem's internal
    // DeletionQueue) -- never throws, never blocks.
    internalSystem_->releaseTexture(handle_);
    system_->release();
}

RxResult RX_CALL TextureImpl::queryInterface(const RxGuid& iid, void** outObject) {
    if (outObject == nullptr) {
        return RX_E_INVALIDARG;
    }
    if (guidEquals(iid, kIID_IRxUnknown) || guidEquals(iid, kIID_IRxTexture)) {
        addRef();
        *outObject = static_cast<IRxTexture*>(this);
        return RX_OK;
    }
    if (guidEquals(iid, kIID_InternalTextureBridge)) {
        // Deliberately NO addRef() -- see kIID_InternalTextureBridge's own
        // comment for why this one GUID is the sole exception to this
        // file's otherwise-universal "every successful queryInterface()
        // addRefs" rule.
        *outObject = static_cast<IInternalTextureBridge*>(this);
        return RX_OK;
    }
    *outObject = nullptr;
    return RX_E_NOINTERFACE;
}

uint32_t TextureImpl::bindlessIndex() { return internalSystem_->textureBindlessIndex(handle_); }

// --- MaterialSystemImpl ------------------------------------------------
RxResult RX_CALL MaterialSystemImpl::queryInterface(const RxGuid& iid, void** outObject) {
    if (outObject == nullptr) {
        return RX_E_INVALIDARG;
    }
    if (guidEquals(iid, kIID_IRxUnknown) || guidEquals(iid, kIID_IRxMaterialSystem)) {
        addRef();
        *outObject = static_cast<IRxMaterialSystem*>(this);
        return RX_OK;
    }
    *outObject = nullptr;
    return RX_E_NOINTERFACE;
}

RxResult RX_CALL MaterialSystemImpl::loadMaterial(const char* slangModulePath, IRxMaterial** outMaterial) {
    if (outMaterial == nullptr) {
        return RX_E_INVALIDARG;
    }
    *outMaterial = nullptr;
    if (slangModulePath == nullptr) {
        return RX_E_INVALIDARG;
    }
    if (internal_ == nullptr) {
        RX_LOG_ERROR(
            "rx_material: rx_api: loadMaterial called on an IRxMaterialSystem created with a null "
            "internalMaterialSystem (a device-free/QI-only instance -- see rx_api.h's own comment on "
            "RxMaterialSystemDesc)");
        return RX_E_FAIL;
    }

    std::filesystem::path path(slangModulePath);
    try {
        // [Task 7] No more separate reflection-only pass to run first --
        // MaterialSystem::loadMaterial() itself now computes every field's
        // name/kind/offset/size from the SAME already-linked program that
        // produces its real SPIR-V, and (per Task 5's own already-correct
        // exception safety, unchanged here) never registers a
        // MaterialRecord at all if it throws -- so there is nothing this
        // layer could orphan by calling it directly.
        rx::material::MaterialHandle handle = internal_->loadMaterial(path);
        auto* material = new MaterialImpl(this, internal_, handle, path.stem().string());
        *outMaterial = material;
        return RX_OK;
    } catch (const std::exception& e) {
        RX_LOG_ERROR("rx_material: rx_api: loadMaterial('{}') failed: {}", path.string(), e.what());
        return RX_E_COMPILE;
    }
}

RxResult RX_CALL MaterialSystemImpl::reloadChanged() {
    // [Task 7] Wired to the real internal MaterialSystem::reloadChanged()
    // (D9) -- see rx_api.h's own updated comment on this method. Never
    // touches `internal_` when it is null, so this stays valid to call on
    // a device-free/QI-only instance. reloadChanged() itself keeps-last-
    // good on any per-material failure (logged there, not surfaced here)
    // and cannot throw for that reason; the try/catch below is defensive
    // only, for a genuine allocation failure or similar -- this method's
    // own documented contract is "always returns RX_OK".
    if (internal_ != nullptr) {
        try {
            internal_->reloadChanged();
        } catch (const std::exception& e) {
            RX_LOG_ERROR("rx_material: rx_api: reloadChanged failed unexpectedly: {}", e.what());
        }
    }
    return RX_OK;
}

RxResult RX_CALL MaterialSystemImpl::createTexture2D(const RxTextureDesc* desc, IRxTexture** outTexture) {
    if (outTexture == nullptr) {
        return RX_E_INVALIDARG;
    }
    *outTexture = nullptr;
    if (desc == nullptr) {
        return RX_E_INVALIDARG;
    }
    if (desc->width == 0 || desc->height == 0) {
        return RX_E_INVALIDARG;
    }
    if (desc->pixels == nullptr || desc->pixelBytes == 0) {
        return RX_E_INVALIDARG;
    }

    VkFormat vkFormat;
    if (desc->format == RX_FORMAT_RGBA8_UNORM) {
        vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
    } else if (desc->format == RX_FORMAT_RGBA8_SRGB) {
        vkFormat = VK_FORMAT_R8G8B8A8_SRGB;
    } else {
        return RX_E_INVALIDARG;
    }

    if (internal_ == nullptr) {
        RX_LOG_ERROR(
            "rx_material: rx_api: createTexture2D called on an IRxMaterialSystem created with a null "
            "internalMaterialSystem (a device-free/QI-only instance)");
        return RX_E_FAIL;
    }

    try {
        rx::material::TextureCreateInfo info;
        info.width = desc->width;
        info.height = desc->height;
        info.format = vkFormat;
        info.generateMips = desc->generateMips != 0;
        info.pixels = desc->pixels;
        info.pixelBytes = static_cast<size_t>(desc->pixelBytes);

        rx::material::TextureHandle handle = internal_->createTexture2D(info);
        auto* texture = new TextureImpl(this, internal_, handle);
        *outTexture = texture;
        return RX_OK;
    } catch (const std::exception& e) {
        RX_LOG_ERROR("rx_material: rx_api: createTexture2D failed: {}", e.what());
        return RX_E_FAIL;
    }
}

}  // namespace

namespace detail {

uint64_t debugLiveApiObjectCount() { return g_liveApiObjectCount.load(std::memory_order_relaxed); }

rx::material::MaterialHandle materialHandle(IRxMaterial* material) {
    return static_cast<MaterialImpl*>(material)->handle();
}

const void* materialInstanceBlobData(IRxMaterialInstance* instance) {
    return static_cast<MaterialInstanceImpl*>(instance)->blob().data();
}

size_t materialInstanceBlobSize(IRxMaterialInstance* instance) {
    return static_cast<MaterialInstanceImpl*>(instance)->blob().size();
}

}  // namespace detail

extern "C" RxResult rxCreateMaterialSystem(const RxMaterialSystemDesc* desc, IRxMaterialSystem** outSystem) {
    if (outSystem == nullptr) {
        return RX_E_INVALIDARG;
    }
    *outSystem = nullptr;
    if (desc == nullptr) {
        return RX_E_INVALIDARG;
    }
    try {
        auto* internal = static_cast<rx::material::MaterialSystem*>(desc->internalMaterialSystem);
        *outSystem = new MaterialSystemImpl(internal);
        return RX_OK;
    } catch (const std::exception& e) {
        RX_LOG_ERROR("rx_material: rx_api: rxCreateMaterialSystem failed: {}", e.what());
        return RX_E_FAIL;
    }
}

// rxSetLogCallback() [spec Phase 4 design D23, seed 13] -- process-wide,
// not tied to any IRxMaterialSystem instance (unlike everything else in
// this file), so it lives here as a second, independent extern "C" entry
// point rather than a method on any interface above. init() is called
// defensively first (matching every other rx_core-logging entry point in
// this codebase, e.g. MaterialSystem::create()/Compiler::create()) so
// this is safe to call before any device/material system has been
// created at all -- log::init()'s own call_once makes repeat calls free.
//
// RxLogCallback's signature (RxLogSeverity, const char*, const char*,
// void*) is the SAME C++ type as rx_core::log::ForwardCallback's
// (int32_t, const char*, const char*, void*) after typedef resolution --
// RxLogSeverity IS int32_t (a plain alias, not a distinct enum type; see
// rx_api.h's own comment on it), so `cb` converts to ForwardCallback with
// no cast, no calling-convention change, and no UB: they are not merely
// ABI-compatible, they are literally the identical function pointer type.
//
// [Fix round 1, task-5-review.md Medium] Wrapped in the same catch-all
// every other RxResult-returning entry point in this file already uses
// (docs/abi.md's unconditional "never throw across the boundary" rule) --
// this was previously the one exception to that pattern in this file.
// LogForwardSink::set() itself only returns `false` for exactly one
// documented misuse (see rx_api.h's own (c)) -- mapped to RX_E_FAIL here,
// matching this file's existing "narrow, documented rejection -> specific
// RxResult" convention elsewhere (e.g. setTexture()'s RX_E_INVALIDARG for
// a non-engine texture).
extern "C" RxResult rxSetLogCallback(RxLogCallback cb, void* userData) {
    try {
        rx::core::log::init();
        if (!rx::core::log::forwardSink()->set(cb, userData)) {
            RX_LOG_ERROR(
                "rx_material: rx_api: rxSetLogCallback called from inside the currently-installed callback's own "
                "invocation (directly or indirectly) -- rejected, no state changed (see rx_api.h's own (c): this "
                "would otherwise self-deadlock)");
            return RX_E_FAIL;
        }
        return RX_OK;
    } catch (const std::exception& e) {
        RX_LOG_ERROR("rx_material: rx_api: rxSetLogCallback failed: {}", e.what());
        return RX_E_FAIL;
    }
}

}  // namespace rx::material
