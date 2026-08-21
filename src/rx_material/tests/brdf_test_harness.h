#pragma once
// src/rx_material/tests/brdf_test_harness.h -- Phase 5 Stage 1 Task 7
// [#43]: shared Slang session/compile plumbing for brdf.slang's own
// compute-based numerical test harness (test_brdf_module_gpu.cpp,
// test_brdf_white_furnace_gpu.cpp, test_brdf_spirv_link_composition_gpu.cpp).
//
// Drives slang::ISession directly, exactly like rx_material::MaterialSystem
// itself does -- see material_system.cpp's own header comment ("MaterialSystem
// drives slang::ISession directly... rx_shader::Compiler's public surface is
// too narrow for the multi-module link-time-specialization composition this
// library needs") and its own createMaterialSession()/compileMaterial()
// (material_system.cpp:949-987, :1067-1160), mirrored here rather than
// shared -- this codebase's own established per-file-duplicated-helper
// idiom (e.g. compileCompute() independently duplicated in
// rx_graph/tests/test_compute_gpu.cpp and
// rx_rhi_vk/tests/compute_pipeline_test.cpp).
//
// WHY NOT rx_shader::Compiler -- these tests need a session with a real
// search path (RX_MATERIAL_SHADER_DIR, shaders/material/) so a synthetic
// compute module can `import brdf;`; rx_shader::Compiler::create() sets no
// searchPaths (compiler.h's own doc comment: it has no multi-file import to
// resolve for its own callers -- triangle.vert/frag.slang and friends never
// import anything).
//
// A FRESH slang::IGlobalSession per test case (never rx_shader's own
// process-wide shared one, which is private to that library) -- the same
// tradeoff MaterialSystem::create() itself makes and documents (Slang's
// stdlib-load cost paid once more), acceptable here for the same reason:
// this is a small, finite set of test cases, not a hot per-frame path.
//
// THREAD SAFETY -- rx_shader::Compiler's own front-end calls are
// synchronized against a process-wide mutex private to that library
// (detail::global_session_mutex.h, not part of rx_shader's public include/
// surface). MaterialSystem has no access to that mutex either (it does not
// link rx_shader) and ships with no additional synchronization of its own,
// relying on each IGlobalSession/ISession pair being independent and on
// single-threaded call discipline (RX_ASSERT_MAIN_THREAD elsewhere in this
// codebase) -- this header follows the identical precedent: no mutex, each
// helper below assumes single-threaded, sequential use (doctest's own
// default execution model for one binary).

#include <rx_shader/compiler.h>
#include <rx_shader/reflection.h>

#include <slang-com-ptr.h>
#include <slang.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rx::brdf_test {

// Keeps BOTH the global and per-test session alive together -- ISession
// does not itself keep its creating IGlobalSession alive (verified against
// material_system.cpp's own Impl, which stores both as separate,
// co-lifetime class members for exactly this reason).
struct BrdfSession {
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    Slang::ComPtr<slang::ISession> session;
};

// Mirrors createMaterialSession() (material_system.cpp) -- SPIR-V target,
// "sm_6_0" profile, explicit spirv_1_3 capability floor PLUS an explicit
// CompilerOptionName::Profile entry [Task 2 (#38) empirical finding,
// compiler.cpp's own header comment: required for a compute-only linked
// program's getLayout() call to not crash inside a process that has
// already initialized Vulkan/vk-bootstrap, which every GPU test fixture in
// this codebase has by the time these helpers run] -- plus `searchPaths`
// pointed at `shaderDir` (RX_MATERIAL_SHADER_DIR, shaders/material/), so
// `import brdf;` resolves to the real shipped module.
inline std::optional<BrdfSession> createBrdfSession(const std::string& shaderDir) {
    BrdfSession result;
    if (SLANG_FAILED(slang::createGlobalSession(result.globalSession.writeRef())) ||
        result.globalSession.get() == nullptr) {
        return std::nullopt;
    }

    slang::TargetDesc targetDesc{};
    targetDesc.format = SLANG_SPIRV;
    targetDesc.profile = result.globalSession->findProfile("sm_6_0");

    std::array<slang::CompilerOptionEntry, 2> compilerOptionEntries{};
    uint32_t entryCount = 0;

    SlangCapabilityID spirvFloor = result.globalSession->findCapability("spirv_1_3");
    if (spirvFloor != SLANG_CAPABILITY_UNKNOWN) {
        slang::CompilerOptionEntry capabilityEntry{};
        capabilityEntry.name = slang::CompilerOptionName::Capability;
        capabilityEntry.value.kind = slang::CompilerOptionValueKind::Int;
        capabilityEntry.value.intValue0 = static_cast<int32_t>(spirvFloor);
        compilerOptionEntries[entryCount++] = capabilityEntry;
    }

    slang::CompilerOptionEntry profileEntry{};
    profileEntry.name = slang::CompilerOptionName::Profile;
    profileEntry.value.kind = slang::CompilerOptionValueKind::Int;
    profileEntry.value.intValue0 = static_cast<int32_t>(targetDesc.profile);
    compilerOptionEntries[entryCount++] = profileEntry;

    targetDesc.compilerOptionEntries = compilerOptionEntries.data();
    targetDesc.compilerOptionEntryCount = entryCount;

    const char* searchPath = shaderDir.c_str();

    slang::SessionDesc sessionDesc{};
    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;
    sessionDesc.searchPaths = &searchPath;
    sessionDesc.searchPathCount = 1;

    if (SLANG_FAILED(result.globalSession->createSession(sessionDesc, result.session.writeRef())) ||
        result.session.get() == nullptr) {
        return std::nullopt;
    }
    return result;
}

