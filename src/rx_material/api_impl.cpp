#include <rx_material/rx_api.h>

#include <rx_material/material_system.h>
#include <rx_material/rx_api_detail.h>

#include <rx_core/log.h>

#include <slang-com-ptr.h>
#include <slang.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// api_impl.cpp -- the renderer-side implementation behind rx_api.h's
// COM-lite interfaces [spec Phase 3 design, D5]. Every public entry point
// here (the extern "C" factory, and every virtual method reachable from
// a caller holding only an IRx*-typed pointer) follows the same
// exception-boundary contract: internal C++ exceptions (thrown by
// rx::material::MaterialSystem, by Slang's own C++ wrapper on OOM, or by
// `new`) are caught HERE and mapped to an RxResult -- never allowed to
// propagate across the ABI [R:M§1.3 point 2]. Diagnostics that would be
// useful to a developer (Slang's own compile/link error text) are logged
// via spdlog on this side, matching the brief's "no strings across the
// ABI in Phase 3" decision.
//
// This file also implements IRxMaterialInstance's parameter-name/type
// validation using a SECOND, independent, reflection-only Slang session
// (see reflectMaterialParams() below) -- not because MaterialSystem
// (material_system.h) is missing this by oversight, but because Task 5's
// scope for that class stopped at binding-level reflection (one
// UNIFORM_BUFFER binding for the whole `ParameterBlock<TParams> gParams`,
// no per-field name/type breakdown -- see material_system.cpp's own
// reflectMaterialLayout()) and Task 6's Modify list does not include
// material_system.h/.cpp. Rather than fabricate a name/type table or
// hand-parse Slang source text (this project's own policy: prefer an
// already-integrated library's real API over reinventing a parser), this
// runs Slang's OWN reflection API a second time, scoped to exactly the
// question this ABI layer needs answered ("what are gParams's field
// names and shapes"), stopping before codegen/linking (composite->
// getLayout() works on an unlinked IComponentType -- verified directly
// against this project's shipped Slang build before writing this, same
// as every other empirical Slang behavior this codebase documents rather
// than assumes). This is real, measurable duplicate work (a second Slang
// session per loadMaterial() call, on top of MaterialSystem's own) --
// flagged explicitly in task-6-report.md as something Task 7 should
// collapse by having MaterialSystem itself expose a per-field parameter
// table computed from the ALREADY-linked program it retains, once that
// task is touching material_system.h anyway for GPU-binding.
namespace rx::material {

namespace {

// --- GUID comparison -----------------------------------------------------
// RxGuid has no padding (16 contiguous bytes: rx_api.h's own static_assert
// pins this) -- a plain memcmp of the whole struct is exactly IsEqualGUID's
// own technique (Win32 COM) and correct here for the same reason.
bool guidEquals(const RxGuid& a, const RxGuid& b) { return std::memcmp(&a, &b, sizeof(RxGuid)) == 0; }

// --- Reflected material parameter shapes ---------------------------------
// The only shapes IRxMaterialInstance's three setters can ever bind:
// Float (setFloat), Float4 (setFloat4), TextureIndex (setTexture -- a
// plain `uint` field per this engine's own D8 convention: bindless
// texture indices are plain data inside TParams, never a resource-typed
// field -- see material_system.cpp's own comment on gParams for the
// exact citation). Unsupported covers every other real field shape
// (float3, matrices, ints, ...) this ABI has no setter for yet: a lookup
// that finds an Unsupported field is a real, named parameter with the
// WRONG type for whichever setter was called (RX_E_INVALIDARG), not an
// unknown one (RX_E_NOTFOUND).
enum class ParamKind : uint8_t { Float, Float4, TextureIndex, Unsupported };

struct ReflectedParam {
    std::string name;
    ParamKind kind;
};

std::optional<std::string> readFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    if (!file.good() && !file.eof()) {
        return std::nullopt;
    }
    return contents.str();
}

ParamKind classifyFieldType(slang::TypeLayoutReflection* typeLayout) {
    if (typeLayout == nullptr) {
        return ParamKind::Unsupported;
    }
    switch (typeLayout->getKind()) {
        case slang::TypeReflection::Kind::Scalar: {
            switch (typeLayout->getScalarType()) {
                case slang::TypeReflection::ScalarType::Float32:
                    return ParamKind::Float;
                case slang::TypeReflection::ScalarType::UInt32:
                    return ParamKind::TextureIndex;
                default:
                    return ParamKind::Unsupported;
            }
        }
        case slang::TypeReflection::Kind::Vector: {
            slang::TypeLayoutReflection* elementLayout = typeLayout->getElementTypeLayout();
            if (elementLayout == nullptr || typeLayout->getElementCount() != 4) {
                return ParamKind::Unsupported;
            }
            if (elementLayout->getScalarType() == slang::TypeReflection::ScalarType::Float32) {
                return ParamKind::Float4;
            }
            return ParamKind::Unsupported;
        }
        default:
            return ParamKind::Unsupported;
    }
}

// Reflection-only pass over `materialPath`'s top-level `gParams`
// ParameterBlock (see this file's own header comment for why this exists
// as a SECOND Slang session rather than extending material_system.h).
// `globalSession` is owned by the caller (MaterialSystemImpl keeps one
// alive for as long as the wrapping IRxMaterialSystem lives, paying
// Slang's real fixed cost -- its standard-library load -- once, not once
// per loadMaterial() call, mirroring MaterialSystem::Impl's own globalSession
// field for the identical reason). Returns an empty vector and leaves
// `error` empty for a material with a valid (possibly field-less)
// ParameterBlock; returns an empty vector with `error` set on any real
// reflection failure (session/module/layout construction failed) -- the
// caller treats the two cases differently (RX_OK with zero known params
// vs. RX_E_COMPILE).
std::vector<ReflectedParam> reflectMaterialParams(slang::IGlobalSession* globalSession,
                                                     const std::filesystem::path& materialPath, std::string& error) {
    slang::TargetDesc targetDesc{};
    targetDesc.format = SLANG_SPIRV;
    targetDesc.profile = globalSession->findProfile("sm_6_0");

    // [Fix round 1, task-6-review.md F3] Mirrors MaterialSystem::create()'s
    // own target setup (material_system.cpp) EXACTLY, field for field --
    // the authoritative session that will eventually govern GPU binding
    // (Task 7) and this reflection-only session must agree on every
    // TargetDesc/SessionDesc knob, not just format/profile/searchPaths,
    // so there is no configuration-driven risk of the two ever
    // classifying a field's shape differently. After this, the only
    // remaining difference between the two sessions is that they are
    // necessarily separate `slang::ISession`/`slang::IGlobalSession`
    // instances (this file cannot reach into MaterialSystem::Impl's
    // private session at all -- see this file's own top comment) --
    // structural, already-disclosed, and not a configuration divergence.
    slang::CompilerOptionEntry capabilityEntry{};
    SlangCapabilityID spirvFloor = globalSession->findCapability("spirv_1_3");
    if (spirvFloor != SLANG_CAPABILITY_UNKNOWN) {
        capabilityEntry.name = slang::CompilerOptionName::Capability;
        capabilityEntry.value.kind = slang::CompilerOptionValueKind::Int;
        capabilityEntry.value.intValue0 = static_cast<int32_t>(spirvFloor);
        targetDesc.compilerOptionEntries = &capabilityEntry;
        targetDesc.compilerOptionEntryCount = 1;
    } else {
        RX_LOG_WARN("rx_material: rx_api: Slang capability 'spirv_1_3' not found by this Slang build; reflecting "
                    "material parameters without an explicit SPIR-V version floor");
    }

    const std::string shaderDir = RX_MATERIAL_SHADER_DIR;
    const char* searchPath = shaderDir.c_str();

    slang::SessionDesc sessionDesc{};
    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;
    sessionDesc.searchPaths = &searchPath;
    sessionDesc.searchPathCount = 1;

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())) || session.get() == nullptr) {
        error = "rx_material: rx_api: failed to create a reflection-only Slang session for '" +
                materialPath.string() + "'";
        return {};
    }

    auto source = readFileBytes(materialPath);
    if (!source.has_value()) {
        error = "rx_material: rx_api: could not read material module '" + materialPath.string() +
                "' for parameter reflection";
        return {};
    }

    std::string moduleName = materialPath.stem().string();
    if (moduleName.empty()) {
        moduleName = "material";
    }

    Slang::ComPtr<slang::IBlob> sourceBlob(Slang::INIT_ATTACH, slang_createBlob(source->data(), source->size()));
    Slang::ComPtr<slang::IBlob> loadDiag;
    slang::IModule* materialModule = session->loadModuleFromSource(moduleName.c_str(), materialPath.string().c_str(),
                                                                      sourceBlob, loadDiag.writeRef());
    if (materialModule == nullptr) {
        error = "rx_material: rx_api: failed to load material module '" + materialPath.string() +
                "' for parameter reflection";
        return {};
    }

    std::vector<slang::IComponentType*> parts{materialModule};
    Slang::ComPtr<slang::IComponentType> composite;
    Slang::ComPtr<slang::IBlob> composeDiag;
    if (SLANG_FAILED(session->createCompositeComponentType(parts.data(), static_cast<SlangInt>(parts.size()),
                                                              composite.writeRef(), composeDiag.writeRef())) ||
        composite.get() == nullptr) {
        error = "rx_material: rx_api: failed to compose material module '" + materialPath.string() +
                "' for parameter reflection";
        return {};
    }

    // Deliberately composite->getLayout(), never composite->link(): this
    // pass only ever reads global-parameter TYPE/NAME information, which
    // Slang resolves during type-checking/layout assignment at composite
    // construction, not at link time -- no codegen, no entry points, no
    // forward_entry.slang involvement needed at all.
    Slang::ComPtr<slang::IBlob> layoutDiag;
    slang::ProgramLayout* layout = composite->getLayout(0, layoutDiag.writeRef());
    if (layout == nullptr) {
        error = "rx_material: rx_api: IComponentType::getLayout failed for '" + materialPath.string() +
                "' during parameter reflection";
        return {};
    }

    std::vector<ReflectedParam> params;
    unsigned paramCount = layout->getParameterCount();
    for (unsigned i = 0; i < paramCount; ++i) {
        slang::VariableLayoutReflection* param = layout->getParameterByIndex(i);
        if (param == nullptr) {
            continue;
        }
        slang::TypeLayoutReflection* paramTypeLayout = param->getTypeLayout();
        bool isParamBlock = param->getCategory() == slang::ParameterCategory::SubElementRegisterSpace &&
                             paramTypeLayout != nullptr &&
                             paramTypeLayout->getKind() == slang::TypeReflection::Kind::ParameterBlock;
        if (!isParamBlock) {
            // Not this engine's gParams -- material_system.cpp's own
            // reflectMaterialLayout() already rejects any material whose
            // top-level globals don't match this exact shape at
            // internal->loadMaterial() time, so by construction there is
            // nothing else to classify here.
            continue;
        }

        slang::TypeLayoutReflection* elementLayout = paramTypeLayout->getElementTypeLayout();
        if (elementLayout == nullptr) {
            continue;
        }
        unsigned fieldCount = elementLayout->getFieldCount();
        for (unsigned f = 0; f < fieldCount; ++f) {
            slang::VariableLayoutReflection* field = elementLayout->getFieldByIndex(f);
            if (field == nullptr) {
                continue;
            }
            const char* rawName = field->getName();
            if (rawName == nullptr) {
                continue;
            }
            params.push_back(ReflectedParam{std::string(rawName), classifyFieldType(field->getTypeLayout())});
        }
        break;  // exactly one ParameterBlock per Phase 3 material (D8) -- first match is authoritative.
    }
    return params;
}

