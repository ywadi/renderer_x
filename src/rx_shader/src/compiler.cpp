#include <rx_shader/compiler.h>

#include "detail/global_session_mutex.h"

#include <rx_core/log.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

namespace rx::shader {

// See the class comment on rx::shader::Compiler (compiler.h) for why this
// is process-wide (one IGlobalSession, ever) rather than per-Compiler, and
// why it is guarded by a mutex rather than left to Slang's own (nonexistent)
// internal synchronization. The mutex itself has process lifetime and is
// never explicitly torn down, matching Slang's own guidance to create and
// reuse a single global session for as long as the application runs
// [R:A4].
//
// Given external (not anonymous-namespace) linkage, unlike every other
// helper in this file, specifically so reflection.cpp -- a second
// translation unit in this same library -- can lock the exact same mutex
// object before touching a linked program's reflection data (see
// detail/global_session_mutex.h's comment for why that needs the same
// lock as front-end compilation).
namespace detail {
std::mutex& globalSessionMutex() {
    static std::mutex mutex;
    return mutex;
}
}  // namespace detail

namespace {

Slang::ComPtr<slang::IGlobalSession>& sharedGlobalSession() {
    static Slang::ComPtr<slang::IGlobalSession> session;
    return session;
}

// Maps a Slang entry-point stage to the corresponding Vulkan shader-stage
// bit. Slang's stage enum also covers ray-tracing/mesh/work-graph stages
// this project doesn't use yet (see slang.h's SlangStage) -- they're
// mapped for completeness since the underlying VkShaderStageFlagBits
// values already exist in Vulkan-Headers regardless of whether any device
// this project targets supports the extensions that make them meaningful.
VkShaderStageFlagBits mapStage(SlangStage stage) {
    switch (stage) {
        case SLANG_STAGE_VERTEX:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case SLANG_STAGE_HULL:
            return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case SLANG_STAGE_DOMAIN:
            return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case SLANG_STAGE_GEOMETRY:
            return VK_SHADER_STAGE_GEOMETRY_BIT;
        case SLANG_STAGE_FRAGMENT:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case SLANG_STAGE_COMPUTE:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        case SLANG_STAGE_RAY_GENERATION:
            return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        case SLANG_STAGE_INTERSECTION:
            return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
        case SLANG_STAGE_ANY_HIT:
            return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        case SLANG_STAGE_CLOSEST_HIT:
            return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        case SLANG_STAGE_MISS:
            return VK_SHADER_STAGE_MISS_BIT_KHR;
        case SLANG_STAGE_CALLABLE:
            return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
        case SLANG_STAGE_MESH:
            return VK_SHADER_STAGE_MESH_BIT_EXT;
        case SLANG_STAGE_AMPLIFICATION:
            return VK_SHADER_STAGE_TASK_BIT_EXT;
        default:
            RX_LOG_WARN("rx_shader: no VkShaderStageFlagBits mapping for SlangStage {}; using VK_SHADER_STAGE_ALL",
                        static_cast<int>(stage));
            return VK_SHADER_STAGE_ALL;
    }
}

// Appends one diagnostic step's text (if any) to `out`, labelled with
// `stepName`. Slang hands back a non-null, non-empty IBlob for warnings
// just as it does for errors, so this runs unconditionally after every
// step regardless of that step's own success/failure -- diagnostics are
// never gated on `ok` [R:A2, spec Fixed decision #2].
void appendDiagnostics(std::string& out, const char* stepName, slang::IBlob* blob) {
    if (blob == nullptr || blob->getBufferSize() == 0) {
        return;
    }
    out.append("[").append(stepName).append("] ");
    out.append(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
    if (out.empty() || out.back() != '\n') {
        out.push_back('\n');
    }
}

// The shared compile pipeline behind both Compiler::compileFromSource and
// Compiler::compileFromFile: load module from source text -> find each
// requested entry point -> compose -> link -> extract SPIR-V per entry
// point [R:A2]. `sourcePath` is purely informational (it's what shows up
// in diagnostic message locations); it does not need to exist on disk.
CompileResult compileImpl(slang::ISession* session,
                           const std::string& moduleName,
                           const std::string& sourcePath,
                           const std::string& sourceText,
                           const std::vector<std::string>& entryPointNames) {
    CompileResult result;
    std::string diagnostics;

    std::lock_guard<std::mutex> lock(detail::globalSessionMutex());

    // slang_createBlob returns an already-owned (refcount 1) ISlangBlob*,
    // so attach rather than construct-with-addRef.
    Slang::ComPtr<slang::IBlob> sourceBlob(Slang::INIT_ATTACH,
                                            slang_createBlob(sourceText.data(), sourceText.size()));

    Slang::ComPtr<slang::IBlob> loadDiagnostics;
    slang::IModule* module =
        session->loadModuleFromSource(moduleName.c_str(), sourcePath.c_str(), sourceBlob, loadDiagnostics.writeRef());
    appendDiagnostics(diagnostics, "load", loadDiagnostics);

    if (module == nullptr) {
        result.ok = false;
        result.diagnostics = diagnostics;
        RX_LOG_ERROR("rx_shader: failed to load module '{}':\n{}", moduleName, result.diagnostics);
        return result;
    }

    std::vector<slang::IComponentType*> parts;
    parts.reserve(1 + entryPointNames.size());
    parts.push_back(module);

    // IModule::getDefinedEntryPointCount()/IComponentType::getEntryPointCount()
    // on a raw IModule is documented to always be 0 (an entry point defined
    // in a module is not automatically part of the linkage) -- so `module`
    // contributes zero entry points to the composite below, and the
    // explicit IEntryPoint objects appended after it are the only ones,
    // in exactly the order pushed. That ordering is load-bearing: it is
    // what lets the getLayout()-based stage lookup further down assume
    // ProgramLayout::getEntryPointByIndex(i) corresponds to
    // entryPointNames[i].
    std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints;
    entryPoints.reserve(entryPointNames.size());
    for (const auto& name : entryPointNames) {
        Slang::ComPtr<slang::IEntryPoint> entryPoint;
        SlangResult entryPointResult = module->findEntryPointByName(name.c_str(), entryPoint.writeRef());
        if (SLANG_FAILED(entryPointResult) || entryPoint.get() == nullptr) {
            diagnostics.append("error: entry point '")
                .append(name)
                .append("' not found in module '")
                .append(moduleName)
                .append("'\n");
            result.ok = false;
            result.diagnostics = diagnostics;
            RX_LOG_ERROR("rx_shader: {}", result.diagnostics);
            return result;
        }
        entryPoints.push_back(std::move(entryPoint));
    }
    for (auto& entryPoint : entryPoints) {
        parts.push_back(entryPoint.get());
    }

    Slang::ComPtr<slang::IBlob> compositeDiagnostics;
    Slang::ComPtr<slang::IComponentType> composite;
    SlangResult compositeResult = session->createCompositeComponentType(
        parts.data(), static_cast<SlangInt>(parts.size()), composite.writeRef(), compositeDiagnostics.writeRef());
    appendDiagnostics(diagnostics, "compose", compositeDiagnostics);
    if (SLANG_FAILED(compositeResult) || composite.get() == nullptr) {
        result.ok = false;
        result.diagnostics = diagnostics;
        RX_LOG_ERROR("rx_shader: failed to compose module '{}':\n{}", moduleName, result.diagnostics);
        return result;
    }

    Slang::ComPtr<slang::IBlob> linkDiagnostics;
    Slang::ComPtr<slang::IComponentType> linked;
    SlangResult linkResult = composite->link(linked.writeRef(), linkDiagnostics.writeRef());
    appendDiagnostics(diagnostics, "link", linkDiagnostics);
    if (SLANG_FAILED(linkResult) || linked.get() == nullptr) {
        result.ok = false;
        result.diagnostics = diagnostics;
        RX_LOG_ERROR("rx_shader: failed to link module '{}':\n{}", moduleName, result.diagnostics);
        return result;
    }

    // Needed only to recover each entry point's VkShaderStageFlagBits (see
    // the ordering note above); diagnostics from this step are still
    // captured even though a null layout is tolerated below (code
    // generation can still proceed without it -- stage just falls back to
    // VK_SHADER_STAGE_ALL for that blob, which mapStage() also warns about
    // for the analogous unknown-stage case).
    Slang::ComPtr<slang::IBlob> layoutDiagnostics;
    slang::ProgramLayout* layout = linked->getLayout(0, layoutDiagnostics.writeRef());
    appendDiagnostics(diagnostics, "layout", layoutDiagnostics);

    bool allEntryPointsOk = true;
    std::vector<SpirvBlob> blobs;
    blobs.reserve(entryPointNames.size());
    for (size_t i = 0; i < entryPointNames.size(); ++i) {
        Slang::ComPtr<slang::IBlob> codeDiagnostics;
        Slang::ComPtr<slang::IBlob> codeBlob;
        SlangResult codeResult =
            linked->getEntryPointCode(static_cast<SlangInt>(i), 0, codeBlob.writeRef(), codeDiagnostics.writeRef());
        appendDiagnostics(diagnostics, "codegen", codeDiagnostics);
        if (SLANG_FAILED(codeResult) || codeBlob.get() == nullptr) {
            allEntryPointsOk = false;
            diagnostics.append("error: code generation failed for entry point '")
                .append(entryPointNames[i])
                .append("'\n");
            continue;
        }

        SpirvBlob blob;
        blob.entryPointName = entryPointNames[i];
        blob.stage = (layout != nullptr && i < static_cast<size_t>(layout->getEntryPointCount()))
                         ? mapStage(layout->getEntryPointByIndex(i)->getStage())
                         : VK_SHADER_STAGE_ALL;

        size_t byteSize = codeBlob->getBufferSize();
        const auto* wordData = static_cast<const uint32_t*>(codeBlob->getBufferPointer());
        blob.code.assign(wordData, wordData + byteSize / sizeof(uint32_t));
        blobs.push_back(std::move(blob));
    }

    result.ok = allEntryPointsOk;
    result.diagnostics = diagnostics;
    result.entryPointCode = std::move(blobs);
    if (result.ok) {
        result.linkedProgram = linked;
        // See CompileResult::cachedLayout's own doc comment (compiler.h):
        // reused by rx::shader::reflect() instead of a second getLayout()
        // call on the same `linked` component.
        result.cachedLayout = layout;
    }

    if (!diagnostics.empty()) {
        if (result.ok) {
            RX_LOG_WARN("rx_shader: diagnostics while compiling '{}':\n{}", moduleName, diagnostics);
        } else {
            RX_LOG_ERROR("rx_shader: compile of '{}' failed:\n{}", moduleName, diagnostics);
        }
    }

    return result;
}

}  // namespace

Compiler::Compiler(Slang::ComPtr<slang::ISession> session) : session_(std::move(session)) {}

std::optional<Compiler> Compiler::create() {
    // Deliberately called before this function's own mutex lock below --
    // that used to be a real race (two threads both first-creating a
    // Compiler concurrently could race inside log::init() itself, since it
    // used to guard its one-time setup with a bare, non-atomic `static
    // bool`). Fixed at the source: rx::core::log::init() (src/rx_core/src/
    // log.cpp) now uses std::call_once, so it is safe to call from any
    // number of concurrent threads with no lock of this function's own
    // needed around it.
    rx::core::log::init();

    std::lock_guard<std::mutex> lock(detail::globalSessionMutex());

    Slang::ComPtr<slang::IGlobalSession>& global = sharedGlobalSession();
    if (global.get() == nullptr) {
        SlangResult sessionResult = slang::createGlobalSession(global.writeRef());
        if (SLANG_FAILED(sessionResult) || global.get() == nullptr) {
            RX_LOG_ERROR("rx_shader: slang::createGlobalSession failed (SlangResult={})",
                         static_cast<int>(sessionResult));
            return std::nullopt;
        }
    }

    slang::TargetDesc targetDesc = {};
    targetDesc.format = SLANG_SPIRV;
    // "sm_6_0" matches the profile Phase 1's slangc-based offline path
    // already uses for the same SPIR-V target (shaders/CMakeLists.txt),
    // so both the build-time and runtime compilation paths agree on
    // feature/shader-model availability.
    targetDesc.profile = global->findProfile("sm_6_0");

    // Explicit SPIR-V 1.3 capability floor, matching the Vulkan 1.3 /
    // Steam Deck baseline [R:A5, spec Fixed decision #3]. findCapability
    // returns SLANG_CAPABILITY_UNKNOWN for a name it doesn't recognize;
    // rather than silently feeding that sentinel into compilerOptionEntries
    // (which could produce a confusing downstream compiler error), fall
    // back to compiling without an explicit floor and log loudly -- this
    // should never actually trigger against the pinned Slang version this
    // was verified against, but a future version bump changing capability
    // names is exactly the kind of drift [R:A6] this guards against.
    std::array<slang::CompilerOptionEntry, 2> compilerOptionEntries{};
    uint32_t compilerOptionEntryCount = 0;

    slang::CompilerOptionEntry capabilityEntry{};
    SlangCapabilityID spirvFloor = global->findCapability("spirv_1_3");
    if (spirvFloor != SLANG_CAPABILITY_UNKNOWN) {
        capabilityEntry.name = slang::CompilerOptionName::Capability;
        capabilityEntry.value.kind = slang::CompilerOptionValueKind::Int;
        capabilityEntry.value.intValue0 = static_cast<int32_t>(spirvFloor);
        compilerOptionEntries[compilerOptionEntryCount++] = capabilityEntry;
    } else {
        RX_LOG_WARN(
            "rx_shader: Slang capability 'spirv_1_3' not found by this Slang build; "
            "compiling without an explicit SPIR-V version floor");
    }

    // [Task 2 (#38), gate ruling RC2 -- empirical finding, not in the
    // matrix's own "zero changes needed" text] `targetDesc.profile` above
    // (the raw TargetDesc struct field) is NOT sufficient on its own to
    // populate the per-target-request CompilerOptionSet
    // Slang::TargetRequest::getTargetCaps() reads via
    // Slang::CompilerOptionSet::getProfile() -- verified directly (gdb,
    // this session): a compute-ONLY linked program's first-ever
    // IComponentType::getLayout(0, ...) call (rx::shader::reflect()'s own
    // entry point) crashes with SIGFPE, an unmasked integer divide-by-zero
    // inside getProfile(), specifically when NO explicit
    // CompilerOptionName::Profile entry was ALSO passed via
    // compilerOptionEntries -- reproduced reliably (a real, deterministic
    // crash under realistic conditions: a process that has already
    // initialized Vulkan/vk-bootstrap, matching what every real compute
    // consumer's own runtime environment looks like), NOT reproducible for
    // a vertex+fragment-linked program under the exact same conditions
    // (graphics reflection already implicitly ends up with a populated
    // CompilerOptionSet through Slang's own multi-stage linking path;
    // compute's single-entry-point link does not). Passing the SAME
    // profile explicitly here, via the documented CompilerOptionName::
    // Profile entry (slang.h: "intValue0: profile"), populates that
    // CompilerOptionSet unconditionally for every target -- verified fixed
    // against this exact repro (rx_rhi_vk/tests/compute_pipeline_test.cpp)
    // both with and without a live Vulkan instance already created in the
    // process. Harmless for the existing vertex+fragment path (an
    // identical redundant restatement of the same profile
    // targetDesc.profile already carries).
    slang::CompilerOptionEntry profileEntry{};
    profileEntry.name = slang::CompilerOptionName::Profile;
    profileEntry.value.kind = slang::CompilerOptionValueKind::Int;
    profileEntry.value.intValue0 = static_cast<int32_t>(targetDesc.profile);
    compilerOptionEntries[compilerOptionEntryCount++] = profileEntry;

    targetDesc.compilerOptionEntries = compilerOptionEntries.data();
    targetDesc.compilerOptionEntryCount = compilerOptionEntryCount;

    slang::SessionDesc sessionDesc = {};
    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;

    Slang::ComPtr<slang::ISession> session;
    SlangResult createSessionResult = global->createSession(sessionDesc, session.writeRef());
    if (SLANG_FAILED(createSessionResult) || session.get() == nullptr) {
        RX_LOG_ERROR("rx_shader: IGlobalSession::createSession failed (SlangResult={})",
                     static_cast<int>(createSessionResult));
        return std::nullopt;
    }

    return Compiler(std::move(session));
}

CompileResult Compiler::compileFromSource(const std::string& moduleName,
                                           const std::string& source,
                                           const std::vector<std::string>& entryPointNames) {
    // The path is purely a diagnostic label here -- there is no real file
    // backing a compileFromSource call.
    return compileImpl(session_.get(), moduleName, moduleName + ".slang", source, entryPointNames);
}

CompileResult Compiler::compileFromFile(const std::string& path, const std::vector<std::string>& entryPointNames) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        CompileResult result;
        result.ok = false;
        result.diagnostics = "error: could not open shader file '" + path + "'\n";
        RX_LOG_ERROR("rx_shader: {}", result.diagnostics);
        return result;
    }

    std::ostringstream contents;
    contents << file.rdbuf();

    std::string moduleName = std::filesystem::path(path).stem().string();
    if (moduleName.empty()) {
        moduleName = "shaderModule";
    }

    return compileImpl(session_.get(), moduleName, path, contents.str(), entryPointNames);
}

}  // namespace rx::shader
