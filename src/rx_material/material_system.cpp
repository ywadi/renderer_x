#include <rx_material/material_system.h>

#include <rx_core/handle.h>
#include <rx_core/log.h>
#include <rx_rhi_vk/device.h>
#include <rx_rhi_vk/pipeline_layout.h>

#include <slang-com-ptr.h>
#include <slang.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// MaterialSystem drives slang::ISession/IModule/IComponentType DIRECTLY,
// rather than through rx_shader::Compiler's compileFromSource()/
// compileFromFile(): those two methods compile exactly ONE module plus its
// OWN entry points, compose, link, and immediately extract SPIR-V --
// there is no way to hand them a SECOND, separately-loaded module (this
// library's shared forward_entry.slang) to compose the first module's
// entry points against, which is exactly the shape every material load
// needs [D6]. This still follows Compiler's own established idioms
// end to end -- see each function below for exactly which one (session/
// target-desc construction, diagnostics-blob capture regardless of
// success/failure, the SAME-MODULE-NAME-per-session caveat) -- rather than
// inventing a parallel, differently-shaped Slang wrapper [Task 5 brief].
namespace rx::material {

namespace {

// --- FNV-1a -------------------------------------------------------------
// Implemented locally rather than shared with rx_graph/pass_signature.h's
// identical algorithm, for the same reason that header gives for not
// sharing the reverse direction: no rx_core hash utility exists yet
// (checked before writing this), and FNV-1a is small, canonical, and fully
// specified by its two 64-bit constants -- not the kind of subsystem this
// repo's ready-made-library-first policy is aimed at duplicating instead
// of reusing.
constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

uint64_t fnv1aBytes(const void* data, size_t len) {
    uint64_t h = kFnvOffsetBasis;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= bytes[i];
        h *= kFnvPrime;
    }
    return h;
}

void fnv1aMix64(uint64_t& h, uint64_t v) {
    for (int byteIndex = 0; byteIndex < 8; ++byteIndex) {
        h ^= (v >> (byteIndex * 8)) & 0xFF;
        h *= kFnvPrime;
    }
}

void fnv1aMix32(uint64_t& h, uint32_t v) {
    for (int byteIndex = 0; byteIndex < 4; ++byteIndex) {
        h ^= (v >> (byteIndex * 8)) & 0xFF;
        h *= kFnvPrime;
    }
}

// --- Fixed engine conventions for a material's parameter block ---------
// Descriptor set 1 is this engine's fixed, mandatory location for every
// material's `ParameterBlock<TParams> gParams` (set 0 stays the external
// bindless set, substituted in at pipelineLayout()-build time below) --
// [D6/D8, Task 5 brief: "the material ParameterBlock lands at set 1 via
// reflection"]. Binding 0 within that set is not this engine's own
// invention -- it is Slang's own documented behavior for a ParameterBlock
// with no nested resource-typed fields ("every resource is placed into the
// set with binding index starting from 0... ordinary data fields...
// appear as binding 0 of the resulting descriptor set" --
// docs/user-guide/a2-01-spirv-target-specific.md, "ParameterBlock for
// SPIR-V target"), which is exactly this engine's own material-parameter
// shape [D8: bound parameters are plain data -- floats/vectors/bindless
// table indices -- never a resource-typed field inside TParams itself].
constexpr uint32_t kMaterialParamBlockSet = 1;
constexpr uint32_t kMaterialParamBlockBinding = 0;

// Every material pipeline's shader stages are exactly these two, always --
// forward_entry.slang declares exactly `vertexMain`/`fragmentMain` and
// nothing else, composed the same way for every material this
// MaterialSystem ever loads. Used both as the (conservative, matching
// rx_shader::reflect()'s own documented policy of "merge every entry
// point's stage, not just the ones provably touching this exact global")
// stage mask on gParams's own binding, and as the fixed
// VkPipelineShaderStageCreateInfo pair getPipeline() builds every
// VkGraphicsPipelineCreateInfo from.
constexpr VkShaderStageFlags kMaterialStageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// The fixed vertex-input layout every material pipeline assumes -- see
// forward_entry.slang's own "VERTEX INPUT LAYOUT" header comment for why
// this is fixed here rather than derived from anything Phase 2 already
// established (nothing in Phase 2 fixed one canonical host-side Vertex
// layout across samples). position/normal float3, uv float2, tightly
// packed, one interleaved binding -- matching that entry point's own
// vertexMain(float3 position, float3 normal, float2 uv) parameter list
// and the attribute locations Slang's SPIR-V backend assigns them
// (verified directly via spirv-dis before writing this: locations 0/1/2
// in declaration order -- see task-5-report.md).
struct MaterialVertexLayout {
    float position[3];
    float normal[3];
    float uv[2];
};

struct VertexInputState {
    VkVertexInputBindingDescription binding{};
    std::array<VkVertexInputAttributeDescription, 3> attributes{};
};

VertexInputState makeVertexInputState() {
    VertexInputState state;
    state.binding.binding = 0;
    state.binding.stride = sizeof(MaterialVertexLayout);
    state.binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    state.attributes[0].location = 0;
    state.attributes[0].binding = 0;
    state.attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    state.attributes[0].offset = offsetof(MaterialVertexLayout, position);

    state.attributes[1].location = 1;
    state.attributes[1].binding = 0;
    state.attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    state.attributes[1].offset = offsetof(MaterialVertexLayout, normal);

    state.attributes[2].location = 2;
    state.attributes[2].binding = 0;
    state.attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    state.attributes[2].offset = offsetof(MaterialVertexLayout, uv);
    return state;
}

// --- Slang diagnostics plumbing -----------------------------------------
// Mirrors rx_shader's compiler.cpp appendDiagnostics() exactly (same
// "Slang hands back a non-null, non-empty IBlob for warnings just as it
// does for errors" behavior applies here, on the same shipped Slang
// build) -- duplicated rather than shared because it is not part of
// rx_shader's public surface (an anonymous-namespace-private helper in
// compiler.cpp), matching reflection.cpp's own precedent for duplicating
// compiler.cpp's equally small mapStage() rather than hoisting it.
void appendDiagnostics(std::string& out, const char* step, slang::IBlob* blob) {
    if (blob == nullptr || blob->getBufferSize() == 0) {
        return;
    }
    out.append("[").append(step).append("] ");
    out.append(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
    if (out.empty() || out.back() != '\n') {
        out.push_back('\n');
    }
}

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

// --- Material-specific reflection ---------------------------------------
// rx_shader::reflect() (reflection.h) deliberately does not handle
// `ParameterBlock<T>` at all ("Slang's ParameterBlock<T>/nested-parameter-
// block layouts are out of scope for this walk") -- it only classifies a
// global parameter whose `getCategory() == ParameterCategory::
// DescriptorTableSlot` (an ordinary flat resource) or `PushConstantBuffer`.
// A material's `ParameterBlock<TParams> gParams` reports a THIRD category,
// `SubElementRegisterSpace` (verified directly against this project's
// shipped Slang v2026.14.1 build -- see task-5-report.md for the probe),
// which is why this needs its own walk rather than reusing/extending
// reflect() (also outside this task's Modify list -- rx_shader is not
// touched by Task 5).
//
// The load-bearing empirical finding for THAT category: for a
// SubElementRegisterSpace-category top-level parameter,
// `VariableLayoutReflection::getBindingIndex()` -- NOT getBindingSpace(),
// which is meaningless here (observed as 0 regardless of the parameter's
// real set) -- reports the parameter's real descriptor SET number.
// Verified at three different explicit `[[vk::binding(0, N)]]` values (1,
// 3, and the default test fixture's 1 again) against spirv-dis on the
// actual emitted SPIR-V, every time: reflection's getBindingIndex()
// matched the real DescriptorSet decoration exactly, while getBindingSpace()
// stayed 0. The actual Vulkan BINDING NUMBER within that set is not
// reported by either accessor for this category at all -- Slang's own
// docs state it is always 0 for a ParameterBlock with no nested
// resource-typed fields (see kMaterialParamBlockBinding's own comment
// above), which is this engine's own fixed material-parameter shape [D8],
// so that part is a documented Slang convention this code relies on, not
// a workaround for a gap in reflection.
//
// Rejects (returns std::nullopt, `error` filled in) anything else it
// encounters as a top-level global parameter: more than one
// ParameterBlock, one bound to the wrong set, or any OTHER kind of global
// (an ordinary flat resource, a push constant, ...) -- Phase 3 materials
// support exactly one ParameterBlock<TParams> gParams and nothing else at
// global scope; textures/other bindless resources route through gParams's
// own bindless-table INDICES (plain uint data), never a resource-typed
// field or a second global [D8]. This is a deliberate scope boundary, not
// an oversight: a material declaring extra flat globals is a real,
// supportable future case (the underlying flat-resource reflection
// technique reflect() already uses is directly reusable for it), but
// nothing in this task's test list exercises one, and writing that mapping
// without a single verified case to check it against would be exactly the
// kind of unverified, speculative code this project's engineering
// discipline argues against -- so it is called out here, explicitly, as
// future work rather than shipped half-tested.
std::optional<rx::shader::ShaderLayoutInfo> reflectMaterialLayout(slang::ProgramLayout* layout,
                                                                    const std::string& moduleLabel,
                                                                    std::string& error) {
    rx::shader::ShaderLayoutInfo info;
    bool foundParamBlock = false;

    unsigned paramCount = layout->getParameterCount();
    for (unsigned i = 0; i < paramCount; ++i) {
        slang::VariableLayoutReflection* param = layout->getParameterByIndex(i);
        const char* rawName = param->getName();
        std::string name = (rawName != nullptr) ? rawName : "<anonymous>";

        const bool isParamBlock = param->getCategory() == slang::ParameterCategory::SubElementRegisterSpace &&
                                   param->getTypeLayout() != nullptr &&
                                   param->getTypeLayout()->getKind() == slang::TypeReflection::Kind::ParameterBlock;
        if (!isParamBlock) {
            error = "material module '" + moduleLabel + "' declares unsupported top-level global parameter '" +
                    name +
                    "' (Phase 3 materials support exactly one ParameterBlock<...> gParams and nothing else at "
                    "global scope -- route textures through gParams's own bindless-table indices instead)";
            return std::nullopt;
        }
        if (foundParamBlock) {
            error = "material module '" + moduleLabel +
                    "' declares more than one top-level ParameterBlock global ('" + name +
                    "' is the second) -- Phase 3 materials support exactly one";
            return std::nullopt;
        }

        auto set = static_cast<uint32_t>(param->getBindingIndex());
        if (set != kMaterialParamBlockSet) {
            error = "material module '" + moduleLabel + "': parameter block '" + name +
                     "' is bound to descriptor set " + std::to_string(set) + "; this engine requires set " +
                     std::to_string(kMaterialParamBlockSet) +
                     " -- declare it as `[[vk::binding(0, 1)]] ParameterBlock<...> gParams;`";
            return std::nullopt;
        }

        rx::shader::ShaderLayoutInfo::Binding binding;
        binding.set = kMaterialParamBlockSet;
        binding.binding = kMaterialParamBlockBinding;
        binding.count = 1;
        binding.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binding.stages = kMaterialStageFlags;
        binding.unboundedArray = false;
        info.bindings.push_back(binding);
        foundParamBlock = true;
    }

    if (!foundParamBlock) {
        error = "material module '" + moduleLabel +
                "' does not declare a ParameterBlock<...> gParams global (every material must bind its "
                "parameters through exactly one, at descriptor set " +
                std::to_string(kMaterialParamBlockSet) + ")";
        return std::nullopt;
    }
    return info;
}

// --- Pipeline variant cache key ------------------------------------------
struct PipelineKey {
    uint64_t moduleHash = 0;
    uint64_t passHash = 0;
    uint32_t specializationBits = 0;

    bool operator==(const PipelineKey&) const = default;
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey& key) const noexcept {
        uint64_t h = kFnvOffsetBasis;
        fnv1aMix64(h, key.moduleHash);
        fnv1aMix64(h, key.passHash);
        fnv1aMix32(h, key.specializationBits);
        return static_cast<size_t>(h);
    }
};

}  // namespace