// --- Live-object bookkeeping (test seam) ----------------------------------
std::atomic<uint64_t> g_liveApiObjectCount{0};

// --- COM-lite root plumbing shared by every concrete object below --------
// CRTP, not a virtual base: `Interface` (IRxMaterialSystem, IRxMaterial,
// IRxMaterialInstance) is the ONE class each concrete type below publicly,
// singly inherits -- RxUnknownBase sits between them in the C++ class
// hierarchy purely to share addRef()/release()'s identical bodies, which
// does not change the ABI-facing vtable's slot order/count at all (the
// consumer only ever holds an Interface*-typed pointer and only ever
// calls through the slots ITS OWN pure-virtual declarations specify,
// which RxUnknownBase overrides in place, adding none). No class in this
// entire hierarchy declares a virtual destructor (see release() below:
// it always deletes through the exact static Derived* type, so virtual
// dispatch to a destructor is never needed) -- consistent with the "no
// virtual destructor" ABI rule [R:M§1.3 point 1] applying transitively to
// every object that could ever be reached only through an interface
// pointer.
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

// Forward declarations -- MaterialInstanceImpl/MaterialImpl/
// MaterialSystemImpl reference each other's pointers before each is
// fully declared (a real mutual dependency: instances own a ref to their
// material, materials own a ref to their system, and the system
// constructs materials, which construct instances). Every method whose
// body needs another one of these three types COMPLETE (constructors/
// destructors that addRef()/release() each other, setters that call
// MaterialImpl::paramKind()) is declared here and DEFINED out-of-line,
// after all three classes are fully declared below.
class MaterialSystemImpl;
class MaterialImpl;
class MaterialInstanceImpl;