namespace detail {

// Shared compose->link->codegen tail for both single- and two-module
// compiles below -- `parts` already contains every slang::IComponentType
// to compose (module(s) + entry point(s), in the SAME "module(s) first,
// entry points after" order compiler.cpp's own compileImpl() documents as
// load-bearing for the stage lookup). Always requests exactly ONE entry
// point's code (index 0) -- every caller in this test suite links exactly
// one compute entry point per composite.
inline rx::shader::CompileResult finishLink(slang::ISession* session,
                                             std::vector<slang::IComponentType*>& parts,
                                             const std::string& entryPointName) {
    rx::shader::CompileResult result;

    Slang::ComPtr<slang::IComponentType> composite;
    Slang::ComPtr<slang::IBlob> composeDiag;
    SlangResult composeResult = session->createCompositeComponentType(
        parts.data(), static_cast<SlangInt>(parts.size()), composite.writeRef(), composeDiag.writeRef());
    if (composeDiag.get() != nullptr && composeDiag->getBufferSize() > 0) {
        result.diagnostics.append(static_cast<const char*>(composeDiag->getBufferPointer()), composeDiag->getBufferSize());
    }
    if (SLANG_FAILED(composeResult) || composite.get() == nullptr) {
        result.ok = false;
        return result;
    }

    Slang::ComPtr<slang::IComponentType> linked;
    Slang::ComPtr<slang::IBlob> linkDiag;
    SlangResult linkResult = composite->link(linked.writeRef(), linkDiag.writeRef());
    if (linkDiag.get() != nullptr && linkDiag->getBufferSize() > 0) {
        result.diagnostics.append(static_cast<const char*>(linkDiag->getBufferPointer()), linkDiag->getBufferSize());
    }
    if (SLANG_FAILED(linkResult) || linked.get() == nullptr) {
        result.ok = false;
        return result;
    }

    Slang::ComPtr<slang::IBlob> layoutDiag;
    slang::ProgramLayout* layout = linked->getLayout(0, layoutDiag.writeRef());

    Slang::ComPtr<slang::IBlob> codeDiag;
    Slang::ComPtr<slang::IBlob> codeBlob;
    SlangResult codeResult = linked->getEntryPointCode(0, 0, codeBlob.writeRef(), codeDiag.writeRef());
    if (codeDiag.get() != nullptr && codeDiag->getBufferSize() > 0) {
        result.diagnostics.append(static_cast<const char*>(codeDiag->getBufferPointer()), codeDiag->getBufferSize());
    }
    if (SLANG_FAILED(codeResult) || codeBlob.get() == nullptr) {
        result.ok = false;
        return result;
    }

    rx::shader::SpirvBlob blob;
    blob.entryPointName = entryPointName;
    blob.stage = VK_SHADER_STAGE_COMPUTE_BIT;  // every compile in this test suite is compute-only.
    const auto* words = static_cast<const uint32_t*>(codeBlob->getBufferPointer());
    blob.code.assign(words, words + codeBlob->getBufferSize() / sizeof(uint32_t));

    result.ok = true;
    result.entryPointCode.push_back(std::move(blob));
    result.linkedProgram = linked;
    result.cachedLayout = layout;
    return result;
}

}  // namespace detail