// One loaded material: its retained linked Slang program (needed for
// getEntryPointCode() -- called once, right here, never again), reflected
// layout, built pipeline layout, and the two VkShaderModules every
// getPipeline() call for this material reuses verbatim.
struct MaterialRecord {
    std::filesystem::path path;
    uint64_t contentHash = 0;

    Slang::ComPtr<slang::IComponentType> linkedProgram;
    rx::shader::ShaderLayoutInfo layoutInfo;
    rx::rhi::PipelineLayoutBundle layoutBundle;

    VkShaderModule vertexModule = VK_NULL_HANDLE;
    VkShaderModule fragmentModule = VK_NULL_HANDLE;
};

struct MaterialSystem::Impl {
    VkDevice device = VK_NULL_HANDLE;
    rx::rhi::BindlessTable* bindless = nullptr;
    std::filesystem::path pipelineCachePath;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;

    // One process session per MaterialSystem instance -- NOT the
    // process-wide shared IGlobalSession rx_shader::Compiler keeps behind
    // a private, non-exported static (compiler.cpp's sharedGlobalSession())
    // -- since that instance is private to rx_shader and this task's
    // Modify list does not include touching rx_shader to expose it. This
    // pays Slang's real fixed cost (its standard-library load) once more
    // than the process's rx_shader-owned Compilers already do if both
    // happen to be alive at once; acceptable for a class that itself is
    // typically constructed once and lives for the process's whole
    // renderer lifetime, same as rx::graph::Executor/RenderGraph.
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    Slang::ComPtr<slang::ISession> session;