// --- IRxMaterialInstance -----------------------------------------------
struct StoredParam {
    ParamKind kind = ParamKind::Unsupported;
    float floats[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    IRxTexture* texture = nullptr;  // addRef'd while stored here.
};

class MaterialInstanceImpl final : public RxUnknownBase<MaterialInstanceImpl, IRxMaterialInstance> {
public:
    explicit MaterialInstanceImpl(MaterialImpl* material);
    ~MaterialInstanceImpl();

    RxResult RX_CALL queryInterface(const RxGuid& iid, void** outObject) override;
    RxResult RX_CALL setFloat(const char* name, float value) override;
    RxResult RX_CALL setFloat4(const char* name, const float value[4]) override;
    RxResult RX_CALL setTexture(const char* name, IRxTexture* texture) override;

private:
    void releaseStoredTextureIfAny(const std::string& paramName);

    MaterialImpl* material_;
    std::unordered_map<std::string, StoredParam> params_;
};

// --- IRxMaterial ----------------------------------------------------------
class MaterialImpl final : public RxUnknownBase<MaterialImpl, IRxMaterial> {
public:
    MaterialImpl(MaterialSystemImpl* system, rx::material::MaterialHandle handle, std::string name,
                 std::unordered_map<std::string, ParamKind> params);
    ~MaterialImpl();