// Single-module compile: `source` may `import brdf;` (or anything else on
// the session's own search path); `entryPointName` must be defined in
// `source` itself. Mirrors rx_shader::compiler.cpp's own compileImpl(), one
// entry point only.
inline rx::shader::CompileResult compileBrdfComputeModule(slang::ISession* session, const std::string& moduleName,
                                                            const std::string& source,
                                                            const std::string& entryPointName) {
    Slang::ComPtr<slang::IBlob> sourceBlob(Slang::INIT_ATTACH, slang_createBlob(source.data(), source.size()));
    Slang::ComPtr<slang::IBlob> loadDiag;
    slang::IModule* module =
        session->loadModuleFromSource(moduleName.c_str(), (moduleName + ".slang").c_str(), sourceBlob, loadDiag.writeRef());
    if (module == nullptr) {
        rx::shader::CompileResult result;
        result.ok = false;
        if (loadDiag.get() != nullptr && loadDiag->getBufferSize() > 0) {
            result.diagnostics.append(static_cast<const char*>(loadDiag->getBufferPointer()), loadDiag->getBufferSize());
        }
        return result;
    }

    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    if (SLANG_FAILED(module->findEntryPointByName(entryPointName.c_str(), entryPoint.writeRef())) ||
        entryPoint.get() == nullptr) {
        rx::shader::CompileResult result;
        result.ok = false;
        result.diagnostics = "entry point not found: " + entryPointName;
        return result;
    }

    std::vector<slang::IComponentType*> parts{module, entryPoint.get()};
    return detail::finishLink(session, parts, entryPointName);
}

// Two-module compile for the link-time feature-composition proof
// (test_brdf_spirv_link_composition_gpu.cpp): `entrySource` declares
// `extern struct <Name> : <Interface>;` plus the compute entry point (which
// must be named `entryPointName`); `implSource` supplies the matching
// `export struct <Name> : <Interface> = <Concrete>;` alias. NEITHER source
// imports the other -- both may `import brdf;` independently, exactly
// forward_entry.slang/standard_pbr.slang's own established shape (see
// brdf.slang's own "Link-time feature composition" header comment).
inline rx::shader::CompileResult compileLinkedFeatureVariant(slang::ISession* session, const std::string& entryModuleName,
                                                               const std::string& entrySource,
                                                               const std::string& entryPointName,
                                                               const std::string& implModuleName,
                                                               const std::string& implSource) {
    Slang::ComPtr<slang::IBlob> entryBlob(Slang::INIT_ATTACH, slang_createBlob(entrySource.data(), entrySource.size()));
    Slang::ComPtr<slang::IBlob> entryLoadDiag;
    slang::IModule* entryModule = session->loadModuleFromSource(
        entryModuleName.c_str(), (entryModuleName + ".slang").c_str(), entryBlob, entryLoadDiag.writeRef());
    if (entryModule == nullptr) {
        rx::shader::CompileResult result;
        result.ok = false;
        if (entryLoadDiag.get() != nullptr && entryLoadDiag->getBufferSize() > 0) {
            result.diagnostics.append(static_cast<const char*>(entryLoadDiag->getBufferPointer()), entryLoadDiag->getBufferSize());
        }
        return result;
    }

    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    if (SLANG_FAILED(entryModule->findEntryPointByName(entryPointName.c_str(), entryPoint.writeRef())) ||
        entryPoint.get() == nullptr) {
        rx::shader::CompileResult result;
        result.ok = false;
        result.diagnostics = "entry point not found: " + entryPointName;
        return result;
    }

    Slang::ComPtr<slang::IBlob> implBlob(Slang::INIT_ATTACH, slang_createBlob(implSource.data(), implSource.size()));
    Slang::ComPtr<slang::IBlob> implLoadDiag;
    slang::IModule* implModule = session->loadModuleFromSource(
        implModuleName.c_str(), (implModuleName + ".slang").c_str(), implBlob, implLoadDiag.writeRef());
    if (implModule == nullptr) {
        rx::shader::CompileResult result;
        result.ok = false;
        if (implLoadDiag.get() != nullptr && implLoadDiag->getBufferSize() > 0) {
            result.diagnostics.append(static_cast<const char*>(implLoadDiag->getBufferPointer()), implLoadDiag->getBufferSize());
        }
        return result;
    }

    std::vector<slang::IComponentType*> parts{entryModule, entryPoint.get(), implModule};
    return detail::finishLink(session, parts, entryPointName);
}

}  // namespace rx::brdf_test