    // The shared entry-point module every material composes against --
    // loaded once here, in create(), and its two entry points retained for
    // every subsequent loadMaterial() call. See forward_entry.slang's own
    // header comment for the full link-time-specialization mechanism this
    // drives.
    slang::IModule* forwardEntryModule = nullptr;  // owned by `session`, not this Impl -- see loadMaterial()'s
                                                    // own comment on why material modules are handled the same way.
    Slang::ComPtr<slang::IEntryPoint> vertexEntryPoint;
    Slang::ComPtr<slang::IEntryPoint> fragmentEntryPoint;

    rx::core::HandlePool<MaterialTag, MaterialRecord> materials;

    // rx::core::HandlePool (rx_core/handle.h) exposes no iteration of its
    // own live slots -- acquire()/release()/get() only, since nothing
    // before this task ever needed to walk every handle it had ever
    // handed out. Adding that is outside Task 5's Modify list (rx_core is
    // not touched by this task), so ~MaterialSystem() instead destroys
    // every MaterialRecord's owned Vulkan objects by re-deriving a fresh
    // `MaterialRecord*` from each handle gathered here, via materials.get(),
    // AT DESTRUCTION TIME -- not by caching the pointer loadMaterial()
    // itself once saw. That distinction is load-bearing, not stylistic:
    // HandlePool::acquire() backs its slots with a plain std::vector and
    // pushes onto it on a cache-miss (handle.h), so a SECOND loadMaterial()
    // call can reallocate and invalidate a `MaterialRecord*` a FIRST call
    // already handed out. Every other accessor in this file
    // (moduleHash()/pipelineLayout()/layoutInfo()/getPipeline()) already
    // avoids this correctly by re-fetching its own pointer fresh on every
    // call; this vector of handles (not pointers) is what lets the
    // destructor -- which only ever runs after every loadMaterial() call
    // this instance will ever make -- do the same.
    std::vector<MaterialHandle> materialHandles;

    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;