    RxResult RX_CALL queryInterface(const RxGuid& iid, void** outObject) override;
    RxResult RX_CALL createInstance(IRxMaterialInstance** outInstance) override;
    const char* RX_CALL name() override;

    // Internal accessor (not part of the ABI -- only MaterialInstanceImpl
    // calls this): the reflected shape of parameter `paramName`, or
    // nullopt if this material has no such reflected parameter at all.
    [[nodiscard]] std::optional<ParamKind> paramKind(const std::string& paramName) const;

private:
    MaterialSystemImpl* system_;
    rx::material::MaterialHandle handle_;
    std::string name_;
    std::unordered_map<std::string, ParamKind> params_;
};

// --- IRxMaterialSystem ------------------------------------------------
class MaterialSystemImpl final : public RxUnknownBase<MaterialSystemImpl, IRxMaterialSystem> {
public:
    explicit MaterialSystemImpl(rx::material::MaterialSystem* internalSystem) : internal_(internalSystem) {}

    RxResult RX_CALL queryInterface(const RxGuid& iid, void** outObject) override;
    RxResult RX_CALL loadMaterial(const char* slangModulePath, IRxMaterial** outMaterial) override;
    RxResult RX_CALL reloadChanged() override;

private:
    bool ensureReflectionGlobalSession();