    // Test-only seam -- see detail::debugCompileCount()'s own comment
    // (material_system.h).
    uint64_t compileCount = 0;
};

namespace detail {

uint64_t debugCompileCount(const MaterialSystem& system) { return system.impl_->compileCount; }

}  // namespace detail

namespace {

// Loads `filename` (one of the two fixed, shared files under
// RX_MATERIAL_SHADER_DIR -- material.slang/forward_entry.slang) as a
// module named after its own stem, exactly like rx_shader::Compiler's
// compileFromFile() reads a file itself and calls loadModuleFromSource()
// rather than session->loadModule() -- see MaterialSystem::create()'s own
// comment for why this project's established idiom is followed here too,
// and material_system.h's comment on RX_MATERIAL_SHADER_DIR for why this
// directory is a compile-time-baked absolute path rather than a
// create()-time parameter. Returns nullptr (logged) on any failure --
// `diagnostics` always receives whatever Slang reported, on success or
// failure alike, matching CompileResult's own "diagnostics are never
// silently swallowed" contract.
slang::IModule* loadSharedModule(slang::ISession* session, const std::filesystem::path& dir, const char* filename,
                                   std::string& diagnostics) {
    std::filesystem::path path = dir / filename;
    auto source = readFileBytes(path);
    if (!source.has_value()) {
        RX_LOG_ERROR("rx_material: could not read shared shader file '{}'", path.string());
        return nullptr;
    }

    std::string moduleName = path.stem().string();
    Slang::ComPtr<slang::IBlob> sourceBlob(Slang::INIT_ATTACH, slang_createBlob(source->data(), source->size()));
    Slang::ComPtr<slang::IBlob> diagBlob;
    slang::IModule* module =
        session->loadModuleFromSource(moduleName.c_str(), path.string().c_str(), sourceBlob, diagBlob.writeRef());
    appendDiagnostics(diagnostics, moduleName.c_str(), diagBlob);
    if (module == nullptr) {
        RX_LOG_ERROR("rx_material: failed to load shared module '{}':\n{}", path.string(), diagnostics);
    }
    return module;
}

void destroyMaterialRecord(VkDevice device, MaterialRecord& record) {
    if (record.vertexModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, record.vertexModule, nullptr);
        record.vertexModule = VK_NULL_HANDLE;
    }
    if (record.fragmentModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, record.fragmentModule, nullptr);
        record.fragmentModule = VK_NULL_HANDLE;
    }
    // record.layoutBundle destroys its own VkDescriptorSetLayout(s)/
    // VkPipelineLayout on destruction (PipelineLayoutBundle's own
    // destructor, pipeline_layout.h) -- nothing further to do here.
}

}  // namespace

MaterialSystem::MaterialSystem(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MaterialSystem::~MaterialSystem() {
    if (!impl_) {
        return;
    }
    Impl& impl = *impl_;

    // Same vkDeviceWaitIdle-before-destroying-anything discipline
    // rx::graph::Executor::~Executor() documents as load-bearing -- proves
    // no in-flight command buffer can still reference a pipeline this
    // destructor is about to destroy.
    vkDeviceWaitIdle(impl.device);

    for (auto& [key, pipeline] : impl.pipelines) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(impl.device, pipeline, nullptr);
        }
    }
    impl.pipelines.clear();

    // See Impl::materialHandles' own comment for why this re-derives each
    // MaterialRecord* fresh via materials.get() here, rather than walking a
    // vector of pointers gathered earlier.
    for (MaterialHandle handle : impl.materialHandles) {
        MaterialRecord* record = impl.materials.get(handle);
        if (record != nullptr) {
            destroyMaterialRecord(impl.device, *record);
        }
    }

    // Best-effort save -- never fatal, matching create()'s equally
    // best-effort LOAD of this same file [Task 5 ambiguity resolution].
    if (impl.pipelineCache != VK_NULL_HANDLE) {
        size_t dataSize = 0;
        vkGetPipelineCacheData(impl.device, impl.pipelineCache, &dataSize, nullptr);
        if (dataSize > 0) {
            std::vector<char> data(dataSize);
            VkResult result = vkGetPipelineCacheData(impl.device, impl.pipelineCache, &dataSize, data.data());
            if (result == VK_SUCCESS) {
                std::ofstream out(impl.pipelineCachePath, std::ios::binary | std::ios::trunc);
                if (out) {
                    out.write(data.data(), static_cast<std::streamsize>(dataSize));
                    if (!out) {
                        RX_LOG_WARN("rx_material: failed writing pipeline cache to '{}'",
                                    impl.pipelineCachePath.string());
                    } else {
                        RX_LOG_INFO("rx_material: saved {} bytes of pipeline cache data to '{}'", dataSize,
                                    impl.pipelineCachePath.string());
                    }
                } else {
                    RX_LOG_WARN("rx_material: could not open '{}' for writing pipeline cache",
                                impl.pipelineCachePath.string());
                }
            } else {
                RX_LOG_WARN("rx_material: vkGetPipelineCacheData (data fetch) failed: VkResult={}",
                            static_cast<int>(result));
            }
        } else {
            RX_LOG_WARN("rx_material: vkGetPipelineCacheData reported zero bytes; not writing pipeline cache file");
        }
        vkDestroyPipelineCache(impl.device, impl.pipelineCache, nullptr);
    }
}

std::unique_ptr<MaterialSystem> MaterialSystem::create(rx::rhi::Device& device, rx::rhi::BindlessTable& bindless,
                                                         const std::filesystem::path& pipelineCachePath) {
    rx::core::log::init();

    Slang::ComPtr<slang::IGlobalSession> globalSession;
    if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())) || globalSession.get() == nullptr) {
        RX_LOG_ERROR("rx_material: slang::createGlobalSession failed");
        return nullptr;
    }

    // Target/session setup mirrors rx_shader::Compiler::create() exactly
    // (SPIR-V target, "sm_6_0" profile, explicit spirv_1_3 capability
    // floor) [R:A5, spec Fixed decision #3] -- plus `searchPaths`, which
    // Compiler's own sessions never set (it has no multi-file import to
    // resolve): this is what lets `import material;` inside
    // forward_entry.slang and every material module resolve to
    // RX_MATERIAL_SHADER_DIR/material.slang without this class needing to
    // pre-load it explicitly.
    slang::TargetDesc targetDesc{};
    targetDesc.format = SLANG_SPIRV;
    targetDesc.profile = globalSession->findProfile("sm_6_0");

    slang::CompilerOptionEntry capabilityEntry{};
    SlangCapabilityID spirvFloor = globalSession->findCapability("spirv_1_3");
    if (spirvFloor != SLANG_CAPABILITY_UNKNOWN) {
        capabilityEntry.name = slang::CompilerOptionName::Capability;
        capabilityEntry.value.kind = slang::CompilerOptionValueKind::Int;
        capabilityEntry.value.intValue0 = static_cast<int32_t>(spirvFloor);
        targetDesc.compilerOptionEntries = &capabilityEntry;
        targetDesc.compilerOptionEntryCount = 1;
    } else {
        RX_LOG_WARN("rx_material: Slang capability 'spirv_1_3' not found by this Slang build; "
                    "compiling without an explicit SPIR-V version floor");
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
        RX_LOG_ERROR("rx_material: IGlobalSession::createSession failed");
        return nullptr;
    }

    std::string diagnostics;
    slang::IModule* forwardEntryModule =
        loadSharedModule(session, shaderDir, "forward_entry.slang", diagnostics);
    if (forwardEntryModule == nullptr) {
        return nullptr;
    }

    Slang::ComPtr<slang::IEntryPoint> vertexEntryPoint;
    if (SLANG_FAILED(forwardEntryModule->findEntryPointByName("vertexMain", vertexEntryPoint.writeRef())) ||
        vertexEntryPoint.get() == nullptr) {
        RX_LOG_ERROR("rx_material: forward_entry.slang has no 'vertexMain' entry point");
        return nullptr;
    }
    Slang::ComPtr<slang::IEntryPoint> fragmentEntryPoint;
    if (SLANG_FAILED(forwardEntryModule->findEntryPointByName("fragmentMain", fragmentEntryPoint.writeRef())) ||
        fragmentEntryPoint.get() == nullptr) {
        RX_LOG_ERROR("rx_material: forward_entry.slang has no 'fragmentMain' entry point");
        return nullptr;
    }

    // --- Pipeline cache: load if present, fresh+empty otherwise --------
    // [Task 5 ambiguity resolution: an unreadable/corrupt cache file is a
    // logged warning, never fatal -- Vulkan itself guarantees
    // vkCreatePipelineCache never rejects malformed initialData outright
    // (a driver must treat it as though none were given), so the only
    // real failure mode to guard here is the file READ step, not the
    // blob's own internal structure.]
    std::vector<char> initialCacheData;
    std::error_code existsError;
    if (std::filesystem::exists(pipelineCachePath, existsError) && !existsError) {
        std::ifstream in(pipelineCachePath, std::ios::binary | std::ios::ate);
        if (!in) {
            RX_LOG_WARN("rx_material: could not open existing pipeline cache '{}'; starting with a fresh cache",
                        pipelineCachePath.string());
        } else {
            std::streamoff size = in.tellg();
            in.seekg(0, std::ios::beg);
            if (size > 0) {
                initialCacheData.resize(static_cast<size_t>(size));
                in.read(initialCacheData.data(), size);
                if (!in) {
                    RX_LOG_WARN(
                        "rx_material: failed reading existing pipeline cache '{}'; starting with a fresh cache",
                        pipelineCachePath.string());
                    initialCacheData.clear();
                } else {
                    RX_LOG_INFO("rx_material: loading {} bytes of pipeline cache data from '{}'",
                                initialCacheData.size(), pipelineCachePath.string());
                }
            }
        }
    } else {
        RX_LOG_INFO("rx_material: no existing pipeline cache at '{}'; starting fresh", pipelineCachePath.string());
    }

    VkPipelineCacheCreateInfo cacheCreateInfo{};
    cacheCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cacheCreateInfo.initialDataSize = initialCacheData.size();
    cacheCreateInfo.pInitialData = initialCacheData.empty() ? nullptr : initialCacheData.data();

    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(device.device(), &cacheCreateInfo, nullptr, &pipelineCache) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_material: vkCreatePipelineCache failed");
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->device = device.device();
    impl->bindless = &bindless;
    impl->pipelineCachePath = pipelineCachePath;
    impl->pipelineCache = pipelineCache;
    impl->globalSession = std::move(globalSession);
    impl->session = std::move(session);
    impl->forwardEntryModule = forwardEntryModule;
    impl->vertexEntryPoint = std::move(vertexEntryPoint);
    impl->fragmentEntryPoint = std::move(fragmentEntryPoint);

    return std::unique_ptr<MaterialSystem>(new MaterialSystem(std::move(impl)));
}