    rx::material::MaterialSystem* internal_;  // non-owning; may be null (see rx_api.h's own comment on
                                                // RxMaterialSystemDesc for why that is a legal, documented state).
    Slang::ComPtr<slang::IGlobalSession> reflectionGlobalSession_;
};

// ===== Out-of-line definitions (every referenced type is complete now) ====

// --- MaterialInstanceImpl -------------------------------------------------
MaterialInstanceImpl::MaterialInstanceImpl(MaterialImpl* material) : material_(material) { material_->addRef(); }

MaterialInstanceImpl::~MaterialInstanceImpl() {
    for (auto& [paramName, param] : params_) {
        (void)paramName;
        if (param.kind == ParamKind::TextureIndex && param.texture != nullptr) {
            param.texture->release();
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

void MaterialInstanceImpl::releaseStoredTextureIfAny(const std::string& paramName) {
    auto it = params_.find(paramName);
    if (it != params_.end() && it->second.kind == ParamKind::TextureIndex && it->second.texture != nullptr) {
        it->second.texture->release();
    }
}

RxResult RX_CALL MaterialInstanceImpl::setFloat(const char* name, float value) {
    if (name == nullptr) {
        return RX_E_INVALIDARG;
    }
    // [Fix round 1, task-6-review.md F1] The `const char*` -> std::string
    // conversion inside paramKind()'s lookup, and the `unordered_map`
    // insertion below (which can rehash/allocate), can both throw
    // std::bad_alloc -- exactly the cross-ABI-exception failure mode D5
    // forbids. Every other public entry point in this file already
    // catches at its own boundary; this setter (and setFloat4/setTexture
    // below) previously did not -- see the review finding this fixes.
    try {
        std::optional<ParamKind> kind = material_->paramKind(name);
        if (!kind.has_value()) {
            return RX_E_NOTFOUND;
        }
        if (*kind != ParamKind::Float) {
            return RX_E_INVALIDARG;
        }
        releaseStoredTextureIfAny(name);
        StoredParam param;
        param.kind = ParamKind::Float;
        param.floats[0] = value;
        params_[name] = param;
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
        std::optional<ParamKind> kind = material_->paramKind(name);
        if (!kind.has_value()) {
            return RX_E_NOTFOUND;
        }
        if (*kind != ParamKind::Float4) {
            return RX_E_INVALIDARG;
        }
        releaseStoredTextureIfAny(name);
        StoredParam param;
        param.kind = ParamKind::Float4;
        param.floats[0] = value[0];
        param.floats[1] = value[1];
        param.floats[2] = value[2];
        param.floats[3] = value[3];
        params_[name] = param;
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
        std::optional<ParamKind> kind = material_->paramKind(name);
        if (!kind.has_value()) {
            return RX_E_NOTFOUND;
        }
        if (*kind != ParamKind::TextureIndex) {
            return RX_E_INVALIDARG;
        }

        // Ordering matters here beyond just the try/catch: capture
        // whatever this name previously held, but do not touch ANY
        // refcount until AFTER the (possibly-throwing) map mutation has
        // already succeeded. If `params_[name] = newParam` throws
        // (bad_alloc during a rehash), nothing has been addRef()'d or
        // release()'d yet, so this call leaves every refcount exactly as
        // it was on entry -- no leaked reference on `texture`, no
        // double-release of whatever was previously stored.
        IRxTexture* previousTexture = nullptr;
        auto it = params_.find(name);
        if (it != params_.end() && it->second.kind == ParamKind::TextureIndex) {
            previousTexture = it->second.texture;
        }

        StoredParam newParam;
        newParam.kind = ParamKind::TextureIndex;
        newParam.texture = texture;
        params_[name] = newParam;

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
MaterialImpl::MaterialImpl(MaterialSystemImpl* system, rx::material::MaterialHandle handle, std::string name,
                             std::unordered_map<std::string, ParamKind> params)
    : system_(system), handle_(handle), name_(std::move(name)), params_(std::move(params)) {
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

std::optional<ParamKind> MaterialImpl::paramKind(const std::string& paramName) const {
    auto it = params_.find(paramName);
    if (it == params_.end()) {
        return std::nullopt;
    }
    return it->second;
}

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

bool MaterialSystemImpl::ensureReflectionGlobalSession() {
    if (reflectionGlobalSession_.get() != nullptr) {
        return true;
    }
    if (SLANG_FAILED(slang::createGlobalSession(reflectionGlobalSession_.writeRef())) ||
        reflectionGlobalSession_.get() == nullptr) {
        RX_LOG_ERROR("rx_material: rx_api: slang::createGlobalSession failed (parameter reflection)");
        return false;
    }
    return true;
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
        // [Fix round 1, task-6-review.md F2] The reflection-only pass
        // runs FIRST, before ever touching `internal_` -- deliberately
        // reordered from the original submission, which called
        // `internal_->loadMaterial()` (expensive and STATEFUL: it
        // creates a real VkDescriptorSetLayout/VkPipelineLayout and
        // registers a MaterialRecord inside `internal_`) before this
        // side-effect-free validation step. That ordering meant a
        // reflection failure AFTER a successful internal load
        // permanently orphaned the just-created internal record (Task
        // 5's MaterialSystem exposes no unload/release path). Running
        // reflection first means: if it fails, `internal_` is never
        // touched at all, so there is nothing to orphan; if it succeeds,
        // `internal_->loadMaterial()` runs exactly once, same as before.
        // (Reflection succeeding does not GUARANTEE internal_'s own,
        // stricter shape checks also pass -- reflectMaterialParams()
        // only takes the first ParameterBlock it finds and silently
        // ignores any OTHER top-level global, while internal_'s own
        // reflectMaterialLayout() rejects those outright -- but if
        // internal_->loadMaterial() throws in THAT direction, it is
        // Task 5's own already-correct exception safety: a throwing
        // loadMaterial() never registers a MaterialRecord in the first
        // place, so there is still nothing orphaned either way.)
        if (!ensureReflectionGlobalSession()) {
            return RX_E_COMPILE;
        }
        std::string reflectError;
        std::vector<ReflectedParam> reflected =
            reflectMaterialParams(reflectionGlobalSession_.get(), path, reflectError);
        if (!reflectError.empty()) {
            RX_LOG_ERROR("rx_material: rx_api: {}", reflectError);
            return RX_E_COMPILE;
        }

        rx::material::MaterialHandle handle = internal_->loadMaterial(path);

        std::unordered_map<std::string, ParamKind> params;
        params.reserve(reflected.size());
        for (auto& param : reflected) {
            params.emplace(std::move(param.name), param.kind);
        }

        auto* material = new MaterialImpl(this, handle, path.stem().string(), std::move(params));
        *outMaterial = material;
        return RX_OK;
    } catch (const std::exception& e) {
        RX_LOG_ERROR("rx_material: rx_api: loadMaterial('{}') failed: {}", path.string(), e.what());
        return RX_E_COMPILE;
    }
}

RxResult RX_CALL MaterialSystemImpl::reloadChanged() {
    // Documented no-op for Task 6 -- declared in rx_api.h now purely for
    // GUID/vtable stability. Task 7 wires the real behavior: watch every
    // module path `loadMaterial()` has been called with on this instance,
    // re-load/re-link the ones whose file content changed (reusing
    // `internal_`'s own MaterialSystem::loadMaterial() -- which already
    // hashes module content -- against a fresh MaterialSystem/session
    // per material_system.cpp's own documented same-module-name reload
    // caveat), and invalidate exactly the pipeline-cache entries derived
    // from a changed module's content hash. Deliberately does not read
    // `internal_` at all, so this stays valid to call even on a
    // device-free/QI-only instance (internal_ == nullptr).
    return RX_OK;
}

}  // namespace

namespace detail {

uint64_t debugLiveApiObjectCount() { return g_liveApiObjectCount.load(std::memory_order_relaxed); }

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

}  // namespace rx::material