MaterialHandle MaterialSystem::loadMaterial(const std::filesystem::path& slangModulePath) {
    Impl& impl = *impl_;

    auto source = readFileBytes(slangModulePath);
    if (!source.has_value()) {
        RX_LOG_ERROR("rx_material: could not read material module '{}'", slangModulePath.string());
        throw std::runtime_error("rx_material: could not read material module '" + slangModulePath.string() + "'");
    }
    uint64_t contentHash = fnv1aBytes(source->data(), source->size());

    // Loaded via loadModuleFromSource() + a self-read ifstream, exactly
    // like rx_shader::Compiler::compileFromFile() -- not
    // session->loadModule(slangModulePath), which resolves purely through
    // Slang's own search-path-based file lookup and would make a
    // material's own path (as opposed to the two fixed, shared,
    // search-path-resolved files) subject to that same lookup for no
    // benefit. `moduleName` is the file's stem, so this inherits
    // compiler.h's own documented SAME-MODULE-NAME CAVEAT verbatim:
    // reloading a DIFFERENT file that happens to share a stem already
    // loaded once this session -- or the SAME path with changed content --
    // through this SAME MaterialSystem instance fails with Slang's
    // "already loaded with different source" diagnostic. Not exercised by
    // this task's own test list (no reload test), and out of scope for
    // Task 5's brief; a future hot-reload path needs a fresh
    // MaterialSystem (fresh session) per reload, mirroring
    // samples/02_hotreload's own established fix for the identical
    // constraint on rx_shader::Compiler.
    std::string moduleName = slangModulePath.stem().string();
    if (moduleName.empty()) {
        moduleName = "material";
    }

    std::string diagnostics;
    Slang::ComPtr<slang::IBlob> sourceBlob(Slang::INIT_ATTACH, slang_createBlob(source->data(), source->size()));
    Slang::ComPtr<slang::IBlob> loadDiag;
    slang::IModule* materialModule = impl.session->loadModuleFromSource(
        moduleName.c_str(), slangModulePath.string().c_str(), sourceBlob, loadDiag.writeRef());
    appendDiagnostics(diagnostics, "load", loadDiag);
    if (materialModule == nullptr) {
        RX_LOG_ERROR("rx_material: failed to load material module '{}':\n{}", slangModulePath.string(), diagnostics);
        throw std::runtime_error("rx_material: failed to load material module '" + slangModulePath.string() +
                                  "':\n" + diagnostics);
    }

    std::vector<slang::IComponentType*> parts;
    parts.push_back(impl.forwardEntryModule);
    parts.push_back(impl.vertexEntryPoint.get());
    parts.push_back(impl.fragmentEntryPoint.get());
    parts.push_back(materialModule);

    Slang::ComPtr<slang::IComponentType> composite;
    Slang::ComPtr<slang::IBlob> composeDiag;
    SlangResult composeResult = impl.session->createCompositeComponentType(
        parts.data(), static_cast<SlangInt>(parts.size()), composite.writeRef(), composeDiag.writeRef());
    appendDiagnostics(diagnostics, "compose", composeDiag);
    if (SLANG_FAILED(composeResult) || composite.get() == nullptr) {
        RX_LOG_ERROR("rx_material: failed to compose material '{}':\n{}", slangModulePath.string(), diagnostics);
        throw std::runtime_error("rx_material: failed to compose material '" + slangModulePath.string() + "':\n" +
                                  diagnostics);
    }

    Slang::ComPtr<slang::IComponentType> linked;
    Slang::ComPtr<slang::IBlob> linkDiag;
    // Every composite+link attempt is counted here, success or failure --
    // see detail::debugCompileCount()'s own comment (material_system.h):
    // this is the ONLY place MaterialSystem ever invokes Slang's front-end
    // beyond the one-time forward_entry.slang load in create().
    ++impl.compileCount;
    SlangResult linkResult = composite->link(linked.writeRef(), linkDiag.writeRef());
    appendDiagnostics(diagnostics, "link", linkDiag);
    if (SLANG_FAILED(linkResult) || linked.get() == nullptr) {
        RX_LOG_ERROR("rx_material: failed to link material '{}':\n{}", slangModulePath.string(), diagnostics);
        throw std::runtime_error("rx_material: failed to link material '" + slangModulePath.string() + "':\n" +
                                  diagnostics);
    }

    Slang::ComPtr<slang::IBlob> layoutDiag;
    slang::ProgramLayout* layout = linked->getLayout(0, layoutDiag.writeRef());
    appendDiagnostics(diagnostics, "layout", layoutDiag);
    if (layout == nullptr) {
        RX_LOG_ERROR("rx_material: IComponentType::getLayout failed for material '{}':\n{}",
                     slangModulePath.string(), diagnostics);
        throw std::runtime_error("rx_material: IComponentType::getLayout failed for material '" +
                                  slangModulePath.string() + "':\n" + diagnostics);
    }

    std::string reflectError;
    auto layoutInfo = reflectMaterialLayout(layout, slangModulePath.string(), reflectError);
    if (!layoutInfo.has_value()) {
        RX_LOG_ERROR("rx_material: {}", reflectError);
        throw std::runtime_error("rx_material: " + reflectError);
    }

    auto layoutBundle =
        rx::rhi::PipelineLayoutBuilder::build(impl.device, *layoutInfo, impl.bindless->descriptorSetLayout());
    if (!layoutBundle.has_value()) {
        RX_LOG_ERROR("rx_material: PipelineLayoutBuilder::build failed for material '{}'", slangModulePath.string());
        throw std::runtime_error("rx_material: PipelineLayoutBuilder::build failed for material '" +
                                  slangModulePath.string() + "'");
    }

    // Entry point order in the composite above is fixed
    // (forwardEntryModule, vertexEntryPoint, fragmentEntryPoint,
    // materialModule) -- vertex is always index 0, fragment always index 1,
    // matching compiler.cpp's own "entry points contribute in exactly the
    // order pushed" contract. No per-entry-point stage lookup needed (this
    // is always exactly these two, in this order, for every material).
    Slang::ComPtr<slang::IBlob> vertexCodeDiag;
    Slang::ComPtr<slang::IBlob> vertexCode;
    SlangResult vertexCodeResult = linked->getEntryPointCode(0, 0, vertexCode.writeRef(), vertexCodeDiag.writeRef());
    appendDiagnostics(diagnostics, "codegen(vertex)", vertexCodeDiag);
    if (SLANG_FAILED(vertexCodeResult) || vertexCode.get() == nullptr) {
        RX_LOG_ERROR("rx_material: vertex code generation failed for material '{}':\n{}", slangModulePath.string(),
                     diagnostics);
        throw std::runtime_error("rx_material: vertex code generation failed for material '" +
                                  slangModulePath.string() + "':\n" + diagnostics);
    }

    Slang::ComPtr<slang::IBlob> fragmentCodeDiag;
    Slang::ComPtr<slang::IBlob> fragmentCode;
    SlangResult fragmentCodeResult =
        linked->getEntryPointCode(1, 0, fragmentCode.writeRef(), fragmentCodeDiag.writeRef());
    appendDiagnostics(diagnostics, "codegen(fragment)", fragmentCodeDiag);
    if (SLANG_FAILED(fragmentCodeResult) || fragmentCode.get() == nullptr) {
        RX_LOG_ERROR("rx_material: fragment code generation failed for material '{}':\n{}", slangModulePath.string(),
                     diagnostics);
        throw std::runtime_error("rx_material: fragment code generation failed for material '" +
                                  slangModulePath.string() + "':\n" + diagnostics);
    }

    MaterialRecord record;
    record.path = slangModulePath;
    record.contentHash = contentHash;
    record.linkedProgram = linked;
    record.layoutInfo = std::move(*layoutInfo);
    record.layoutBundle = std::move(*layoutBundle);

    VkShaderModuleCreateInfo vertexModuleInfo{};
    vertexModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertexModuleInfo.codeSize = vertexCode->getBufferSize();
    vertexModuleInfo.pCode = static_cast<const uint32_t*>(vertexCode->getBufferPointer());
    if (vkCreateShaderModule(impl.device, &vertexModuleInfo, nullptr, &record.vertexModule) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_material: vkCreateShaderModule (vertex) failed for material '{}'",
                     slangModulePath.string());
        destroyMaterialRecord(impl.device, record);
        throw std::runtime_error("rx_material: vkCreateShaderModule (vertex) failed for material '" +
                                  slangModulePath.string() + "'");
    }

    VkShaderModuleCreateInfo fragmentModuleInfo{};
    fragmentModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragmentModuleInfo.codeSize = fragmentCode->getBufferSize();
    fragmentModuleInfo.pCode = static_cast<const uint32_t*>(fragmentCode->getBufferPointer());
    if (vkCreateShaderModule(impl.device, &fragmentModuleInfo, nullptr, &record.fragmentModule) != VK_SUCCESS) {
        RX_LOG_ERROR("rx_material: vkCreateShaderModule (fragment) failed for material '{}'",
                     slangModulePath.string());
        destroyMaterialRecord(impl.device, record);
        throw std::runtime_error("rx_material: vkCreateShaderModule (fragment) failed for material '" +
                                  slangModulePath.string() + "'");
    }

    if (!diagnostics.empty()) {
        RX_LOG_WARN("rx_material: diagnostics while loading material '{}':\n{}", slangModulePath.string(),
                     diagnostics);
    }

    MaterialHandle handle = impl.materials.acquire(std::move(record));
    impl.materialHandles.push_back(handle);
    return handle;
}

uint64_t MaterialSystem::moduleHash(MaterialHandle handle) const {
    MaterialRecord* record = impl_->materials.get(handle);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::moduleHash: invalid or stale MaterialHandle");
    }
    return record->contentHash;
}

VkPipelineLayout MaterialSystem::pipelineLayout(MaterialHandle handle) const {
    MaterialRecord* record = impl_->materials.get(handle);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::pipelineLayout: invalid or stale MaterialHandle");
    }
    return record->layoutBundle.layout;
}

const rx::shader::ShaderLayoutInfo& MaterialSystem::layoutInfo(MaterialHandle handle) const {
    MaterialRecord* record = impl_->materials.get(handle);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::layoutInfo: invalid or stale MaterialHandle");
    }
    return record->layoutInfo;
}

VkPipeline MaterialSystem::getPipeline(const PipelineRequest& req) {
    Impl& impl = *impl_;

    MaterialRecord* record = impl.materials.get(req.material);
    if (record == nullptr) {
        throw std::out_of_range("rx_material: MaterialSystem::getPipeline: invalid or stale MaterialHandle");
    }

    // [Task 5 ambiguity resolution] no Phase 3 use case for a graphics
    // pipeline with neither a color nor a depth attachment.
    if (req.pass.colorCount == 0 && req.pass.depthFormat == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error(
            "rx_material: MaterialSystem::getPipeline: PassSignature declares no color attachments and no depth "
            "attachment; rejecting (no use case in Phase 3)");
    }

    PipelineKey key{record->contentHash, req.pass.hash(), req.specializationBits};
    auto it = impl.pipelines.find(key);
    if (it != impl.pipelines.end()) {
        return it->second;
    }

    // --- Build a new VkPipeline from this material's already-compiled  --
    // --- SPIR-V (record->vertexModule/fragmentModule) and req.pass's   --
    // --- attachment shape. No Slang involvement at all past this point --
    // --- -- see detail::debugCompileCount()'s own comment.             --
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = record->vertexModule;
    // Slang's SPIR-V backend always names each entry point's OpEntryPoint
    // "main" regardless of source-level function name -- verified
    // directly for this exact multi-part-composite path before writing
    // this file (see task-5-report.md), matching
    // samples/03_bindless_mesh/main.cpp's and samples/02_hotreload's own
    // established finding for the simpler single-module case.
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = record->fragmentModule;
    stages[1].pName = "main";

    VertexInputState vertexInput = makeVertexInputState();
    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputState.vertexBindingDescriptionCount = 1;
    vertexInputState.pVertexBindingDescriptions = &vertexInput.binding;
    vertexInputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInput.attributes.size());
    vertexInputState.pVertexAttributeDescriptions = vertexInput.attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
    inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationState.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = req.pass.samples;

    const bool hasDepth = req.pass.depthFormat != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo depthStencilState{};
    depthStencilState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState.depthTestEnable = hasDepth ? VK_TRUE : VK_FALSE;
    depthStencilState.depthWriteEnable = hasDepth ? VK_TRUE : VK_FALSE;
    depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS;

    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(req.pass.colorCount);
    for (auto& blendAttachment : blendAttachments) {
        blendAttachment.blendEnable = VK_FALSE;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    VkPipelineColorBlendStateCreateInfo colorBlendState{};
    colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendState.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    colorBlendState.pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data();

    std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineRenderingCreateInfo renderingCreateInfo{};
    renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCreateInfo.colorAttachmentCount = req.pass.colorCount;
    renderingCreateInfo.pColorAttachmentFormats = req.pass.colorCount > 0 ? req.pass.colorFormats.data() : nullptr;
    renderingCreateInfo.depthAttachmentFormat = req.pass.depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingCreateInfo;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInputState;
    pipelineInfo.pInputAssemblyState = &inputAssemblyState;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizationState;
    pipelineInfo.pMultisampleState = &multisampleState;
    pipelineInfo.pDepthStencilState = &depthStencilState;
    pipelineInfo.pColorBlendState = &colorBlendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = record->layoutBundle.layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;  // dynamic rendering
    pipelineInfo.basePipelineIndex = -1;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(impl.device, impl.pipelineCache, 1, &pipelineInfo, nullptr, &pipeline) !=
        VK_SUCCESS) {
        RX_LOG_ERROR("rx_material: vkCreateGraphicsPipelines failed for material '{}'", record->path.string());
        throw std::runtime_error("rx_material: vkCreateGraphicsPipelines failed for material '" +
                                  record->path.string() + "'");
    }

    impl.pipelines.emplace(key, pipeline);
    return pipeline;
}

}  // namespace rx::material
